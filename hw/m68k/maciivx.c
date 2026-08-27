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
static bool maciivx_egret_pseudo_try_process(MOS6522MacIIvxState *v1s);

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
    MemoryRegion rom24_ram;     /* writable RAM copy of ROM at 24-bit 0x800000 */
    MemoryRegion gap24_ram;     /* writable RAM for the 0x900000-0xAFFFFF gap */
    MemoryRegion rom_alias_hi;
    MemoryRegion rom_slotE;     /* onboard-video DeclROM aliased into slot $E */
    MemoryRegion vram_slotE;    /* onboard VRAM framebuffer aliased into slot $E */
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
    /* reply staging: 4-byte header + up to a full 256-byte MCU/XPRAM window */
    uint8_t egret_resp[4 + 256];
    int egret_resp_len;
    int egret_resp_idx;
    bool egret_session;
    bool egret_no_resp;
    /*
     * /XCVR_SESSION (PB3) is an INPUT driven by the Egret; track the
     * line state so port-B reads reflect the Egret, not the ORB latch
     * (see maciivx_via1_read).  Idle = deasserted (PB3 high).
     */
    bool egret_xcvr_asserted;
    /*
     * Egret pseudo-command transactions (packet type byte 0x01:
     * self-test/autopoll/PRAM/RTC control), driven with a PB4/PB5
     * byte handshake and /SYS_SESSION released throughout -- a separate
     * framing from the formal-session ADB commands above.
     */
    uint8_t egret_pseudo_cmd[4 + 256];
    uint8_t egret_mcu_mem[256];
    int egret_pseudo_cmd_len;
    bool egret_pseudo_active;
    bool egret_pseudo_closing;
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

    /*
     * PB3 (/XCVR_SESSION) is an Egret-driven INPUT: report the Egret's
     * line state, not the ROM's last ORB write.  The ROM's cold-start
     * Egret sync (0x40814cc8/0x40814cfa: btst #3) branches on this bit;
     * reading back the host's own ORB latch would spin forever.  The
     * Egret only asserts /XCVR (PB3 low) while actually mid-delivery of
     * a genuine reply; it stays deasserted (PB3 high) for the
     * no-response/discard framing and at power-on, so the sync runs its
     * send/timeout path through to boot.  (Ported from macclassicii.c.)
     */
    if (reg == VIA_REG_B && v1s->machine) {
        MacIIvxMachineState *m = v1s->machine;
        bool xcvr = m->egret_xcvr_asserted && !m->egret_no_resp &&
                    m->egret_resp_len > 0;

        if (xcvr) {
            val &= ~(uint64_t)EGRET_XCVR;
        } else {
            val |= EGRET_XCVR;
        }
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

/* Egret microcontroller behaviour (ported from macclassicii.c) */
static void maciivx_egret_set_xcvr(MOS6522MacIIvxState *v1s,
                                        bool assert)
{
    MOS6522State *s = MOS6522(v1s);

    /*
     * PB3 is an Egret-driven input: the authoritative line state lives
     * in the machine struct so port-B reads report it regardless of the
     * ROM's ORB writes (see maciivx_via1_read).  Keep s->b in sync
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

    /*
     * Pseudo-command packets (type byte 0x01) can also arrive over the
     * formal-session framing -- the post-MMU interrupt-driven Egret
     * driver (ISR 0x40814912) sends e.g. [01 08 addrHi addrLo <16
     * bytes>] (write MCU memory, uploading autopoll parameters) and
     * [01 1b/0e/1c ...] control settings this way.  These are MCU
     * control traffic, not ADB: don't feed them to the ADB bus (a
     * bogus adb_request would disturb device state); for the
     * write/set-style commands the ROM only needs the exchange to
     * terminate cleanly, which the no-response turnaround provides.
     */
    if (c[0] == 0x01 && n >= 2) {
        qemu_log_mask(LOG_UNIMP,
                      "maciivx egret: pseudo (session) cmd 0x%02x"
                      " len=%d\n", c[1], n);
        maciivx_egret_no_response(m);
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
            maciivx_egret_no_response(m);
        }
    }
}

/*
 * Egret "pseudo-command" packets (packet type byte 0x01): host->MCU
 * control requests distinct from ADB pass-through -- self-test,
 * autopoll, PRAM/RTC access, etc (cf. the Cuda/Egret pseudo-command
 * family, e.g. MAME's apple/egret.cpp CUDA_* command set).  The
 * Classic II ROM issues these with a byte-level PB4(/VIA_FULL)/
 * PB5(/SYS_SESSION) handshake and /SYS_SESSION *released* throughout
 * -- see ROM 0x4084a556, decoded instruction-by-instruction:
 *   send  [0x01, cmd, ...params]         (SR-out, external clock)
 *   recv  [_, _, _, cmd-echo, data]       (SR-in, external clock)
 * with /XCVR_SESSION required asserted (low) for every reply byte
 * except a deassert exactly at the last one (the ROM's end marker).
 *
 * The reply is always [0x00, 0x00, 0x00, cmd-echo, <optional data...>]:
 * the ROM validates only that the 4th byte (index 3) echoes the
 * command code, then reads any trailing data bytes while /XCVR stays
 * asserted (GET_PRAM 0x07 returns one data byte; SET_PRAM 0x0c none).
 * Commands not modelled here are left unrecognised so the caller falls
 * back to the pre-existing cold-start-sync-safe generic behaviour
 * (interrupt only, no /XCVR assert) rather than guessing wrong.
 */
#define EGRET_PSEUDO_TYPE       0x01
#define EGRET_PSEUDO_READ_MCU   0x02
#define EGRET_PSEUDO_GET_TIME   0x03
#define EGRET_PSEUDO_GET_PRAM   0x07
#define EGRET_PSEUDO_WRITE_MCU  0x08
#define EGRET_PSEUDO_SET_TIME   0x09
#define EGRET_PSEUDO_SET_PRAM   0x0c

/* stage the reply header [0,0,0,cmd]; caller appends any data bytes */
static void maciivx_egret_pseudo_reply(MacIIvxMachineState *m,
                                            uint8_t cmd)
{
    m->egret_resp_len = 0;
    m->egret_resp[m->egret_resp_len++] = 0x00;
    m->egret_resp[m->egret_resp_len++] = 0x00;
    m->egret_resp[m->egret_resp_len++] = 0x00;
    m->egret_resp[m->egret_resp_len++] = cmd;
    m->egret_resp_idx = 0;
}

/*
 * Egret MCU address map, as the ROM uses it: 0x0100-0x01FF is the
 * MCU-resident XPRAM -- the SAME 256-byte parameter RAM the GET_PRAM/
 * SET_PRAM pseudo-commands address directly (and the Egret's classic
 * 343-0042 protocol emulation serves on real hardware), so it must be
 * backed by the one authoritative store, via1.PRAM[].  The ROM's
 * _ReadXPRam/_WriteXPRam (trap A051/A052 -> Egret path 0x40a15204)
 * access XPRAM exclusively through READ_MCU/WRITE_MCU at 0x100+offset;
 * the OS-startup autopoll parameter uploads live elsewhere in MCU
 * space and keep the scratch egret_mcu_mem[] backing.
 */
static uint8_t maciivx_egret_mcu_read(MacIIvxMachineState *m,
                                           uint16_t addr)
{
    if (addr >= 0x100 && addr <= 0x1ff) {
        return m->via1.PRAM[addr - 0x100];
    }
    return m->egret_mcu_mem[addr & 0xff];
}

static void maciivx_egret_mcu_write(MacIIvxMachineState *m,
                                         uint16_t addr, uint8_t data)
{
    if (addr >= 0x100 && addr <= 0x1ff) {
        m->via1.PRAM[addr - 0x100] = data;
    } else {
        m->egret_mcu_mem[addr & 0xff] = data;
    }
}

static bool maciivx_egret_pseudo_build(MacIIvxMachineState *m)
{
    uint8_t *c = m->egret_pseudo_cmd;
    int n = m->egret_pseudo_cmd_len;

    if (n < 2) {
        return false;
    }

    /*
     * Type-0 packets over this framing are ADB pass-through: [00
     * adbcmd <listen data...>].  The post-MMU interrupt-driven Egret
     * driver (ISR 0x40a14912, trap A092) sends its ADB traffic --
     * starting with [00 00] SendReset during ADB-manager init -- with
     * exactly the same PB4//PB5 byte handshake as the pseudo commands,
     * so decode them here rather than in the formal-session path.
     * Reply framing observed to satisfy the ISR: the standard 4-byte
     * header [0, 0, 0, cmd-echo] followed by any Talk register data
     * (the ISR routes bytes past the header into the request block's
     * declared receive buffer via its own a2@(20)/a2@(18) state, so
     * data after the header is safe here -- unlike header-overflow
     * bytes on a request that declared none).
     */
    if (c[0] == 0x00) {
        uint8_t obuf[ADB_MAX_OUT_LEN];
        ADBBusState *adb_bus = &m->via1.adb_bus;
        int olen;

        adb_autopoll_block(adb_bus);
        olen = adb_request(adb_bus, obuf, c + 1, n - 1);
        adb_autopoll_unblock(adb_bus);

        qemu_log_mask(LOG_UNIMP,
                      "maciivx egret: pseudo-framed ADB cmd 0x%02x"
                      " len=%d -> olen=%d\n", c[1], n - 1, olen);
        maciivx_egret_pseudo_reply(m, c[1]);
        if (olen > 0) {
            memcpy(m->egret_resp + m->egret_resp_len, obuf, olen);
            m->egret_resp_len += olen;
        }
        return true;
    }

    if (c[0] != EGRET_PSEUDO_TYPE) {
        return false;
    }

    switch (c[1]) {
    case EGRET_PSEUDO_GET_PRAM:
        if (n >= 4) {
            uint16_t addr = ((uint16_t)c[2] << 8) | c[3];
            uint8_t data = m->via1.PRAM[addr & 0xff];

            qemu_log_mask(LOG_UNIMP,
                          "maciivx egret: pseudo GET_PRAM addr=0x%04x"
                          " -> 0x%02x\n", addr, data);
            maciivx_egret_pseudo_reply(m, EGRET_PSEUDO_GET_PRAM);
            m->egret_resp[m->egret_resp_len++] = data;
            return true;
        }
        break;
    case EGRET_PSEUDO_SET_PRAM:
        if (n >= 5) {
            uint16_t addr = ((uint16_t)c[2] << 8) | c[3];

            m->via1.PRAM[addr & 0xff] = c[4];
            qemu_log_mask(LOG_UNIMP,
                          "maciivx egret: pseudo SET_PRAM addr=0x%04x"
                          " <- 0x%02x\n", addr, c[4]);
            maciivx_egret_pseudo_reply(m, EGRET_PSEUDO_SET_PRAM);
            return true;
        }
        break;
    case EGRET_PSEUDO_WRITE_MCU:
        /*
         * Write MCU memory: [01 08 addrHi addrLo data...].  The
         * post-MMU boot uploads an autopoll/timing parameter block
         * (ROM table 0x4080c676, 20 bytes to MCU address 0x0110) this
         * way.  The parameters have no behavioural model here; store
         * them in a scratch array so READ_MCU reads back consistently,
         * and ACK.
         */
        if (n >= 4) {
            uint16_t addr = ((uint16_t)c[2] << 8) | c[3];
            int j;

            for (j = 0; j + 4 < n; j++) {
                maciivx_egret_mcu_write(m, addr + j, c[4 + j]);
            }
            qemu_log_mask(LOG_UNIMP,
                          "maciivx egret: pseudo WRITE_MCU addr=0x%04x"
                          " len=%d\n", addr, n - 4);
            maciivx_egret_pseudo_reply(m, EGRET_PSEUDO_WRITE_MCU);
            return true;
        }
        break;
    case EGRET_PSEUDO_GET_TIME:
        /* Read the RTC: reply carries 4 big-endian seconds bytes */
        {
            uint32_t t = m->via1.tick_offset +
                         (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) /
                          NANOSECONDS_PER_SECOND);

            qemu_log_mask(LOG_UNIMP,
                          "maciivx egret: pseudo GET_TIME -> 0x%08x\n", t);
            maciivx_egret_pseudo_reply(m, EGRET_PSEUDO_GET_TIME);
            m->egret_resp[m->egret_resp_len++] = t >> 24;
            m->egret_resp[m->egret_resp_len++] = t >> 16;
            m->egret_resp[m->egret_resp_len++] = t >> 8;
            m->egret_resp[m->egret_resp_len++] = t;
            return true;
        }
    case EGRET_PSEUDO_SET_TIME:
        /* Set the RTC: [01 09 t3 t2 t1 t0] */
        if (n >= 6) {
            uint32_t t = ((uint32_t)c[2] << 24) | ((uint32_t)c[3] << 16) |
                         ((uint32_t)c[4] << 8) | c[5];

            m->via1.tick_offset = t - (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) /
                                       NANOSECONDS_PER_SECOND);
            qemu_log_mask(LOG_UNIMP,
                          "maciivx egret: pseudo SET_TIME <- 0x%08x\n", t);
            maciivx_egret_pseudo_reply(m, EGRET_PSEUDO_SET_TIME);
            return true;
        }
        break;
    case EGRET_PSEUDO_READ_MCU:
        /*
         * Read MCU memory: [01 02 addrHi addrLo] -> STREAMED data.
         * The wire command carries no length: the Egret hands out
         * successive bytes from addr for as long as the host keeps
         * clocking, and the HOST ends the reply (its requested length
         * lives only in its own A092 parameter block, a1@(6)) by
         * closing /SYS_SESSION -- handled as the early-close case in
         * maciivx_egret_session_update.  Stage bytes to the end
         * of the 256-byte window; undrained ones are simply dropped at
         * the close.  (The previous single-byte reply made every
         * multi-byte _ReadXPRam -- e.g. GetOSDefault reading XPRAM
         * 0x76/0x77, len 2 -- return one real byte plus stale-SR
         * garbage, which fed a garbage expected-ddType (0x6a) into the
         * boot-driver installer at 0x40a07264 and blocked the disk
         * boot in an endless bus-rescan.)
         */
        if (n >= 4) {
            uint16_t addr = ((uint16_t)c[2] << 8) | c[3];
            int j, len;

            len = 0x100 - (addr & 0xff);
            qemu_log_mask(LOG_UNIMP,
                          "maciivx egret: pseudo READ_MCU addr=0x%04x"
                          " (stream %d) first=0x%02x\n", addr, len,
                          maciivx_egret_mcu_read(m, addr));
            maciivx_egret_pseudo_reply(m, EGRET_PSEUDO_READ_MCU);
            for (j = 0; j < len; j++) {
                m->egret_resp[m->egret_resp_len++] =
                    maciivx_egret_mcu_read(m, addr + j);
            }
            return true;
        }
        break;
    default:
        break;
    }

    /*
     * Any other type-1 packet is a set/control-style MCU pseudo-command
     * with no data reply (the post-MMU Egret driver sends e.g. [01 1b
     * 03] -- an autopoll/one-second control setting -- through its
     * interrupt-driven framing).  ACK it with the bare [0,0,0,cmd]
     * header.  This must NOT fall through to the interrupt-only
     * fallback: the ROM's Egret ISR (0x40a14912) then clocks stale SR
     * bytes as a "reply", and on a byte-1==0 header it copies the
     * trailing garbage through the request block's DATA POINTER at
     * +8 -- which set-style callers (e.g. the trap-A092 thunk at
     * 0x40a154d2, which only fills bytes 0-2 and the callback long)
     * leave UNINITIALISED, corrupting whatever low-memory address the
     * stack garbage points at (observed: the 0x192 Egret state vector,
     * ending in a double fault).  A real Egret ACKs every pseudo
     * command it accepts, so this is also simply truer to hardware.
     *
     * The cold-start sync's framing bytes still fall through to the
     * old behaviour: they never form a [01 cc ...] packet at a
     * turnaround (single 0x00/0x01 bytes only, len < 2 -- verified in
     * a full -d unimp boot log).
     */
    if (n >= 2 && c[0] == EGRET_PSEUDO_TYPE) {
        qemu_log_mask(LOG_UNIMP,
                      "maciivx egret: pseudo generic-ACK cmd 0x%02x"
                      " (len=%d)\n", c[1], n);
        maciivx_egret_pseudo_reply(m, c[1]);
        return true;
    }

    qemu_log_mask(LOG_UNIMP,
                  "maciivx egret: unrecognised pseudo-command"
                  " [%02x %02x %02x %02x] (len=%d)\n",
                  n > 0 ? c[0] : 0, n > 1 ? c[1] : 0, n > 2 ? c[2] : 0,
                  n > 3 ? c[3] : 0, n);
    return false;
}

/*
 * Called at the send->receive turnaround (ACR external-clock SR-OUT ->
 * SR-IN, /SYS_SESSION released, no formal session) to decide whether
 * the bytes just shifted out were a real pseudo-command.  On success,
 * stages the reply and drives the receive handshake (first byte +
 * /XCVR assert) the ROM's wait loop at 0x4084a5f8 needs; on failure
 * (unrecognised content -- e.g. the cold-start sync's own framing
 * bytes, which share this exact ACR pattern but carry no real pseudo-
 * command) does nothing, leaving the caller to fall back to the
 * existing interrupt-only behaviour.
 */
static bool maciivx_egret_pseudo_try_process(MOS6522MacIIvxState *v1s)
{
    MacIIvxMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (!maciivx_egret_pseudo_build(m)) {
        m->egret_pseudo_cmd_len = 0;
        return false;
    }

    m->egret_pseudo_cmd_len = 0;
    m->egret_pseudo_active = true;
    m->egret_no_resp = false;

    /*
     * The first reply byte must already be in the shift register by
     * the time the ROM's receive wait loop sees the completion
     * interrupt and checks /XCVR -- stage it now.
     */
    s->sr = m->egret_resp[0];
    m->egret_resp_idx = 1;
    maciivx_egret_set_xcvr(v1s, true);
    maciivx_egret_schedule_int(m);
    return true;
}

/*
 * /XCVR_SESSION (PB3) as sampled by the ROM driver at each shift
 * interrupt: real responses keep PB3 high while bytes flow and drop it
 * as the end marker; a no-response exchange holds PB3 low throughout
 * (the driver still clocks two junk bytes through SR).
 */
static void maciivx_egret_ack_toggle(MOS6522MacIIvxState *v1s)
{
    MacIIvxMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (m->egret_resp_idx < m->egret_resp_len) {
        s->sr = m->egret_resp[m->egret_resp_idx++];
        maciivx_egret_set_xcvr(v1s, m->egret_no_resp);
        maciivx_egret_schedule_int(m);
        qemu_log_mask(LOG_UNIMP,
                      "maciivx egret: feed 0x%02x (#%d/%d) pc=%08x b=%02x\n",
                      s->sr, m->egret_resp_idx, m->egret_resp_len,
                      maciivx_trace_pc(), s->b);
    } else {
        /* final ack: PB3 low = end-of-response, the exchange is over */
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
        m->egret_session = false;
        maciivx_egret_set_xcvr(v1s, true);
        maciivx_egret_schedule_int(m);
        qemu_log_mask(LOG_UNIMP,
                      "maciivx egret: response complete pc=%08x b=%02x\n",
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
     * Pseudo-command reply delivery (byte-handshake framing) is driven
     * entirely from maciivx_egret_sr_read -- the /VIA_FULL (PB4)
     * and /SYS_SESSION (PB5) writes the ROM's receive helper
     * (0x4084a6b4) makes around each shift are bookkeeping on its side
     * only.  But those writes DO satisfy the /SYS_SESSION-based
     * session-open edge test below (m->egret_session is never set for
     * this framing, so "!session" always holds) -- without this guard
     * the receive turnaround's own /SYS_SESSION-released write
     * (0x4084a5f4) would be misread as opening a formal-session receive
     * against whatever response state pseudo delivery has staged.
     * Suppress all of it while a pseudo exchange is in flight.
     */
    if (m->egret_pseudo_active) {
        /*
         * Host-side early close of a STREAMED reply: the interrupt-
         * driven Egret driver's receive (ISR 0x40a14912) reads exactly
         * the number of data bytes its request block asked for and
         * then drops /SYS_SESSION (bclr #5 at 0x40a149c8) and waits
         * for /XCVR to rise as the end-of-exchange handshake.  A
         * streamed READ_MCU reply usually stages more bytes than the
         * host wants (the wire carries no length), so the last-byte
         * close in maciivx_egret_sr_read never triggers -- end
         * the exchange here on that /SYS_SESSION falling edge instead.
         * Gated on the header already being fully consumed (idx > 4)
         * so the ISR's own header-phase /SYS_SESSION toggling (e.g.
         * the bset #5 at 0x40a149a6) can't be mistaken for the close.
         */
        if ((v1s->last_b & EGRET_SYS_SESSION) &&
            !(s->b & EGRET_SYS_SESSION) && m->egret_resp_idx > 4) {
            /*
             * A next byte was already staged in SR with its completion
             * interrupt scheduled (the read-side chain stages ahead);
             * the host is abandoning it -- cancel that interrupt, or
             * it fires after the close and the ISR mistakes the stale
             * byte for the start of an unsolicited Egret packet
             * (observed: garbage dispatched through the autopoll
             * handler -> Address Error at a junk PC -> sad mac).
             */
            timer_del(m->egret_timer);
            maciivx_egret_set_xcvr(v1s, false);
            m->egret_pseudo_active = false;
            m->egret_pseudo_closing = true;
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
        }
        return;
    }

    /*
     * Teardown tail: the ROM clears /SYS_SESSION then /VIA_FULL
     * (0x4084a646/0x4084a650) after the last reply byte.  The first of
     * those writes is a /SYS_SESSION-released edge that would otherwise
     * match the formal-session-open test below; keep suppressing until
     * both lines have settled low (/VIA_FULL also clear).
     */
    if (m->egret_pseudo_closing) {
        if (!(s->b & (EGRET_SYS_SESSION | EGRET_VIA_FULL))) {
            m->egret_pseudo_closing = false;
        }
        return;
    }

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
            maciivx_egret_set_xcvr(v1s, false);
            maciivx_egret_sr_written(v1s);
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
            maciivx_egret_set_xcvr(v1s, m->egret_no_resp);
            maciivx_egret_schedule_int(m);
        } else {
            /*
             * Receive session with nothing to deliver: the Egret has
             * nothing to say -- run the no-response signature (/XCVR
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
     * in SR and clock the next one; BOTH released while a response is
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
            maciivx_egret_process(m);
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
        maciivx_egret_set_xcvr(v1s, false);
        /*
         * The Egret clocks one final shift-register interrupt when the
         * host releases /TIP: the "session closed" acknowledgement,
         * needed to advance the poll cadence's ORB ^= 0x20 reopen.
         */
        maciivx_egret_schedule_int(m);
    }
}

static void maciivx_egret_sr_written(MOS6522MacIIvxState *v1s)
{
    MacIIvxMachineState *m = v1s->machine;
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
                      "maciivx egret: <- 0x%02x (#%d) pc=%08x b=%02x\n",
                      s->sr, m->egret_cmd_len, maciivx_trace_pc(), s->b);
    } else if ((s->acr & SR_CTRL) == SR_CTRL) {
        /*
         * Outside a formal session, with the shifter in the exact
         * external-clock SR-OUT mode the byte-handshake pseudo-command
         * send uses (ROM 0x4084a556 sets ACR to 0x1c before shifting
         * the first byte): collect the raw byte.  Whether this forms a
         * recognised pseudo-command is decided at the send->receive
         * turnaround (maciivx_egret_acr_changed); the cold-start
         * sync's own framing bytes share this same ACR pattern but are
         * simply discarded there when unrecognised.
         */
        if (m->egret_pseudo_cmd_len < (int)sizeof(m->egret_pseudo_cmd)) {
            m->egret_pseudo_cmd[m->egret_pseudo_cmd_len++] = s->sr;
        }
        qemu_log_mask(LOG_UNIMP,
                      "maciivx egret: pseudo <- 0x%02x (#%d) pc=%08x"
                      " b=%02x\n", s->sr, m->egret_pseudo_cmd_len,
                      maciivx_trace_pc(), s->b);
    }

    /*
     * Regardless of session framing, the Egret provides the shift clock:
     * each byte the host shifts OUT in external-clock SR mode completes
     * and raises the SR interrupt.  This is what carries the ROM's Egret
     * cold-start byte-framing sync (0x40814cc8), whose send helpers clock
     * bytes with /SYS_SESSION released (no formal session open).
     */
    maciivx_egret_schedule_int(m);
}

static void maciivx_egret_acr_changed(MOS6522MacIIvxState *v1s)
{
    MacIIvxMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    qemu_log_mask(LOG_UNIMP,
                  "maciivx egret: DBG acr=%02x sess=%d cmdlen=%d "
                  "resplen=%d respidx=%d pseudoact=%d pc=%08x\n",
                  s->acr, m->egret_session, m->egret_cmd_len,
                  m->egret_resp_len, m->egret_resp_idx,
                  m->egret_pseudo_active, maciivx_trace_pc());

    /*
     * A fresh byte-handshake pseudo-command send always (re)starts here
     * (ROM 0x4084a556 sets ACR to exactly this mode before shifting the
     * first byte): drop any stale, never-turned-around collection from
     * a prior attempt so it can't leak into this one.
     */
    if (!m->egret_session && (s->acr & SR_CTRL) == SR_CTRL) {
        m->egret_pseudo_cmd_len = 0;
    }

    /*
     * The host turns the shifter around to receive while still holding
     * the session: treat the bytes collected so far as the command and
     * build the response; the host's following TACK toggle clocks the
     * first byte out.  /XCVR_SESSION stays HIGH for a real response
     * (low at the first data interrupt = "discard" to this driver) and
     * goes LOW for the no-response turnaround.
     */
    if (m->egret_session && !(s->acr & SR_OUT)
        && m->egret_resp_idx >= m->egret_resp_len && m->egret_cmd_len > 0) {
        /*
         * A previous exchange's response counts as "pending" only while
         * it is still partially undelivered (idx < len); a fully
         * consumed one that just never saw its final ack toggle (the
         * interrupt-driven Egret driver at 0x40814912 goes straight
         * from draining the old reply into sending the next command)
         * must not shadow this turnaround.
         */
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
    } else if (!m->egret_session && (s->acr & SR_CTRL) && !(s->acr & SR_OUT)) {
        /*
         * Outside a formal session, turning the shifter to external-
         * clock INPUT is a send->receive turnaround.  If the bytes just
         * collected form a recognised Egret pseudo-command, stage its
         * reply and drive the receive handshake for it (see ROM
         * 0x4084a5ee).  Otherwise -- including the cold-start
         * byte-framing sync's own turnarounds (0x40814e6a), which share
         * this same ACR pattern but carry no real command -- fall back
         * to the original behaviour: just raise the completion
         * interrupt so the ROM's receive helper advances with SR left
         * at 0.
         */
        if (!maciivx_egret_pseudo_try_process(v1s)) {
            /*
             * Drop any stale, already-consumed response left over from
             * an earlier exchange: with the interrupt-driven Egret
             * driver's IER SR-int enabled, a leftover resp_idx <
             * resp_len would make the read-side chain in
             * maciivx_egret_sr_read feed its junk bytes into this
             * fresh turnaround's receive as if they were a reply
             * (observed as "feed 0x00 (#2/2)" garbage after an
             * unrecognised command, cascading into the ISR copying
             * stale SR bytes through an uninitialised buffer pointer).
             */
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
            maciivx_egret_schedule_int(m);
        }
    }
}

static void maciivx_egret_sr_read(MOS6522MacIIvxState *v1s)
{
    MacIIvxMachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (s->acr & SR_OUT) {
        return;
    }

    /*
     * Pseudo-command reply delivery.  The ROM's per-byte receive helper
     * (0x4084a6b4) WAITS for the shift-complete interrupt, THEN reads
     * SR, THEN (a short delay later) checks /XCVR_SESSION -- so the
     * byte this read just consumed (index egret_resp_idx-1, staged by
     * the previous trigger) must have /XCVR asserted at read time for
     * every byte except the last, which the ROM never checks /XCVR
     * against directly (it only waits for /XCVR to rise afterwards, see
     * ROM 0x4084a64a).  So: stage the NEXT byte here immediately
     * (keeping /XCVR asserted) unless the byte just consumed WAS the
     * last one, in which case drop /XCVR now as the end-of-reply marker
     * instead.  Chaining forward from each read (rather than the
     * request-side /VIA_FULL toggle) sidesteps that /VIA_FULL already
     * reads low, with no edge, going into the very first receive call
     * (it was left low by the preceding send phase).
     */
    if (m->egret_pseudo_active) {
        int just_read = m->egret_resp_idx - 1;

        qemu_log_mask(LOG_UNIMP,
                      "maciivx egret: pseudo -> 0x%02x (#%d/%d)"
                      " pc=%08x b=%02x\n", s->sr, m->egret_resp_idx,
                      m->egret_resp_len, maciivx_trace_pc(), s->b);
        if (just_read >= m->egret_resp_len - 1) {
            /*
             * That was the last byte: drop /XCVR, exchange is over.
             * The ROM still has its own teardown writes coming
             * (0x4084a646/0x4084a650) -- keep suppressing the formal-
             * session detector through those (see egret_pseudo_closing
             * in maciivx_egret_session_update).
             */
            maciivx_egret_set_xcvr(v1s, false);
            m->egret_pseudo_active = false;
            m->egret_pseudo_closing = true;
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
        } else {
            s->sr = m->egret_resp[m->egret_resp_idx++];
            maciivx_egret_schedule_int(m);
        }
        return;
    }

    /*
     * Interrupt-driven Egret driver (post-MMU boot: IER SR-int enabled,
     * ISR 0x40814912): each reply byte is consumed by the ISR's SR
     * read, after which the Egret clocks the next byte in -- chain
     * delivery from the read side, exactly like the pseudo path above.
     * (The polled boot-ROM flows instead drive delivery from their
     * PB4//PB5 handshake edges via maciivx_egret_ack_toggle, so
     * this is gated on IER to avoid double-feeding them.)
     */
    if ((s->ier & SR_INT) && m->egret_resp_idx < m->egret_resp_len) {
        maciivx_egret_ack_toggle(v1s);
        return;
    }

    /*
     * Cold-start framing sync (no formal session): each byte the host
     * reads in external-clock INPUT mode is immediately followed by the
     * Egret clocking the next one -- schedule its completion interrupt.
     *
     * ONLY while the ROM runs the sync by POLLING IFR (VIA1 IER has the
     * SR interrupt disabled).  Once the interrupt-driven Egret driver
     * is installed (post-MMU boot: IER SR-int enabled, ISR 0x40814912),
     * its own end-of-transaction ACK read of SR would re-arm the
     * interrupt here every time, producing a permanent level-1 storm
     * whose re-entered ISR eventually walks a stale request pointer
     * into an Access-Fault cascade and a double fault (observed).  A
     * real Egret does not clock the shifter after a transaction ends.
     */
    if (!m->egret_session && (s->acr & SR_CTRL) && !(s->ier & SR_INT)) {
        maciivx_egret_schedule_int(m);
        return;
    }

    if (m->egret_resp_len == 0) {
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "maciivx egret: -> 0x%02x (#%d/%d) pc=%08x b=%02x\n",
                  s->sr, m->egret_resp_idx, m->egret_resp_len,
                  maciivx_trace_pc(), s->b);
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
    /*
     * Default OS / startup-device XPRAM bytes, matching the working
     * Egret siblings (macclassicii.c / the quadra700 oracle).  The ROM's
     * XPRAM default-rebuild pass covers 0x01-0x6d but NOT this range, so
     * with a zeroed store the boot-driver installer's GetOSDefault
     * (XPRAM[0x76..0x77], the disk driver ddType) reads 0, matches no
     * driver on any disk, and the boot rescans the SCSI bus forever.
     * 0x0001 = the standard Mac SCSI driver ddType every stock disk
     * carries; startup-device record 0x78-0x7b = 0xff = "no preference,
     * scan the bus".  NOTE (session 6): this is forward-prep only -- it
     * is read far past the current blocker (the cold-boot MicroBug
     * console, see IIVX-NOTES.md), so it does not itself change boot
     * behaviour yet, but is required once the disk-boot branch is taken.
     */
    m->via1.PRAM[0x76] = 0x00;
    m->via1.PRAM[0x77] = 0x01;      /* default OS: ddType 1 (Mac SCSI) */
    m->via1.PRAM[0x78] = 0xff;      /* default startup device: none */
    m->via1.PRAM[0x79] = 0xff;
    m->via1.PRAM[0x7a] = 0xff;
    m->via1.PRAM[0x7b] = 0xff;
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
     * Onboard-video Declaration ROM in NuBus standard slot $E
     * (0xFE000000-0xFEFFFFFF).  The IIvx's built-in "Brazil" video is a
     * NuBus pseudo-slot whose DeclROM is embedded in the top of the system
     * ROM: the last 0x174c bytes are a valid Apple format block
     * (testPattern 0x5A932BC7, byteLanes 0x0F, revision/format 0x01,
     * directoryOffset -0x1738 pointing back to the DeclROM base) whose
     * sResource directory names a board sResource + the video sResources
     * (".Display_Video_Apple_Brazil" driver, "Macintosh Built-In Video",
     * VRAM-size/mode variants + gamma tables).  On real hardware VASP
     * aliases the top of the system ROM into slot $E, so the Slot/Display
     * Manager finds it: the video driver reads the format block at the top
     * of the slot (0xFEFFFFFC) and follows its directory.  Alias the whole
     * 1 MB ROM at 0xFEF00000 so ROM offset 0xFFFFF (the byteLanes byte)
     * lands at 0xFEFFFFFF -- the DeclROM's backward directoryOffset/sOffsets
     * then resolve inside slot $E.  This is embedded-ROM aliasing (like the
     * SuperMario/RBV ROMs' internal built-in-video slot ROM), NOT the wrong
     * "whole family DeclROM" copy the RBV siblings warn against: this is the
     * IIvx-specific Brazil DeclROM only.  Overlaps the NuBus bridge's slot
     * window (added at priority -1 below) at higher priority so slot $E
     * decodes to the onboard-video DeclROM while card slots 9-D keep the
     * bridge.
     */
    memory_region_init_alias(&m->rom_slotE, NULL, "maciivx.rom-slotE",
                             &m->rom, 0, MACIIVX_ROM_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        0xFEF00000, &m->rom_slotE, 1);

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
     *
     * Session 8: this is a WRITABLE RAM region PRE-LOADED with the (patched)
     * ROM image, not a read-only alias.  Reason: the ROM's 32-bit power-on
     * write-verify memory test (ROM 0x408466bc) sweeps [0..0xC00000] and, in
     * 32-bit addressing mode, expects that whole window to be contiguous
     * writable DRAM (a real IIvx has up to 68 MB there).  A read-only ROM
     * alias here fails that test (pattern can't be written back) -> the ROM
     * parks in the MicroBug serial console.  Backing 0x800000-0x8FFFFF with a
     * writable RAM copy of the ROM satisfies BOTH users: the 32-bit memory
     * test (writable DRAM) AND the 24-bit-mode ROM fetches/reads the ROM
     * makes here (the copy holds real ROM code/data).  The memory test is
     * effectively non-destructive for boot (it restores, or the values it
     * leaves are re-derived), verified by boot progressing past it.
     *
     * These 24-bit RAM-window backings are ONLY added when installed RAM is
     * small (< 0xC00000): they exist purely so the 32-bit memory test over
     * [0..0xC00000] has writable backing at 0x800000-0xBFFFFF.  With >=12 MB
     * of contiguous DRAM the real RAM already covers that window, and adding
     * these aliases would FRAGMENT it (0x800000-0xBFFFFF served by the
     * aliases instead of contiguous RAM), corrupting the boot's 32-bit heap/
     * structures -> a wild jump / NOP-sled.  So skip them for large RAM.
     */
    if (ram_size < 0x00C00000) {
        memory_region_init_ram(&m->rom24_ram, NULL, "maciivx.rom24-ram",
                               MACIIVX_ROM_SIZE, &error_abort);
        memory_region_add_subregion(get_system_memory(), ADDR24_ROM_BASE,
                                    &m->rom24_ram);
        memory_region_init_ram(&m->gap24_ram, NULL, "maciivx.gap24-ram",
                               0x00200000, &error_abort);
        memory_region_add_subregion(get_system_memory(), 0x00900000,
                                    &m->gap24_ram);
    }

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

    /*
     * The onboard-video slot-$E DeclROM (see the rom_slotE alias above)
     * declares the frame buffer at MinorBaseOS 0 / vpBaseOffset 0, i.e. at
     * the slot's own base address 0xFE000000 (MinorLength 0xC0000).  The
     * Brazil video driver reads/writes pixels through that NuBus slot view,
     * so alias the same dedicated VRAM into the bottom of slot $E as well
     * as at its Table 1-2 window (0x60B00000).  Overlaps the NuBus bridge
     * (priority -1) at higher priority; sits below the DeclROM alias at the
     * top of the slot (0xFEF00000), no overlap between the two.
     */
    memory_region_init_alias(&m->vram_slotE, NULL, "maciivx.vram-slotE",
                             &m->vram, 0, VRAM_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(), 0xFE000000,
                                        &m->vram_slotE, 1);

    /*
     * 24-bit VRAM alias (0xB00000): only for small RAM, same rationale as
     * the rom24/gap24 backings above -- with >=12 MB it would fragment the
     * contiguous DRAM window the 32-bit boot uses.
     */
    if (ram_size < 0x00C00000) {
        memory_region_init_alias(&m->vram_alias24, NULL,
                                 "maciivx.vram-alias24", &m->vram, 0,
                                 VRAM_SIZE);
        memory_region_add_subregion(get_system_memory(), VRAM_ADDR24,
                                    &m->vram_alias24);
    }

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
         * POST-stack relocation fix (default on; IIVX_NOSPFIX disables).
         * The RAM-sizing routine (ROM 0x4084a6b0) builds a per-bank descriptor
         * [base, top, ...] at the top of RAM, then relocates the POST stack to
         * `*(SP) + 0x8000` (ROM 0x4084a75c/0x4084a75e) where *(SP) is the
         * lowest bank's BASE.  On a real multi-SIMM IIvx that base is non-zero
         * and the stack lands safely; on our single contiguous DRAM bank based
         * at 0 it is 0, so SP becomes 0x8000 -- INSIDE the [0..0x80000] window
         * the very next per-bank scrubber (ROM 0x40848d90) fills, which
         * destroys the stack (return addr reads back the 0xB6DB6DB6 pattern)
         * -> rts to garbage -> NOP-sled.
         *
         * Fix: keep the ROM's stack-switch intact (it still reads *(SP)=base
         * and links the old high stack at 0x4a764, which the sizing's caller
         * relies on) but enlarge the `addal #imm,sp` guard from 0x8000 to
         * 0x1000000 so the relocated POST stack lands at 16 MB -- above every
         * per-bank scrub window, well within our >=32 MB DRAM, and low enough
         * to leave the high MemTop region for the a5-world/heap.  (Reading the
         * descriptor TOP instead was tried and broke the sizing loop.)
         *
         * Default-on (IIVX_NOSPFIX disables).  Combined with the VIA-timer
         * diagnostic bypass and the vector-preservation patch below, the boot
         * now clears the per-bank scrub, the VIA1-timer POST storm, and the
         * scrubbed-vector FC7 cascade (all Session 12), and runs the full
         * POST -> Egret cold-start/pseudo-commands -> cold-boot path without
         * any DOUBLE MMU FAULT; it currently idles in the ROM's cold-boot
         * MicroBug serial console (0x4084a0f0) awaiting the disk-boot
         * continuation (the Session 5-7 OS-startup frontier), so `-M maciivx`
         * boots cleanly (no abort) without env vars.
         */
        if (!getenv("IIVX_NOSPFIX") && bios_size > 0x4a764) {
            stl_be_p(ptr + 0x4a760, 0x01000000);   /* was 0x00008000 */
        }

        /*
         * VIA1-timer POST diagnostic bypass (SETUPTIMEK-class hack, cf.
         * maclc550.c / macclassicii.c).  Past the per-bank scrub the ROM's
         * POST sequencer runs a VIA1 T1/T2 interrupt-timing diagnostic (test
         * id 0x0C01, entry ROM 0x4084722e -> body 0x40847248): it programs
         * VIA1 T1/T2, installs a Level-1 handler at 0x408473bc, enables
         * interrupts, and counts CA1/T1/T2 IRQs (into d3/d4/d5) over a fixed
         * dbra delay loop, requiring exact ratios (d3==10, d4 in [128,208],
         * d5==1) to pass (d6==0).  Under icount the VIA1 T1/T2 interrupts
         * re-assert every instruction -> a Level-1 storm, the measured ratios
         * are garbage, d6 != 0, and the POST sequencer's failure path re-runs
         * an FC7 machine-ID probe (0x40803982) that bus-errors through the
         * now-scrubbed low-RAM exception vectors -> DOUBLE MMU FAULT.  This is
         * the same "dbra timer calibration is unmeasurable under TCG/icount"
         * wall the siblings hit; like them, short-circuit the diagnostic to
         * report success without the interrupt-driven measurement.  The test
         * is self-contained (its setup helper 0x40847086 saves VIA state on
         * the stack and restores it at the end), so replacing its entry with
         * `moveq #0,d6; jmp %fp@` cleanly returns "pass" and leaves the VIA in
         * its pre-test state.  ROM checksum is repaired below.
         */
        if (bios_size > 0x4722e + 4) {
            stw_be_p(ptr + 0x4722e, 0x7c00);   /* moveq #0,%d6 */
            stw_be_p(ptr + 0x47230, 0x4ed6);   /* jmp %fp@      */
        }

        /*
         * Preserve the low exception-vector page across the POST memory
         * scrub (coordinator step 3: keep the bus-/address-error vectors
         * sane so a stray FC7 machine-ID probe is non-fatal).  The ROM's
         * write-verify fill/scrub routine (0x40846950, called by both the
         * [0..0xC00000] integrity test at 0x408466b4 and the per-bank
         * scrubber at 0x40848dc2) fills [a0..a1] with the 0x6DB6DB6D POST
         * pattern.  For bank 0 a0==0, so it overwrites the 68k vector table
         * at 0-0x3FF -- including the bus-error (0x8) and address-error
         * (0xC) vectors the ROM installed at 0x408468ae -- and never
         * restores them.  On real multi-bank hardware the lowest bank base
         * is non-zero, so the vectors survive; on our single bank based at 0
         * the post-scrub FC7 probe (0x40803982) then bus-errors through the
         * scrubbed vector -> the bus-error handler jumps to the 0x6DB6DB6D
         * pattern -> an infinite Access-Fault cascade -> DOUBLE MMU FAULT.
         *
         * Fix: clamp the routine's start address a0 up to 0x2000 so the low
         * 8 KB (vector table) is never scrubbed.  The routine keys its fill,
         * eor-scramble and verify passes all off a0 (it re-derives a2 from a0
         * at 0x408469a8), so clamping a0 once at entry is self-consistent and
         * the verify still passes (the skipped low page simply isn't tested;
         * our emulated DRAM never faults there).  A0>=0x2000 callers are
         * unaffected (the clamp is a no-op).  Implemented as a short thunk in
         * free ROM padding at 0x4084ac3c, branched to from the routine entry
         * (replacing the `moveal %a0,%a2 / subaw #120,%a1 / bras` prologue).
         * ROM checksum is repaired below.
         */
        if (bios_size > 0x4ac3c + 0x18) {
            /* entry 0x40846956: bra.w thunk; nop; nop */
            stw_be_p(ptr + 0x46956, 0x6000);
            stw_be_p(ptr + 0x46958, 0x42e4);   /* -> 0x4084ac3c */
            stl_be_p(ptr + 0x4695a, 0x4e714e71);
            /* thunk @ 0x4084ac3c */
            stw_be_p(ptr + 0x4ac3c, 0xb1fc);   /* cmpal #0x2000,%a0 */
            stl_be_p(ptr + 0x4ac3e, 0x00002000);
            stw_be_p(ptr + 0x4ac42, 0x6406);   /* bcc.s +6 (a0>=0x2000) */
            stw_be_p(ptr + 0x4ac44, 0x207c);   /* moveal #0x2000,%a0 */
            stl_be_p(ptr + 0x4ac46, 0x00002000);
            stw_be_p(ptr + 0x4ac4a, 0x2448);   /* moveal %a0,%a2 */
            stw_be_p(ptr + 0x4ac4c, 0x92fc);   /* subaw #120,%a1 */
            stw_be_p(ptr + 0x4ac4e, 0x0078);
            stw_be_p(ptr + 0x4ac50, 0x6000);   /* bra.w 0x4084697e */
            stw_be_p(ptr + 0x4ac52, 0xbd2c);
        }

        /*
         * Decoder-record selection: make the ROM identify this board as the
         * IIvx's own Egret machine (record rom+0x3b02, kind 0x0c05: decoder-5
         * RBV-compatible + Egret) instead of the IIci (rom+0x35b8, kind
         * 0x0505, discrete RTC), which it otherwise defaults to.
         *
         * Why a ROM patch and not a hardware strap: the identify walk
         * (ROM 0x40802f78) picks a record by decoder byte rec+19 == d2.b
         * (==0x05) AND (d1 & rec+32) == rec+36.  The machine-ID word d1 is
         * built by the identify probes as 0xefff0000 -- top byte 0xef, then
         * 0xff, then a <<16 that ZEROES d1's byte 1.  The Egret 0c05 record's
         * criterion needs (d1 & 0x01265600) == 0x00001600, i.e. d1 byte 1 ==
         * 0x16 -- unreachable because byte 1 is always 0 after that <<16, for
         * ANY VIA1 PA strap (verified: sweeping PA leaves d1 == 0xefff0000).
         * The real distinction is VASP's decoder-kind identification
         * registers, which Apple never published, so we cannot reproduce the
         * natural d1/d2 that selects 0c05.  Instead -- exactly as this file
         * already patches the relocation slot and repairs the checksum, and
         * as macclassicii.c patches its decoder records -- we edit the
         * in-memory decoder table so the walk selects 0c05: disable the
         * IIci-family 05-decoder records (tried first) by making their
         * (d1 & mask)==match impossible, and make 0c05 always-match.
         *
         * IIVX_FORCE=<hex rom offset> overrides the target (debug only);
         * IIVX_FORCE=off disables the patch (reverts to IIci identification).
         */
        {
            const char *fe = getenv("IIVX_FORCE");
            unsigned long tgt = 0x3b02;         /* 0c05 Egret record */

            if (fe && !strcmp(fe, "off")) {
                tgt = 0;
            } else if (fe) {
                tgt = strtoul(fe, NULL, 16);
            }
            if (tgt) {
                static const int comp[] = {
                    0x35b8, 0x35f8, 0x36f8, 0x3638, 0x3678
                };
                int i;
                for (i = 0; i < (int)ARRAY_SIZE(comp); i++) {
                    stl_be_p(ptr + comp[i] + 36, 0xffffffff);
                }
                stl_be_p(ptr + tgt + 32, 0);
                stl_be_p(ptr + tgt + 36, 0);
                ptr[tgt + 19] = 0x05;
            }
        }

        /*
         * EXPERIMENT (env-gated IIVX_SKIPMEMTEST): the ROM's power-on
         * write-verify memory test (fill routine 0x40846950, called from the
         * loop at 0x408466b4 over [0..0xC00000]) DESTRUCTIVELY fills that
         * whole window with a pattern and does not restore it, clobbering the
         * ROM's own low-memory globals; the boot then jumps through a
         * corrupted vector into unbacked/zeroed RAM (NOP-sled).  Skip the
         * fill call (NOP the `jmp 0x40846950` at 0x466b4) so d6 stays 0 and
         * the boot continues without corrupting low memory.
         */
        if (getenv("IIVX_SKIPMEMTEST") && bios_size > 0x466b4 + 4) {
            stw_be_p(ptr + 0x466b4, 0x4e71);   /* nop */
            stw_be_p(ptr + 0x466b6, 0x4e71);   /* nop */
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
         * Pre-load the writable 24-bit ROM window (0x800000-0x8FFFFF) with
         * the fully-patched ROM image, so 24-bit-mode fetches/reads there see
         * real ROM code/data while the 32-bit power-on memory test can still
         * write-verify it as DRAM (see the rom24_ram mapping above).
         */
        if (machine->ram_size < 0x00C00000) {
            void *r24 = memory_region_get_ram_ptr(&m->rom24_ram);
            memcpy(r24, ptr, MIN(bios_size, (int)MACIIVX_ROM_SIZE));
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
    mc->default_ram_size = 32 * MiB;
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
