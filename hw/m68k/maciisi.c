/*
 * QEMU Macintosh IIsi hardware system emulator
 *
 * 68030 machine with RBV (RAM-Based Video), Egret ADB/PRAM MCU, EASC
 * sound, SWIM floppy, NCR5380 SCSI and Z8530 SCC.  Modelled on q800.c;
 * device set stubbed and grown empirically against the IIsi ROM.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/datadir.h"
#include "system/system.h"
#include "target/m68k/cpu.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/escc.h"
#include "hw/misc/mos6522.h"
#include "hw/audio/asc.h"
#include "hw/block/swim.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "system/rtc.h"
#include "system/qtest.h"
#include "system/reset.h"

#define MACIISI_ROM_ADDR      0x40800000
#define MACIISI_ROM_SIZE      0x00080000
#define MACIISI_ROM_FILENAME  "maciisi.rom"

#define IO_BASE               0x50000000
#define IO_SLICE              0x00040000
#define IO_SLICE_MASK         (IO_SLICE - 1)
#define IO_SIZE               0x04000000

/* offsets within the repeating I/O slice (0x50F0xxxx aliases to these) */
#define VIA1_OFS              0x00000
#define SCC_OFS               0x04000
#define SCSI_OFS              0x10000
#define ASC_OFS               0x14000
#define SWIM_OFS              0x16000
#define VDAC_OFS              0x24000
#define RBV_OFS               0x26000

#define VIA_SPACING_SHIFT     9       /* VIA regs every 0x200 */
#define VIA_REGION_SIZE       0x2000
#define VIA_TIMER_FREQ        783360

/* 24-bit mode physical layout (MMU translation is a no-op on this branch,
 * so the classic 24-bit map is provided as physical aliases instead) */
#define ADDR24_RAM_LIMIT      0x00800000
#define ADDR24_ROM_BASE       0x00800000
#define ADDR24_IO_BASE        0x00F00000
#define ADDR24_IO_SIZE        0x00100000

#define MAC_CLOCK             3686418

/* Size of whole RAM area (unbacked reads return 0 for RAM sizing) */
#define RAM_SIZE              0x40000000

/*
 * Interrupt glue: one input per 680x0 interrupt level (input n asserts
 * IPL n+1, autovectored).
 */
#define TYPE_MACIISI_GLUE "maciisi-glue"
OBJECT_DECLARE_SIMPLE_TYPE(MacIIsiGlueState, MACIISI_GLUE)

struct MacIIsiGlueState {
    SysBusDevice parent_obj;
    M68kCPU *cpu;
    uint8_t ipr;
};

#define MACIISI_GLUE_VIA1     0       /* level 1 */
#define MACIISI_GLUE_RBV      1       /* level 2 */
#define MACIISI_GLUE_SCC      3       /* level 4 */
#define MACIISI_GLUE_NMI      6      /* level 7 */

static void maciisi_glue_set_irq(void *opaque, int irq, int level)
{
    MacIIsiGlueState *s = opaque;
    int i;

    if (level) {
        s->ipr |= 1 << irq;
    } else {
        s->ipr &= ~(1 << irq);
    }

    for (i = 7; i >= 0; i--) {
        if ((s->ipr >> i) & 1) {
            m68k_set_irq_level(s->cpu, i + 1, i + 25);
            return;
        }
    }
    m68k_set_irq_level(s->cpu, 0, 0);
}

static void maciisi_glue_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    MacIIsiGlueState *s = MACIISI_GLUE(dev);

    qdev_init_gpio_in(dev, maciisi_glue_set_irq, 8);
    s->ipr = 0;
}

static const Property maciisi_glue_properties[] = {
    DEFINE_PROP_LINK("cpu", MacIIsiGlueState, cpu, TYPE_M68K_CPU, M68kCPU *),
};

static void maciisi_glue_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, maciisi_glue_properties);
}

/*
 * VIA1 subclass.  The IIsi ROM speaks the classic 343-0042 RTC/PRAM
 * serial protocol on PB0 (data), PB1 (clock), PB2 (/enable) — on real
 * hardware these lines go to the Egret MCU, which emulates the old RTC
 * chip for them.  RTC engine ported from hw/misc/mac_via.c.
 */

#define VIA1B_vRTCEnb  0x04
#define VIA1B_vRTCClk  0x02
#define VIA1B_vRTCData 0x01

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
    REG_PRAM_SECT,
    REG_PRAM_SECT_LAST = REG_PRAM_SECT + 7,
    REG_INVALID,
    REG_EMPTY = 0xff,
};

#define TYPE_MOS6522_MACIISI "mos6522-maciisi"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522MacIIsiState, MOS6522_MACIISI)

struct MacIIsiMachineState;

struct MOS6522MacIIsiState {
    MOS6522State parent_obj;

    struct MacIIsiMachineState *machine;
    uint8_t pins_a;
    uint8_t pins_b;
    uint8_t last_b;

    /* RTC */
    uint8_t PRAM[256];
    uint32_t tick_offset;
    uint8_t data_out;
    int data_out_cnt;
    uint8_t data_in;
    int data_in_cnt;
    uint8_t cmd;
    uint8_t alt;
    uint8_t wprotect;
};

static int via1_rtc_compact_cmd(uint8_t value)
{
    uint8_t read = value & 0x80;

    value &= 0x7f;

    /* the last 2 bits of a command byte must always be 0b01 ... */
    if ((value & 0x78) == 0x38) {
        /* except for the extended memory designator */
        return read | (REG_PRAM_SECT + (value & 0x07));
    }
    if ((value & 0x03) == 0x01) {
        value >>= 2;
        if ((value & 0x18) == 0) {
            /* seconds registers */
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

static void via1_rtc_update(MOS6522MacIIsiState *v1s)
{
    MOS6522State *s = MOS6522(v1s);
    int cmd, sector, addr;
    uint32_t time;

    if (s->b & VIA1B_vRTCEnb) {
        return;
    }

    if (s->dirb & VIA1B_vRTCData) {
        /* send bits to the RTC */
        if (!(v1s->last_b & VIA1B_vRTCClk) && (s->b & VIA1B_vRTCClk)) {
            v1s->data_out <<= 1;
            v1s->data_out |= s->b & VIA1B_vRTCData;
            v1s->data_out_cnt++;
        }
    } else {
        /* receive bits from the RTC */
        if ((v1s->last_b & VIA1B_vRTCClk) &&
            !(s->b & VIA1B_vRTCClk) &&
            v1s->data_in_cnt) {
            s->b = (s->b & ~VIA1B_vRTCData) |
                   ((v1s->data_in >> 7) & VIA1B_vRTCData);
            v1s->data_in <<= 1;
            v1s->data_in_cnt--;
        }
        return;
    }

    if (v1s->data_out_cnt != 8) {
        return;
    }

    v1s->data_out_cnt = 0;

    /* first byte: it's a command */
    if (v1s->cmd == REG_EMPTY) {
        cmd = via1_rtc_compact_cmd(v1s->data_out);

        if (cmd == REG_INVALID) {
            qemu_log_mask(LOG_UNIMP, "maciisi rtc: invalid cmd 0x%02x\n",
                          v1s->data_out);
            return;
        }

        if (cmd & 0x80) { /* this is a read command */
            switch (cmd & 0x7f) {
            case REG_0...REG_3: /* seconds registers */
                time = v1s->tick_offset +
                       (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                        / NANOSECONDS_PER_SECOND);
                v1s->data_in = (time >> ((cmd & 0x03) << 3)) & 0xff;
                v1s->data_in_cnt = 8;
                break;
            case REG_PRAM_ADDR...REG_PRAM_ADDR_LAST:
                /* PRAM address 0x00 -> 0x13 */
                v1s->data_in = v1s->PRAM[(cmd & 0x7f) - REG_PRAM_ADDR];
                v1s->data_in_cnt = 8;
                break;
            case REG_PRAM_SECT...REG_PRAM_SECT_LAST:
                /*
                 * extended memory designator and sector number
                 * the only two-byte read command
                 */
                v1s->cmd = cmd;
                break;
            default:
                g_assert_not_reached();
            }
            return;
        }

        /* this is a write command, needs a parameter */
        if (cmd == REG_WPROTECT || !v1s->wprotect) {
            v1s->cmd = cmd;
        }
        return;
    }

    /* second byte: it's a parameter */
    if (v1s->alt == REG_EMPTY) {
        switch (v1s->cmd & 0x7f) {
        case REG_0...REG_3: /* seconds register */
            v1s->cmd = REG_EMPTY;
            break;
        case REG_TEST:
            /* device control: nothing to do */
            v1s->cmd = REG_EMPTY;
            break;
        case REG_WPROTECT:
            v1s->wprotect = !!(v1s->data_out & 0x80);
            v1s->cmd = REG_EMPTY;
            break;
        case REG_PRAM_ADDR...REG_PRAM_ADDR_LAST:
            v1s->PRAM[v1s->cmd - REG_PRAM_ADDR] = v1s->data_out;
            v1s->cmd = REG_EMPTY;
            break;
        case REG_PRAM_SECT...REG_PRAM_SECT_LAST:
            addr = (v1s->data_out >> 2) & 0x1f;
            sector = (v1s->cmd & 0x7f) - REG_PRAM_SECT;
            if (v1s->cmd & 0x80) {
                /* it's a read */
                v1s->data_in = v1s->PRAM[sector * 32 + addr];
                /*
                 * Force 32-bit addressing (XPRAM 0x8A): the 24-bit MMU
                 * path derefs tagged handles that our translation
                 * doesn't wrap; 32-bit-clean boot avoids it entirely.
                 */
                if (sector * 32 + addr == 0x8a) {
                    v1s->data_in |= 0x05;
                }
                v1s->data_in_cnt = 8;
                v1s->cmd = REG_EMPTY;
            } else {
                /* it's a write, we need one more parameter */
                v1s->alt = addr;
            }
            break;
        default:
            g_assert_not_reached();
        }
        return;
    }

    /* third byte: it's the data of a REG_PRAM_SECT write */
    g_assert(REG_PRAM_SECT <= v1s->cmd && v1s->cmd <= REG_PRAM_SECT_LAST);
    sector = v1s->cmd - REG_PRAM_SECT;
    v1s->PRAM[sector * 32 + v1s->alt] = v1s->data_out;
    v1s->alt = REG_EMPTY;
    v1s->cmd = REG_EMPTY;
}

/*
 * Egret handshake lines on VIA1 port B (all active low):
 *   PB5 out  /SYS_SESSION  host requests a session
 *   PB4 out  /VIA_FULL     host byte-level handshake
 *   PB3 in   /XCVR_SESSION driven by the Egret
 * Data moves through the VIA shift register with the Egret as the
 * external clock source.
 */
#define EGRET_SYS_SESSION  0x20
#define EGRET_VIA_FULL     0x10
#define EGRET_XCVR         0x08

static void maciisi_egret_session_update(MOS6522MacIIsiState *v1s);
static void maciisi_egret_sr_written(MOS6522MacIIsiState *v1s);
static void maciisi_egret_sr_read(MOS6522MacIIsiState *v1s);
static void maciisi_egret_acr_changed(MOS6522MacIIsiState *v1s);

static void maciisi_via1_portA_write(MOS6522State *s)
{
}

static void maciisi_via1_portB_write(MOS6522State *s)
{
    MOS6522MacIIsiState *v1s = MOS6522_MACIISI(s);

    if (v1s->machine) {
        maciisi_egret_session_update(v1s);
    }
}

static void mos6522_maciisi_init(Object *obj)
{
    MOS6522MacIIsiState *v1s = MOS6522_MACIISI(obj);

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;
}

static void mos6522_maciisi_reset_hold(Object *obj, ResetType type)
{
    MOS6522MacIIsiState *v1s = MOS6522_MACIISI(obj);
    MOS6522State *ms = MOS6522(v1s);
    MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(ms);

    if (mdc->parent_phases.hold) {
        mdc->parent_phases.hold(obj, type);
    }

    /* board-specific idle state of the input pins */
    ms->a = v1s->pins_a;
    ms->b = v1s->pins_b;

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;
    v1s->data_out_cnt = 0;
    v1s->data_in_cnt = 0;
}

static const Property mos6522_maciisi_properties[] = {
    DEFINE_PROP_UINT8("pins-a", MOS6522MacIIsiState, pins_a, 0xff),
    DEFINE_PROP_UINT8("pins-b", MOS6522MacIIsiState, pins_b, 0xff),
};

static void mos6522_maciisi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    MOS6522DeviceClass *mdc = MOS6522_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_props(dc, mos6522_maciisi_properties);
    mdc->portA_write = maciisi_via1_portA_write;
    mdc->portB_write = maciisi_via1_portB_write;
    resettable_class_set_parent_phases(rc, NULL, mos6522_maciisi_reset_hold,
                                       NULL, &mdc->parent_phases);
}

/*
 * Onboard video framebuffer: 640x480 1-bit at the slot $E aperture
 * (ScrnBase 0xFEE00000), rowbytes 80.
 */

#define TYPE_MACIISI_FB "maciisi-fb"
OBJECT_DECLARE_SIMPLE_TYPE(MacIIsiFbState, MACIISI_FB)

#define MACIISI_FB_WIDTH   640
#define MACIISI_FB_HEIGHT  480
#define MACIISI_FB_ROWBYTES 80

struct MacIIsiFbState {
    SysBusDevice parent_obj;

    MemoryRegion vram;
    MemoryRegionSection fbsection;
    QemuConsole *con;
    int invalidate;
};

static void maciisi_fb_draw_line(void *opaque, uint8_t *d, const uint8_t *s,
                                 int width, int pitch)
{
    uint32_t *buf = (uint32_t *)d;
    int i, b;

    for (i = 0; i < MACIISI_FB_WIDTH / 8; i++) {
        uint8_t src = s[i];

        for (b = 0; b < 8; b++) {
            buf[i * 8 + b] = (src & 0x80) ? 0xFF000000 : 0xFFFFFFFF;
            src <<= 1;
        }
    }
}

static bool maciisi_fb_update(void *opaque)
{
    MacIIsiFbState *s = MACIISI_FB(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first = 0, last = 0;

    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection, &s->vram, 0,
                                          MACIISI_FB_HEIGHT,
                                          MACIISI_FB_ROWBYTES);
        s->invalidate = 0;
    }

    framebuffer_update_display(surface, &s->fbsection,
                               MACIISI_FB_WIDTH, MACIISI_FB_HEIGHT,
                               MACIISI_FB_ROWBYTES, MACIISI_FB_WIDTH * 4,
                               0, 1, maciisi_fb_draw_line, s,
                               &first, &last);
    qemu_console_update(s->con, 0, 0, MACIISI_FB_WIDTH, MACIISI_FB_HEIGHT);
    return true;
}

static void maciisi_fb_invalidate(void *opaque)
{
    MacIIsiFbState *s = MACIISI_FB(opaque);

    s->invalidate = 1;
}

static const GraphicHwOps maciisi_fb_ops = {
    .invalidate = maciisi_fb_invalidate,
    .gfx_update = maciisi_fb_update,
};

static void maciisi_fb_realize(DeviceState *dev, Error **errp)
{
    MacIIsiFbState *s = MACIISI_FB(dev);

    memory_region_init_ram(&s->vram, OBJECT(dev), "maciisi.vram", 0x180000,
                           &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->vram);

    s->invalidate = 1;
    s->con = qemu_graphic_console_create(dev, 0, &maciisi_fb_ops, s);
    qemu_console_resize(s->con, MACIISI_FB_WIDTH, MACIISI_FB_HEIGHT);
}

static void maciisi_fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->realize = maciisi_fb_realize;
}

/* machine */

struct MacIIsiMachineState {
    MachineState parent_obj;

    M68kCPU cpu;
    MacIIsiGlueState glue;
    MOS6522MacIIsiState via1;
    ESCCState escc;
    OrIRQState escc_orgate;
    ASCState asc;
    Swim swim;

    MacIIsiFbState fb;
    MemoryRegion rom;
    MemoryRegion rom_alias;
    MemoryRegion rom_slot_alias;
    MemoryRegion vram_alias;
    MemoryRegion rom_alias24;
    MemoryRegion ramio;
    MemoryRegion macio;
    MemoryRegion macio_alias;
    MemoryRegion macio_alias24;
    MemoryRegion via1mem;
    MemoryRegion rbv_via2mem;
    MemoryRegion escc_mirror;
    MemoryRegion swim_mirror;
    MemoryRegion rbvmem;
    MemoryRegion vdacmem;
    MemoryRegion scsimem;
    MemoryRegion iotrace;

    uint8_t rbv_regs[0x100];
    uint8_t rbv_ifr;
    uint8_t rbv_sifr;
    uint8_t rbv_ier;
    uint8_t rbv_sier;
    uint8_t rbv_via2_regs[16];
    uint8_t vdac_regs[0x40];
    uint8_t scsi_regs[8];

    /* VIA1 CA1 60Hz tick and CA2 one-second interrupts */
    QEMUTimer *sixty_hz_timer;
    QEMUTimer *vbl_off_timer;
    QEMUTimer *one_second_timer;

    /* Egret ADB/system MCU on the VIA1 shift register */
    QEMUTimer *egret_timer;
    uint8_t egret_cmd[16];
    int egret_cmd_len;
    uint8_t egret_resp[16];
    int egret_resp_len;
    int egret_resp_idx;
    bool egret_session;
    bool egret_no_resp;
};

#define TYPE_MACIISI_MACHINE MACHINE_TYPE_NAME("maciisi")
OBJECT_DECLARE_SIMPLE_TYPE(MacIIsiMachineState, MACIISI_MACHINE)

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static uint32_t maciisi_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

static uint64_t ramio_read(void *opaque, hwaddr addr, unsigned size)
{
    static int count;

    if (count < 200) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "maciisi ram-hole: read  0x%08x (%d) pc=0x%08x\n",
                      (unsigned)addr, size, maciisi_trace_pc());
    }
    return 0x0;
}

static void ramio_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    static int count;

    if (count < 200) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "maciisi ram-hole: write 0x%08x (%d) <- 0x%08" PRIx64
                      " pc=0x%08x\n",
                      (unsigned)addr, size, val, maciisi_trace_pc());
    }
}

static const MemoryRegionOps ramio_ops = {
    .read = ramio_read,
    .write = ramio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static MemTxResult macio_alias_read(void *opaque, hwaddr addr, uint64_t *data,
                                    unsigned size, MemTxAttrs attrs)
{
    MemTxResult r;
    uint32_t val;

    addr &= IO_SLICE_MASK;
    addr |= IO_BASE;

    switch (size) {
    case 4:
        val = address_space_ldl_be(&address_space_memory, addr, attrs, &r);
        break;
    case 2:
        val = address_space_lduw_be(&address_space_memory, addr, attrs, &r);
        break;
    case 1:
        val = address_space_ldub(&address_space_memory, addr, attrs, &r);
        break;
    default:
        g_assert_not_reached();
    }

    *data = val;
    return r;
}

static MemTxResult macio_alias_write(void *opaque, hwaddr addr, uint64_t value,
                                     unsigned size, MemTxAttrs attrs)
{
    MemTxResult r;

    addr &= IO_SLICE_MASK;
    addr |= IO_BASE;

    switch (size) {
    case 4:
        address_space_stl_be(&address_space_memory, addr, value, attrs, &r);
        break;
    case 2:
        address_space_stw_be(&address_space_memory, addr, value, attrs, &r);
        break;
    case 1:
        address_space_stb(&address_space_memory, addr, value, attrs, &r);
        break;
    default:
        g_assert_not_reached();
    }

    return r;
}

static const MemoryRegionOps macio_alias_ops = {
    .read_with_attrs = macio_alias_read,
    .write_with_attrs = macio_alias_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* SCC mirror at +0xC000: the 8 SCC bytes repeat through the window */

static MemTxResult maciisi_escc_mirror_read(void *opaque, hwaddr addr,
                                            uint64_t *data, unsigned size,
                                            MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_read(opaque, addr, data, size, attrs);
}

static MemTxResult maciisi_escc_mirror_write(void *opaque, hwaddr addr,
                                             uint64_t val, unsigned size,
                                             MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_write(opaque, addr, val, size, attrs);
}

static const MemoryRegionOps maciisi_escc_mirror_ops = {
    .read_with_attrs = maciisi_escc_mirror_read,
    .write_with_attrs = maciisi_escc_mirror_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t maciisi_via1_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS6522MacIIsiState *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;
    uint64_t val;

    val = mos6522_read(s, reg, size);

    /* input pins on port A read the board straps, not the last output */
    if (reg == VIA_REG_A || reg == VIA_REG_ANH) {
        val = (val & s->dira) | (v1s->pins_a & ~s->dira);
    }

    if (reg == VIA_REG_SR && v1s->machine) {
        maciisi_egret_sr_read(v1s);
    }

    return val;
}

static void maciisi_via1_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MOS6522MacIIsiState *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    mos6522_write(s, reg, val, size);

    if (reg == VIA_REG_SR || reg == VIA_REG_ACR) {
        qemu_log_mask(LOG_UNIMP,
                      "maciisi via1: reg%d <- 0x%02" PRIx64 " pc=0x%08x\n",
                      (int)reg, val, maciisi_trace_pc());
    }

    if (reg == VIA_REG_SR && v1s->machine) {
        maciisi_egret_sr_written(v1s);
    }
    if (reg == VIA_REG_ACR && v1s->machine) {
        maciisi_egret_acr_changed(v1s);
    }

    if (reg == VIA_REG_B) {
        via1_rtc_update(v1s);
        v1s->last_b = s->b;
    }
}

static const MemoryRegionOps maciisi_via1_ops = {
    .read = maciisi_via1_read,
    .write = maciisi_via1_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};


/*
 * RBV: VIA2 functionality (IFR/IER with the VIA set/clear protocol,
 * slot interrupt flags) plus video control.  Register 0x10 carries the
 * monitor sense in the low bits; 6 = Apple 13" 640x480 RGB.
 */

#define RBV_RSIFR  0x02    /* slot interrupt flags (bit 6 = slot $E VBL) */
#define RBV_RIFR   0x03    /* interrupt flags (bit 1 = any slot) */
#define RBV_RMONP  0x10
#define RBV_RSIER  0x12    /* slot interrupt enables, VIA set/clr protocol */
#define RBV_RIER   0x13

#define RBV_MONITOR_SENSE  0x06
#define RBV_SLOT_E_INT     0x40
#define RBV_IFR_SLOT       0x02

static void maciisi_rbv_update_irq(MacIIsiMachineState *m);

static uint64_t maciisi_rbv_read(void *opaque, hwaddr addr, unsigned size)
{
    MacIIsiMachineState *m = opaque;
    uint64_t val;

    /* only the low 5 address bits select a register */
    addr &= 0x1f;
    switch (addr) {
    case RBV_RIFR:
        val = m->rbv_ifr;
        if (m->rbv_ifr & m->rbv_ier & 0x7f) {
            val |= 0x80;
        }
        break;
    case RBV_RSIFR:
        val = m->rbv_sifr;
        break;
    case RBV_RIER:
        val = m->rbv_ier | 0x80;
        break;
    case RBV_RSIER:
        val = m->rbv_sier | 0x80;
        break;
    case RBV_RMONP:
        val = (m->rbv_regs[addr] & 0xf8) | RBV_MONITOR_SENSE;
        break;
    default:
        val = m->rbv_regs[addr];
        break;
    }

    qemu_log_mask(LOG_UNIMP, "maciisi rbv: read  +0x%02x -> 0x%02" PRIx64
                  " pc=0x%08x\n", (unsigned)addr, val, maciisi_trace_pc());
    return val;
}

static void maciisi_rbv_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MacIIsiMachineState *m = opaque;

    addr &= 0x1f;
    qemu_log_mask(LOG_UNIMP, "maciisi rbv: write +0x%02x <- 0x%02" PRIx64
                  " pc=0x%08x\n", (unsigned)addr, val, maciisi_trace_pc());

    switch (addr) {
    case RBV_RIFR:
        m->rbv_ifr &= ~(val & 0x7f);
        break;
    case RBV_RSIFR:
        m->rbv_sifr &= ~(val & 0x7f);
        break;
    case RBV_RIER:
        if (val & 0x80) {
            m->rbv_ier |= val & 0x7f;
        } else {
            m->rbv_ier &= ~(val & 0x7f);
        }
        break;
    case RBV_RSIER:
        if (val & 0x80) {
            m->rbv_sier |= val & 0x7f;
        } else {
            m->rbv_sier &= ~(val & 0x7f);
        }
        break;
    default:
        m->rbv_regs[addr] = val;
        break;
    }
    maciisi_rbv_update_irq(m);
}

static const MemoryRegionOps maciisi_rbv_ops = {
    .read = maciisi_rbv_read,
    .write = maciisi_rbv_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* 60.15Hz VBL tick on CA1 and one-second tick on CA2, as on mac_via */

#define VIA_60HZ_TIMER_PERIOD_NS   16625800

static void maciisi_rbv_update_irq(MacIIsiMachineState *m)
{
    /*
     * The onboard-video VBL (slot $E, SIFR bit 6) is a pollable status
     * line only — the ROM's frame wait reads it directly and does not
     * install a slot interrupt handler for it, so it must not raise a
     * CPU interrupt (that yields dsBadSlotInt).  Other slot bits do
     * summarise into IFR bit 1 → level 2.
     */
    uint8_t slot_cpu = m->rbv_sifr & m->rbv_sier & 0x3f;

    if (slot_cpu) {
        m->rbv_ifr |= RBV_IFR_SLOT;
    } else {
        m->rbv_ifr &= ~RBV_IFR_SLOT;
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&m->glue), MACIISI_GLUE_RBV),
                 (m->rbv_ifr & m->rbv_ier & 0x7f) != 0);
}

static void maciisi_vbl_off(void *opaque)
{
    MacIIsiMachineState *m = opaque;

    /*
     * End of vertical blank: the slot line and the level-triggered IFR
     * summary drop together, so an interrupt masked past the blank is
     * simply missed rather than serviced stale (dsBadSlotInt).
     */
    m->rbv_sifr &= ~RBV_SLOT_E_INT;
    maciisi_rbv_update_irq(m);
}

static void maciisi_sixty_hz(void *opaque)
{
    MacIIsiMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA1_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    /* onboard video vertical blank = slot $E line pulses via the RBV */
    m->rbv_sifr |= RBV_SLOT_E_INT;
    maciisi_rbv_update_irq(m);
    timer_mod(m->vbl_off_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1300000);

    timer_mod(m->sixty_hz_timer,
              (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
               VIA_60HZ_TIMER_PERIOD_NS) /
              VIA_60HZ_TIMER_PERIOD_NS * VIA_60HZ_TIMER_PERIOD_NS);
}

static void maciisi_one_second(void *opaque)
{
    MacIIsiMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA2_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->one_second_timer,
              (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000) / 1000 * 1000);
}

/* Egret microcontroller behaviour (grown empirically against the ROM) */

static void maciisi_egret_set_xcvr(MOS6522MacIIsiState *v1s, bool assert)
{
    MOS6522State *s = MOS6522(v1s);

    if (assert) {
        s->b &= ~EGRET_XCVR;            /* active low */
    } else {
        s->b |= EGRET_XCVR;
    }
}

static void maciisi_egret_schedule_int(MacIIsiMachineState *m)
{
    timer_mod(m->egret_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * 1000);
}

static void maciisi_egret_timer_cb(void *opaque)
{
    MacIIsiMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), SR_INT_BIT);

    qemu_set_irq(irq, 1);
}

static void maciisi_egret_process(MacIIsiMachineState *m)
{
    uint8_t *c = m->egret_cmd;
    int n = m->egret_cmd_len;

    m->egret_resp_len = 0;
    m->egret_resp_idx = 0;

    qemu_log_mask(LOG_UNIMP,
                  "maciisi egret: cmd len=%d [%02x %02x %02x %02x]\n",
                  n, n > 0 ? c[0] : 0, n > 1 ? c[1] : 0, n > 2 ? c[2] : 0,
                  n > 3 ? c[3] : 0);

    if (n == 0) {
        return;
    }

    m->egret_no_resp = false;
    switch (c[0]) {
    case 0x00:                          /* get version */
        /* Egret 341S0851 (IIsi) reports firmware 1.01 */
        m->egret_resp[m->egret_resp_len++] = 0x01;
        m->egret_resp[m->egret_resp_len++] = 0x01;
        break;
    default:
        /*
         * No response: run the same interrupt sequence but with /XCVR
         * high at the turnaround so the ROM discards the bytes (count
         * is zeroed when the response-pending flag is unset).
         */
        m->egret_no_resp = true;
        m->egret_resp[m->egret_resp_len++] = 0x00;
        m->egret_resp[m->egret_resp_len++] = 0x00;
        break;
    }
}

/*
 * /XCVR_SESSION (PB3) as sampled by the ROM at each shift interrupt:
 * LOW at the turnaround interrupt ("a response follows", the first byte
 * is already in SR), HIGH at the interrupts delivering the middle
 * bytes, and LOW again at the final interrupt after the last byte's
 * ack toggle — that pattern is the receive loop's exit condition.
 */
static void maciisi_egret_ack_toggle(MOS6522MacIIsiState *v1s)
{
    MacIIsiMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (m->egret_resp_idx < m->egret_resp_len) {
        s->sr = m->egret_resp[m->egret_resp_idx++];
        /* PB3 low only for the first byte's (turnaround) interrupt */
        maciisi_egret_set_xcvr(v1s, m->egret_resp_idx == 1 &&
                                    !m->egret_no_resp);
        maciisi_egret_schedule_int(m);
        qemu_log_mask(LOG_UNIMP, "maciisi egret: feed 0x%02x (#%d/%d)\n",
                      s->sr, m->egret_resp_idx, m->egret_resp_len);
    } else {
        /* final ack: signal end-of-transfer, the exchange is over */
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
        m->egret_session = false;
        maciisi_egret_set_xcvr(v1s, true);      /* PB3 low: done */
        maciisi_egret_schedule_int(m);
        qemu_log_mask(LOG_UNIMP, "maciisi egret: response complete\n");
    }
}

static void maciisi_egret_session_update(MOS6522MacIIsiState *v1s)
{
    MacIIsiMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);
    bool sys = !(s->b & EGRET_SYS_SESSION);
    uint8_t hs_change = (s->b ^ v1s->last_b) & (EGRET_SYS_SESSION |
                                                EGRET_VIA_FULL);

    if (sys && !m->egret_session) {
        m->egret_session = true;
        m->egret_cmd_len = 0;
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
        /* /XCVR returns to idle before the new exchange */
        maciisi_egret_set_xcvr(v1s, false);
        /* the ROM preloads the first byte before asserting the session */
        if (s->acr & SR_OUT) {
            maciisi_egret_sr_written(v1s);
        }
        return;
    }

    /*
     * TIP/TACK toggles during the receive phase acknowledge the byte
     * in SR and clock the next one; /TIP alternates as part of the ack
     * so it does not signify session end while a response is flowing.
     */
    if (m->egret_session && !(s->acr & SR_OUT) && hs_change
        && m->egret_resp_len > 0) {
        maciisi_egret_ack_toggle(v1s);
        return;
    }

    if (!sys && m->egret_session) {
        m->egret_session = false;
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
        maciisi_egret_set_xcvr(v1s, false);
    }
}

static void maciisi_egret_sr_written(MOS6522MacIIsiState *v1s)
{
    MacIIsiMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (!m->egret_session || !(s->acr & SR_OUT)) {
        return;
    }
    if (m->egret_cmd_len < (int)sizeof(m->egret_cmd)) {
        m->egret_cmd[m->egret_cmd_len++] = s->sr;
    }
    qemu_log_mask(LOG_UNIMP, "maciisi egret: <- 0x%02x (#%d)\n", s->sr,
                  m->egret_cmd_len);
    maciisi_egret_schedule_int(m);
}

static void maciisi_egret_acr_changed(MOS6522MacIIsiState *v1s)
{
    MacIIsiMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    /*
     * The host turns the shifter around to receive while still holding
     * the session: treat the bytes collected so far as the command,
     * build the response and raise /TREQ; the host's following TACK
     * toggle clocks the first byte out.
     */
    if (m->egret_session && !(s->acr & SR_OUT) && m->egret_resp_len == 0
        && m->egret_cmd_len > 0) {
        maciisi_egret_process(m);
        m->egret_cmd_len = 0;
        if (m->egret_resp_len > 0) {
            maciisi_egret_set_xcvr(v1s, true);
        }
    }
}

static void maciisi_egret_sr_read(MOS6522MacIIsiState *v1s)
{
    MacIIsiMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if ((s->acr & SR_OUT) || m->egret_resp_len == 0) {
        return;
    }
    qemu_log_mask(LOG_UNIMP, "maciisi egret: -> 0x%02x (#%d/%d)\n", s->sr,
                  m->egret_resp_idx, m->egret_resp_len);
}

/*
 * The RBV also emulates a VIA2 at the classic VIA2 site (slice +0x2000):
 * registers are VIA-spaced (offset >> 9), low address bits don't care.
 * IFR (reg 13) and IER (reg 14) share the RBV interrupt state.
 */

static uint64_t maciisi_rbv_via2_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    MacIIsiMachineState *m = opaque;
    int reg = (addr >> VIA_SPACING_SHIFT) & 0xf;
    uint64_t val;

    switch (reg) {
    case VIA_REG_IFR:
        val = m->rbv_ifr;
        break;
    case VIA_REG_IER:
        val = m->rbv_ier | 0x80;
        break;
    default:
        val = m->rbv_via2_regs[reg];
        break;
    }

    qemu_log_mask(LOG_UNIMP, "maciisi rbv-via2: read  reg%d -> 0x%02" PRIx64
                  " pc=0x%08x\n", reg, val, maciisi_trace_pc());
    return val;
}

static void maciisi_rbv_via2_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    MacIIsiMachineState *m = opaque;
    int reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    qemu_log_mask(LOG_UNIMP, "maciisi rbv-via2: write reg%d <- 0x%02" PRIx64
                  " pc=0x%08x\n", reg, val, maciisi_trace_pc());

    switch (reg) {
    case VIA_REG_IFR:
        m->rbv_ifr &= ~(val & 0x7f);
        break;
    case VIA_REG_IER:
        if (val & 0x80) {
            m->rbv_ier |= val & 0x7f;
        } else {
            m->rbv_ier &= ~(val & 0x7f);
        }
        break;
    default:
        m->rbv_via2_regs[reg] = val;
        break;
    }
    maciisi_rbv_update_irq(m);
}

static const MemoryRegionOps maciisi_rbv_via2_ops = {
    .read = maciisi_rbv_via2_read,
    .write = maciisi_rbv_via2_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* VDAC (CLUT): store + log */

static uint64_t maciisi_vdac_read(void *opaque, hwaddr addr, unsigned size)
{
    MacIIsiMachineState *m = opaque;
    uint64_t val = m->vdac_regs[addr & 0x3f];

    qemu_log_mask(LOG_UNIMP, "maciisi vdac: read  +0x%02x -> 0x%02" PRIx64 "\n",
                  (unsigned)(addr & 0x3f), val);
    return val;
}

static void maciisi_vdac_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MacIIsiMachineState *m = opaque;

    qemu_log_mask(LOG_UNIMP, "maciisi vdac: write +0x%02x <- 0x%02" PRIx64 "\n",
                  (unsigned)(addr & 0x3f), val);
    m->vdac_regs[addr & 0x3f] = val;
}

static const MemoryRegionOps maciisi_vdac_ops = {
    .read = maciisi_vdac_read,
    .write = maciisi_vdac_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * SCSI (NCR5380) stub: enough behaviour for the boot-device scan on an
 * empty bus — arbitration always wins (AIP set while MODE.arbitrate,
 * LA never), selection sees no BSY so every target times out.
 */

static uint64_t maciisi_scsi_read(void *opaque, hwaddr addr, unsigned size)
{
    MacIIsiMachineState *m = opaque;
    int reg = (addr >> 4) & 7;
    uint64_t val;

    switch (reg) {
    case 1:                             /* ICR: reflect asserts + AIP */
        val = m->scsi_regs[1] & 0x9f;
        if (m->scsi_regs[2] & 1) {
            val |= 0x40;                /* arbitration in progress */
        }
        break;
    case 2:                             /* MODE reads back */
        val = m->scsi_regs[2];
        break;
    default:
        val = 0;                        /* bus free, no target */
        break;
    }
    /* reads are polled hard during arbitration/selection; don't log */
    return val;
}

static void maciisi_scsi_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MacIIsiMachineState *m = opaque;
    int reg = (addr >> 4) & 7;

    m->scsi_regs[reg] = val;
    qemu_log_mask(LOG_UNIMP, "maciisi scsi: write +0x%03x <- 0x%02" PRIx64 "\n",
                  (unsigned)addr, val);
}

static const MemoryRegionOps maciisi_scsi_ops = {
    .read = maciisi_scsi_read,
    .write = maciisi_scsi_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* catch-all for everything else in the I/O slice */

static void maciisi_irq_sink(void *opaque, int n, int level)
{
}

/* unmapped I/O space bus-errors on the real machine */

static MemTxResult maciisi_iotrace_read(void *opaque, hwaddr addr,
                                        uint64_t *data, unsigned size,
                                        MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP,
                  "maciisi io: read  +0x%05x (%d) -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, maciisi_trace_pc());
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult maciisi_iotrace_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size,
                                         MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP,
                  "maciisi io: write +0x%05x (%d) <- 0x%08" PRIx64
                  " -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, val, maciisi_trace_pc());
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps maciisi_iotrace_ops = {
    .read_with_attrs = maciisi_iotrace_read,
    .write_with_attrs = maciisi_iotrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void maciisi_machine_init(MachineState *machine)
{
    MacIIsiMachineState *m = MACIISI_MACHINE(machine);
    ram_addr_t ram_size = machine->ram_size;
    const char *bios_name = machine->firmware ?: MACIISI_ROM_FILENAME;
    char *filename;
    int bios_size;
    uint8_t *ptr;
    CPUState *cs;
    DeviceState *dev;
    SysBusDevice *sysbus;

    if (ram_size > ADDR24_RAM_LIMIT) {
        error_report("Too much memory for this machine: %" PRId64 " MiB, "
                     "maximum 8 MiB (24-bit map aliasing)", ram_size / MiB);
        exit(1);
    }

    /* CPU */
    object_initialize_child(OBJECT(machine), "cpu", &m->cpu, machine->cpu_type);
    qdev_realize(DEVICE(&m->cpu), NULL, &error_fatal);
    qemu_register_reset(main_cpu_reset, &m->cpu);

    /* RAM inside a container whose unbacked reads return 0 (RAM sizing) */
    memory_region_init_io(&m->ramio, OBJECT(machine), &ramio_ops, &m->ramio,
                          "ram", RAM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x0, &m->ramio);
    memory_region_add_subregion(&m->ramio, 0, machine->ram);

    /* IRQ glue */
    object_initialize_child(OBJECT(machine), "glue", &m->glue,
                            TYPE_MACIISI_GLUE);
    object_property_set_link(OBJECT(&m->glue), "cpu", OBJECT(&m->cpu),
                             &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&m->glue), &error_fatal);

    /* I/O container + repeating alias + 24-bit alias */
    memory_region_init(&m->macio, OBJECT(machine), "mac-io", IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE, &m->macio);

    memory_region_init_io(&m->macio_alias, OBJECT(machine), &macio_alias_ops,
                          &m->macio, "mac-io.alias", IO_SIZE - IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE + IO_SLICE,
                                &m->macio_alias);

    /*
     * NOTE: no static 24-bit-map aliases here.  Pre-MMU, physical
     * 0x800000/0xF00000 must read as empty (the ROM RAM-sizing test
     * probes them); the 24-bit map only exists once the ROM programs
     * the PMMU, which is not reached yet.
     */

    /* catch-all trace region behind the devices */
    memory_region_init_io(&m->iotrace, OBJECT(machine), &maciisi_iotrace_ops,
                          m, "mac-io.trace", IO_SLICE);
    memory_region_add_subregion_overlap(&m->macio, 0, &m->iotrace, -1);

    /* VIA1 */
    object_initialize_child(OBJECT(machine), "via1", &m->via1,
                            TYPE_MOS6522_MACIISI);
    qdev_prop_set_uint64(DEVICE(&m->via1), "frequency", VIA_TIMER_FREQ);
    /*
     * Machine-ID straps on port A (read with DDRA all-input during
     * identification): the ROM requires (PA & 0x56) == 0x46 — PA4 low —
     * to select the Mac IIsi configuration; PA4 high selects a sibling
     * config that boots into the factory test monitor.
     */
    qdev_prop_set_uint8(DEVICE(&m->via1), "pins-a", 0xef);
    sysbus = SYS_BUS_DEVICE(&m->via1);
    sysbus_realize(sysbus, &error_fatal);
    {
        struct tm tm;

        qemu_get_timedate(&tm, 0);
        m->via1.tick_offset = (uint32_t)mktimegm(&tm) + RTC_OFFSET;
    }
    m->via1.machine = m;
    m->egret_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maciisi_egret_timer_cb,
                                  m);
    m->vbl_off_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maciisi_vbl_off, m);
    m->sixty_hz_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maciisi_sixty_hz, m);
    timer_mod(m->sixty_hz_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VIA_60HZ_TIMER_PERIOD_NS);
    m->one_second_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, maciisi_one_second,
                                       m);
    timer_mod(m->one_second_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), MACIISI_GLUE_VIA1));
    memory_region_init_io(&m->via1mem, OBJECT(machine), &maciisi_via1_ops,
                          &m->via1, "via1", VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, VIA1_OFS, &m->via1mem);

    /*
     * Machine identification fingerprint (probe entry for decoder kind
     * 5, the IIsi): VIA1 IER mirrors at +0x40000 (the whole I/O slice
     * repeats, so this comes for free) but must NOT respond at +0x20000
     * (bus error there), and RBV/VDAC must be present.
     */

    /* RBV's VIA2-emulation window at the classic VIA2 site */
    memory_region_init_io(&m->rbv_via2mem, OBJECT(machine),
                          &maciisi_rbv_via2_ops, m, "rbv-via2",
                          VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, VIA1_OFS + VIA_REGION_SIZE,
                                &m->rbv_via2mem);

    /* SCC */
    object_initialize_child(OBJECT(machine), "escc", &m->escc, TYPE_ESCC);
    dev = DEVICE(&m->escc);
    qdev_prop_set_uint32(dev, "disabled", 0);
    qdev_prop_set_uint32(dev, "frequency", MAC_CLOCK);
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
                                           MACIISI_GLUE_SCC));
    memory_region_add_subregion(&m->macio, SCC_OFS,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->escc),
                                                       0));

    /* EASC (interrupt wiring to RBV once RBV is real) */
    object_initialize_child(OBJECT(machine), "asc", &m->asc, TYPE_ASC);
    qdev_prop_set_uint8(DEVICE(&m->asc), "asctype", ASC_TYPE_EASC);
    if (machine->audiodev) {
        qdev_prop_set_string(DEVICE(&m->asc), "audiodev", machine->audiodev);
    }
    sysbus = SYS_BUS_DEVICE(&m->asc);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, ASC_OFS,
                                sysbus_mmio_get_region(sysbus, 0));
    /* TODO: route via VIA2 interrupt inputs; direct CPU wiring storms */
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(maciisi_irq_sink, m, 0));

    /* SWIM floppy */
    object_initialize_child(OBJECT(machine), "swim", &m->swim, TYPE_SWIM);
    sysbus = SYS_BUS_DEVICE(&m->swim);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, SWIM_OFS,
                                sysbus_mmio_get_region(sysbus, 0));

    /*
     * The IIsi address decoder ignores A15 for the SCC and SWIM selects,
     * so they also answer at base|0x8000.  The ROM fingerprints these
     * mirrors during machine identification.
     */
    memory_region_init_io(&m->escc_mirror, OBJECT(machine),
                          &maciisi_escc_mirror_ops, NULL, "escc-mirror",
                          0x2000);
    memory_region_add_subregion(&m->macio, SCC_OFS | 0x8000, &m->escc_mirror);

    memory_region_init_alias(&m->swim_mirror, OBJECT(machine), "swim-mirror",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->swim),
                                                    0),
                             0, 0x2000);
    memory_region_add_subregion(&m->macio, SWIM_OFS | 0x8000, &m->swim_mirror);

    /* RBV / VDAC / SCSI stubs */
    memory_region_init_io(&m->rbvmem, OBJECT(machine), &maciisi_rbv_ops, m,
                          "rbv", 0x2000);
    memory_region_add_subregion(&m->macio, RBV_OFS, &m->rbvmem);

    memory_region_init_io(&m->vdacmem, OBJECT(machine), &maciisi_vdac_ops, m,
                          "vdac", 0x40);
    memory_region_add_subregion(&m->macio, VDAC_OFS, &m->vdacmem);

    memory_region_init_io(&m->scsimem, OBJECT(machine), &maciisi_scsi_ops, m,
                          "scsi", 0x200);
    memory_region_add_subregion(&m->macio, SCSI_OFS, &m->scsimem);

    /* ROM */
    memory_region_init_rom(&m->rom, NULL, "maciisi.rom", MACIISI_ROM_SIZE,
                           &error_abort);
    memory_region_add_subregion(get_system_memory(), MACIISI_ROM_ADDR,
                                &m->rom);

    memory_region_init_alias(&m->rom_alias, NULL, "maciisi.rom-alias",
                             &m->rom, 0, MACIISI_ROM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x40000000,
                                &m->rom_alias);

    /*
     * The onboard video is a pseudo NuBus slot: the ROM (whose top holds
     * the video Declaration ROM) also decodes at the top of slot $E
     * super space, where the Slot Manager's byte-lane probe finds it.
     */
    memory_region_init_alias(&m->rom_slot_alias, NULL, "maciisi.rom-slote",
                             &m->rom, 0, MACIISI_ROM_SIZE);
    memory_region_add_subregion(get_system_memory(),
                                0xff000000 - MACIISI_ROM_SIZE,
                                &m->rom_slot_alias);

    /*
     * Onboard video framebuffer.  The machine table in the ROM hardwires
     * the physical base 0xFBB08000; the same memory also appears at the
     * slot $E view 0xFEE00000 that the Declaration ROM's video driver
     * publishes as ScrnBase.
     */
    object_initialize_child(OBJECT(machine), "fb", &m->fb, TYPE_MACIISI_FB);
    sysbus = SYS_BUS_DEVICE(&m->fb);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0xfbb08000,
                                sysbus_mmio_get_region(sysbus, 0));

    memory_region_init_alias(&m->vram_alias, NULL, "maciisi.vram-slote",
                             sysbus_mmio_get_region(sysbus, 0), 0, 0x180000);
    memory_region_add_subregion(get_system_memory(), 0xfee00000,
                                &m->vram_alias);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, MACIISI_ROM_ADDR,
                                        MACIISI_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }

    if (!qtest_enabled()) {
        if (bios_size <= 0 || bios_size > MACIISI_ROM_SIZE) {
            error_report("could not load MacROM '%s'", bios_name);
            exit(1);
        }

        cs = CPU(&m->cpu);
        ptr = rom_ptr(MACIISI_ROM_ADDR, bios_size);
        assert(ptr != NULL);
        stl_phys(cs->as, 0, ldl_be_p(ptr));    /* reset initial SP */
        stl_phys(cs->as, 4,
                 MACIISI_ROM_ADDR + ldl_be_p(ptr + 4)); /* reset initial PC */
    }
}

static void maciisi_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68030"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Macintosh IIsi";
    mc->init = maciisi_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->default_ram_size = 8 * MiB;
    mc->default_ram_id = "maciisi.ram";
    machine_add_audiodev_property(mc);
}

static const TypeInfo maciisi_machine_typeinfo[] = {
    {
        .name       = TYPE_MACIISI_GLUE,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacIIsiGlueState),
        .instance_init = maciisi_glue_init,
        .class_init = maciisi_glue_class_init,
    },
    {
        .name       = TYPE_MOS6522_MACIISI,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522MacIIsiState),
        .instance_init = mos6522_maciisi_init,
        .class_init = mos6522_maciisi_class_init,
    },
    {
        .name       = TYPE_MACIISI_FB,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacIIsiFbState),
        .class_init = maciisi_fb_class_init,
    },
    {
        .name       = TYPE_MACIISI_MACHINE,
        .parent     = TYPE_MACHINE,
        .instance_size = sizeof(MacIIsiMachineState),
        .class_init = maciisi_machine_class_init,
    },
};

DEFINE_TYPES(maciisi_machine_typeinfo)
