/*
 * QEMU Macintosh IIvx / IIvi / Performa 600 hardware system emulator
 *
 * 68030 machine built around Apple's VASP integrated controller: VASP
 * folds the RBV-family VIA2/video/sound/GLU functions of the IIci/IIsi
 * chipset into one ASIC, but unlike RBV it drives a DEDICATED VRAM
 * frame buffer instead of stealing the bottom of main RAM (see the
 * "Macintosh IIvx" Developer Note, 1992/1995, chapter 1: "VASP
 * Integrated Controller" and Table 1-2, "Macintosh IIvx memory map
 * summary").  ADB/RTC/PRAM/power are handled by the same 68HC05 Egret
 * microcontroller first used on the Mac IIsi ("ADB Microcontroller",
 * same note) — so the VIA1/Egret transport in this file is ported
 * directly from maciisi.c.  The confirmed physical map (32-bit mode)
 * is: RAM 0x00000000-0x043FFFFF, ROM 0x40000000-0x403FFFFF (1 MB),
 * I/O 0x50F00000-0x50FFFFFF, VRAM 0x60B00000-0x60BFFFFF (1 MB max),
 * NuBus 0x80000000-0xEFFFFFFF — i.e. the same $50Fxxxxx I/O decode
 * and NuBus siting as the RBV-family maciici.c/maciisi.c, so the
 * VIA1/SCC/SCSI/SWIM/ASC/RBV-VIA2 register layout is carried over
 * unverified beyond the I/O base itself (Apple did not publish VASP's
 * internal register map; grown empirically against the ROM, like its
 * siblings).  The IIvi and Performa 600 share this ROM image and, per
 * the note, differ only in clock speed (32/16/32 MHz), cache (32 KB/
 * none/none) and FPU (standard/optional/optional) — none of which
 * this model distinguishes at the hardware-strap level (undetermined
 * without disassembly); all three machine types boot the one ROM
 * identically here.  Gestalt IDs (Developer Note, "Identifying the
 * Macintosh IIvx"): IIvx 48, IIvi 44, Performa 600 45.
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
#include "hw/nubus/mac-nubus-bridge.h"
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
#include "standard-headers/asm-m68k/bootinfo-mac.h"

/*
 * Confirmed from the Developer Note Table 1-2 ("Macintosh IIvx memory
 * map summary"): ROM 32-bit window $4000 0000-$403F FFFF, i.e. based
 * at 0x40000000 (NOT +0x800000 like the IIci/IIsi window) and exactly
 * 1 MB — matching the "4957EB49 - MacIIvx & IIvi.ROM" image (1 MB,
 * checksum-verified) this machine loads.
 */
#define MACIIVX_ROM_ADDR      0x40000000
#define MACIIVX_ROM_HI_ADDR   0x40800000   /* canonical base per ROM's own table */
#define MACIIVX_ROM_SIZE      0x00100000
#define MACIIVX_ROM_FILENAME  "maciivx.rom"

/*
 * Dedicated VRAM (Table 1-2): 32-bit window $60B0 0000-$60BF FFFF,
 * 24-bit window $B0 0000-$BF FFFF; both exactly 1 MB, the maximum
 * addressable VRAM per "VRAM" (chapter 1).  We back the whole 1 MB
 * window regardless of the 512 KB-vs-1 MB SIMM configuration a real
 * board could have.
 */
#define VRAM_ADDR              0x60B00000
#define VRAM_ADDR24            0x00B00000
#define VRAM_SIZE              0x00100000

#define IO_BASE               0x50000000
#define IO_SLICE              0x00040000
#define IO_SLICE_MASK         (IO_SLICE - 1)
#define IO_SIZE               0x04000000

/*
 * Offsets within the repeating I/O slice (0x50F0xxxx aliases to
 * these).  VASP's internal register map was never published by Apple
 * (the Developer Note explicitly withholds it -- "never use absolute
 * addresses"); these offsets are carried over unverified from the
 * RBV-family maciici.c/maciisi.c, which share the same $50F00000 I/O
 * base per Table 1-2, and adjusted empirically against boot traces
 * where they demonstrably didn't fit.
 */
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
 * Real max (Table 1-2): RAM 32-bit window is $0000 0000-$043F FFFF,
 * 68 MiB -- 4 MiB soldered plus up to 64 MiB across the four SIMM
 * sockets (Features: "Four SIMM sockets for expansion up to 68 MB").
 */
#define MACIIVX_RAM_MAX        0x04400000

/*
 * Interrupt glue: one input per 680x0 interrupt level (input n asserts
 * IPL n+1, autovectored).
 */
#define TYPE_MACIIVX_GLUE "maciivx-glue"
OBJECT_DECLARE_SIMPLE_TYPE(MacIIvxGlueState, MACIIVX_GLUE)

struct MacIIvxGlueState {
    SysBusDevice parent_obj;
    M68kCPU *cpu;
    uint8_t ipr;
};

#define MACIIVX_GLUE_VIA1     0       /* level 1 */
#define MACIIVX_GLUE_RBV      1       /* level 2 */
#define MACIIVX_GLUE_SCC      3       /* level 4 */
#define MACIIVX_GLUE_NMI      6      /* level 7 */

static void maciivx_glue_set_irq(void *opaque, int irq, int level)
{
    MacIIvxGlueState *s = opaque;
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

static void maciivx_glue_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    MacIIvxGlueState *s = MACIIVX_GLUE(dev);

    qdev_init_gpio_in(dev, maciivx_glue_set_irq, 8);
    s->ipr = 0;
}

static const Property maciivx_glue_properties[] = {
    DEFINE_PROP_LINK("cpu", MacIIvxGlueState, cpu, TYPE_M68K_CPU, M68kCPU *),
};

static void maciivx_glue_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, maciivx_glue_properties);
}

/*
 * VIA1 subclass.  The IIvx ROM speaks the classic 343-0042 RTC/PRAM
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

#define TYPE_MOS6522_MACIIVX "mos6522-maciivx"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522MacIIvxState, MOS6522_MACIIVX)

struct MacIIvxMachineState;

struct MOS6522MacIIvxState {
    MOS6522State parent_obj;

    struct MacIIvxMachineState *machine;
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

static void via1_rtc_update(MOS6522MacIIvxState *v1s)
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

    qemu_log_mask(LOG_UNIMP, "maciivx rtc: byte 0x%02x (cmd=%02x alt=%02x)\n",
                  v1s->data_out, v1s->cmd, v1s->alt);

    /* first byte: it's a command */
    if (v1s->cmd == REG_EMPTY) {
        cmd = via1_rtc_compact_cmd(v1s->data_out);

        if (cmd == REG_INVALID) {
            qemu_log_mask(LOG_UNIMP, "maciivx rtc: invalid cmd 0x%02x\n",
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

static void maciivx_egret_session_update(MOS6522MacIIvxState *v1s);
static void maciivx_egret_sr_written(MOS6522MacIIvxState *v1s);
static void maciivx_egret_sr_read(MOS6522MacIIvxState *v1s);
static void maciivx_egret_acr_changed(MOS6522MacIIvxState *v1s);

static void maciivx_via1_portA_write(MOS6522State *s)
{
}

static void maciivx_via1_portB_write(MOS6522State *s)
{
    MOS6522MacIIvxState *v1s = MOS6522_MACIIVX(s);

    if (v1s->machine) {
        maciivx_egret_session_update(v1s);
    }
}

static void mos6522_maciivx_init(Object *obj)
{
    MOS6522MacIIvxState *v1s = MOS6522_MACIIVX(obj);

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;

    /* the ADB bus hangs off the Egret; the VIA1 device stands in for it */
    qbus_init(&v1s->adb_bus, sizeof(v1s->adb_bus), TYPE_ADB_BUS,
              DEVICE(obj), "adb.0");
}

static void mos6522_maciivx_reset_hold(Object *obj, ResetType type)
{
    MOS6522MacIIvxState *v1s = MOS6522_MACIIVX(obj);
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

static const Property mos6522_maciivx_properties[] = {
    DEFINE_PROP_UINT8("pins-a", MOS6522MacIIvxState, pins_a, 0xff),
    DEFINE_PROP_UINT8("pins-b", MOS6522MacIIvxState, pins_b, 0xff),
};

static void mos6522_maciivx_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    MOS6522DeviceClass *mdc = MOS6522_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_props(dc, mos6522_maciivx_properties);
    mdc->portA_write = maciivx_via1_portA_write;
    mdc->portB_write = maciivx_via1_portB_write;
    resettable_class_set_parent_phases(rc, NULL, mos6522_maciivx_reset_hold,
                                       NULL, &mdc->parent_phases);
}

/*
 * Onboard video framebuffer: 640x480 1-bit, rowbytes 80.
 *
 * Unlike the RBV-family IIci/IIsi, the IIvx/IIvi/Performa 600's VASP
 * chip drives a DEDICATED VRAM frame buffer (two 68-pin VRAM SIMMs,
 * 512 KB standard / 1 MB max) rather than stealing the bottom of main
 * RAM -- confirmed by the Developer Note's block diagram ("SIMM VRAM
 * 512 KB or 1MB" as a component separate from "SIMM DRAM") and memory
 * map (Table 1-2: VRAM is its own $60B0 0000-$60BF FFFF / $B0 0000-
 * $BF FFFF window, disjoint from the RAM window).  s->ram below points
 * at that dedicated VRAM MemoryRegion, not system RAM.  The renderer
 * itself is still a placeholder: it always reads the buffer as 1-bit
 * black/white, though the ROM's actual default is 8-bit color (see
 * "Default Video Mode" in the Developer Note) driven through the VDAC
 * CLUT (vdac_regs, currently store+log only, same as maciici/maciisi).
 * Getting real color depth would need the VDAC's address/data register
 * semantics and the RBV-analogous depth/mode register, neither of
 * which Apple published for VASP; left for a follow-up pass guided by
 * boot traces.
 */

#define TYPE_MACIIVX_FB "maciivx-fb"
OBJECT_DECLARE_SIMPLE_TYPE(MacIIvxFbState, MACIIVX_FB)

#define MACIIVX_FB_WIDTH   640
#define MACIIVX_FB_HEIGHT  480
#define MACIIVX_FB_ROWBYTES 80

struct MacIIvxFbState {
    SysBusDevice parent_obj;

    MemoryRegion *ram;          /* scanout source: dedicated VRAM, offset 0 */
    MemoryRegionSection fbsection;
    QemuConsole *con;
    int invalidate;
};

static void maciivx_fb_draw_line(void *opaque, uint8_t *d, const uint8_t *s,
                                 int width, int pitch)
{
    uint32_t *buf = (uint32_t *)d;
    int i, b;

    for (i = 0; i < MACIIVX_FB_WIDTH / 8; i++) {
        uint8_t src = s[i];

        for (b = 0; b < 8; b++) {
            buf[i * 8 + b] = (src & 0x80) ? 0xFF000000 : 0xFFFFFFFF;
            src <<= 1;
        }
    }
}

static bool maciivx_fb_update(void *opaque)
{
    MacIIvxFbState *s = MACIIVX_FB(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first = 0, last = 0;

    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection, s->ram, 0,
                                          MACIIVX_FB_HEIGHT,
                                          MACIIVX_FB_ROWBYTES);
        s->invalidate = 0;
    }

    framebuffer_update_display(surface, &s->fbsection,
                               MACIIVX_FB_WIDTH, MACIIVX_FB_HEIGHT,
                               MACIIVX_FB_ROWBYTES, MACIIVX_FB_WIDTH * 4,
                               0, 1, maciivx_fb_draw_line, s,
                               &first, &last);
    qemu_console_update(s->con, 0, 0, MACIIVX_FB_WIDTH, MACIIVX_FB_HEIGHT);
    return true;
}

static void maciivx_fb_invalidate(void *opaque)
{
    MacIIvxFbState *s = MACIIVX_FB(opaque);

    s->invalidate = 1;
}

static const GraphicHwOps maciivx_fb_ops = {
    .invalidate = maciivx_fb_invalidate,
    .gfx_update = maciivx_fb_update,
};

static void maciivx_fb_realize(DeviceState *dev, Error **errp)
{
    MacIIvxFbState *s = MACIIVX_FB(dev);

    if (!s->ram) {
        error_setg(errp, "maciivx-fb: scanout RAM region not set");
        return;
    }

    s->invalidate = 1;
    s->con = qemu_graphic_console_create(dev, 0, &maciivx_fb_ops, s);
    qemu_console_resize(s->con, MACIIVX_FB_WIDTH, MACIIVX_FB_HEIGHT);
}

static void maciivx_fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->realize = maciivx_fb_realize;
}

/* machine */

struct MacIIvxMachineState {
    MachineState parent_obj;

    M68kCPU cpu;
    MacIIvxGlueState glue;
    MOS6522MacIIvxState via1;
    ESCCState escc;
    OrIRQState escc_orgate;
    ASCState asc;
    Swim swim;
    NCR5380State scsi;
    MacNubusBridge mac_nubus_bridge;

    MacIIvxFbState fb;
    MemoryRegion rom;
    MemoryRegion rom_alias24;
    MemoryRegion rom_alias_hi;
    uint32_t reset_sp;
    uint32_t reset_pc;
    MemoryRegion vram;
    MemoryRegion vram_alias24;
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
    MemoryRegion vdacmem;
    MemoryRegion asc_status_fixup;
    MemoryRegion scsi_pdma;
    MemoryRegion scsi_hsk;
    MemoryRegion iotrace;

    uint8_t rbv_regs[0x100];
    uint8_t rbv_ifr;
    uint8_t rbv_sifr;
    uint8_t rbv_ier;
    uint8_t rbv_sier;
    uint8_t rbv_via2_regs[16];
    uint8_t vdac_regs[0x40];

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

/*
 * maciivx / maciivi / performa600 share one ROM image and, as far as
 * this model goes, one hardware configuration: the Developer Note's
 * only documented differences (clock speed, cache, FPU) aren't things
 * this emulation distinguishes at the strap level (see file header).
 * The class only carries the Gestalt "machine ID" apart from the
 * description, mirroring the maciici/maciisi (PA & 0x56) == 0x46
 * strap idea in case a real per-model strap difference is discovered
 * later -- for now all three siblings use the same pins_a value.
 */
struct MacIIvxMachineClass {
    MachineClass parent_class;

    uint8_t pins_a;
    int mac_model;              /* Linux BI_MAC_MODEL Gestalt ID; unused
                                  * by classic Mac OS boot, kept for
                                  * documentation / future use. */
};

typedef struct MacIIvxMachineClass MacIIvxMachineClass;

#define TYPE_MACIIVX_MACHINE MACHINE_TYPE_NAME("maciivx-common")
OBJECT_DECLARE_TYPE(MacIIvxMachineState, MacIIvxMachineClass, MACIIVX_MACHINE)

static void main_cpu_reset(void *opaque)
{
    MacIIvxMachineState *m = opaque;
    M68kCPU *cpu = &m->cpu;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    /*
     * Reset vector.  We must NOT read it back with ldl_phys here: the
     * ROM image is copied into the ROM region by the rom-loader's own
     * reset handler, which is registered later than this one and so runs
     * AFTER it -- ldl_phys(0/4) would see un-loaded (zero) memory.  So
     * SP (=ROM[0]) and PC (the HIGH-window entry, base + ROM[4]; see the
     * relocation-slot patch in machine_init) are captured from the loaded
     * image and applied directly.  We start in the high ROM window so the
     * patched self-relocation is a no-op and the ROM runs from clean high
     * ROM with low memory a plain writable DRAM.
     */
    cpu->env.aregs[7] = m->reset_sp;
    cpu->env.pc = m->reset_pc;
}

static uint32_t maciivx_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

static uint64_t ramio_read(void *opaque, hwaddr addr, unsigned size)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "maciivx ram-hole: read  0x%08x (%d) pc=0x%08x\n",
                      (unsigned)addr, size, maciivx_trace_pc());
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
                      "maciivx ram-hole: write 0x%08x (%d) <- 0x%08" PRIx64
                      " pc=0x%08x\n",
                      (unsigned)addr, size, val, maciivx_trace_pc());
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

static MemTxResult maciivx_a31_read(void *opaque, hwaddr addr, uint64_t *data,
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

static MemTxResult maciivx_a31_write(void *opaque, hwaddr addr, uint64_t value,
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

static const MemoryRegionOps maciivx_a31_ops = {
    .read_with_attrs = maciivx_a31_read,
    .write_with_attrs = maciivx_a31_write,
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

static MemTxResult maciivx_escc_mirror_read(void *opaque, hwaddr addr,
                                            uint64_t *data, unsigned size,
                                            MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_read(opaque, addr, data, size, attrs);
}

static MemTxResult maciivx_escc_mirror_write(void *opaque, hwaddr addr,
                                             uint64_t val, unsigned size,
                                             MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_write(opaque, addr, val, size, attrs);
}

static const MemoryRegionOps maciivx_escc_mirror_ops = {
    .read_with_attrs = maciivx_escc_mirror_read,
    .write_with_attrs = maciivx_escc_mirror_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t maciivx_via1_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS6522MacIIvxState *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;
    uint64_t val;

    val = mos6522_read(s, reg, size);

    /* input pins on port A read the board straps, not the last output */
    if (reg == VIA_REG_A || reg == VIA_REG_ANH) {
        val = (val & s->dira) | (v1s->pins_a & ~s->dira);
    }

    if (reg == VIA_REG_SR && v1s->machine) {
        maciivx_egret_sr_read(v1s);
    }

    return val;
}

static void maciivx_via1_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MOS6522MacIIvxState *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    mos6522_write(s, reg, val, size);

    if (reg == VIA_REG_SR || reg == VIA_REG_ACR) {
        qemu_log_mask(LOG_UNIMP,
                      "maciivx via1: reg%d <- 0x%02" PRIx64 " pc=0x%08x\n",
                      (int)reg, val, maciivx_trace_pc());
    }

    if (reg == VIA_REG_SR && v1s->machine) {
        maciivx_egret_sr_written(v1s);
    }
    if (reg == VIA_REG_ACR && v1s->machine) {
        maciivx_egret_acr_changed(v1s);
    }

    if (reg == VIA_REG_B) {
        via1_rtc_update(v1s);
        v1s->last_b = s->b;
    }
}

static const MemoryRegionOps maciivx_via1_ops = {
    .read = maciivx_via1_read,
    .write = maciivx_via1_write,
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

static void maciivx_rbv_update_irq(MacIIvxMachineState *m);

static uint64_t maciivx_rbv_read(void *opaque, hwaddr addr, unsigned size)
{
    MacIIvxMachineState *m = opaque;
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

    qemu_log_mask(LOG_UNIMP, "maciivx rbv: read  +0x%02x -> 0x%02" PRIx64
                  " pc=0x%08x\n", (unsigned)addr, val, maciivx_trace_pc());
    return val;
}

static void maciivx_rbv_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MacIIvxMachineState *m = opaque;

    addr &= 0x1f;
    qemu_log_mask(LOG_UNIMP, "maciivx rbv: write +0x%02x <- 0x%02" PRIx64
                  " pc=0x%08x\n", (unsigned)addr, val, maciivx_trace_pc());

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
    maciivx_rbv_update_irq(m);
}

static const MemoryRegionOps maciivx_rbv_ops = {
    .read = maciivx_rbv_read,
    .write = maciivx_rbv_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* 60.15Hz VBL tick on CA1 and one-second tick on CA2, as on mac_via */

#define VIA_60HZ_TIMER_PERIOD_NS   16625800

static void maciivx_rbv_update_irq(MacIIvxMachineState *m)
{
    /*
     * All enabled slot lines summarise into IFR bit 1 → level 2,
     * INCLUDING the onboard-video VBL (slot $E, SIFR bit 6): the OS
     * runs its Vertical Retrace Manager slot tasks — among them the
     * cursor task that couples MTemp → RawMouse and redraws the mouse
     * pointer — off this interrupt (SIER reads 0x7F at the Finder).
     * The ROM-era boot polls the line before any handler exists; that
     * is safe because the line PULSES (1.3ms per frame, dropped by
     * maciivx_vbl_off) instead of latching, so a masked blank is
     * missed rather than serviced stale (the old dsBadSlotInt came
     * from the pre-pulse latch model, not from delivering bit 6).
     */
    uint8_t slot_cpu = m->rbv_sifr & m->rbv_sier & 0x7f;

    if (slot_cpu) {
        m->rbv_ifr |= RBV_IFR_SLOT;
    } else {
        m->rbv_ifr &= ~RBV_IFR_SLOT;
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&m->glue), MACIIVX_GLUE_RBV),
                 (m->rbv_ifr & m->rbv_ier & 0x7f) != 0);
}

/*
 * NCR5380 interrupt: on the IIvx the SCSI IRQ is an RBV/VIA2 interrupt
 * source (IFR bit 3, same layout as the classic VIA2 used by the OS's
 * level-2 dispatcher), NOT a direct CPU line.  Wiring it straight to
 * the level-2 glue input starved the RBV IFR of any visible/clearable
 * cause and produced an endless level-2 interrupt storm the moment the
 * System enabled SCSI interrupts.
 */
#define RBV_IFR_SCSI_IRQ   0x08

static void maciivx_scsi_irq(void *opaque, int n, int level)
{
    MacIIvxMachineState *m = opaque;

    if (level) {
        m->rbv_ifr |= RBV_IFR_SCSI_IRQ;
    } else {
        m->rbv_ifr &= ~RBV_IFR_SCSI_IRQ;
    }
    maciivx_rbv_update_irq(m);
}

static void maciivx_vbl_off(void *opaque)
{
    MacIIvxMachineState *m = opaque;

    /*
     * End of vertical blank: the slot line and the level-triggered IFR
     * summary drop together, so an interrupt masked past the blank is
     * simply missed rather than serviced stale (dsBadSlotInt).
     */
    m->rbv_sifr &= ~RBV_SLOT_E_INT;
    maciivx_rbv_update_irq(m);
}

static void maciivx_sixty_hz(void *opaque)
{
    MacIIvxMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA1_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    /* onboard video vertical blank = slot $E line pulses via the RBV */
    m->rbv_sifr |= RBV_SLOT_E_INT;
    maciivx_rbv_update_irq(m);
    timer_mod(m->vbl_off_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1300000);

    timer_mod(m->sixty_hz_timer,
              (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
               VIA_60HZ_TIMER_PERIOD_NS) /
              VIA_60HZ_TIMER_PERIOD_NS * VIA_60HZ_TIMER_PERIOD_NS);
}

static void maciivx_one_second(void *opaque)
{
    MacIIvxMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA2_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->one_second_timer,
              (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000) / 1000 * 1000);
}

/* Egret microcontroller behaviour (grown empirically against the ROM) */

static void maciivx_egret_set_xcvr(MOS6522MacIIvxState *v1s, bool assert)
{
    MOS6522State *s = MOS6522(v1s);

    if (assert) {
        s->b &= ~EGRET_XCVR;            /* active low */
    } else {
        s->b |= EGRET_XCVR;
    }
}

static void maciivx_egret_schedule_int(MacIIvxMachineState *m)
{
    timer_mod(m->egret_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * 1000);
}

static void maciivx_egret_timer_cb(void *opaque)
{
    MacIIvxMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), SR_INT_BIT);

    qemu_set_irq(irq, 1);
}

static void maciivx_egret_no_response(MacIIvxMachineState *m)
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

static void maciivx_egret_process(MacIIvxMachineState *m)
{
    ADBBusState *adb_bus = &m->via1.adb_bus;
    uint8_t *c = m->egret_cmd;
    int n = m->egret_cmd_len;

    m->egret_resp_len = 0;
    m->egret_resp_idx = 0;

    qemu_log_mask(LOG_UNIMP,
                  "maciivx egret: cmd len=%d [%02x %02x %02x %02x]\n",
                  n, n > 0 ? c[0] : 0, n > 1 ? c[1] : 0, n > 2 ? c[2] : 0,
                  n > 3 ? c[3] : 0);

    if (n == 0) {
        return;
    }

    m->egret_no_resp = false;

    /*
     * Packet framing as the IIvx ROM/OS drivers actually speak it
     * (observed on the wire, session 7): a packet is the raw ADB
     * command byte followed by optional listen data — no type byte.
     * PRAM/RTC traffic goes over the emulated 343-0042 bit-bang
     * protocol instead, so ADB is all these packets ever carry.
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
             * Listen/no-data/absent device: ADB bus timeout — the
             * Egret turns around without a response.
             */
            maciivx_egret_no_response(m);
        }
    }
}

/*
 * /XCVR_SESSION (PB3) as sampled by the ROM driver at each shift
 * interrupt (dispatch 0x4080a700: btst #3, then the continuation
 * branches on it):
 *  - receive loop: a byte is consumed while PB3 is HIGH; PB3 LOW at an
 *    interrupt ends the response (cont 0x4080a63c).
 *  - PB3 LOW already at the first post-turnaround interrupt sets flag
 *    bit5 (cont 0x4080a624) and the whole byte count is DISCARDED at
 *    the end (seq/and at 0x4080a646) — that is the driver's "the Egret
 *    had no response / wants the bus" case.
 * So: real responses keep PB3 high while bytes flow and drop it as the
 * end marker; a no-response exchange holds PB3 low throughout (the
 * driver still clocks two junk bytes through SR).
 */
static void maciivx_egret_ack_toggle(MOS6522MacIIvxState *v1s)
{
    MacIIvxMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (m->egret_resp_idx < m->egret_resp_len) {
        s->sr = m->egret_resp[m->egret_resp_idx++];
        maciivx_egret_set_xcvr(v1s, m->egret_no_resp);
        maciivx_egret_schedule_int(m);
        qemu_log_mask(LOG_UNIMP, "maciivx egret: feed 0x%02x (#%d/%d) pc=%08x b=%02x\n",
                      s->sr, m->egret_resp_idx, m->egret_resp_len,
                      maciivx_trace_pc(), s->b);
    } else {
        /* final ack: PB3 low = end-of-response, the exchange is over */
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
        m->egret_session = false;
        maciivx_egret_set_xcvr(v1s, true);
        maciivx_egret_schedule_int(m);
        qemu_log_mask(LOG_UNIMP, "maciivx egret: response complete pc=%08x b=%02x\n",
                      maciivx_trace_pc(), s->b);
    }
}

static void maciivx_egret_session_update(MOS6522MacIIvxState *v1s)
{
    MacIIvxMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);
    bool sys = !(s->b & EGRET_SYS_SESSION);
    uint8_t hs_change = (s->b ^ v1s->last_b) & (EGRET_SYS_SESSION |
                                                EGRET_VIA_FULL);

    /*
     * Session-open detection must be EDGE-triggered: after an exchange
     * completes, /TIP is often left asserted (the OS-era driver chains
     * poll exchanges without ever parking the bus), and unrelated port
     * B writes — the RTC bit-bang on PB0-2 runs from the Time Manager
     * between exchanges — must not re-open a session (observed: such a
     * write reset /XCVR to idle underneath an armed end-of-response
     * continuation; the pending "session closed" interrupt then read
     * /XCVR high = "more data" and clocked junk until the exchange
     * died).  A session opens on:
     *  - a /TIP assert edge (fresh session, or the poll cadence's
     *    ORB ^= 0x20 receive reopen after its ORB ^= 0x30 close), or
     *  - a /TACK assert edge with /TIP already held and the shifter
     *    outbound (chained send via 0x4080a656: ORB &= 0xCF straight
     *    after the previous exchange, /TIP never released).
     */
    if (sys && !m->egret_session &&
        ((hs_change & EGRET_SYS_SESSION) ||
         ((hs_change & EGRET_VIA_FULL) && !(s->b & EGRET_VIA_FULL) &&
          (s->acr & SR_OUT)))) {
        m->egret_session = true;
        m->egret_cmd_len = 0;
        if (s->acr & SR_OUT) {
            /*
             * Send session: a new command begins; any response still
             * held from the previous exchange is stale — discard it.
             * The ROM preloads the first byte into SR before asserting
             * the session, so collect it now.
             */
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
            /* /XCVR returns to idle before the new exchange */
            maciivx_egret_set_xcvr(v1s, false);
            maciivx_egret_sr_written(v1s);
        } else if (m->egret_resp_len > 0 && m->egret_resp_idx == 0) {
            /*
             * Receive session with a held response: the poll cadence
             * (ROM 0x4080a5f6/0x4080a5fc: turnaround ORB ^= 0x30 which
             * transiently releases /TIP, then ORB ^= 0x20 re-asserting
             * it with the shifter inbound) closes the command session
             * and opens a fresh session to collect the answer.  The
             * first byte must already be in SR when the opening shift
             * interrupt is dispatched (cont 0x4080a624 reads SR at the
             * NEXT interrupt via 0x4080a68e).
             */
            s->sr = m->egret_resp[0];
            m->egret_resp_idx = 1;
            maciivx_egret_set_xcvr(v1s, m->egret_no_resp);
            maciivx_egret_schedule_int(m);
        } else {
            /*
             * Receive session with nothing to deliver: the Egret has
             * nothing to say — run the no-response signature (/XCVR
             * low throughout, two junk bytes clocked and discarded).
             */
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
            maciivx_egret_no_response(m);
            s->sr = m->egret_resp[0];
            m->egret_resp_idx = 1;
            maciivx_egret_set_xcvr(v1s, true);
            maciivx_egret_schedule_int(m);
        }
        return;
    }

    /*
     * TIP/TACK toggles during the receive phase acknowledge the byte
     * in SR and clock the next one; /TIP alternates as part of the ack
     * cadence (ORB ^= 0x30), so mid-receive states always have exactly
     * one of TIP/TACK asserted.  BOTH released while a response is
     * flowing is not an ack (it is the poll cadence's session close,
     * handled below).
     */
    if (m->egret_session && !(s->acr & SR_OUT) && hs_change
        && m->egret_resp_len > 0
        && (s->b & (EGRET_SYS_SESSION | EGRET_VIA_FULL)) !=
           (EGRET_SYS_SESSION | EGRET_VIA_FULL)) {
        maciivx_egret_ack_toggle(v1s);
        return;
    }

    /*
     * Host released /TIP: the session is closed.  Only act on the
     * transition — further port B writes with /TIP high (e.g. the RTC
     * bit-bang on PB0-2) must not disturb the transport state.
     */
    if (!sys && (hs_change & EGRET_SYS_SESSION)) {
        /*
         * A packet sent without the receive turnaround (Listen
         * commands: the driver expects no response) is processed now —
         * it was never seen by maciivx_egret_process, and ADB Listens
         * must reach the devices (address relocation!).
         */
        if (m->egret_cmd_len > 0 && m->egret_resp_len == 0
            && m->egret_resp_idx == 0) {
            maciivx_egret_process(m);
        }
        m->egret_session = false;
        m->egret_cmd_len = 0;
        /*
         * A staged but undelivered response SURVIVES the close: the
         * poll cadence closes the command session (ORB ^= 0x30) and
         * immediately reopens a receive session (ORB ^= 0x20) to
         * collect it.  A response already being delivered (idx > 0)
         * was abandoned mid-read — drop it.
         */
        if (m->egret_resp_idx > 0) {
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
        }
        /* /XCVR_SESSION returns to idle (PB3 high) */
        maciivx_egret_set_xcvr(v1s, false);
        /*
         * The Egret clocks one final shift-register interrupt when the
         * host releases /TIP: the "session closed" acknowledgement.
         * The OS Egret driver parks its state machine (state byte 0x01)
         * on this interrupt after every exchange, and the poll cadence
         * advances on it (cont 0x4080a5fc does the ORB ^= 0x20 reopen);
         * without it the next queued ADB request is never started.
         * The ROM startup driver simply eats the extra interrupt.
         */
        maciivx_egret_schedule_int(m);
    }
}

static void maciivx_egret_sr_written(MOS6522MacIIvxState *v1s)
{
    MacIIvxMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (!m->egret_session || !(s->acr & SR_OUT)) {
        return;
    }
    /*
     * If an Egret-initiated packet is staged (resp pending), /XCVR is
     * low: the host's send interrupt takes the collision path
     * (0x4080a614), turns around and receives our packet before
     * re-sending — the command bytes collected here get dropped at the
     * turnaround.
     */
    if (m->egret_cmd_len < (int)sizeof(m->egret_cmd)) {
        m->egret_cmd[m->egret_cmd_len++] = s->sr;
    }
    qemu_log_mask(LOG_UNIMP, "maciivx egret: <- 0x%02x (#%d) pc=%08x b=%02x\n", s->sr,
                  m->egret_cmd_len, maciivx_trace_pc(), s->b);
    maciivx_egret_schedule_int(m);
}

static void maciivx_egret_acr_changed(MOS6522MacIIvxState *v1s)
{
    MacIIvxMachineState *m = v1s->machine;
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
        maciivx_egret_process(m);
        m->egret_cmd_len = 0;
        maciivx_egret_set_xcvr(v1s, m->egret_no_resp &&
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
    }
}

static void maciivx_egret_sr_read(MOS6522MacIIvxState *v1s)
{
    MacIIvxMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if ((s->acr & SR_OUT) || m->egret_resp_len == 0) {
        return;
    }
    qemu_log_mask(LOG_UNIMP, "maciivx egret: -> 0x%02x (#%d/%d) pc=%08x b=%02x\n", s->sr,
                  m->egret_resp_idx, m->egret_resp_len, maciivx_trace_pc(), s->b);
}

/*
 * The RBV also emulates a VIA2 at the classic VIA2 site (slice +0x2000):
 * registers are VIA-spaced (offset >> 9), low address bits don't care.
 * IFR (reg 13) and IER (reg 14) share the RBV interrupt state.
 */

static uint64_t maciivx_rbv_via2_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    MacIIvxMachineState *m = opaque;
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

    qemu_log_mask(LOG_UNIMP, "maciivx rbv-via2: read  reg%d -> 0x%02" PRIx64
                  " pc=0x%08x\n", reg, val, maciivx_trace_pc());
    return val;
}

static void maciivx_rbv_via2_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    MacIIvxMachineState *m = opaque;
    int reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    qemu_log_mask(LOG_UNIMP, "maciivx rbv-via2: write reg%d <- 0x%02" PRIx64
                  " pc=0x%08x\n", reg, val, maciivx_trace_pc());

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
    maciivx_rbv_update_irq(m);
}

static const MemoryRegionOps maciivx_rbv_via2_ops = {
    .read = maciivx_rbv_via2_read,
    .write = maciivx_rbv_via2_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* VDAC (CLUT): store + log */

static uint64_t maciivx_vdac_read(void *opaque, hwaddr addr, unsigned size)
{
    MacIIvxMachineState *m = opaque;
    uint64_t val = m->vdac_regs[addr & 0x3f];

    qemu_log_mask(LOG_UNIMP, "maciivx vdac: read  +0x%02x -> 0x%02" PRIx64 "\n",
                  (unsigned)(addr & 0x3f), val);
    return val;
}

static void maciivx_vdac_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MacIIvxMachineState *m = opaque;

    qemu_log_mask(LOG_UNIMP, "maciivx vdac: write +0x%02x <- 0x%02" PRIx64 "\n",
                  (unsigned)(addr & 0x3f), val);
    m->vdac_regs[addr & 0x3f] = val;
}

static const MemoryRegionOps maciivx_vdac_ops = {
    .read = maciivx_vdac_read,
    .write = maciivx_vdac_write,
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

static uint64_t maciivx_scsi_pdma_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    NCR5380State *s = opaque;

    return ncr5380_pdma_read(s);
}

static void maciivx_scsi_pdma_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    NCR5380State *s = opaque;

    ncr5380_pdma_write(s, val);
}

static const MemoryRegionOps maciivx_scsi_pdma_ops = {
    .read = maciivx_scsi_pdma_read,
    .write = maciivx_scsi_pdma_write,
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

static MemTxResult maciivx_scsi_hsk_read(void *opaque, hwaddr addr,
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

static MemTxResult maciivx_scsi_hsk_write(void *opaque, hwaddr addr,
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

static const MemoryRegionOps maciivx_scsi_hsk_ops = {
    .read_with_attrs = maciivx_scsi_hsk_read,
    .write_with_attrs = maciivx_scsi_hsk_write,
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

static void maciivx_irq_sink(void *opaque, int n, int level)
{
}

/* unmapped I/O space bus-errors on the real machine */

static MemTxResult maciivx_iotrace_read(void *opaque, hwaddr addr,
                                        uint64_t *data, unsigned size,
                                        MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP,
                  "maciivx io: read  +0x%05x (%d) -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, maciivx_trace_pc());
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult maciivx_iotrace_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size,
                                         MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP,
                  "maciivx io: write +0x%05x (%d) <- 0x%08" PRIx64
                  " -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, val, maciivx_trace_pc());
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps maciivx_iotrace_ops = {
    .read_with_attrs = maciivx_iotrace_read,
    .write_with_attrs = maciivx_iotrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * A NuBus card in slot S raises its interrupt through the RBV slot-interrupt
 * register: RSIFR bit (S - 8).  Bits for slots 9-D summarise into IFR bit 1
 * (level 2); slot E's bit is the pollable onboard-video VBL, handled above.
 */
static void maciivx_nubus_irq(void *opaque, int n, int level)
{
    MacIIvxMachineState *m = opaque;
    uint8_t bit = 1 << ((n & 0xf) - 8);

    if (level) {
        m->rbv_sifr |= bit;
    } else {
        m->rbv_sifr &= ~bit;
    }
    maciivx_rbv_update_irq(m);
}

/*
 * ASC FIFO-IRQ-status fixup (register 0x804 of the ASC).
 *
 * The IIvx ROM's sound-hardware init (ROM 0x85e00-0x85f02, reached after
 * VASP/RBV identification and RTC/PRAM setup) writes ONE sample byte to
 * each ASC FIFO and then tight-polls the FIFO-IRQ-status register for the
 * FULL/EMPTY bit before writing the next -- a playback-paced,
 * drain-timed loop, much like the ROM's other TimeDBRA/SETUPTIMEK
 * calibrations (cf. lc475.c / mac_via.c) that don't survive TCG.  The
 * shared ASC drains its FIFO from the audio backend on a WALL-CLOCK
 * cadence; under -icount a CPU spinning in this tight poll advances
 * virtual time far faster than the backend consumes, so the one queued
 * byte is never drained and FULL/EMPTY never re-asserts -> the ROM hangs
 * here forever.  (The working RBV siblings dodge this: their ROMs blast
 * the whole boot chime through the FIFO in bulk and read the status just
 * a couple of times, so the drain keeps up.)  Report the FIFO as
 * perpetually empty for THIS register read so the paced loop always
 * advances; the shared ASC is otherwise untouched (this is a maciivx
 * overlay, not a change to hw/audio/asc.c).
 */
#define ASC_FIFOIRQ_OFS         0x804
/*
 * The paced loop polls this register for DIFFERENT bits in successive
 * phases: first the FULL/EMPTY bit (ch A btst #1) to pace the drain,
 * then the HALF_FULL bit (ch A btst #0) on the following fill phase.  A
 * single constant that satisfies one starves the other, so report every
 * FIFO status bit (both channels, both HALF_FULL and FULL/EMPTY) as
 * asserted -- every phase of the calibration then advances immediately.
 * The timing value the ROM derives from this is meaningless under TCG
 * anyway (as with the TimeDBRA/SETUPTIMEK hacks on the other Macs).
 */
#define ASC_FIFO_STATUS_ALL     0x0f

static uint64_t maciivx_asc_status_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    return ASC_FIFO_STATUS_ALL;
}

static void maciivx_asc_status_write(void *opaque, hwaddr addr, uint64_t val,
                                     unsigned size)
{
    /* The real register is read-to-clear; the ROM never writes it. */
}

static const MemoryRegionOps maciivx_asc_status_ops = {
    .read = maciivx_asc_status_read,
    .write = maciivx_asc_status_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

static void maciivx_machine_init(MachineState *machine)
{
    MacIIvxMachineState *m = MACIIVX_MACHINE(machine);
    ram_addr_t ram_size = machine->ram_size;
    const char *bios_name = machine->firmware ?: MACIIVX_ROM_FILENAME;
    char *filename;
    int bios_size;
    uint8_t *ptr;
    DeviceState *dev;
    SysBusDevice *sysbus;

    if (ram_size > MACIIVX_RAM_MAX) {
        error_report("Too much memory for this machine: %" PRId64 " MiB, "
                     "maximum 68 MiB (4 MiB soldered + 4 SIMM sockets)",
                     ram_size / MiB);
        exit(1);
    }

    /* CPU */
    object_initialize_child(OBJECT(machine), "cpu", &m->cpu, machine->cpu_type);
    qdev_realize(DEVICE(&m->cpu), NULL, &error_fatal);
    qemu_register_reset(main_cpu_reset, m);

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
                          &maciivx_a31_ops, m, "maciivx.ram-a31",
                          0x80000000);
    memory_region_add_subregion_overlap(get_system_memory(), 0x80000000,
                                        &m->ramio_a31, -2);

    /* IRQ glue */
    object_initialize_child(OBJECT(machine), "glue", &m->glue,
                            TYPE_MACIIVX_GLUE);
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
    memory_region_init_io(&m->iotrace, OBJECT(machine), &maciivx_iotrace_ops,
                          m, "mac-io.trace", IO_SLICE);
    memory_region_add_subregion_overlap(&m->macio, 0, &m->iotrace, -1);

    /* VIA1 */
    {
        MacIIvxMachineClass *qmc = MACIIVX_MACHINE_GET_CLASS(machine);

        object_initialize_child(OBJECT(machine), "via1", &m->via1,
                                TYPE_MOS6522_MACIIVX);
        qdev_prop_set_uint64(DEVICE(&m->via1), "frequency", VIA_TIMER_FREQ);
        /*
         * Machine-ID straps on port A (read with DDRA all-input during
         * identification), carried over from the maciici/maciisi
         * decoder-kind-5 fingerprint: (PA & 0x56) == 0x46 selects the
         * production configuration rather than a factory test monitor.
         * Whatever distinguishes IIvx/IIvi/Performa 600 to the ROM
         * (clock-speed timing, a cache-presence probe, or a strap this
         * value doesn't cover) is undetermined without disassembly; all
         * three sibling machine types use the same value for now.
         */
        qdev_prop_set_uint8(DEVICE(&m->via1), "pins-a", qmc->pins_a);
    }
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
    m->egret_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maciivx_egret_timer_cb,
                                  m);

    /*
     * ADB devices behind the Egret.  No Egret-initiated/unsolicited
     * delivery is modelled: on this machine the HOST polls — the ADB
     * manager's completion path (ROM 0x4080a70e-0x4080a734) walks the
     * active-device bitmap (transport globals +334) and re-issues Talk
     * R0 to the next device after every no-data poll, so keyboard and
     * mouse data is collected by the guest's own continuous rotation.
     */
    {
        BusState *adb_bus = qdev_get_child_bus(DEVICE(&m->via1), "adb.0");

        dev = qdev_new(TYPE_ADB_KEYBOARD);
        qdev_realize_and_unref(dev, adb_bus, &error_fatal);
        dev = qdev_new(TYPE_ADB_MOUSE);
        qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    }
    m->vbl_off_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maciivx_vbl_off, m);
    m->sixty_hz_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maciivx_sixty_hz, m);
    timer_mod(m->sixty_hz_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VIA_60HZ_TIMER_PERIOD_NS);
    m->one_second_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, maciivx_one_second,
                                       m);
    timer_mod(m->one_second_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), MACIIVX_GLUE_VIA1));
    memory_region_init_io(&m->via1mem, OBJECT(machine), &maciivx_via1_ops,
                          &m->via1, "via1", VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, VIA1_OFS, &m->via1mem);

    /*
     * Machine identification fingerprint (probe entry for decoder kind
     * 5, the IIvx): VIA1 IER mirrors at +0x40000 (the whole I/O slice
     * repeats, so this comes for free) but must NOT respond at +0x20000
     * (bus error there), and RBV/VDAC must be present.
     */

    /* RBV's VIA2-emulation window at the classic VIA2 site */
    memory_region_init_io(&m->rbv_via2mem, OBJECT(machine),
                          &maciivx_rbv_via2_ops, m, "rbv-via2",
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
                                           MACIIVX_GLUE_SCC));
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
    /* maciivx-local FIFO-status fixup over ASC reg 0x804 (see ops above) */
    memory_region_init_io(&m->asc_status_fixup, OBJECT(machine),
                          &maciivx_asc_status_ops, m, "asc-status-fixup", 1);
    memory_region_add_subregion_overlap(&m->macio, ASC_OFS + ASC_FIFOIRQ_OFS,
                                        &m->asc_status_fixup, 1);
    /* TODO: route via VIA2 interrupt inputs; direct CPU wiring storms */
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(maciivx_irq_sink, m, 0));

    /* SWIM floppy */
    object_initialize_child(OBJECT(machine), "swim", &m->swim, TYPE_SWIM);
    sysbus = SYS_BUS_DEVICE(&m->swim);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, SWIM_OFS,
                                sysbus_mmio_get_region(sysbus, 0));

    /*
     * The IIvx address decoder ignores A15 for the SCC and SWIM selects,
     * so they also answer at base|0x8000.  The ROM fingerprints these
     * mirrors during machine identification.
     */
    memory_region_init_io(&m->escc_mirror, OBJECT(machine),
                          &maciivx_escc_mirror_ops, NULL, "escc-mirror",
                          0x2000);
    memory_region_add_subregion(&m->macio, SCC_OFS | 0x8000, &m->escc_mirror);

    memory_region_init_alias(&m->swim_mirror, OBJECT(machine), "swim-mirror",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->swim),
                                                    0),
                             0, 0x2000);
    memory_region_add_subregion(&m->macio, SWIM_OFS | 0x8000, &m->swim_mirror);

    /* RBV / VDAC / SCSI stubs */
    memory_region_init_io(&m->rbvmem, OBJECT(machine), &maciivx_rbv_ops, m,
                          "rbv", 0x2000);
    memory_region_add_subregion(&m->macio, RBV_OFS, &m->rbvmem);

    memory_region_init_io(&m->vdacmem, OBJECT(machine), &maciivx_vdac_ops, m,
                          "vdac", 0x40);
    memory_region_add_subregion(&m->macio, VDAC_OFS, &m->vdacmem);

    /* NCR5380 SCSI controller and its pseudo-DMA aperture (+0x2000) */
    object_initialize_child(OBJECT(machine), "scsi", &m->scsi, TYPE_NCR5380);
    qdev_prop_set_uint8(DEVICE(&m->scsi), "reg-shift", 4);
    sysbus = SYS_BUS_DEVICE(&m->scsi);
    sysbus_realize(sysbus, &error_fatal);
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(maciivx_scsi_irq, m, 0));
    memory_region_add_subregion(&m->macio, SCSI_OFS,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_init_io(&m->scsi_pdma, OBJECT(machine),
                          &maciivx_scsi_pdma_ops, &m->scsi, "scsi-pdma", 0x2000);
    memory_region_add_subregion(&m->macio, SCSI_OFS + 0x2000, &m->scsi_pdma);

    /* handshake pseudo-DMA aperture ("SCSI+DRQ", 0x50F06000) */
    memory_region_init_io(&m->scsi_hsk, OBJECT(machine),
                          &maciivx_scsi_hsk_ops, &m->scsi, "scsi-hsk", 0x2000);
    memory_region_add_subregion(&m->macio, 0x6000, &m->scsi_hsk);

    scsi_bus_legacy_handle_cmdline(&m->scsi.bus);

    /*
     * ROM.  Table 1-2 names the 32-bit ROM window 0x40000000, but the
     * ROM's own hardware base-address table (ROM offset 0x3484) and its
     * StartBoot both use 0x40800000 as the canonical base (same site the
     * IIci/IIsi use), and after leaving the low overlay execution jumps
     * to 0x408xxxxx.  On real hardware the 1 MB ROM is decoded across a
     * wide window; here we map it at 0x40000000 (Table 1-2) AND mirror
     * it at 0x40800000 (the base the ROM actually branches to).
     */
    memory_region_init_rom(&m->rom, NULL, "maciivx.rom", MACIIVX_ROM_SIZE,
                           &error_abort);
    memory_region_add_subregion(get_system_memory(), MACIIVX_ROM_ADDR,
                                &m->rom);

    memory_region_init_alias(&m->rom_alias_hi, NULL, "maciivx.rom-alias-hi",
                             &m->rom, 0, MACIIVX_ROM_SIZE);
    memory_region_add_subregion(get_system_memory(), MACIIVX_ROM_HI_ADDR,
                                &m->rom_alias_hi);

    /*
     * 24-bit ROM window (Table 1-2: $80 0000-$8F FFFF, 1 MB, exactly
     * ADDR24_ROM_BASE/MACIIVX_ROM_SIZE): unlike the IIci/IIsi ROMs,
     * this ROM demonstrably switches to 24-bit addressing and jumps to
     * execute from here VERY early in boot -- well before the PMMU
     * could plausibly be programmed (observed: right after the first
     * RBV/pseudo-VIA2 identification reads, PC lands at 0x00800000 and
     * every fetch missed as an unbacked "ram-hole" 0 read without this
     * alias, decoding zero-filled memory as a runaway NOP chain).  So,
     * unlike the RBV siblings' "no static 24-bit alias" rule, this one
     * IS mapped unconditionally.  This does not clash with 24-bit
     * RAM-sizing probes: Table 1-2 puts 24-bit RAM at $00 0000-$7F
     * FFFF (8 MB) specifically because ROM starts right at $80 0000.
     */
    memory_region_init_alias(&m->rom_alias24, NULL, "maciivx.rom-alias24",
                             &m->rom, 0, MACIIVX_ROM_SIZE);
    memory_region_add_subregion(get_system_memory(), ADDR24_ROM_BASE,
                                &m->rom_alias24);

    /*
     * No RAM shadow / overlay at physical 0.  StartBoot's self-relocation
     * (ROM 0x398e: d3 = *(a0+0x70) - a2; a0/a1/a4 += d3; jmp 0x2e3e+d3)
     * relocates the ROM to the base stored at its table slot ROM[0x34f4]
     * (a0 = the 0x3484 base table, +0x70).  That slot ships as 0-padding
     * (a bug/no-op the IIci baseline tolerates), which under any physical
     * base makes d3 = -base and sends execution into low DRAM -> the
     * double-fault.  Rather than emulate the power-on overlay + a
     * write-through DRAM alias to run the whole POST from a low ROM shadow
     * (which then can't satisfy BOTH the ROM self-checksum -- it re-reads
     * a clean image -- AND the stack/globals StartBoot reads back from low
     * DRAM), do what the ROM's own decoder table intends and what the
     * Classic II bring-up did (macclassicii.c: patch ROM 0x34fc/0x3ae6 to
     * the canonical base): patch the relocation slot to the real ROM base
     * so the reloc is a NO-OP, start execution in the HIGH ROM window, and
     * let the ROM run the entire POST from clean high ROM with low memory
     * a plain writable DRAM for its stack/VBR/globals.  See the ROM patch
     * + checksum repair in the bios-load block below.
     */

    /*
     * NOTE (inherited caution, unverified against this ROM): on the
     * RBV-family siblings, aliasing the top of the system ROM into
     * slot $E standard space as a second onboard-video DeclROM copy
     * made the OS open a wrong-model .Display_Video driver and
     * bus-error.  Not done here either, pending confirmation the IIvx
     * ROM's onboard-video pseudo-slot works the same way.
     */

    /*
     * Onboard video: dedicated VRAM (see the "VASP Integrated
     * Controller" note in the file header and the maciivx-fb comment
     * above), NOT the bottom of system RAM like the RBV-family
     * siblings.  Table 1-2: 32-bit window $60B0 0000-$60BF FFFF,
     * 24-bit window $B0 0000-$BF FFFF, both 1 MB.
     */
    memory_region_init_ram(&m->vram, NULL, "maciivx.vram",
                           VRAM_SIZE, &error_abort);
    memory_region_add_subregion(get_system_memory(), VRAM_ADDR, &m->vram);

    memory_region_init_alias(&m->vram_alias24, NULL,
                             "maciivx.vram-alias24", &m->vram, 0, VRAM_SIZE);
    memory_region_add_subregion(get_system_memory(), VRAM_ADDR24,
                                &m->vram_alias24);

    object_initialize_child(OBJECT(machine), "fb", &m->fb, TYPE_MACIIVX_FB);
    m->fb.ram = &m->vram;
    sysbus = SYS_BUS_DEVICE(&m->fb);
    sysbus_realize(sysbus, &error_fatal);

    /*
     * NuBus card bus.  The IIvx has only the 030 PDS, but a PDS->NuBus
     * adapter (and QEMU) presents card slots 9-D in the standard-slot and
     * super-slot spaces; slot E remains the onboard-video pseudo-slot mapped
     * above.  Map the bridge windows below the onboard-video decodes
     * (priority -1, still above the A31 catch-all at -2) so those keep their
     * own decode, and route card slot interrupts into the RBV.
     */
    {
        SysBusDevice *nb;
        qemu_irq *slot_irq;
        int slot;

        object_initialize_child(OBJECT(machine), "mac-nubus-bridge",
                                &m->mac_nubus_bridge, TYPE_MAC_NUBUS_BRIDGE);
        nb = SYS_BUS_DEVICE(&m->mac_nubus_bridge);
        sysbus_realize(nb, &error_fatal);
        memory_region_add_subregion_overlap(get_system_memory(),
                            MAC_NUBUS_FIRST_SLOT * NUBUS_SUPER_SLOT_SIZE,
                            sysbus_mmio_get_region(nb, 0), -1);
        memory_region_add_subregion_overlap(get_system_memory(),
                            NUBUS_SLOT_BASE +
                            MAC_NUBUS_FIRST_SLOT * NUBUS_SLOT_SIZE,
                            sysbus_mmio_get_region(nb, 1), -1);

        slot_irq = qemu_allocate_irqs(maciivx_nubus_irq, m,
                                      MAC_NUBUS_LAST_SLOT + 1);
        for (slot = MAC_NUBUS_FIRST_SLOT; slot <= MAC_NUBUS_LAST_SLOT; slot++) {
            qdev_connect_gpio_out(DEVICE(&m->mac_nubus_bridge), slot,
                                  slot_irq[slot]);
        }
    }

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, MACIIVX_ROM_ADDR,
                                        MACIIVX_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }

    if (!qtest_enabled()) {
        if (bios_size <= 0 || bios_size > MACIIVX_ROM_SIZE) {
            error_report("could not load MacROM '%s'", bios_name);
            exit(1);
        }

        ptr = rom_ptr(MACIIVX_ROM_ADDR, bios_size);
        assert(ptr != NULL);

        /*
         * Relocation-slot patch (cf. macclassicii.c 0x34fc/0x3ae6): the
         * StartBoot self-reloc reads its target base from ROM[0x34f4]
         * (the 0x3484 decoder base table, +0x70), which ships as 0.  Set
         * it to the canonical high ROM base so d3 = base - a2 == 0 when we
         * start from the high window: the reloc becomes a no-op and the
         * ROM runs from clean high ROM (low memory stays plain DRAM).
         */
        if (bios_size > 0x34f4 + 4) {
            stl_be_p(ptr + 0x34f4, MACIIVX_ROM_HI_ADDR);
        }

        /*
         * Repair the ROM's power-on self-checksum after the patch above,
         * exactly as the Classic II bring-up does (macclassicii.c).  The
         * POST word-sum test (ROM 0x46bd0) adds every big-endian 16-bit
         * word from offset 4 to the end and compares against the 32-bit
         * total stored in the first longword (offset 0); a mismatch sets
         * the POST error mask d6 and the phase sequencer (ROM 0x465fe:
         * tstl d6; bne 0x48dea) parks in the serial diagnostic console.
         * (This ROM uses only the single word-sum scheme -- the checksum
         * routine at 0x46bd0 has no separate byte-lane block, unlike the
         * Classic II's, so only offset 0 is recomputed.)
         */
        {
            uint32_t sum = 0;
            int off;
            for (off = 4; off + 1 < bios_size; off += 2) {
                sum += lduw_be_p(ptr + off);
            }
            stl_be_p(ptr, sum);
        }

        /*
         * Reset vector.  SP = ROM[0] (now the repaired checksum -- the ROM
         * reloads SP immediately, so the value is immaterial), PC = the
         * HIGH-window entry (base + ROM[4] offset 0x2a): execution begins
         * in clean high ROM (a2 == high base -> reloc no-op).  Applied by
         * main_cpu_reset directly (reset-handler ordering vs ROM loader).
         */
        m->reset_sp = ldl_be_p(ptr);
        m->reset_pc = MACIIVX_ROM_HI_ADDR + (ldl_be_p(ptr + 4) &
                                             (MACIIVX_ROM_SIZE - 1));
    }
}

/*
 * pins-a: same value maciici/maciisi use for "production, not factory
 * test monitor" (see the VIA1-init comment above); identical across
 * all three siblings until a real per-model strap difference turns up.
 */
#define MACIIVX_VIA1_PINS_A   0xef

typedef struct MacIIvxMachineData {
    const char *desc;
    uint8_t pins_a;
    int mac_model;
} MacIIvxMachineData;

static const MacIIvxMachineData maciivx_data = {
    .desc = "Macintosh IIvx",
    .pins_a = MACIIVX_VIA1_PINS_A,
    .mac_model = MAC_MODEL_IIVX,
};

static const MacIIvxMachineData maciivi_data = {
    .desc = "Macintosh IIvi",
    .pins_a = MACIIVX_VIA1_PINS_A,
    .mac_model = MAC_MODEL_IIVI,
};

static const MacIIvxMachineData performa600_data = {
    .desc = "Performa 600",
    .pins_a = MACIIVX_VIA1_PINS_A,
    .mac_model = MAC_MODEL_P600,
};

static void maciivx_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68030"),
        NULL
    };
    const MacIIvxMachineData *md = data;
    MacIIvxMachineClass *qmc = MACIIVX_MACHINE_CLASS(oc);
    MachineClass *mc = MACHINE_CLASS(oc);

    qmc->pins_a = md->pins_a;
    qmc->mac_model = md->mac_model;
    mc->desc = md->desc;
    mc->init = maciivx_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->block_default_type = IF_SCSI;
    mc->default_ram_size = 8 * MiB;
    mc->default_ram_id = "maciivx.ram";
    machine_add_audiodev_property(mc);
}

static const TypeInfo maciivx_machine_typeinfo[] = {
    {
        .name       = TYPE_MACIIVX_GLUE,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacIIvxGlueState),
        .instance_init = maciivx_glue_init,
        .class_init = maciivx_glue_class_init,
    },
    {
        .name       = TYPE_MOS6522_MACIIVX,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522MacIIvxState),
        .instance_init = mos6522_maciivx_init,
        .class_init = mos6522_maciivx_class_init,
    },
    {
        .name       = TYPE_MACIIVX_FB,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacIIvxFbState),
        .class_init = maciivx_fb_class_init,
    },
    {
        .name       = TYPE_MACIIVX_MACHINE,
        .parent     = TYPE_MACHINE,
        .abstract   = true,
        .instance_size = sizeof(MacIIvxMachineState),
        .class_size = sizeof(MacIIvxMachineClass),
    },
    {
        .name       = MACHINE_TYPE_NAME("maciivx"),
        .parent     = TYPE_MACIIVX_MACHINE,
        .class_init = maciivx_machine_class_init,
        .class_data = &maciivx_data,
    },
    {
        .name       = MACHINE_TYPE_NAME("maciivi"),
        .parent     = TYPE_MACIIVX_MACHINE,
        .class_init = maciivx_machine_class_init,
        .class_data = &maciivi_data,
    },
    {
        .name       = MACHINE_TYPE_NAME("performa600"),
        .parent     = TYPE_MACIIVX_MACHINE,
        .class_init = maciivx_machine_class_init,
        .class_data = &performa600_data,
    },
};

DEFINE_TYPES(maciivx_machine_typeinfo)
