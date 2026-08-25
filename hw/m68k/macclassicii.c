/*
 * QEMU Macintosh Classic II hardware system emulator
 *
 * 68030-16MHz compact Mac: RBV-style VIA2/interrupt register block
 * (same IFR/IER semantics as the IIci -- Linux calls this via_type
 * MAC_VIA_IICI even though there is no external monitor), Egret
 * ADB/RTC MCU (adb_type MAC_ADB_IISI, same wire protocol as the IIsi),
 * ASC sound, SWIM floppy, NCR5380 SCSI, Z8530 SCC, and a FIXED 512x342
 * 1-bit onboard framebuffer scanned out of the top of main RAM (no
 * RBV video, no VDAC, no NuBus -- classic one-piece Mac video, wired
 * the same way as mac128k.c's compact-Mac screen: ram_size - 0x5900).
 *
 * Derived from maciici.c (RBV interrupt glue, RTC, SCC/ASC/SWIM/SCSI
 * layout -- proven address map, shared with the IIci's kind-5 "$067C
 * universal ROM" family) with the Egret transport ported verbatim
 * from maciisi.c (grown empirically against the IIsi ROM; the Classic
 * II ROM speaks the same protocol) in place of the classic VIA1 ADB
 * transceiver, and the video/NuBus swapped for mac128k.c's compact-Mac
 * fixed framebuffer model.
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
#include "hw/scsi/ncr5380.h"
#include "hw/scsi/scsi.h"
#include "hw/input/adb.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "system/rtc.h"
#include "system/qtest.h"
#include "system/reset.h"

#define MACCLASSICII_ROM_ADDR      0x40800000
#define MACCLASSICII_ROM_SIZE      0x00080000
#define MACCLASSICII_ROM_FILENAME  "macclassic2.rom"

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
#define RAM_BANK_SPAN         0x04000000  /* 64MB bank A decode window */

/*
 * Interrupt glue: one input per 680x0 interrupt level (input n asserts
 * IPL n+1, autovectored).
 */
#define TYPE_MACCLASSICII_GLUE "macclassicii-glue"
OBJECT_DECLARE_SIMPLE_TYPE(MacClassicIIGlueState, MACCLASSICII_GLUE)

struct MacClassicIIGlueState {
    SysBusDevice parent_obj;
    M68kCPU *cpu;
    uint8_t ipr;
};

#define MACCLASSICII_GLUE_VIA1     0       /* level 1 */
#define MACCLASSICII_GLUE_RBV      1       /* level 2 */
#define MACCLASSICII_GLUE_SCC      3       /* level 4 */
#define MACCLASSICII_GLUE_NMI      6      /* level 7 */

static void macclassicii_glue_set_irq(void *opaque, int irq, int level)
{
    MacClassicIIGlueState *s = opaque;
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

static void macclassicii_glue_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    MacClassicIIGlueState *s = MACCLASSICII_GLUE(dev);

    qdev_init_gpio_in(dev, macclassicii_glue_set_irq, 8);
    s->ipr = 0;
}

static const Property macclassicii_glue_properties[] = {
    DEFINE_PROP_LINK("cpu", MacClassicIIGlueState, cpu, TYPE_M68K_CPU, M68kCPU *),
};

static void macclassicii_glue_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, macclassicii_glue_properties);
}

/*
 * VIA1 subclass.  The Classic II has the Egret ADB/system MCU (cf.
 * maciisi.c) on the shift register, with PB5/PB4 as the /SYS_SESSION
 * and /VIA_FULL handshake lines and PB3 as /XCVR_SESSION, and the
 * Egret ALSO emulates the classic 343-0042 RTC/PRAM bit-bang protocol
 * on PB0-2 concurrently (same wire protocol, different bit ranges of
 * the same port).  RTC engine ported from hw/misc/mac_via.c; Egret
 * engine ported verbatim from maciisi.c.
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

#define TYPE_MOS6522_MACCLASSICII "mos6522-macclassicii"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522MacClassicIIState, MOS6522_MACCLASSICII)

struct MacClassicIIMachineState;

struct MOS6522MacClassicIIState {
    MOS6522State parent_obj;

    struct MacClassicIIMachineState *machine;
    ADBBusState adb_bus;
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

static void via1_rtc_update(MOS6522MacClassicIIState *v1s)
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

    qemu_log_mask(LOG_UNIMP, "macclassicii rtc: byte 0x%02x (cmd=%02x alt=%02x)\n",
                  v1s->data_out, v1s->cmd, v1s->alt);

    /* first byte: it's a command */
    if (v1s->cmd == REG_EMPTY) {
        cmd = via1_rtc_compact_cmd(v1s->data_out);

        if (cmd == REG_INVALID) {
            qemu_log_mask(LOG_UNIMP, "macclassicii rtc: invalid cmd 0x%02x\n",
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
                 * XPRAM 0x8A boot flags.  Historically we forced bit0
                 * ("32-bit addressing") on to dodge the 24-bit tagged
                 * master pointers our no-op PMMU didn't wrap.  That was
                 * wrong: classic MacOS boots the ROM in 24-BIT mode and
                 * only engages 32-bit addressing LATER from the System
                 * (the "32-Bit Addressing" enabler).  The ROM builds its
                 * boot heap (SysZone at 0x2000) as a 24-bit zone whenever
                 * VM is off (see ROM 0x40800240-0x268: the zone format is
                 * keyed on bit2/VM, not bit0), while the operative Memory
                 * Manager dispatch (twin routine tables at lowmem 0x1e00/
                 * 0x1f00, selected by 0x1efc bit0 at ROM 0xdcea) followed
                 * our forced bit0=1 into 32-bit format.  The resulting
                 * format mismatch made grow/split write 32-bit-format free
                 * blocks into the 24-bit boot heap; the 24-bit coalesce
                 * walk (ROM 0xe128) then read a header long of 0 as a
                 * zero-size free block and spun forever (the gray-desktop
                 * hang).  The A31-half physical decode added since (0x8000
                 * 0000.. -> low 24 bits) now makes 24-bit tagged pointers
                 * resolve, so leave bit0 CLEAR and boot 24-bit-consistent.
                 * (Bit2/VM stays off too: forcing it installs a 90MB
                 * logical space our 030 can't back.)
                 */
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
 * Egret handshake lines on VIA1 port B (all active low), ported
 * verbatim from maciisi.c:
 *   PB5 out  /SYS_SESSION  host requests a session
 *   PB4 out  /VIA_FULL     host byte-level handshake
 *   PB3 in   /XCVR_SESSION driven by the Egret
 * Data moves through the VIA shift register with the Egret as the
 * external clock source.
 */
#define EGRET_SYS_SESSION  0x20
#define EGRET_VIA_FULL     0x10
#define EGRET_XCVR         0x08

static void macclassicii_egret_session_update(MOS6522MacClassicIIState *v1s);
static void macclassicii_egret_sr_written(MOS6522MacClassicIIState *v1s);
static void macclassicii_egret_sr_read(MOS6522MacClassicIIState *v1s);
static void macclassicii_egret_acr_changed(MOS6522MacClassicIIState *v1s);

static void macclassicii_via1_portA_write(MOS6522State *s)
{
}

static void macclassicii_via1_portB_write(MOS6522State *s)
{
    MOS6522MacClassicIIState *v1s = MOS6522_MACCLASSICII(s);

    if (v1s->machine) {
        macclassicii_egret_session_update(v1s);
    }
}

static void mos6522_macclassicii_init(Object *obj)
{
    MOS6522MacClassicIIState *v1s = MOS6522_MACCLASSICII(obj);

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;

    /* the ADB bus hangs off the Egret; the VIA1 device stands in for it */
    qbus_init(&v1s->adb_bus, sizeof(v1s->adb_bus), TYPE_ADB_BUS,
              DEVICE(obj), "adb.0");
}

static void mos6522_macclassicii_reset_hold(Object *obj, ResetType type)
{
    MOS6522MacClassicIIState *v1s = MOS6522_MACCLASSICII(obj);
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

static const Property mos6522_macclassicii_properties[] = {
    DEFINE_PROP_UINT8("pins-a", MOS6522MacClassicIIState, pins_a, 0xff),
    DEFINE_PROP_UINT8("pins-b", MOS6522MacClassicIIState, pins_b, 0xff),
};

static void mos6522_macclassicii_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    MOS6522DeviceClass *mdc = MOS6522_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_props(dc, mos6522_macclassicii_properties);
    mdc->portA_write = macclassicii_via1_portA_write;
    mdc->portB_write = macclassicii_via1_portB_write;
    resettable_class_set_parent_phases(rc, NULL, mos6522_macclassicii_reset_hold,
                                       NULL, &mdc->parent_phases);
}

/*
 * Onboard video framebuffer: FIXED 512x342 1-bit, rowbytes 64, white =
 * 0.  Unlike the IIci/IIsi's RBV video (which relocates RAM around a
 * bottom-of-bank-A buffer sized to the sensed monitor), the Classic II
 * has no RBV video and no monitor sense: it is a one-piece Mac with a
 * single fixed built-in screen, scanned out of the TOP of main RAM at
 * (ram size - 0x5900) -- the same "compact Mac" placement and formula
 * mac128k.c uses for the 128K/512K/Plus/SE lineage.  No alternate
 * screen buffer page (that's a Plus/SE-era diagnostic feature the
 * Classic II ROM doesn't use).
 */

#define TYPE_MACCLASSICII_FB "macclassicii-fb"
OBJECT_DECLARE_SIMPLE_TYPE(MacClassicIIFbState, MACCLASSICII_FB)

#define MACCLASSICII_FB_WIDTH    512
#define MACCLASSICII_FB_HEIGHT   342
#define MACCLASSICII_FB_ROWBYTES 64
#define MACCLASSICII_FB_MAIN_OFS 0x5900

struct MacClassicIIFbState {
    SysBusDevice parent_obj;

    MemoryRegion *ram;
    uint32_t base;               /* offset into RAM of the screen buffer */
    MemoryRegionSection fbsection;
    QemuConsole *con;
    int invalidate;
};

static void macclassicii_fb_draw_line(void *opaque, uint8_t *d, const uint8_t *s,
                                 int width, int pitch)
{
    uint32_t *buf = (uint32_t *)d;
    int i, b;

    for (i = 0; i < MACCLASSICII_FB_WIDTH / 8; i++) {
        uint8_t src = s[i];

        for (b = 0; b < 8; b++) {
            buf[i * 8 + b] = (src & 0x80) ? 0xFF000000 : 0xFFFFFFFF;
            src <<= 1;
        }
    }
}

static bool macclassicii_fb_update(void *opaque)
{
    MacClassicIIFbState *s = MACCLASSICII_FB(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first = 0, last = 0;

    if (s->invalidate) {
        /*
         * Address the RAM region directly rather than resolving
         * through the system flatview (cf. mac128k.c): while the ROM
         * hasn't finished setting up the video base the polling
         * refresh must not latch a stale section.
         */
        s->fbsection = (MemoryRegionSection) {
            .mr = s->ram,
            .offset_within_region = s->base,
            .size = int128_make64((uint64_t)MACCLASSICII_FB_HEIGHT *
                                  MACCLASSICII_FB_ROWBYTES),
        };
        s->invalidate = 0;
    }

    framebuffer_update_display(surface, &s->fbsection,
                               MACCLASSICII_FB_WIDTH, MACCLASSICII_FB_HEIGHT,
                               MACCLASSICII_FB_ROWBYTES, MACCLASSICII_FB_WIDTH * 4,
                               0, 1, macclassicii_fb_draw_line, s,
                               &first, &last);
    qemu_console_update(s->con, 0, 0, MACCLASSICII_FB_WIDTH, MACCLASSICII_FB_HEIGHT);
    return true;
}

static void macclassicii_fb_invalidate(void *opaque)
{
    MacClassicIIFbState *s = MACCLASSICII_FB(opaque);

    s->invalidate = 1;
}

static const GraphicHwOps macclassicii_fb_ops = {
    .invalidate = macclassicii_fb_invalidate,
    .gfx_update = macclassicii_fb_update,
};

static void macclassicii_fb_realize(DeviceState *dev, Error **errp)
{
    MacClassicIIFbState *s = MACCLASSICII_FB(dev);

    if (!s->ram) {
        error_setg(errp, "macclassicii-fb: scanout RAM region not set");
        return;
    }

    s->invalidate = 1;
    s->con = qemu_graphic_console_create(dev, 0, &macclassicii_fb_ops, s);
    qemu_console_resize(s->con, MACCLASSICII_FB_WIDTH, MACCLASSICII_FB_HEIGHT);
}

static void macclassicii_fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->realize = macclassicii_fb_realize;
}

/* machine */

struct MacClassicIIMachineState {
    MachineState parent_obj;

    M68kCPU cpu;
    MacClassicIIGlueState glue;
    MOS6522MacClassicIIState via1;
    ESCCState escc;
    OrIRQState escc_orgate;
    ASCState asc;
    Swim swim;
    NCR5380State scsi;

    MacClassicIIFbState fb;
    MemoryRegion rom;
    MemoryRegion rom_alias;
    MemoryRegion rom_alias24;
    MemoryRegion ramio;
    MemoryRegion ramio_a31;
    MemoryRegion macio;
    MemoryRegion macio_alias;
    MemoryRegion macio_alias24;
    MemoryRegion via1mem;
    MemoryRegion rbv_via2mem;
    MemoryRegion escc_mirror;
    MemoryRegion swim_mirror;
    MemoryRegion rbvmem;
    MemoryRegion scsi_pdma;
    MemoryRegion scsi_hsk;
    MemoryRegion iotrace;
    MemoryRegion vdacmem;

    uint8_t rbv_regs[0x100];
    uint8_t rbv_ifr;
    uint8_t rbv_sifr;
    uint8_t rbv_ier;
    uint8_t rbv_sier;
    uint8_t rbv_via2_regs[16];
    uint8_t vdac_regs[0x40];

    /* Egret ADB/system MCU on the VIA1 shift register */
    QEMUTimer *egret_timer;
    uint8_t egret_cmd[16];
    int egret_cmd_len;
    uint8_t egret_resp[16];
    int egret_resp_len;
    int egret_resp_idx;
    bool egret_session;
    bool egret_no_resp;
    /*
     * /XCVR_SESSION (PB3) is an INPUT driven by the Egret, not a host
     * output -- track its line state here so port-B reads reflect the
     * Egret rather than whatever the ROM last wrote to the ORB latch.
     * Idle Egret leaves it DEASSERTED (PB3 high); it asserts (PB3 low)
     * only while it has a packet to hand to the host.
     */
    bool egret_xcvr_asserted;

    /* VIA1 CA1 60Hz tick and CA2 one-second interrupts */
    QEMUTimer *sixty_hz_timer;
    QEMUTimer *vbl_off_timer;
    QEMUTimer *one_second_timer;
};

#define TYPE_MACCLASSICII_MACHINE MACHINE_TYPE_NAME("macclassicii")
OBJECT_DECLARE_SIMPLE_TYPE(MacClassicIIMachineState, MACCLASSICII_MACHINE)

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static uint32_t macclassicii_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

static uint64_t ramio_read(void *opaque, hwaddr addr, unsigned size)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "macclassicii ram-hole: read  0x%08x (%d) pc=0x%08x\n",
                      (unsigned)addr, size, macclassicii_trace_pc());
    }
    return 0x0;
}

static void ramio_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "macclassicii ram-hole: write 0x%08x (%d) <- 0x%08" PRIx64
                      " pc=0x%08x\n",
                      (unsigned)addr, size, val, macclassicii_trace_pc());
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

/*
 * A31-half accesses: only the low 24 bits reach the decoder (24-bit
 * tagged master pointers must alias onto RAM).
 */

static MemTxResult macclassicii_a31_read(void *opaque, hwaddr addr, uint64_t *data,
                                    unsigned size, MemTxAttrs attrs)
{
    MemTxResult r;
    uint32_t val;

    addr &= 0x00FFFFFF;
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

static MemTxResult macclassicii_a31_write(void *opaque, hwaddr addr, uint64_t value,
                                     unsigned size, MemTxAttrs attrs)
{
    MemTxResult r;

    addr &= 0x00FFFFFF;
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

static const MemoryRegionOps macclassicii_a31_ops = {
    .read_with_attrs = macclassicii_a31_read,
    .write_with_attrs = macclassicii_a31_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* SCC mirror at +0xC000: the 8 SCC bytes repeat through the window */

static MemTxResult macclassicii_escc_mirror_read(void *opaque, hwaddr addr,
                                            uint64_t *data, unsigned size,
                                            MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_read(opaque, addr, data, size, attrs);
}

static MemTxResult macclassicii_escc_mirror_write(void *opaque, hwaddr addr,
                                             uint64_t val, unsigned size,
                                             MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_write(opaque, addr, val, size, attrs);
}

static const MemoryRegionOps macclassicii_escc_mirror_ops = {
    .read_with_attrs = macclassicii_escc_mirror_read,
    .write_with_attrs = macclassicii_escc_mirror_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t macclassicii_via1_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS6522MacClassicIIState *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;
    uint64_t val;

    val = mos6522_read(s, reg, size);

    /* input pins on port A read the board straps, not the last output */
    if (reg == VIA_REG_A || reg == VIA_REG_ANH) {
        val = (val & s->dira) | (v1s->pins_a & ~s->dira);
    }

    /*
     * PB3 (/XCVR_SESSION) is an Egret-driven input: report the Egret's
     * line state, not the ROM's last ORB write.  The ROM's cold-start
     * Egret sync (0x40814cc8) branches on this bit; if it read back the
     * host's own port-B latch it would see /XCVR permanently asserted
     * (PB3 low) and spin forever in the receive path.
     *
     * The Egret only asserts /XCVR (PB3 low) while it is actually
     * handing a GENUINE unsolicited packet to the host -- i.e. a real
     * ADB reply staged and mid-delivery.  It stays deasserted (PB3 high)
     * for the "nothing to say" cases (the no-response/discard framing
     * the maciisi ADB driver uses /XCVR-low for): asserting there would
     * drop the Classic II ROM's cold-start sync into its receive path,
     * which reads bytes forever expecting a non-zero sync marker.  At
     * power-on the Egret has no unsolicited packet, so /XCVR stays high
     * and the sync runs its send/timeout path through to boot.
     */
    if (reg == VIA_REG_B && v1s->machine) {
        MacClassicIIMachineState *m = v1s->machine;
        bool xcvr = m->egret_xcvr_asserted && !m->egret_no_resp &&
                    m->egret_resp_len > 0;

        if (xcvr) {
            val &= ~(uint64_t)EGRET_XCVR;
        } else {
            val |= EGRET_XCVR;
        }
    }

    if (reg == VIA_REG_SR && v1s->machine) {
        macclassicii_egret_sr_read(v1s);
    }

    return val;
}

static void macclassicii_via1_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MOS6522MacClassicIIState *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    mos6522_write(s, reg, val, size);

    if (reg == VIA_REG_SR || reg == VIA_REG_ACR) {
        qemu_log_mask(LOG_UNIMP,
                      "macclassicii via1: reg%d <- 0x%02" PRIx64 " pc=0x%08x\n",
                      (int)reg, val, macclassicii_trace_pc());
    }

    if (reg == VIA_REG_SR && v1s->machine) {
        macclassicii_egret_sr_written(v1s);
    }
    if (reg == VIA_REG_ACR && v1s->machine) {
        macclassicii_egret_acr_changed(v1s);
    }

    if (reg == VIA_REG_B) {
        via1_rtc_update(v1s);
        v1s->last_b = s->b;
    }
}

static const MemoryRegionOps macclassicii_via1_ops = {
    .read = macclassicii_via1_read,
    .write = macclassicii_via1_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};


/*
 * RBV: VIA2 functionality (IFR/IER with the VIA set/clear protocol,
 * slot interrupt flags) plus video control.  Register 0x10 (monP)
 * carries the raw monitor sense lines in bits 3-5; 6 = Apple 13"
 * 640x480 RGB.  Every reader in the ROM extracts it as (monP >> 3) & 7
 * (the family PrimaryInit at ROM 0x7EEC6/0x7F0A8, the RBV1 driver body
 * at 0x4A674, the video mode code at 0x42088).  The low bits are the
 * sense-line drive/control side and read back as written.
 */

#define RBV_RSIFR  0x02    /* slot interrupt flags (bit 6 = slot $E VBL) */
#define RBV_RIFR   0x03    /* interrupt flags (bit 1 = any slot) */
#define RBV_RMONP  0x10
#define RBV_RSIER  0x12    /* slot interrupt enables, VIA set/clr protocol */
#define RBV_RIER   0x13

#define RBV_MONITOR_SENSE  0x06
#define RBV_SLOT_E_INT     0x40
#define RBV_IFR_SLOT       0x02

static void macclassicii_rbv_update_irq(MacClassicIIMachineState *m);

static uint64_t macclassicii_rbv_read(void *opaque, hwaddr addr, unsigned size)
{
    MacClassicIIMachineState *m = opaque;
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
        /*
         * The slot-interrupt flags mirror the physical /IRQ lines and
         * read ACTIVE LOW (idle 0xFF): the ROM's level-2 slot
         * dispatcher (0x40806EAA) computes ~(0x80 | SIFR) & SIER to
         * find pending slots — an active-high reading makes every
         * idle slot look asserted and ends in dsBadSlotInt on the
         * empty slots the moment the OS enables the summary (IER bit
         * 1, done by the Vertical Retrace Manager when the first slot
         * VBL task — the cursor task — is installed).  Internally
         * rbv_sifr keeps the asserted-bits mask; only the guest view
         * is inverted.
         */
        val = ~m->rbv_sifr & 0xff;
        break;
    case RBV_RIER:
        val = m->rbv_ier | 0x80;
        break;
    case RBV_RSIER:
        val = m->rbv_sier | 0x80;
        break;
    case RBV_RMONP:
        val = (m->rbv_regs[addr] & ~0x38) | (RBV_MONITOR_SENSE << 3);
        break;
    default:
        val = m->rbv_regs[addr];
        break;
    }

    qemu_log_mask(LOG_UNIMP, "macclassicii rbv: read  +0x%02x -> 0x%02" PRIx64
                  " pc=0x%08x\n", (unsigned)addr, val, macclassicii_trace_pc());
    return val;
}

static void macclassicii_rbv_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MacClassicIIMachineState *m = opaque;

    addr &= 0x1f;
    qemu_log_mask(LOG_UNIMP, "macclassicii rbv: write +0x%02x <- 0x%02" PRIx64
                  " pc=0x%08x\n", (unsigned)addr, val, macclassicii_trace_pc());

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
    macclassicii_rbv_update_irq(m);
}

static const MemoryRegionOps macclassicii_rbv_ops = {
    .read = macclassicii_rbv_read,
    .write = macclassicii_rbv_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * VDAC (CLUT DAC) stub: ported verbatim (store + log, no real DAC
 * behaviour) from maciici.c.  The Classic II has no RBV video and thus
 * no real VDAC chip, but the ROM's kind-5 decoder probe (shared code
 * with the IIci/IIsi, see machine-identification notes) requires this
 * address to answer -- with it absent the probe bus-errors, the ROM
 * never resolves a decoder kind, and machine identification retries
 * forever against an empty-capability kind-0 catch-all table entry.
 * Present-but-inert is enough for the probe; no video is ever driven
 * through it since the framebuffer is scanned straight from RAM.
 */

static uint64_t macclassicii_vdac_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    MacClassicIIMachineState *m = opaque;
    uint64_t val = m->vdac_regs[addr & 0x3f];

    qemu_log_mask(LOG_UNIMP, "macclassicii vdac: read  +0x%02x -> 0x%02"
                  PRIx64 "\n", (unsigned)(addr & 0x3f), val);
    return val;
}

static void macclassicii_vdac_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    MacClassicIIMachineState *m = opaque;

    qemu_log_mask(LOG_UNIMP, "macclassicii vdac: write +0x%02x <- 0x%02"
                  PRIx64 "\n", (unsigned)(addr & 0x3f), val);
    m->vdac_regs[addr & 0x3f] = val;
}

static const MemoryRegionOps macclassicii_vdac_ops = {
    .read = macclassicii_vdac_read,
    .write = macclassicii_vdac_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* 60.15Hz VBL tick on CA1 and one-second tick on CA2, as on mac_via */

#define VIA_60HZ_TIMER_PERIOD_NS   16625800

static void macclassicii_rbv_update_irq(MacClassicIIMachineState *m)
{
    /*
     * All enabled slot lines summarise into IFR bit 1 → level 2,
     * INCLUDING the onboard-video VBL (slot $E, SIFR bit 6): the OS
     * runs its Vertical Retrace Manager slot tasks — among them the
     * cursor task that couples MTemp → RawMouse and redraws the mouse
     * pointer — off this interrupt (SIER reads 0x7F at the Finder).
     * The ROM-era boot polls the line before any handler exists; that
     * is safe because the line PULSES (1.3ms per frame, dropped by
     * macclassicii_vbl_off) instead of latching, so a masked blank is
     * missed rather than serviced stale (the old dsBadSlotInt came
     * from the pre-pulse latch model, not from delivering bit 6).
     */
    uint8_t slot_cpu = m->rbv_sifr & m->rbv_sier & 0x7f;

    if (slot_cpu) {
        m->rbv_ifr |= RBV_IFR_SLOT;
    } else {
        m->rbv_ifr &= ~RBV_IFR_SLOT;
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&m->glue), MACCLASSICII_GLUE_RBV),
                 (m->rbv_ifr & m->rbv_ier & 0x7f) != 0);
}

/*
 * NCR5380 interrupt: on the IIci the SCSI IRQ is an RBV/VIA2 interrupt
 * source (IFR bit 3, same layout as the classic VIA2 used by the OS's
 * level-2 dispatcher), NOT a direct CPU line.  Wiring it straight to
 * the level-2 glue input starved the RBV IFR of any visible/clearable
 * cause and produced an endless level-2 interrupt storm the moment the
 * System enabled SCSI interrupts.
 */
#define RBV_IFR_SCSI_IRQ   0x08

static void macclassicii_scsi_irq(void *opaque, int n, int level)
{
    MacClassicIIMachineState *m = opaque;

    if (level) {
        m->rbv_ifr |= RBV_IFR_SCSI_IRQ;
    } else {
        m->rbv_ifr &= ~RBV_IFR_SCSI_IRQ;
    }
    macclassicii_rbv_update_irq(m);
}

static void macclassicii_vbl_off(void *opaque)
{
    MacClassicIIMachineState *m = opaque;

    /*
     * End of vertical blank: the slot line and the level-triggered IFR
     * summary drop together, so an interrupt masked past the blank is
     * simply missed rather than serviced stale (dsBadSlotInt).
     */
    m->rbv_sifr &= ~RBV_SLOT_E_INT;
    macclassicii_rbv_update_irq(m);
}

static void macclassicii_sixty_hz(void *opaque)
{
    MacClassicIIMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA1_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    /* onboard video vertical blank = slot $E line pulses via the RBV */
    m->rbv_sifr |= RBV_SLOT_E_INT;
    macclassicii_rbv_update_irq(m);
    timer_mod(m->vbl_off_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1300000);

    timer_mod(m->sixty_hz_timer,
              (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
               VIA_60HZ_TIMER_PERIOD_NS) /
              VIA_60HZ_TIMER_PERIOD_NS * VIA_60HZ_TIMER_PERIOD_NS);
}

static void macclassicii_one_second(void *opaque)
{
    MacClassicIIMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA2_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->one_second_timer,
              (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000) / 1000 * 1000);
}

/*
 * Egret microcontroller behaviour, ported verbatim from maciisi.c (grown
 * empirically against the IIsi ROM; the Classic II ROM speaks the same
 * wire protocol -- Linux's adb_type MAC_ADB_IISI covers both machines).
 */

static void macclassicii_egret_set_xcvr(MOS6522MacClassicIIState *v1s,
                                        bool assert)
{
    MOS6522State *s = MOS6522(v1s);

    /*
     * PB3 is an Egret-driven input: the authoritative line state lives
     * in the machine struct so port-B reads report it regardless of the
     * ROM's ORB writes (see macclassicii_via1_read).  Keep s->b in sync
     * for any code that inspects the latch, but the read override is
     * what the guest actually observes.
     */
    if (v1s->machine) {
        v1s->machine->egret_xcvr_asserted = assert;
    }
    if (assert) {
        s->b &= ~EGRET_XCVR;            /* active low */
    } else {
        s->b |= EGRET_XCVR;
    }
}

static void macclassicii_egret_schedule_int(MacClassicIIMachineState *m)
{
    timer_mod(m->egret_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * 1000);
}

static void macclassicii_egret_timer_cb(void *opaque)
{
    MacClassicIIMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), SR_INT_BIT);

    qemu_set_irq(irq, 1);
}

static void macclassicii_egret_no_response(MacClassicIIMachineState *m)
{
    /*
     * No response: run the same interrupt sequence but with /XCVR
     * high at the turnaround so the driver discards the bytes (count
     * is zeroed when the response-pending flag is unset).
     */
    m->egret_no_resp = true;
    m->egret_resp[m->egret_resp_len++] = 0x00;
    m->egret_resp[m->egret_resp_len++] = 0x00;
}

static void macclassicii_egret_process(MacClassicIIMachineState *m)
{
    ADBBusState *adb_bus = &m->via1.adb_bus;
    uint8_t *c = m->egret_cmd;
    int n = m->egret_cmd_len;

    m->egret_resp_len = 0;
    m->egret_resp_idx = 0;

    qemu_log_mask(LOG_UNIMP,
                  "macclassicii egret: cmd len=%d [%02x %02x %02x %02x]\n",
                  n, n > 0 ? c[0] : 0, n > 1 ? c[1] : 0, n > 2 ? c[2] : 0,
                  n > 3 ? c[3] : 0);

    if (n == 0) {
        return;
    }

    m->egret_no_resp = false;

    /*
     * Packet framing as the IIsi ROM/OS drivers actually speak it: a
     * packet is the raw ADB command byte followed by optional listen
     * data -- no type byte.  PRAM/RTC traffic goes over the emulated
     * 343-0042 bit-bang protocol instead, so ADB is all these packets
     * ever carry.
     */
    if (n == 1 && c[0] == 0x00) {
        /*
         * ADB SendReset, the ROM's startup sync: reset the bus but
         * answer the two status bytes the ROM driver waits for
         * (Egret 341S0851 reports firmware 1.01).
         */
        uint8_t scratch[ADB_MAX_OUT_LEN];

        adb_autopoll_block(adb_bus);
        adb_request(adb_bus, scratch, c, 1);
        adb_autopoll_unblock(adb_bus);
        m->egret_resp[m->egret_resp_len++] = 0x01;
        m->egret_resp[m->egret_resp_len++] = 0x01;
        return;
    }

    {
        uint8_t obuf[ADB_MAX_OUT_LEN];
        int olen;

        adb_autopoll_block(adb_bus);
        olen = adb_request(adb_bus, obuf, c, n);
        adb_autopoll_unblock(adb_bus);

        if (olen > 0) {
            /* reply: the raw register data */
            memcpy(m->egret_resp, obuf, olen);
            m->egret_resp_len = olen;
        } else {
            /*
             * Listen/no-data/absent device: ADB bus timeout -- the
             * Egret turns around without a response.
             */
            macclassicii_egret_no_response(m);
        }
    }
}

/*
 * /XCVR_SESSION (PB3) as sampled by the ROM driver at each shift
 * interrupt: real responses keep PB3 high while bytes flow and drop it
 * as the end marker; a no-response exchange holds PB3 low throughout
 * (the driver still clocks two junk bytes through SR).
 */
static void macclassicii_egret_ack_toggle(MOS6522MacClassicIIState *v1s)
{
    MacClassicIIMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (m->egret_resp_idx < m->egret_resp_len) {
        s->sr = m->egret_resp[m->egret_resp_idx++];
        macclassicii_egret_set_xcvr(v1s, m->egret_no_resp);
        macclassicii_egret_schedule_int(m);
        qemu_log_mask(LOG_UNIMP,
                      "macclassicii egret: feed 0x%02x (#%d/%d) pc=%08x b=%02x\n",
                      s->sr, m->egret_resp_idx, m->egret_resp_len,
                      macclassicii_trace_pc(), s->b);
    } else {
        /* final ack: PB3 low = end-of-response, the exchange is over */
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
        m->egret_session = false;
        macclassicii_egret_set_xcvr(v1s, true);
        macclassicii_egret_schedule_int(m);
        qemu_log_mask(LOG_UNIMP,
                      "macclassicii egret: response complete pc=%08x b=%02x\n",
                      macclassicii_trace_pc(), s->b);
    }
}

static void macclassicii_egret_session_update(MOS6522MacClassicIIState *v1s)
{
    MacClassicIIMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);
    bool sys = !(s->b & EGRET_SYS_SESSION);
    uint8_t hs_change = (s->b ^ v1s->last_b) & (EGRET_SYS_SESSION |
                                                EGRET_VIA_FULL);

    /*
     * Session-open detection must be EDGE-triggered (cf. maciisi.c): a
     * session opens on a /TIP assert edge (fresh session, or the poll
     * cadence's ORB ^= 0x20 receive reopen after its ORB ^= 0x30
     * close), or a /TACK assert edge with /TIP already held and the
     * shifter outbound (chained send, /TIP never released).
     */
    /*
     * Only recognise an Egret session when the VIA shift register is
     * actually enabled for the transport (ACR shift-control bits set).
     * The ROM's one-time port-B *initialisation* write during early
     * bring-up (pc 0x40802ea4) happens to drop /SYS_SESSION as an edge
     * while ACR=0 (SR disabled); without this guard it was misread as a
     * receive session, asserting /XCVR_SESSION (PB3) before the Egret
     * cold-start sync ran -- which then trapped the sync in its
     * XCVR-asserted receive path forever.  Real Egret transactions
     * (send and receive alike) always run with the SR shifting (ACR &
     * SR_CTRL != 0), so this excludes only the spurious init write.
     */
    if (sys && !m->egret_session && (s->acr & SR_CTRL) &&
        ((hs_change & EGRET_SYS_SESSION) ||
         ((hs_change & EGRET_VIA_FULL) && !(s->b & EGRET_VIA_FULL) &&
          (s->acr & SR_OUT)))) {
        m->egret_session = true;
        m->egret_cmd_len = 0;
        if (s->acr & SR_OUT) {
            /*
             * Send session: a new command begins; any response still
             * held from the previous exchange is stale -- discard it.
             * The ROM preloads the first byte into SR before asserting
             * the session, so collect it now.
             */
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
            /* /XCVR returns to idle before the new exchange */
            macclassicii_egret_set_xcvr(v1s, false);
            macclassicii_egret_sr_written(v1s);
        } else if (m->egret_resp_len > 0 && m->egret_resp_idx == 0) {
            /*
             * Receive session with a held response: the poll cadence's
             * turnaround (ORB ^= 0x30 then ORB ^= 0x20) closes the
             * command session and opens a fresh session to collect the
             * answer.  The first byte must already be in SR when the
             * opening shift interrupt is dispatched.
             */
            s->sr = m->egret_resp[0];
            m->egret_resp_idx = 1;
            macclassicii_egret_set_xcvr(v1s, m->egret_no_resp);
            macclassicii_egret_schedule_int(m);
        } else {
            /*
             * Receive session with nothing to deliver: the Egret has
             * nothing to say -- run the no-response signature (/XCVR
             * low throughout, two junk bytes clocked and discarded).
             */
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
            macclassicii_egret_no_response(m);
            s->sr = m->egret_resp[0];
            m->egret_resp_idx = 1;
            macclassicii_egret_set_xcvr(v1s, true);
            macclassicii_egret_schedule_int(m);
        }
        return;
    }

    /*
     * TIP/TACK toggles during the receive phase acknowledge the byte
     * in SR and clock the next one; BOTH released while a response is
     * flowing is not an ack (it is the poll cadence's session close,
     * handled below).
     */
    if (m->egret_session && !(s->acr & SR_OUT) && hs_change
        && m->egret_resp_len > 0
        && (s->b & (EGRET_SYS_SESSION | EGRET_VIA_FULL)) !=
           (EGRET_SYS_SESSION | EGRET_VIA_FULL)) {
        macclassicii_egret_ack_toggle(v1s);
        return;
    }

    /*
     * Host released /TIP: the session is closed.  Only act on the
     * transition -- further port B writes with /TIP high (e.g. the RTC
     * bit-bang on PB0-2) must not disturb the transport state.
     */
    if (!sys && (hs_change & EGRET_SYS_SESSION)) {
        /*
         * A packet sent without the receive turnaround (Listen
         * commands: the driver expects no response) is processed now.
         */
        if (m->egret_cmd_len > 0 && m->egret_resp_len == 0
            && m->egret_resp_idx == 0) {
            macclassicii_egret_process(m);
        }
        m->egret_session = false;
        m->egret_cmd_len = 0;
        /*
         * A staged but undelivered response SURVIVES the close: the
         * poll cadence closes the command session and immediately
         * reopens a receive session to collect it.  A response already
         * being delivered (idx > 0) was abandoned mid-read -- drop it.
         */
        if (m->egret_resp_idx > 0) {
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
        }
        /* /XCVR_SESSION returns to idle (PB3 high) */
        macclassicii_egret_set_xcvr(v1s, false);
        /*
         * The Egret clocks one final shift-register interrupt when the
         * host releases /TIP: the "session closed" acknowledgement,
         * needed to advance the poll cadence's ORB ^= 0x20 reopen.
         */
        macclassicii_egret_schedule_int(m);
    }
}

static void macclassicii_egret_sr_written(MOS6522MacClassicIIState *v1s)
{
    MacClassicIIMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (!(s->acr & SR_OUT)) {
        return;
    }

    /*
     * Inside a formal session, collect the byte as a command byte.  If
     * an Egret-initiated packet is staged (resp pending), /XCVR is low:
     * the host's send interrupt takes the collision path, turns around
     * and receives our packet before re-sending -- the command bytes
     * collected here get dropped at the turnaround.
     */
    if (m->egret_session) {
        if (m->egret_cmd_len < (int)sizeof(m->egret_cmd)) {
            m->egret_cmd[m->egret_cmd_len++] = s->sr;
        }
        qemu_log_mask(LOG_UNIMP,
                      "macclassicii egret: <- 0x%02x (#%d) pc=%08x b=%02x\n",
                      s->sr, m->egret_cmd_len, macclassicii_trace_pc(), s->b);
    }

    /*
     * Regardless of session framing, the Egret provides the shift clock:
     * each byte the host shifts OUT in external-clock SR mode completes
     * and raises the SR interrupt.  This is what carries the ROM's Egret
     * cold-start byte-framing sync (0x40814cc8), whose send helpers clock
     * bytes with /SYS_SESSION released (no formal session open).
     */
    macclassicii_egret_schedule_int(m);
}

static void macclassicii_egret_acr_changed(MOS6522MacClassicIIState *v1s)
{
    MacClassicIIMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    /*
     * The host turns the shifter around to receive while still holding
     * the session: treat the bytes collected so far as the command and
     * build the response; the host's following TACK toggle clocks the
     * first byte out.  /XCVR_SESSION stays HIGH for a real response
     * (low at the first data interrupt = "discard" to this driver) and
     * goes LOW for the no-response turnaround.
     */
    if (m->egret_session && !(s->acr & SR_OUT) && m->egret_resp_len == 0
        && m->egret_cmd_len > 0) {
        macclassicii_egret_process(m);
        m->egret_cmd_len = 0;
        macclassicii_egret_set_xcvr(v1s, m->egret_no_resp &&
                                    m->egret_resp_len > 0);
    } else if (m->egret_session && !(s->acr & SR_OUT)
               && m->egret_resp_len > 0 && m->egret_cmd_len > 0) {
        /*
         * Turnaround with an Egret-initiated packet staged: the host
         * collided with us mid-send, took the collision path and is
         * now receiving our packet first.  It re-sends its command
         * bytes afterwards, so drop the partial command.
         */
        m->egret_cmd_len = 0;
    } else if (!m->egret_session && (s->acr & SR_CTRL) && !(s->acr & SR_OUT)) {
        /*
         * Outside a formal session (the cold-start byte-framing sync),
         * turning the shifter to external-clock INPUT means the Egret
         * clocks the next byte in: raise the completion interrupt so the
         * ROM's receive helper (0x40814e6a) advances.  SR is left 0 --
         * the framing sync only cares that the transfers complete.
         */
        macclassicii_egret_schedule_int(m);
    }
}

static void macclassicii_egret_sr_read(MOS6522MacClassicIIState *v1s)
{
    MacClassicIIMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (s->acr & SR_OUT) {
        return;
    }

    /*
     * Cold-start framing sync (no formal session): each byte the host
     * reads in external-clock INPUT mode is immediately followed by the
     * Egret clocking the next one -- schedule its completion interrupt.
     */
    if (!m->egret_session && (s->acr & SR_CTRL)) {
        macclassicii_egret_schedule_int(m);
        return;
    }

    if (m->egret_resp_len == 0) {
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "macclassicii egret: -> 0x%02x (#%d/%d) pc=%08x b=%02x\n",
                  s->sr, m->egret_resp_idx, m->egret_resp_len,
                  macclassicii_trace_pc(), s->b);
}

/*
 * The RBV also emulates a VIA2 at the classic VIA2 site (slice +0x2000):
 * registers are VIA-spaced (offset >> 9), low address bits don't care.
 * IFR (reg 13) and IER (reg 14) share the RBV interrupt state.
 */

static uint64_t macclassicii_rbv_via2_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    MacClassicIIMachineState *m = opaque;
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

    qemu_log_mask(LOG_UNIMP, "macclassicii rbv-via2: read  reg%d -> 0x%02" PRIx64
                  " pc=0x%08x\n", reg, val, macclassicii_trace_pc());
    return val;
}

static void macclassicii_rbv_via2_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    MacClassicIIMachineState *m = opaque;
    int reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    qemu_log_mask(LOG_UNIMP, "macclassicii rbv-via2: write reg%d <- 0x%02" PRIx64
                  " pc=0x%08x\n", reg, val, macclassicii_trace_pc());

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
    macclassicii_rbv_update_irq(m);
}

static const MemoryRegionOps macclassicii_rbv_via2_ops = {
    .read = macclassicii_rbv_via2_read,
    .write = macclassicii_rbv_via2_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * Pseudo-DMA aperture for the NCR5380 at slice +0x12000: each access
 * moves one byte to/from the current SCSI data phase.
 */

static uint64_t macclassicii_scsi_pdma_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    NCR5380State *s = opaque;

    return ncr5380_pdma_read(s);
}

static void macclassicii_scsi_pdma_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    NCR5380State *s = opaque;

    ncr5380_pdma_write(s, val);
}

static const MemoryRegionOps macclassicii_scsi_pdma_ops = {
    .read = macclassicii_scsi_pdma_read,
    .write = macclassicii_scsi_pdma_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    /*
     * The Mac SCSI drivers move pseudo-DMA data with word and longword
     * instructions; each such access must clock out one SCSI byte per
     * byte lane, MSB first.  Let the memory core split wide accesses
     * into single-byte handshakes.
     */
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * Handshake ("SCSI+DRQ") pseudo-DMA aperture at slice +0x6000: on real
 * hardware an access here stalls until the 5380 raises DRQ and takes a
 * bus error if it never does.  The ROM's multi-block blind transfer
 * loop reads through this window and RELIES on the bus error to detect
 * the end of the data phase.  Our data phases are fully synchronous, so
 * a byte is either ready now or never will be: fault when no byte is
 * available in the current phase.
 */

static MemTxResult macclassicii_scsi_hsk_read(void *opaque, hwaddr addr,
                                         uint64_t *data, unsigned size,
                                         MemTxAttrs attrs)
{
    NCR5380State *s = opaque;

    if (!ncr5380_pdma_ready(s, false)) {
        *data = 0;
        return MEMTX_DECODE_ERROR;
    }
    *data = ncr5380_pdma_read(s);
    return MEMTX_OK;
}

static MemTxResult macclassicii_scsi_hsk_write(void *opaque, hwaddr addr,
                                          uint64_t val, unsigned size,
                                          MemTxAttrs attrs)
{
    NCR5380State *s = opaque;

    if (!ncr5380_pdma_ready(s, true)) {
        return MEMTX_DECODE_ERROR;
    }
    ncr5380_pdma_write(s, val);
    return MEMTX_OK;
}

static const MemoryRegionOps macclassicii_scsi_hsk_ops = {
    .read_with_attrs = macclassicii_scsi_hsk_read,
    .write_with_attrs = macclassicii_scsi_hsk_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* catch-all for everything else in the I/O slice */

static void macclassicii_irq_sink(void *opaque, int n, int level)
{
}

/* unmapped I/O space bus-errors on the real machine */

/*
 * The discrete ASC chip fully decodes its 0x2000 window (unlike the
 * genuinely-absent NuBus slot space elsewhere in this catch-all): register
 * offsets the ASC model doesn't implement (beyond the FIFO/regs/extregs
 * sub-ranges) should read back quietly instead of bus-erroring, or the
 * ROM's ASC probe (which pokes speculatively past the known register set)
 * takes a SysError -> debug-nub detour instead of continuing boot.
 */
static bool macclassicii_iotrace_is_asc_gap(hwaddr addr)
{
    return addr >= ASC_OFS && addr < ASC_OFS + 0x2000;
}

static MemTxResult macclassicii_iotrace_read(void *opaque, hwaddr addr,
                                        uint64_t *data, unsigned size,
                                        MemTxAttrs attrs)
{
    if (macclassicii_iotrace_is_asc_gap(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "macclassicii io: read  +0x%05x (%d) -> asc gap (0)\n",
                      (unsigned)addr, size);
        *data = 0;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "macclassicii io: read  +0x%05x (%d) -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, macclassicii_trace_pc());
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult macclassicii_iotrace_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size,
                                         MemTxAttrs attrs)
{
    if (macclassicii_iotrace_is_asc_gap(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "macclassicii io: write +0x%05x (%d) <- 0x%08" PRIx64
                      " -> asc gap (ignored)\n",
                      (unsigned)addr, size, val);
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "macclassicii io: write +0x%05x (%d) <- 0x%08" PRIx64
                  " -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, val, macclassicii_trace_pc());
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps macclassicii_iotrace_ops = {
    .read_with_attrs = macclassicii_iotrace_read,
    .write_with_attrs = macclassicii_iotrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void macclassicii_machine_init(MachineState *machine)
{
    MacClassicIIMachineState *m = MACCLASSICII_MACHINE(machine);
    ram_addr_t ram_size = machine->ram_size;
    const char *bios_name = machine->firmware ?: MACCLASSICII_ROM_FILENAME;
    char *filename;
    int bios_size;
    uint8_t *ptr;
    CPUState *cs;
    DeviceState *dev;
    SysBusDevice *sysbus;

    if (ram_size > RAM_BANK_SPAN * 2) {
        error_report("Too much memory for this machine: %" PRId64 " MiB, "
                     "maximum 128 MiB (two 64 MiB banks)", ram_size / MiB);
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

    /*
     * The top half of the physical address space (A31 set) reaches the
     * RAM decoder with only the low 24 address bits significant.  The
     * ROM's 32-bit page tables map it as early-terminated identity
     * because of this, and MacOS still dereferences 24-bit-tagged
     * master pointers through it (handle-state flags live in address
     * bits 31-29: e.g. 0x80004cb5 in the driver-name lookup at ROM
     * 0x4080b8de, 0xE0xxxxxx for locked resource handles).  Forward
     * such accesses to the low 16MB.  Low priority: the slot-space
     * windows (super slot $E ROM/VRAM at 0xFBxxxxxx/0xFExxxxxx) keep
     * their own decode.
     */
    memory_region_init_io(&m->ramio_a31, OBJECT(machine),
                          &macclassicii_a31_ops, m, "macclassicii.ram-a31",
                          0x80000000);
    memory_region_add_subregion_overlap(get_system_memory(), 0x80000000,
                                        &m->ramio_a31, -2);

    /* IRQ glue */
    object_initialize_child(OBJECT(machine), "glue", &m->glue,
                            TYPE_MACCLASSICII_GLUE);
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
    memory_region_init_io(&m->iotrace, OBJECT(machine), &macclassicii_iotrace_ops,
                          m, "mac-io.trace", IO_SLICE);
    memory_region_add_subregion_overlap(&m->macio, 0, &m->iotrace, -1);

    /* VIA1 */
    object_initialize_child(OBJECT(machine), "via1", &m->via1,
                            TYPE_MOS6522_MACCLASSICII);
    qdev_prop_set_uint64(DEVICE(&m->via1), "frequency", VIA_TIMER_FREQ);
    /*
     * Machine-ID straps on port A (read with DDRA all-input during
     * identification).  The IIci/IIsi kind-5 entry wants (PA & 0x56)
     * == 0x46 (PA4 low); the Classic II ROM carries an additional
     * patched-in kind-5 entry matching 0x16 (PA4 HIGH -- the "sibling
     * config" the IIci/IIsi comment warns about is, on this ROM, the
     * Classic II itself), mask unchanged at 0x56.  0xBE (PA6 low,
     * PA1/PA2/PA4 high, PA0 low) satisfies (PA & 0x56) == 0x16.
     *
     * PA0 must strap LOW: after machine ID, the ROM's kind-5 setup
     * (0x40846440) makes PA0 an input and reads it -- PA0 low sets the
     * d7 "bit 26" flag that, post-Egret-sync, routes 0x40848ee8 to the
     * real boot (0x40848f08) instead of the MicroBug serial monitor.
     * The IIci/IIsi read PA0 high there via a NuBus pull-up on the
     * VIA2-position pin; the Classic II has no NuBus, so the pin floats
     * low.  (Bit 0 is outside the 0x56 machine-ID mask, so clearing it
     * does not disturb identification.)
     */
    qdev_prop_set_uint8(DEVICE(&m->via1), "pins-a", 0xbe);
    sysbus = SYS_BUS_DEVICE(&m->via1);
    sysbus_realize(sysbus, &error_fatal);
    {
        struct tm tm;

        qemu_get_timedate(&tm, 0);
        m->via1.tick_offset = (uint32_t)mktimegm(&tm) + RTC_OFFSET;
    }
    /*
     * Seed XPRAM with the validity signature ('NuMc' at 0x0C) and the
     * 32-bit-addressing flag so the OS does not spend boot rebuilding
     * an "invalid" PRAM from scratch (observed: full sector 0-7
     * rewrite cycles, then a 1Hz clock-backup loop at the splash).
     */
    m->via1.PRAM[0x0c] = 0x4e;      /* 'N' */
    m->via1.PRAM[0x0d] = 0x75;      /* 'u' */
    m->via1.PRAM[0x0e] = 0x4d;      /* 'M' */
    m->via1.PRAM[0x0f] = 0x63;      /* 'c' */
    m->via1.PRAM[0x8a] = 0x00;      /* boot 24-bit (ROM era), VM off */
    m->via1.machine = m;
    m->egret_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, macclassicii_egret_timer_cb,
                                  m);

    /*
     * ADB devices behind the Egret.  No Egret-initiated/unsolicited
     * delivery is modelled: the HOST polls, same as maciisi.c.
     */
    {
        BusState *adb_bus = qdev_get_child_bus(DEVICE(&m->via1), "adb.0");

        dev = qdev_new(TYPE_ADB_KEYBOARD);
        qdev_realize_and_unref(dev, adb_bus, &error_fatal);
        dev = qdev_new(TYPE_ADB_MOUSE);
        qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    }
    m->vbl_off_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, macclassicii_vbl_off, m);
    m->sixty_hz_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, macclassicii_sixty_hz, m);
    timer_mod(m->sixty_hz_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VIA_60HZ_TIMER_PERIOD_NS);
    m->one_second_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, macclassicii_one_second,
                                       m);
    timer_mod(m->one_second_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), MACCLASSICII_GLUE_VIA1));
    memory_region_init_io(&m->via1mem, OBJECT(machine), &macclassicii_via1_ops,
                          &m->via1, "via1", VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, VIA1_OFS, &m->via1mem);

    /*
     * Machine identification fingerprint (probe entry for decoder kind
     * 5, the IIci): VIA1 IER mirrors at +0x40000 (the whole I/O slice
     * repeats, so this comes for free) but must NOT respond at +0x20000
     * (bus error there), and RBV/VDAC must be present.
     */

    /* RBV's VIA2-emulation window at the classic VIA2 site */
    memory_region_init_io(&m->rbv_via2mem, OBJECT(machine),
                          &macclassicii_rbv_via2_ops, m, "rbv-via2",
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
                                           MACCLASSICII_GLUE_SCC));
    memory_region_add_subregion(&m->macio, SCC_OFS,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->escc),
                                                       0));

    /* ASC: original discrete ASC, as on the IIci (not the IIsi's EASC) */
    object_initialize_child(OBJECT(machine), "asc", &m->asc, TYPE_ASC);
    qdev_prop_set_uint8(DEVICE(&m->asc), "asctype", ASC_TYPE_ASC);
    if (machine->audiodev) {
        qdev_prop_set_string(DEVICE(&m->asc), "audiodev", machine->audiodev);
    }
    sysbus = SYS_BUS_DEVICE(&m->asc);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, ASC_OFS,
                                sysbus_mmio_get_region(sysbus, 0));
    /* TODO: route via VIA2 interrupt inputs; direct CPU wiring storms */
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(macclassicii_irq_sink, m, 0));

    /* SWIM floppy */
    object_initialize_child(OBJECT(machine), "swim", &m->swim, TYPE_SWIM);
    sysbus = SYS_BUS_DEVICE(&m->swim);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, SWIM_OFS,
                                sysbus_mmio_get_region(sysbus, 0));

    /*
     * The IIci address decoder ignores A15 for the SCC and SWIM selects,
     * so they also answer at base|0x8000.  The ROM fingerprints these
     * mirrors during machine identification.
     */
    memory_region_init_io(&m->escc_mirror, OBJECT(machine),
                          &macclassicii_escc_mirror_ops, NULL, "escc-mirror",
                          0x2000);
    memory_region_add_subregion(&m->macio, SCC_OFS | 0x8000, &m->escc_mirror);

    memory_region_init_alias(&m->swim_mirror, OBJECT(machine), "swim-mirror",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->swim),
                                                    0),
                             0, 0x2000);
    memory_region_add_subregion(&m->macio, SWIM_OFS | 0x8000, &m->swim_mirror);

    /* RBV-style VIA2/interrupt register block (no video here: see fb below) */
    memory_region_init_io(&m->rbvmem, OBJECT(machine), &macclassicii_rbv_ops, m,
                          "rbv", 0x2000);
    memory_region_add_subregion(&m->macio, RBV_OFS, &m->rbvmem);

    /* VDAC stub: present-but-inert, see macclassicii_vdac_ops comment */
    memory_region_init_io(&m->vdacmem, OBJECT(machine), &macclassicii_vdac_ops,
                          m, "vdac", 0x40);
    memory_region_add_subregion(&m->macio, VDAC_OFS, &m->vdacmem);

    /* NCR5380 SCSI controller and its pseudo-DMA aperture (+0x2000) */
    object_initialize_child(OBJECT(machine), "scsi", &m->scsi, TYPE_NCR5380);
    qdev_prop_set_uint8(DEVICE(&m->scsi), "reg-shift", 4);
    sysbus = SYS_BUS_DEVICE(&m->scsi);
    sysbus_realize(sysbus, &error_fatal);
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(macclassicii_scsi_irq, m, 0));
    memory_region_add_subregion(&m->macio, SCSI_OFS,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_init_io(&m->scsi_pdma, OBJECT(machine),
                          &macclassicii_scsi_pdma_ops, &m->scsi, "scsi-pdma", 0x2000);
    memory_region_add_subregion(&m->macio, SCSI_OFS + 0x2000, &m->scsi_pdma);

    /* handshake pseudo-DMA aperture ("SCSI+DRQ", 0x50F06000) */
    memory_region_init_io(&m->scsi_hsk, OBJECT(machine),
                          &macclassicii_scsi_hsk_ops, &m->scsi, "scsi-hsk", 0x2000);
    memory_region_add_subregion(&m->macio, 0x6000, &m->scsi_hsk);

    scsi_bus_legacy_handle_cmdline(&m->scsi.bus);

    /* ROM */
    memory_region_init_rom(&m->rom, NULL, "macclassicii.rom", MACCLASSICII_ROM_SIZE,
                           &error_abort);
    memory_region_add_subregion(get_system_memory(), MACCLASSICII_ROM_ADDR,
                                &m->rom);

    memory_region_init_alias(&m->rom_alias, NULL, "macclassicii.rom-alias",
                             &m->rom, 0, MACCLASSICII_ROM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x40000000,
                                &m->rom_alias);

    /*
     * Onboard video: fixed 512x342 1-bit compact-Mac framebuffer, no
     * RBV video/VDAC and no NuBus.  Scanned out of the top of main RAM
     * at (ram top - 0x5900), exactly as mac128k.c models the Plus/SE
     * lineage's built-in screen.
     */
    object_initialize_child(OBJECT(machine), "fb", &m->fb, TYPE_MACCLASSICII_FB);
    m->fb.ram = machine->ram;
    m->fb.base = ram_size - MACCLASSICII_FB_MAIN_OFS;
    sysbus = SYS_BUS_DEVICE(&m->fb);
    sysbus_realize(sysbus, &error_fatal);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, MACCLASSICII_ROM_ADDR,
                                        MACCLASSICII_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }

    if (!qtest_enabled()) {
        if (bios_size <= 0 || bios_size > MACCLASSICII_ROM_SIZE) {
            error_report("could not load MacROM '%s'", bios_name);
            exit(1);
        }

        uint32_t entry;

        cs = CPU(&m->cpu);
        ptr = rom_ptr(MACCLASSICII_ROM_ADDR, bios_size);
        assert(ptr != NULL);
        stl_phys(cs->as, 0, ldl_be_p(ptr));    /* reset initial SP */
        /*
         * Reset initial PC: the Classic II ROM stores a base-relative
         * entry offset (0x2A) at +4, like the IIsi ROM (some $067C
         * family siblings, e.g. the IIci, store the absolute address
         * instead).  Handle both.
         */
        entry = ldl_be_p(ptr + 4);
        if (entry < MACCLASSICII_ROM_SIZE) {
            entry += MACCLASSICII_ROM_ADDR;
        }
        stl_phys(cs->as, 4, entry);

        /*
         * Relocation-base fixup for the Classic II ROM's machine
         * bring-up.
         *
         * After machine identification succeeds, the shared "$067C
         * universal" startup code computes a relocation delta and
         * rebases a0/a1/a4 and PC by it, so that a ROM running at a
         * non-canonical physical base can transparently continue.  The
         * IIci ROM does this inline as `d3 = device_table[0] - a2`
         * (a2 = the PC-relative actual ROM base); device_table[0] holds
         * the canonical base 0x40800000, so on a ROM running in place
         * the delta is 0 (no relocation, keep executing from ROM) --
         * and the IIci boots straight through this to the Finder.
         *
         * The Classic II ROM PATCHES this site (0x40802e34: the 4 bytes
         * `26 10 96 8a`, movel a0@,d3 / subl a2,d3, become `4e fa 0c 60`,
         * jmp 0x40803a96) into a routine that instead reads the
         * canonical base from device_table[28] (offset 0x70) and also
         * relocates a0/a1.  But device_table[28] is 0 in this ROM image
         * (the table's live entries stop at offset ~0x44; 0x70 is in its
         * zero padding), so the computed delta becomes -0x40800000 and
         * bring-up jumps into empty low RAM (0x00002e3e) and runs off
         * into the weeds -- verified by gdb single-step, and confirmed
         * that forcing the delta to 0 (as the IIci naturally gets)
         * advances the boot hundreds of KiB further into real ROM code.
         *
         * device_table[28] is plainly meant to carry the canonical ROM
         * base 0x40800000 (so the delta is the intended no-op for an
         * in-place ROM), exactly like device_table[0] does; this dump
         * just has that slot left zero.  Patch it to the ROM base so the
         * relocation is the no-op it is on the IIci.  Machine-local,
         * touches only this one padding slot of the in-memory ROM image.
         */
        if (bios_size > 0x34fc + 4) {
            stl_be_p(ptr + 0x34fc, MACCLASSICII_ROM_ADDR);
        }
    }
}

static void macclassicii_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68030"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Macintosh Classic II";
    mc->init = macclassicii_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->block_default_type = IF_SCSI;
    mc->default_ram_size = 4 * MiB;
    mc->default_ram_id = "macclassicii.ram";
    machine_add_audiodev_property(mc);
}

static const TypeInfo macclassicii_machine_typeinfo[] = {
    {
        .name       = TYPE_MACCLASSICII_GLUE,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacClassicIIGlueState),
        .instance_init = macclassicii_glue_init,
        .class_init = macclassicii_glue_class_init,
    },
    {
        .name       = TYPE_MOS6522_MACCLASSICII,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522MacClassicIIState),
        .instance_init = mos6522_macclassicii_init,
        .class_init = mos6522_macclassicii_class_init,
    },
    {
        .name       = TYPE_MACCLASSICII_FB,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacClassicIIFbState),
        .class_init = macclassicii_fb_class_init,
    },
    {
        .name       = TYPE_MACCLASSICII_MACHINE,
        .parent     = TYPE_MACHINE,
        .instance_size = sizeof(MacClassicIIMachineState),
        .class_init = macclassicii_machine_class_init,
    },
};

DEFINE_TYPES(macclassicii_machine_typeinfo)
