/*
 * QEMU Apple Macintosh 128K hardware system emulator
 *
 * 68000 at 7.8336 MHz, 128KB RAM, 64KB ROM, and the classic trio:
 * VIA 6522 (timers, RTC, overlay/page bits, mouse button), Z8530 SCC
 * (serial plus the mouse quadrature interrupts on the DCD pins) and
 * the IWM with one Sony 400K GCR floppy drive.  Video is a 512x342
 * 1-bit framebuffer scanned out of main RAM.
 *
 * Memory map (68000, 24-bit bus, everything mirrors generously):
 *   0x000000  RAM (ROM while the overlay is active)
 *   0x400000  ROM 64KB (mirrored through the 1MB block)
 *   0x600000  RAM while the overlay is active
 *   0x9FFFF8  SCC read space  (B ctrl, A ctrl, B data, A data)
 *   0xBFFFF8  SCC write space (same registers, write decode)
 *   0xDFE1FF  IWM, registers every 512 bytes
 *   0xEFE1FE  VIA, registers every 512 bytes
 *
 * The overlay: at reset the ROM appears at address 0 (the CPU fetches
 * its vectors from it) and RAM at 0x600000; the ROM clears VIA port A
 * bit 4 once it has set up RAM, which swaps RAM down to address 0.
 *
 * Modelled on maciisi.c (VIA glue, RTC, framebuffer console) with the
 * device set grown empirically against the 64KB Rev A ROM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/datadir.h"
#include "system/system.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "target/m68k/cpu.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/escc.h"
#include "hw/misc/mos6522.h"
#include "hw/block/iwm.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "system/rtc.h"
#include "ui/console.h"
#include "ui/input.h"
#include "hw/display/framebuffer.h"
#include "system/qtest.h"
#include "system/reset.h"

#define MAC128K_ROM_ADDR      0x400000
#define MAC128K_ROM_SIZE      0x10000
#define MAC128K_ROM_MIRROR    0x100000
#define MAC128K_ROM_FILENAME  "Mac128K.ROM"

#define MAC128K_RAM_WINDOW    0x400000    /* RAM decode, mirrored */
#define MAC128K_OVL_RAM_BASE  0x600000
#define MAC128K_OVL_RAM_SIZE  0x200000

#define MAC128K_SCC_RD_BASE   0x9ffff8
#define MAC128K_SCC_WR_BASE   0xbffff8
#define MAC128K_IWM_BASE      0xdfe000
#define MAC128K_VIA_BASE      0xefe000

#define VIA_SPACING_SHIFT     9           /* VIA/IWM regs every 0x200 */
#define VIA_REGION_SIZE       0x2000

/* the VIA runs on the 783.36 kHz E clock (7.8336 MHz / 10) */
#define VIA_TIMER_FREQ        783360
/* SCC PCLK */
#define MAC_SCC_CLOCK         3672000

/* VIA port A bits */
#define VIA_A_VOLUME          0x07        /* sound volume */
#define VIA_A_SNDPG2          0x08        /* 0 = alternate sound buffer */
#define VIA_A_OVERLAY         0x10        /* 1 = ROM at address 0 */
#define VIA_A_SEL             0x20        /* SEL line to the floppy drive */
#define VIA_A_PAGE2           0x40        /* 0 = alternate screen buffer */
#define VIA_A_SCCWRREQ        0x80        /* input: SCC wait/request */

/* VIA port B bits */
#define VIA_B_RTCDATA         0x01
#define VIA_B_RTCCLK          0x02
#define VIA_B_RTCENB          0x04
#define VIA_B_MOUSEBTN        0x08        /* input, 0 = pressed */
#define VIA_B_MOUSEX2         0x10        /* input, mouse X quadrature */
#define VIA_B_MOUSEY2         0x20        /* input, mouse Y quadrature */
#define VIA_B_HBLANK          0x40        /* input, horizontal blanking */
#define VIA_B_SNDENB          0x80        /* 0 = sound enabled */

/* video */
#define MAC128K_FB_WIDTH      512
#define MAC128K_FB_HEIGHT     342
#define MAC128K_FB_ROWBYTES   64
#define MAC128K_FB_MAIN_OFS   0x5900      /* main buffer at ram top - this */
#define MAC128K_FB_ALT_OFS    0xd900

/* 60.15 Hz vertical blank */
#define VIA_60HZ_TIMER_PERIOD_NS 16625800

/*
 * Interrupt glue: VIA at level 1, SCC at level 2, autovectored.
 *
 * The naive model - VIA on /IPL0, SCC on /IPL1, both asserted = level
 * 3 - livelocks: the ROM's level-3 autovector points at a bare RTE
 * (the tail of its SCC ISR), so with level-triggered re-sampling the
 * CPU takes level 3 forever and neither device ever gets serviced.
 * Behave like a priority encoder instead: SCC pending wins at level
 * 2, the VIA waits at level 1 and is taken once the SCC clears.
 */
#define TYPE_MAC128K_GLUE "mac128k-glue"
OBJECT_DECLARE_SIMPLE_TYPE(Mac128kGlueState, MAC128K_GLUE)

struct Mac128kGlueState {
    SysBusDevice parent_obj;
    M68kCPU *cpu;
    uint8_t levels;
};

#define MAC128K_GLUE_VIA      0
#define MAC128K_GLUE_SCC      1

static void mac128k_glue_set_irq(void *opaque, int irq, int level)
{
    Mac128kGlueState *s = opaque;
    int ipl;

    if (level) {
        s->levels |= 1 << irq;
    } else {
        s->levels &= ~(1 << irq);
    }

    ipl = (s->levels & 2) ? 2 : (s->levels & 1) ? 1 : 0;
    if (ipl) {
        m68k_set_irq_level(s->cpu, ipl, 24 + ipl);
    } else {
        m68k_set_irq_level(s->cpu, 0, 0);
    }
}

static void mac128k_glue_init(Object *obj)
{
    Mac128kGlueState *s = MAC128K_GLUE(obj);

    qdev_init_gpio_in(DEVICE(obj), mac128k_glue_set_irq, 2);
    s->levels = 0;
}

static const Property mac128k_glue_properties[] = {
    DEFINE_PROP_LINK("cpu", Mac128kGlueState, cpu, TYPE_M68K_CPU, M68kCPU *),
};

static void mac128k_glue_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_props(DEVICE_CLASS(klass), mac128k_glue_properties);
}

/*
 * VIA subclass: the classic 343-0042 RTC/PRAM chip hangs off port B
 * bits 0-2 (data, clock, /enable).  Engine as in maciisi.c/mac_via.c.
 */

/* VIA returns time offset from Jan 1, 1904, not 1970 */
#define RTC_OFFSET 2082844800

enum {
    REG_0,
    REG_1,
    REG_2,
    REG_3,
    REG_TEST,
    REG_WPROTECT,
    REG_PRAM_ADDR,
    REG_PRAM_ADDR_LAST = REG_PRAM_ADDR + 19,
    REG_INVALID,
    REG_EMPTY = 0xff,
};

#define TYPE_MOS6522_MAC128K "mos6522-mac128k"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522Mac128kState, MOS6522_MAC128K)

struct Mac128kMachineState;

struct MOS6522Mac128kState {
    MOS6522State parent_obj;

    struct Mac128kMachineState *machine;
    uint8_t last_b;

    /* RTC */
    uint8_t PRAM[20];
    uint32_t tick_offset;
    uint8_t data_out;
    int data_out_cnt;
    uint8_t data_in;
    int data_in_cnt;
    uint8_t cmd;
    uint8_t wprotect;
};

/*
 * The old RTC command byte: 0b z tt ttt 01, top bit set = read.  Only
 * the seconds registers, test/write-protect and 20 bytes of PRAM exist
 * (no extended XPRAM commands on this chip).
 */
static int mac128k_rtc_compact_cmd(uint8_t value)
{
    uint8_t read = value & 0x80;

    value &= 0x7f;
    if ((value & 0x03) == 0x01) {
        value >>= 2;
        if ((value & 0x18) == 0) {
            return read | (REG_0 + (value & 0x03));
        } else if ((value == 0x0c) && !read) {
            return REG_TEST;
        } else if ((value == 0x0d) && !read) {
            return REG_WPROTECT;
        } else if ((value & 0x1c) == 0x08) {
            /* RAM address 0x10 to 0x13 */
            return read | (REG_PRAM_ADDR + 0x10 + (value & 0x03));
        } else if ((value & 0x10) == 0x10) {
            /* RAM address 0x00 to 0x0f */
            return read | (REG_PRAM_ADDR + (value & 0x0f));
        }
    }
    return REG_INVALID;
}

static void mac128k_rtc_update(MOS6522Mac128kState *v1s)
{
    MOS6522State *s = MOS6522(v1s);
    int cmd;
    uint32_t time;

    if (s->b & VIA_B_RTCENB) {
        return;
    }

    if (s->dirb & VIA_B_RTCDATA) {
        /* send bits to the RTC */
        if (!(v1s->last_b & VIA_B_RTCCLK) && (s->b & VIA_B_RTCCLK)) {
            v1s->data_out <<= 1;
            v1s->data_out |= s->b & VIA_B_RTCDATA;
            v1s->data_out_cnt++;
        }
    } else {
        /* receive bits from the RTC */
        if ((v1s->last_b & VIA_B_RTCCLK) &&
            !(s->b & VIA_B_RTCCLK) &&
            v1s->data_in_cnt) {
            s->b = (s->b & ~VIA_B_RTCDATA) |
                   ((v1s->data_in >> 7) & VIA_B_RTCDATA);
            v1s->data_in <<= 1;
            v1s->data_in_cnt--;
        }
        return;
    }

    if (v1s->data_out_cnt != 8) {
        return;
    }
    v1s->data_out_cnt = 0;

    if (v1s->cmd == REG_EMPTY) {
        /* first byte: the command */
        cmd = mac128k_rtc_compact_cmd(v1s->data_out);

        if (cmd == REG_INVALID) {
            qemu_log_mask(LOG_UNIMP, "mac128k rtc: invalid cmd 0x%02x\n",
                          v1s->data_out);
            return;
        }
        if (cmd & 0x80) {
            switch (cmd & 0x7f) {
            case REG_0...REG_3:
                time = v1s->tick_offset +
                       (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                        / NANOSECONDS_PER_SECOND);
                v1s->data_in = (time >> ((cmd & 0x03) << 3)) & 0xff;
                v1s->data_in_cnt = 8;
                break;
            case REG_PRAM_ADDR...REG_PRAM_ADDR_LAST:
                v1s->data_in = v1s->PRAM[(cmd & 0x7f) - REG_PRAM_ADDR];
                v1s->data_in_cnt = 8;
                break;
            default:
                g_assert_not_reached();
            }
            return;
        }
        if (cmd == REG_WPROTECT || !v1s->wprotect) {
            v1s->cmd = cmd;
        }
        return;
    }

    /* second byte: the parameter of a write command */
    switch (v1s->cmd & 0x7f) {
    case REG_0...REG_3:
    case REG_TEST:
        break;
    case REG_WPROTECT:
        v1s->wprotect = !!(v1s->data_out & 0x80);
        break;
    case REG_PRAM_ADDR...REG_PRAM_ADDR_LAST:
        v1s->PRAM[v1s->cmd - REG_PRAM_ADDR] = v1s->data_out;
        break;
    default:
        g_assert_not_reached();
    }
    v1s->cmd = REG_EMPTY;
}

static void mac128k_via_portA_write(MOS6522State *s);
static void mac128k_via_portB_write(MOS6522State *s);

static void mos6522_mac128k_init(Object *obj)
{
    MOS6522Mac128kState *v1s = MOS6522_MAC128K(obj);

    v1s->cmd = REG_EMPTY;
}

static void mos6522_mac128k_reset_hold(Object *obj, ResetType type)
{
    MOS6522Mac128kState *v1s = MOS6522_MAC128K(obj);
    MOS6522State *ms = MOS6522(v1s);
    MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(ms);

    if (mdc->parent_phases.hold) {
        mdc->parent_phases.hold(obj, type);
    }

    /* input pins idle high (button up, no blanking source modelled) */
    ms->a = 0xff;
    ms->b = 0xff;

    v1s->cmd = REG_EMPTY;
    v1s->data_out_cnt = 0;
    v1s->data_in_cnt = 0;
}

static void mos6522_mac128k_class_init(ObjectClass *klass, const void *data)
{
    MOS6522DeviceClass *mdc = MOS6522_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    mdc->portA_write = mac128k_via_portA_write;
    mdc->portB_write = mac128k_via_portB_write;
    resettable_class_set_parent_phases(rc, NULL, mos6522_mac128k_reset_hold,
                                       NULL, &mdc->parent_phases);
}

/*
 * Framebuffer console: 512x342 1-bit, white = 0, scanned from main RAM
 * at (ram top - 0x5900), or - 0xD900 for the alternate page.
 */

#define TYPE_MAC128K_FB "mac128k-fb"
OBJECT_DECLARE_SIMPLE_TYPE(Mac128kFbState, MAC128K_FB)

struct Mac128kFbState {
    SysBusDevice parent_obj;

    MemoryRegion *ram;
    uint32_t base;              /* current page offset into RAM */
    MemoryRegionSection fbsection;
    QemuConsole *con;
    int invalidate;
};

static void mac128k_fb_draw_line(void *opaque, uint8_t *d, const uint8_t *s,
                                 int width, int pitch)
{
    uint32_t *buf = (uint32_t *)d;
    int i, b;

    for (i = 0; i < MAC128K_FB_WIDTH / 8; i++) {
        uint8_t src = s[i];

        for (b = 0; b < 8; b++) {
            buf[i * 8 + b] = (src & 0x80) ? 0xFF000000 : 0xFFFFFFFF;
            src <<= 1;
        }
    }
}

static bool mac128k_fb_update(void *opaque)
{
    Mac128kFbState *s = MAC128K_FB(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first = 0, last = 0;

    if (s->invalidate) {
        /*
         * Address the RAM region directly rather than resolving through
         * the system flatview: while the reset overlay is active the
         * low addresses decode to the ROM alias, and an early refresh
         * (the GTK display polls from power-on) would latch a section
         * pointing into the ROM — the video scanner on the real board
         * reads RAM regardless of the CPU's overlay.
         */
        s->fbsection = (MemoryRegionSection) {
            .mr = s->ram,
            .offset_within_region = s->base,
            .size = int128_make64((uint64_t)MAC128K_FB_HEIGHT *
                                  MAC128K_FB_ROWBYTES),
        };
        s->invalidate = 0;
    }

    framebuffer_update_display(surface, &s->fbsection,
                               MAC128K_FB_WIDTH, MAC128K_FB_HEIGHT,
                               MAC128K_FB_ROWBYTES, MAC128K_FB_WIDTH * 4,
                               0, 1, mac128k_fb_draw_line, s,
                               &first, &last);
    qemu_console_update(s->con, 0, 0, MAC128K_FB_WIDTH, MAC128K_FB_HEIGHT);
    return true;
}

static void mac128k_fb_invalidate(void *opaque)
{
    Mac128kFbState *s = MAC128K_FB(opaque);

    s->invalidate = 1;
}

static const GraphicHwOps mac128k_fb_ops = {
    .invalidate = mac128k_fb_invalidate,
    .gfx_update = mac128k_fb_update,
};

static void mac128k_fb_realize(DeviceState *dev, Error **errp)
{
    Mac128kFbState *s = MAC128K_FB(dev);

    s->invalidate = 1;
    s->con = qemu_graphic_console_create(dev, 0, &mac128k_fb_ops, s);
    qemu_console_resize(s->con, MAC128K_FB_WIDTH, MAC128K_FB_HEIGHT);
}

static void mac128k_fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->realize = mac128k_fb_realize;
}

/* machine */

struct Mac128kMachineState {
    MachineState parent_obj;

    M68kCPU cpu;
    Mac128kGlueState glue;
    MOS6522Mac128kState via;
    ESCCState escc;
    OrIRQState escc_orgate;
    IWMState iwm;
    Mac128kFbState fb;

    MemoryRegion rom;
    MemoryRegion rom_mirrors;   /* container: ROM repeated over 1MB */
    MemoryRegion ram_mirrors;   /* container: RAM repeated over 4MB */
    MemoryRegion overlay_rom;   /* the ROM block aliased at 0 */
    MemoryRegion overlay_ram;   /* container: RAM at 0x600000 */
    MemoryRegion open_bus;
    MemoryRegion viamem;
    MemoryRegion sccwr;

    bool overlay;
    bool sel_line;
    uint32_t reset_sp;
    uint32_t reset_pc;

    QEMUTimer *sixty_hz_timer;
    QEMUTimer *one_second_timer;

    /* mouse: quadrature through SCC DCD (X1/Y1) and VIA PB4/5 (X2/Y2) */
    QemuInputHandlerState *mouse_hs;
    QEMUTimer *mouse_timer;
    int mouse_dx;
    int mouse_dy;
    bool mouse_x1;
    bool mouse_y1;
};

#define TYPE_MAC128K_MACHINE MACHINE_TYPE_NAME("mac128k")
OBJECT_DECLARE_SIMPLE_TYPE(Mac128kMachineState, MAC128K_MACHINE)

static void mac128k_set_overlay(Mac128kMachineState *m, bool overlay)
{
    if (overlay == m->overlay) {
        return;
    }
    m->overlay = overlay;
    memory_region_set_enabled(&m->overlay_rom, overlay);
    memory_region_set_enabled(&m->overlay_ram, overlay);
    memory_region_set_enabled(&m->ram_mirrors, !overlay);
}

/*
 * Port A outputs.  Pins configured as inputs float high (pulled up),
 * which conveniently matches the reset state of the overlay.
 */
static void mac128k_via_portA_update(Mac128kMachineState *m)
{
    MOS6522State *s = MOS6522(&m->via);
    uint8_t eff = s->a | ~s->dira;      /* pull-ups on input pins */
    bool sel = !!(eff & VIA_A_SEL);

    mac128k_set_overlay(m, !!(eff & VIA_A_OVERLAY));

    if (sel != m->sel_line) {
        m->sel_line = sel;
        qemu_set_irq(qdev_get_gpio_in_named(DEVICE(&m->iwm), "sel", 0), sel);
    }

    /* screen page: PA6 high = main buffer */
    {
        uint32_t base = m->parent_obj.ram_size -
                        ((eff & VIA_A_PAGE2) ? MAC128K_FB_MAIN_OFS
                                             : MAC128K_FB_ALT_OFS);
        if (m->fb.base != base) {
            m->fb.base = base;
            m->fb.invalidate = 1;
        }
    }
}

static void mac128k_via_portA_write(MOS6522State *s)
{
    MOS6522Mac128kState *v1s = MOS6522_MAC128K(s);

    if (v1s->machine) {
        mac128k_via_portA_update(v1s->machine);
    }
}

static void mac128k_via_portB_write(MOS6522State *s)
{
    MOS6522Mac128kState *v1s = MOS6522_MAC128K(s);

    mac128k_rtc_update(v1s);
    v1s->last_b = s->b;
}

static uint64_t mac128k_via_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    return mos6522_read(s, reg, size);
}

static void mac128k_via_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MOS6522Mac128kState *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    mos6522_write(s, reg, val, size);

    /* DDR changes move the pulled-up input pins too */
    if (reg == VIA_REG_DIRA && v1s->machine) {
        mac128k_via_portA_update(v1s->machine);
    }
}

static const MemoryRegionOps mac128k_via_ops = {
    .read = mac128k_via_read,
    .write = mac128k_via_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* SCC write space: same chip, different address decode */

static uint64_t mac128k_sccwr_read(void *opaque, hwaddr addr, unsigned size)
{
    return address_space_ldub(&address_space_memory,
                              MAC128K_SCC_RD_BASE + (addr & 6),
                              MEMTXATTRS_UNSPECIFIED, NULL);
}

static void mac128k_sccwr_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    address_space_stb(&address_space_memory,
                      MAC128K_SCC_RD_BASE + (addr & 6), val,
                      MEMTXATTRS_UNSPECIFIED, NULL);
}

static const MemoryRegionOps mac128k_sccwr_ops = {
    .read = mac128k_sccwr_read,
    .write = mac128k_sccwr_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* open bus: reads return 0, writes vanish; the 68000 never bus-errors */

static uint32_t mac128k_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

static uint64_t mac128k_open_bus_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "mac128k: open-bus read 0x%06x (%d) pc=0x%06x\n",
                  (unsigned)addr, size, mac128k_trace_pc());
    return 0;
}

static void mac128k_open_bus_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "mac128k: open-bus write 0x%06x (%d) <- 0x%" PRIx64
                  " pc=0x%06x\n",
                  (unsigned)addr, size, val, mac128k_trace_pc());
}

static const MemoryRegionOps mac128k_open_bus_ops = {
    .read = mac128k_open_bus_read,
    .write = mac128k_open_bus_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* 60.15 Hz vertical blank on CA1, one-second RTC tick on CA2 */

static void mac128k_sixty_hz(void *opaque)
{
    Mac128kMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via), CA1_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->sixty_hz_timer,
              (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
               VIA_60HZ_TIMER_PERIOD_NS) /
              VIA_60HZ_TIMER_PERIOD_NS * VIA_60HZ_TIMER_PERIOD_NS);
}

static void mac128k_one_second(void *opaque)
{
    Mac128kMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via), CA2_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->one_second_timer,
              (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000) / 1000 * 1000);
}

/*
 * Mouse.  Each movement step toggles X1/Y1, which arrive at the SCC
 * DCD pins and raise an external/status interrupt; the handler then
 * compares the DCD level with the X2/Y2 quadrature phase on VIA port B
 * to pick the direction.  Steps are paced from a timer so each edge
 * gets its interrupt serviced.
 */

#define MOUSE_STEP_NS  1500000  /* ~666 counts/s: the 7.8MHz guest must
                                 * service an SCC interrupt per edge */

/* DCD gpio lines on the ESCC are indexed like chn[]: 0 = B, 1 = A */
#define ESCC_DCD_B     0
#define ESCC_DCD_A     1

static void mac128k_mouse_step(Mac128kMachineState *m)
{
    MOS6522State *s = MOS6522(&m->via);

    if (m->mouse_dx) {
        int dir = m->mouse_dx > 0 ? 1 : -1;

        m->mouse_dx -= dir;
        m->mouse_x1 = !m->mouse_x1;
        /* phase of X2 relative to X1 encodes the direction (found
         * empirically: X counts positive when X2 != X1 at the edge) */
        if ((dir > 0) != m->mouse_x1) {
            s->b |= VIA_B_MOUSEX2;
        } else {
            s->b &= ~VIA_B_MOUSEX2;
        }
        qemu_set_irq(qdev_get_gpio_in_named(DEVICE(&m->escc), "dcd",
                                            ESCC_DCD_A), m->mouse_x1);
    }
    if (m->mouse_dy) {
        int dir = m->mouse_dy > 0 ? 1 : -1;

        m->mouse_dy -= dir;
        m->mouse_y1 = !m->mouse_y1;
        if ((dir > 0) == m->mouse_y1) {
            s->b |= VIA_B_MOUSEY2;
        } else {
            s->b &= ~VIA_B_MOUSEY2;
        }
        qemu_set_irq(qdev_get_gpio_in_named(DEVICE(&m->escc), "dcd",
                                            ESCC_DCD_B), m->mouse_y1);
    }
    if (m->mouse_dx || m->mouse_dy) {
        timer_mod(m->mouse_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MOUSE_STEP_NS);
    }
}

static void mac128k_mouse_timer_cb(void *opaque)
{
    mac128k_mouse_step(opaque);
}

static void mac128k_mouse_event(DeviceState *dev, QemuConsole *src,
                                QemuInputEvent *evt)
{
    Mac128kMachineState *m = MAC128K_MACHINE(qdev_get_machine());
    MOS6522State *s = MOS6522(&m->via);

    switch (evt->type) {
    case INPUT_EVENT_KIND_REL:
        if (evt->rel.axis == INPUT_AXIS_X) {
            m->mouse_dx += evt->rel.value;
        } else if (evt->rel.axis == INPUT_AXIS_Y) {
            m->mouse_dy += evt->rel.value;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            if (evt->btn.down) {
                s->b &= ~VIA_B_MOUSEBTN;
            } else {
                s->b |= VIA_B_MOUSEBTN;
            }
        }
        break;
    default:
        break;
    }
}

static void mac128k_mouse_sync(DeviceState *dev)
{
    Mac128kMachineState *m = MAC128K_MACHINE(qdev_get_machine());

    if ((m->mouse_dx || m->mouse_dy) &&
        !timer_pending(m->mouse_timer)) {
        mac128k_mouse_step(m);
    }
}

static const QemuInputHandler mac128k_mouse_handler = {
    .name  = "Macintosh Mouse",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_REL,
    .event = mac128k_mouse_event,
    .sync  = mac128k_mouse_sync,
};

static void main_cpu_reset(void *opaque)
{
    Mac128kMachineState *m = opaque;
    M68kCPU *cpu = &m->cpu;
    CPUState *cs = CPU(cpu);

    /* the hardware overlay is set by the reset line */
    mac128k_set_overlay(m, true);

    cpu_reset(cs);
    /*
     * SP/PC come from the vector table, which the overlay maps to the
     * ROM: the SP slot holds the ROM checksum (the ROM sets up its own
     * stack), the PC slot an absolute 0x40xxxx address.  The values are
     * stashed at init time because the ROM loader's own reset callback
     * (which fills the ROM region) runs after this handler.
     */
    cpu->env.aregs[7] = m->reset_sp;
    cpu->env.pc = m->reset_pc;
}

static void mac128k_machine_init(MachineState *machine)
{
    Mac128kMachineState *m = MAC128K_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    ram_addr_t ram_size = machine->ram_size;
    const char *bios_name = machine->firmware ?: MAC128K_ROM_FILENAME;
    char *filename;
    int bios_size;
    unsigned i, n;
    DriveInfo *dinfo;
    DeviceState *dev;
    SysBusDevice *sysbus;

    if (ram_size < 128 * KiB || ram_size > MAC128K_RAM_WINDOW ||
        !is_power_of_2(ram_size)) {
        error_report("RAM size must be a power of two between 128K and 4M");
        exit(1);
    }

    /* CPU */
    object_initialize_child(OBJECT(machine), "cpu", &m->cpu,
                            machine->cpu_type);
    qdev_realize(DEVICE(&m->cpu), NULL, &error_fatal);

    /* open bus behind everything: no bus errors on this machine */
    memory_region_init_io(&m->open_bus, OBJECT(machine),
                          &mac128k_open_bus_ops, NULL, "mac128k.open-bus",
                          0x1000000);
    memory_region_add_subregion_overlap(sysmem, 0, &m->open_bus, -2);

    /*
     * RAM: the 128KB mirrors through the whole 4MB RAM window (the ROM
     * sizes memory by looking for the wrap-around).
     */
    memory_region_init(&m->ram_mirrors, OBJECT(machine),
                       "mac128k.ram-mirrors", MAC128K_RAM_WINDOW);
    n = MAC128K_RAM_WINDOW / ram_size;
    memory_region_add_subregion(&m->ram_mirrors, 0, machine->ram);
    for (i = 1; i < n; i++) {
        MemoryRegion *alias = g_new(MemoryRegion, 1);

        memory_region_init_alias(alias, OBJECT(machine), "mac128k.ram-alias",
                                 machine->ram, 0, ram_size);
        memory_region_add_subregion(&m->ram_mirrors, i * ram_size, alias);
    }
    memory_region_add_subregion(sysmem, 0, &m->ram_mirrors);

    /* ROM, mirrored through its 1MB block */
    memory_region_init_rom(&m->rom, NULL, "mac128k.rom", MAC128K_ROM_SIZE,
                           &error_abort);
    memory_region_init(&m->rom_mirrors, OBJECT(machine),
                       "mac128k.rom-mirrors", MAC128K_ROM_MIRROR);
    memory_region_add_subregion(&m->rom_mirrors, 0, &m->rom);
    for (i = 1; i < MAC128K_ROM_MIRROR / MAC128K_ROM_SIZE; i++) {
        MemoryRegion *alias = g_new(MemoryRegion, 1);

        memory_region_init_alias(alias, OBJECT(machine), "mac128k.rom-alias",
                                 &m->rom, 0, MAC128K_ROM_SIZE);
        memory_region_add_subregion(&m->rom_mirrors, i * MAC128K_ROM_SIZE,
                                    alias);
    }
    memory_region_add_subregion(sysmem, MAC128K_ROM_ADDR, &m->rom_mirrors);

    /* the overlay: ROM at 0 and RAM at 0x600000 until PA4 is cleared */
    memory_region_init_alias(&m->overlay_rom, OBJECT(machine),
                             "mac128k.overlay-rom", &m->rom_mirrors, 0,
                             MAC128K_ROM_MIRROR);
    memory_region_add_subregion_overlap(sysmem, 0, &m->overlay_rom, 1);

    memory_region_init(&m->overlay_ram, OBJECT(machine),
                       "mac128k.overlay-ram", MAC128K_OVL_RAM_SIZE);
    for (i = 0; i < MAC128K_OVL_RAM_SIZE / ram_size; i++) {
        MemoryRegion *alias = g_new(MemoryRegion, 1);

        memory_region_init_alias(alias, OBJECT(machine),
                                 "mac128k.overlay-ram-alias",
                                 machine->ram, 0, ram_size);
        memory_region_add_subregion(&m->overlay_ram, i * ram_size, alias);
    }
    memory_region_add_subregion(sysmem, MAC128K_OVL_RAM_BASE,
                                &m->overlay_ram);

    m->overlay = false;
    mac128k_set_overlay(m, true);

    /* IRQ glue */
    object_initialize_child(OBJECT(machine), "glue", &m->glue,
                            TYPE_MAC128K_GLUE);
    object_property_set_link(OBJECT(&m->glue), "cpu", OBJECT(&m->cpu),
                             &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&m->glue), &error_fatal);

    /* VIA */
    object_initialize_child(OBJECT(machine), "via", &m->via,
                            TYPE_MOS6522_MAC128K);
    qdev_prop_set_uint64(DEVICE(&m->via), "frequency", VIA_TIMER_FREQ);
    sysbus = SYS_BUS_DEVICE(&m->via);
    sysbus_realize(sysbus, &error_fatal);
    {
        struct tm tm;

        qemu_get_timedate(&tm, 0);
        m->via.tick_offset = (uint32_t)mktimegm(&tm) + RTC_OFFSET;
    }
    m->via.machine = m;
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), MAC128K_GLUE_VIA));
    memory_region_init_io(&m->viamem, OBJECT(machine), &mac128k_via_ops,
                          &m->via, "via", VIA_REGION_SIZE);
    memory_region_add_subregion(sysmem, MAC128K_VIA_BASE, &m->viamem);

    m->sixty_hz_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mac128k_sixty_hz, m);
    timer_mod(m->sixty_hz_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VIA_60HZ_TIMER_PERIOD_NS);
    m->one_second_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mac128k_one_second,
                                       m);
    timer_mod(m->one_second_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);

    /* SCC */
    object_initialize_child(OBJECT(machine), "escc", &m->escc, TYPE_ESCC);
    dev = DEVICE(&m->escc);
    qdev_prop_set_uint32(dev, "disabled", 0);
    qdev_prop_set_uint32(dev, "frequency", MAC_SCC_CLOCK);
    qdev_prop_set_uint32(dev, "it_shift", 1);
    qdev_prop_set_bit(dev, "bit_swap", true);
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));
    qdev_prop_set_uint32(dev, "chnBtype", 0);
    qdev_prop_set_uint32(dev, "chnAtype", 0);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize(sysbus, &error_fatal);

    object_initialize_child(OBJECT(machine), "escc_orgate", &m->escc_orgate,
                            TYPE_OR_IRQ);
    object_property_set_int(OBJECT(&m->escc_orgate), "num-lines", 2,
                            &error_fatal);
    dev = DEVICE(&m->escc_orgate);
    qdev_realize(dev, NULL, &error_fatal);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(dev, 0));
    sysbus_connect_irq(sysbus, 1, qdev_get_gpio_in(dev, 1));
    qdev_connect_gpio_out(dev, 0,
                          qdev_get_gpio_in(DEVICE(&m->glue),
                                           MAC128K_GLUE_SCC));
    memory_region_add_subregion(sysmem, MAC128K_SCC_RD_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->escc),
                                                       0));
    memory_region_init_io(&m->sccwr, OBJECT(machine), &mac128k_sccwr_ops, m,
                          "scc-wr", 8);
    memory_region_add_subregion(sysmem, MAC128K_SCC_WR_BASE, &m->sccwr);

    /* IWM + the internal Sony 400K drive */
    object_initialize_child(OBJECT(machine), "iwm", &m->iwm, TYPE_IWM);
    dinfo = drive_get(IF_FLOPPY, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive(DEVICE(&m->iwm), "drive",
                            blk_by_legacy_dinfo(dinfo));
    }
    sysbus = SYS_BUS_DEVICE(&m->iwm);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(sysmem, MAC128K_IWM_BASE,
                                sysbus_mmio_get_region(sysbus, 0));
    /*
     * Drive-speed PWM: the spindle speed comes from the low bytes of
     * the words in the main sound page (ram top - 0x300); point the
     * drive at them so its tachometer answers with the speed the ROM
     * dialled in.
     */
    m->iwm.pwm_buf = (const uint8_t *)
                     memory_region_get_ram_ptr(machine->ram) +
                     ram_size - 0x300 + 1;

    /* video */
    object_initialize_child(OBJECT(machine), "fb", &m->fb, TYPE_MAC128K_FB);
    m->fb.ram = machine->ram;
    m->fb.base = ram_size - MAC128K_FB_MAIN_OFS;
    sysbus_realize(SYS_BUS_DEVICE(&m->fb), &error_fatal);

    /* mouse */
    m->mouse_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mac128k_mouse_timer_cb,
                                  m);
    m->mouse_hs = qemu_input_handler_register(DEVICE(&m->via),
                                              &mac128k_mouse_handler);

    /* ROM contents */
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, MAC128K_ROM_ADDR,
                                        MAC128K_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }
    if (!qtest_enabled()) {
        uint8_t *ptr;

        if (bios_size <= 0 || bios_size > MAC128K_ROM_SIZE) {
            error_report("could not load Macintosh ROM '%s'", bios_name);
            exit(1);
        }
        ptr = rom_ptr(MAC128K_ROM_ADDR, bios_size);
        assert(ptr != NULL);
        m->reset_sp = ldl_be_p(ptr);
        m->reset_pc = ldl_be_p(ptr + 4);
    }

    qemu_register_reset(main_cpu_reset, m);
}

static void mac128k_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68000"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Apple Macintosh 128K";
    mc->init = mac128k_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->block_default_type = IF_FLOPPY;
    mc->default_ram_size = 128 * KiB;
    mc->default_ram_id = "mac128k.ram";
}

static const TypeInfo mac128k_machine_typeinfo[] = {
    {
        .name       = TYPE_MAC128K_GLUE,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mac128kGlueState),
        .instance_init = mac128k_glue_init,
        .class_init = mac128k_glue_class_init,
    },
    {
        .name       = TYPE_MOS6522_MAC128K,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522Mac128kState),
        .instance_init = mos6522_mac128k_init,
        .class_init = mos6522_mac128k_class_init,
    },
    {
        .name       = TYPE_MAC128K_FB,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mac128kFbState),
        .class_init = mac128k_fb_class_init,
    },
    {
        .name       = TYPE_MAC128K_MACHINE,
        .parent     = TYPE_MACHINE,
        .instance_size = sizeof(Mac128kMachineState),
        .class_init = mac128k_machine_class_init,
    },
};

DEFINE_TYPES(mac128k_machine_typeinfo)
