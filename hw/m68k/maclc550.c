/*
 * QEMU Macintosh LC 550 / Color Classic II / Performa 550 hardware
 * system emulator
 *
 * 68030-33MHz "Sonora"-generation machine: RBV-style VIA2/interrupt
 * register block (same IFR/IER semantics as the IIci/IIsi/Classic II),
 * Egret ADB/RTC MCU with the byte-handshake pseudo-command extension
 * (GET_PRAM/SET_PRAM/READ_MCU/WRITE_MCU/GET_TIME/SET_TIME), EASC
 * sound, SWIM floppy, NCR5380 SCSI, Z8530 SCC, and onboard color video
 * -- the "CSC"/Spur controller, a Valkyrie-family framebuffer (same
 * family as the LC 475's onboard video) with a dedicated VRAM
 * aperture and CLUT, unlike the RBV-video compact Macs that steal
 * scanout from main RAM.  Also unlike the older 24-bit-only 68030
 * boards (IIsi, Classic II), this is a 32-bit-clean machine with a
 * dedicated machine-ID register at 0x5FFFFFFC (same scheme as q800.c
 * and lc475.c) instead of a VIA1 port-A decoder-kind strap dance.
 *
 * Derived from macclassicii.c (RBV interrupt glue, RTC, SCC/ASC/SWIM/
 * SCSI layout and the Egret pseudo-command transport, both proven
 * against MacOS 7.5.3 booting to the Finder) with the video model
 * replaced by lc475.c's Valkyrie/CSC framebuffer (dedicated VRAM,
 * GDevice-tracked geometry, CLUT) and the machine-identification
 * scheme replaced by lc475.c's machine-ID register, grown empirically
 * against the Color Classic II / LC 550 / Performa 275,550,560 /
 * Macintosh TV shared ROM (checksum EDE66CBD).  The LC 550, Color
 * Classic II and Performa 550 machines in this file share the same
 * ROM and hardware, differing only in the machine-ID register value
 * and (for the Color Classic II) the fixed-monitor sense code.
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
#include "ui/input.h"
#include "hw/display/framebuffer.h"
#include "system/rtc.h"
#include "system/qtest.h"
#include "system/reset.h"

#define MACLC550_ROM_ADDR      0x40800000
#define MACLC550_ROM_SIZE      0x00100000
#define MACLC550_ROM_FILENAME  "maclc550.rom"

#define IO_BASE               0x50000000
/*
 * I/O decode granularity: 0x40000, the RBV/kind-5 spacing (as on
 * maciisi.c/macclassicii.c) -- this machine identifies itself through
 * the dedicated machine-ID register below, not a VIA1 mirror-spacing
 * decoder-kind probe, so the exact slice size is not load-bearing the
 * way it was for the Classic II's V8/Eagle identification; kept at
 * the RBV-family value since the RBV-compatible register block below
 * (offset 0x26000) matches that family's layout.
 */
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

#define MAC_CLOCK             3686418

/* Size of whole RAM area (unbacked reads return 0 for RAM sizing) */
#define RAM_SIZE              0x40000000

/*
 * Onboard video VRAM base.  The Sonora "Eric" ROM does NOT use lc475.c's
 * Valkyrie placement (0xf9000000): it probes and memory-tests its VRAM at
 * physical 0x60B00000 (traced -- the kind-7 video capability init writes
 * the '256K'/'768?' size signatures and a 0x6DB6DB6D walking pattern
 * there, and stores 0x60B00000 as the video device globals base).  A
 * wrong base left that whole region unmapped, so the bit-18 video
 * capability's dereference faulted and the ROM bailed the machine-config
 * POST to the serial diagnostic console (via 0x40847042 -> 0x4084a6f6).
 */
#define MACLC550_VRAM_BASE     0x60b00000
#define MACLC550_VRAM_SIZE     0x00100000    /* 1 MiB */

/*
 * Machine ID register at 0x5FFFFFFC (same scheme as q800.c/lc475.c):
 * read-only, upper word must be 0xA55A.  LC 550 = 0xA55A0101 (MAME's
 * maclc3.cpp, same Sonora-generation ASIC and the same EDE66CBD ROM
 * checksum, gives this value for -M maclc550).  Performa 550/560
 * share the LC 550's Gestalt machine ID on real hardware (Linux
 * bootinfo-mac.h: MAC_MODEL_P550 covers all three), so "performa550"
 * is registered as a plain alias of maclc550 rather than a separate
 * ID value.  The Color Classic II's exact register value was not
 * found in any available reference (MAME does not emulate it from
 * this ROM); it is a distinct Gestalt ID from the LC 550 on real
 * hardware (MAC_MODEL_CCLII), so colorclassicii uses a same-shaped
 * but distinct placeholder ID (0xA55A0181) plus the compact built-in
 * monitor's fixed sense code -- flagged as unverified in
 * CCLASSIC-NOTES.md; if the ROM rejects it, the fallback is to reuse
 * the LC 550 ID verbatim and rely on the monitor-sense code alone to
 * pick the compact-case defaults.
 */
#define MACLC550_MACHINE_ID    0xa55a0101
#define MACCCLASSICII_MACHINE_ID 0xa55a0181

/*
 * Interrupt glue: one input per 680x0 interrupt level (input n asserts
 * IPL n+1, autovectored).
 */
#define TYPE_MACLC550_GLUE "maclc550-glue"
OBJECT_DECLARE_SIMPLE_TYPE(MacLc550GlueState, MACLC550_GLUE)

struct MacLc550GlueState {
    SysBusDevice parent_obj;
    M68kCPU *cpu;
    uint8_t ipr;
};

#define MACLC550_GLUE_VIA1     0       /* level 1 */
#define MACLC550_GLUE_RBV      1       /* level 2 */
#define MACLC550_GLUE_SCC      3       /* level 4 */
#define MACLC550_GLUE_NMI      6      /* level 7 */

/*
 * True while the 680x0 exception vector table has not yet been installed.
 *
 * The OS bring-up path re-enters early ROM (0x40800230) after the Egret
 * ADB setup: it clears low RAM -- zeroing the autovector table at
 * VBR+0x64 that the power-on POST had set up -- and only re-installs the
 * handlers a few instructions AFTER it unmasks interrupts at 0x4080023a
 * (`movew #8192,%sr`).  Meanwhile the Time Manager has already armed the
 * VIA1 one-shot T2 (loaded ~0xfffb ~= 84ms at 0x4080b12e); under our
 * virtual-time budget that one-shot expires during the long polled Egret
 * shift-register spin (0x408d15f6) that runs in between, latching VIA1
 * IFR bit5.  Delivering that pending level-1 autovector across the
 * unmask->install window would dispatch through the still-zeroed
 * *(0x64) into garbage -> bus error -> the equally-zeroed bus-error
 * vector *(0x8) -> double fault.
 *
 * Real hardware never dispatches an interrupt into an uninstalled vector
 * table; model that directly.  The bus-error vector (VBR+0x08) is a
 * reliable "vectors present" witness: it is 0 while the table is
 * uninitialised and a real ROM handler address once the OS has set the
 * table up.  While it is 0 we hold every autovectored source off the CPU
 * (the sources stay latched in their IFRs); the next glue re-evaluation
 * after the table is installed -- guaranteed within 16ms by the 60Hz CA1
 * tick -- delivers them cleanly.
 */
static bool maclc550_vectors_uninstalled(MacLc550GlueState *s)
{
    uint32_t vbr = s->cpu->env.vbr;

    return ldl_be_phys(&address_space_memory, vbr + 8) == 0;
}

static void maclc550_glue_set_irq(void *opaque, int irq, int level)
{
    MacLc550GlueState *s = opaque;
    int i;

    if (level) {
        s->ipr |= 1 << irq;
    } else {
        s->ipr &= ~(1 << irq);
    }

    if (!maclc550_vectors_uninstalled(s)) {
        for (i = 7; i >= 0; i--) {
            if ((s->ipr >> i) & 1) {
                m68k_set_irq_level(s->cpu, i + 1, i + 25);
                return;
            }
        }
    }
    m68k_set_irq_level(s->cpu, 0, 0);
}

static void maclc550_glue_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    MacLc550GlueState *s = MACLC550_GLUE(dev);

    qdev_init_gpio_in(dev, maclc550_glue_set_irq, 8);
    s->ipr = 0;
}

static const Property maclc550_glue_properties[] = {
    DEFINE_PROP_LINK("cpu", MacLc550GlueState, cpu, TYPE_M68K_CPU, M68kCPU *),
};

static void maclc550_glue_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, maclc550_glue_properties);
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

#define TYPE_MOS6522_MACLC550 "mos6522-maclc550"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522MacLc550State, MOS6522_MACLC550)

struct MacLc550MachineState;

struct MOS6522MacLc550State {
    MOS6522State parent_obj;

    struct MacLc550MachineState *machine;
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

static void via1_rtc_update(MOS6522MacLc550State *v1s)
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

    qemu_log_mask(LOG_UNIMP, "maclc550 rtc: byte 0x%02x (cmd=%02x alt=%02x)\n",
                  v1s->data_out, v1s->cmd, v1s->alt);

    /* first byte: it's a command */
    if (v1s->cmd == REG_EMPTY) {
        cmd = via1_rtc_compact_cmd(v1s->data_out);

        if (cmd == REG_INVALID) {
            qemu_log_mask(LOG_UNIMP, "maclc550 rtc: invalid cmd 0x%02x\n",
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

static void maclc550_egret_session_update(MOS6522MacLc550State *v1s);
static void maclc550_egret_sr_written(MOS6522MacLc550State *v1s);
static void maclc550_egret_sr_read(MOS6522MacLc550State *v1s);
static void maclc550_egret_acr_changed(MOS6522MacLc550State *v1s);
static bool maclc550_egret_pseudo_try_process(MOS6522MacLc550State *v1s);

static void maclc550_via1_portA_write(MOS6522State *s)
{
}

static void maclc550_via1_portB_write(MOS6522State *s)
{
    MOS6522MacLc550State *v1s = MOS6522_MACLC550(s);

    if (v1s->machine) {
        maclc550_egret_session_update(v1s);
    }
}

static void mos6522_maclc550_init(Object *obj)
{
    MOS6522MacLc550State *v1s = MOS6522_MACLC550(obj);

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;

    /* the ADB bus hangs off the Egret; the VIA1 device stands in for it */
    qbus_init(&v1s->adb_bus, sizeof(v1s->adb_bus), TYPE_ADB_BUS,
              DEVICE(obj), "adb.0");
}

static void mos6522_maclc550_reset_hold(Object *obj, ResetType type)
{
    MOS6522MacLc550State *v1s = MOS6522_MACLC550(obj);
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

static const Property mos6522_maclc550_properties[] = {
    DEFINE_PROP_UINT8("pins-a", MOS6522MacLc550State, pins_a, 0xff),
    DEFINE_PROP_UINT8("pins-b", MOS6522MacLc550State, pins_b, 0xff),
};

static void mos6522_maclc550_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    MOS6522DeviceClass *mdc = MOS6522_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_props(dc, mos6522_maclc550_properties);
    mdc->portA_write = maclc550_via1_portA_write;
    mdc->portB_write = maclc550_via1_portB_write;
    resettable_class_set_parent_phases(rc, NULL, mos6522_maclc550_reset_hold,
                                       NULL, &mdc->parent_phases);
}

/*
 * Onboard video framebuffer display device (Valkyrie/CSC-class,
 * ported from lc475.c): dedicated VRAM, multi-depth draw paths and
 * geometry read every frame from the guest's own idea of the screen
 * (the MainDevice GDevice's PixMap), rather than reverse-engineered
 * mode registers.  See maclc550_fb_track_mode below.
 */

#define TYPE_MACLC550_FB "maclc550-fb"
OBJECT_DECLARE_SIMPLE_TYPE(MacLc550FbState, MACLC550_FB)

struct MacLc550FbState {
    SysBusDevice parent_obj;

    MemoryRegion vram;
    MemoryRegionSection fbsection;
    QemuConsole *con;
    int invalidate;

    int depth;                  /* 1, 2, 4, 8 or 16 bpp */
    int width;
    int height;
    int stride;
    uint32_t palette[256];
    uint32_t fb_offset;
};

static int maclc550_fb_stride(MacLc550FbState *s)
{
    return s->stride;
}

static void maclc550_fb_draw_line(void *opaque, uint8_t *d, const uint8_t *src,
                                  int width, int pitch)
{
    MacLc550FbState *s = opaque;
    uint32_t *buf = (uint32_t *)d;
    int i, b;

    switch (s->depth) {
    case 1:
        for (i = 0; i < s->width / 8; i++) {
            uint8_t v = src[i];
            for (b = 0; b < 8; b++) {
                buf[i * 8 + b] = (v & 0x80) ? 0xFF000000 : 0xFFFFFFFF;
                v <<= 1;
            }
        }
        break;
    case 2:
        for (i = 0; i < s->width / 4; i++) {
            uint8_t v = src[i];
            for (b = 0; b < 4; b++) {
                buf[i * 4 + b] = s->palette[(v >> 6) & 3];
                v <<= 2;
            }
        }
        break;
    case 4:
        for (i = 0; i < s->width / 2; i++) {
            uint8_t v = src[i];
            buf[i * 2] = s->palette[(v >> 4) & 0xf];
            buf[i * 2 + 1] = s->palette[v & 0xf];
        }
        break;
    case 8:
        for (i = 0; i < s->width; i++) {
            buf[i] = s->palette[src[i]];
        }
        break;
    case 16:
        /* big-endian xRGB1555 */
        for (i = 0; i < s->width; i++) {
            uint16_t v = (src[i * 2] << 8) | src[i * 2 + 1];
            uint8_t r = (v >> 10) & 0x1f;
            uint8_t g = (v >> 5) & 0x1f;
            uint8_t bl = v & 0x1f;

            buf[i] = 0xFF000000 | (r << 19) | (r >> 2 << 16)
                     | (g << 11) | (g >> 2 << 8) | (bl << 3) | (bl >> 2);
        }
        break;
    }
}

/*
 * Track the guest's idea of the screen: QuickDraw publishes the main
 * device's PixMap (base address, rowbytes, bounds, depth) through the
 * MainDevice GDevice handle at lowmem 0x8A4 (cf. lc475.c).
 */
static void maclc550_fb_track_mode(MacLc550FbState *s)
{
    hwaddr gdh, gd, pmh, pm, base;
    uint32_t rb, top, left, bottom, right, pixsz;
    int width, height;

    gdh = ldl_be_phys(&address_space_memory, 0x8a4) & 0xffffff;
    if (gdh < 0x1000 || (gdh & 1)) {
        return;
    }
    gd = ldl_be_phys(&address_space_memory, gdh) & 0xffffff;
    if (gd < 0x1000 || (gd & 1)) {
        return;
    }
    pmh = ldl_be_phys(&address_space_memory, gd + 0x16) & 0xffffff;
    if (pmh < 0x1000 || (pmh & 1)) {
        return;
    }
    pm = ldl_be_phys(&address_space_memory, pmh) & 0xffffff;
    if (pm < 0x1000 || (pm & 1)) {
        return;
    }

    base = ldl_be_phys(&address_space_memory, pm);
    rb = lduw_be_phys(&address_space_memory, pm + 4) & 0x3fff;
    top = lduw_be_phys(&address_space_memory, pm + 6);
    left = lduw_be_phys(&address_space_memory, pm + 8);
    bottom = lduw_be_phys(&address_space_memory, pm + 10);
    right = lduw_be_phys(&address_space_memory, pm + 12);
    pixsz = lduw_be_phys(&address_space_memory, pm + 32);

    if ((base >> 20) != (MACLC550_VRAM_BASE >> 20)) {
        return;                 /* not pointing at VRAM (yet) */
    }
    width = (int16_t)right - (int16_t)left;
    height = (int16_t)bottom - (int16_t)top;
    if (width <= 0 || width > 1152 || height <= 0 || height > 870) {
        return;
    }
    if (pixsz != 1 && pixsz != 2 && pixsz != 4 && pixsz != 8 && pixsz != 16) {
        return;
    }
    if (rb < (uint32_t)width * pixsz / 8) {
        return;
    }
    base &= MACLC550_VRAM_SIZE - 1;
    if (base + (uint64_t)rb * height > MACLC550_VRAM_SIZE) {
        return;
    }

    if (s->width != width || s->height != height || s->depth != pixsz ||
        s->stride != (int)rb || s->fb_offset != base) {
        s->width = width;
        s->height = height;
        s->depth = pixsz;
        s->stride = rb;
        s->fb_offset = base;
        s->invalidate = 1;
        qemu_console_resize(s->con, width, height);
    }
}

static bool maclc550_fb_update(void *opaque)
{
    MacLc550FbState *s = MACLC550_FB(opaque);
    DisplaySurface *surface;
    int first = 0, last = 0;

    maclc550_fb_track_mode(s);
    surface = qemu_console_surface(s->con);

    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection, &s->vram,
                                          s->fb_offset,
                                          s->height,
                                          maclc550_fb_stride(s));
        s->invalidate = 0;
    }

    framebuffer_update_display(surface, &s->fbsection,
                               s->width, s->height,
                               maclc550_fb_stride(s), s->width * 4,
                               0, 1, maclc550_fb_draw_line, s,
                               &first, &last);
    qemu_console_update(s->con, 0, 0, s->width, s->height);
    return true;
}

static void maclc550_fb_invalidate(void *opaque)
{
    MacLc550FbState *s = MACLC550_FB(opaque);

    s->invalidate = 1;
}

static const GraphicHwOps maclc550_fb_ops = {
    .invalidate = maclc550_fb_invalidate,
    .gfx_update = maclc550_fb_update,
};

static void maclc550_fb_realize(DeviceState *dev, Error **errp)
{
    MacLc550FbState *s = MACLC550_FB(dev);
    int i;

    memory_region_init_ram(&s->vram, OBJECT(dev), "maclc550.vram",
                           MACLC550_VRAM_SIZE, &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->vram);

    /* defaults until QuickDraw publishes the real geometry */
    s->depth = 8;
    s->width = 640;
    s->height = 480;
    s->stride = 640;
    s->fb_offset = 0x1000;
    for (i = 0; i < 256; i++) {
        uint8_t v = 255 - i;
        s->palette[i] = 0xFF000000 | (v << 16) | (v << 8) | v;
    }
    s->invalidate = 1;
    s->con = qemu_graphic_console_create(dev, 0, &maclc550_fb_ops, s);
    qemu_console_resize(s->con, s->width, s->height);
}

static void maclc550_fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->realize = maclc550_fb_realize;
}

/*
 * Valkyrie/CSC control registers (ported from lc475.c): CLUT
 * address/data and a VBL status bit, plus a generic logging bank for
 * everything else the ROM's video driver pokes.
 */

typedef struct {
    const char *name;
    uint8_t *regs;
    uint32_t size;
    int log_budget;
} MacLc550RegBank;

static uint64_t maclc550_regbank_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    MacLc550RegBank *b = opaque;
    uint64_t val = 0;
    int i;

    for (i = 0; i < size; i++) {
        val = (val << 8) | b->regs[(addr + i) & (b->size - 1)];
    }
    if (b->log_budget > 0) {
        b->log_budget--;
        qemu_log_mask(LOG_UNIMP, "maclc550 %s: read  +0x%04x (%d) -> 0x%08"
                      PRIx64 "\n", b->name, (unsigned)addr, size, val);
    }
    return val;
}

static void maclc550_regbank_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    MacLc550RegBank *b = opaque;
    int i;

    if (b->log_budget > 0) {
        b->log_budget--;
        qemu_log_mask(LOG_UNIMP, "maclc550 %s: write +0x%04x (%d) <- 0x%08"
                      PRIx64 "\n", b->name, (unsigned)addr, size, val);
    }
    for (i = size - 1; i >= 0; i--) {
        b->regs[(addr + i) & (b->size - 1)] = val & 0xff;
        val >>= 8;
    }
}

/* machine */

struct MacLc550MachineState {
    MachineState parent_obj;

    M68kCPU cpu;
    MacLc550GlueState glue;
    MOS6522MacLc550State via1;
    ESCCState escc;
    OrIRQState escc_orgate;
    ASCState asc;
    Swim swim;
    NCR5380State scsi;

    MacLc550FbState fb;
    MemoryRegion rom;
    MemoryRegion rom_alias;
    MemoryRegion machine_id;
    MemoryRegion ramio;
    MemoryRegion ramio_a31;
    MemoryRegion macio;
    MemoryRegion macio_alias;
    MemoryRegion via1mem;
    MemoryRegion rbv_via2mem;
    MemoryRegion escc_mirror;
    MemoryRegion swim_mirror;
    MemoryRegion rbvmem;
    MemoryRegion scsi_pdma;
    MemoryRegion scsi_hsk;
    MemoryRegion iotrace;
    MemoryRegion vdacmem;
    MemoryRegion sonoraidmem;
    MemoryRegion arielmem;
    MemoryRegion valkyrie_mem;
    MemoryRegion sonora_probe;
    uint8_t valkyrie_regs[0x4000];
    MacLc550RegBank *valkyrie_bank;
    uint8_t clut_addr;
    uint8_t clut_phase;
    uint8_t clut_rgb[3];
    MemoryRegion vram_aliases[7];

    uint8_t rbv_regs[0x100];
    uint8_t rbv_ifr;
    uint8_t rbv_sifr;
    uint8_t rbv_ier;
    uint8_t rbv_sier;
    uint8_t rbv_via2_regs[16];
    uint8_t vdac_regs[0x40];
    uint8_t sonoraid_regs[0x40];
    uint8_t ariel_regs[0x40];

    /* Egret ADB/system MCU on the VIA1 shift register */
    QEMUTimer *egret_timer;
    uint8_t egret_cmd[16];
    int egret_cmd_len;
    /*
     * Reply staging: header (4) plus up to a full 256-byte MCU/XPRAM
     * window -- READ_MCU replies are STREAMED (the Egret keeps handing
     * out successive bytes and the HOST decides when it has enough by
     * closing /SYS_SESSION; the wire command carries no length), so a
     * worst-case whole-window read must fit.
     */
    uint8_t egret_resp[4 + 256];
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

    /*
     * Egret "pseudo-command" transactions (packet type byte 0x01;
     * self-test/autopoll/PRAM/RTC control -- cf. the Cuda/Egret
     * pseudo-command family in MAME's apple/egret.cpp).  This ROM
     * drives them with a byte-level PB4(/VIA_FULL)/PB5(/SYS_SESSION)
     * handshake and /SYS_SESSION *released* throughout -- a completely
     * different framing from the formal-session ADB commands above
     * (egret_cmd/egret_session), collected/delivered independently.
     */
    /* command can carry a whole-window WRITE_MCU: [01 08 aH aL] + 256 */
    uint8_t egret_pseudo_cmd[4 + 256];
    uint8_t egret_mcu_mem[256];
    int egret_pseudo_cmd_len;
    bool egret_pseudo_active;
    /*
     * Set when the last reply byte has been delivered but the ROM's own
     * teardown writes (clearing /SYS_SESSION then /VIA_FULL, ROM
     * 0x4084a646/0x4084a650) haven't happened yet -- keeps suppressing
     * the formal-session /SYS_SESSION-edge detector (see
     * maclc550_egret_session_update) through that short window, or
     * the first of those two writes (a /SYS_SESSION-released edge with
     * /VIA_FULL still high) is indistinguishable from a real session
     * open and would otherwise be misread as one.
     */
    bool egret_pseudo_closing;
    /*
     * Set when the in-flight byte-handshake pseudo reply was for a command
     * that arrived over the FORMAL-SESSION framing (see
     * maclc550_egret_session_pseudo_reply): those drivers want a final
     * completion shift-interrupt once the whole reply is drained.
     */
    bool egret_session_pseudo;

    /*
     * Egret power-on cold-start line handshake (this 1MB "Sonora" ROM's
     * pre-driver Egret sync at 0x408d1ae6): before any formal session or
     * byte-handshake pseudo-command, the ROM electrically handshakes with
     * the Egret -- it asserts /VIA_FULL (PB4 low) and busy-waits for the
     * Egret to assert /XCVR (PB3 low), then deasserts /VIA_FULL and waits
     * for /XCVR to rise, clocking a shift-register interrupt each toggle.
     * On real hardware /XCVR tracks /VIA_FULL through the Egret; if it
     * never does, the ROM times out (0x408d1c68) and diverts into its
     * serial diagnostic monitor (0x408b989a) instead of booting.  Active
     * from reset until the first higher-level Egret transaction (formal
     * session or recognised pseudo-command) takes over.
     */
    bool egret_coldstart;
    bool egret_coldstart_session;   /* the open session is a cold-start one */

    /*
     * Egret autopoll crutch for the boot-time mouse rendezvous.  After
     * ADBReInit + device registration and the cursor setup, the ROM's
     * startup dispatch loop at 0x40802a38 spins on lowmem 0x172 (set 0x80
     * at 0x4080067a) until the ADB MOUSE (addr 3) reports through its
     * completion (0x408b67c4, guard a2@4 == ExpandMem[480]->[4] == the
     * mouse DCB 0x5840), which clears 0x172.  On real hardware the Egret
     * autopolls the bus and the mouse's first Talk-R0 reply satisfies
     * this; our Egret models no autopoll, so the mouse is never polled and
     * the boot hangs.  This timer runs the same rendezvous crutch as
     * hw/m68k/macse30.c: while parked on the 0x172 spin, hold a synthetic
     * mouse button DOWN (adb_mouse_force_report re-announces it each poll,
     * WITHOUT injecting cursor movement, which the ROM's post-rendezvous
     * cursor-position check must not see) and deliver the polled mouse
     * register-0 data as an unsolicited Egret autopoll packet, then release
     * the button and go quiet once 0x172 clears.
     */
    QEMUTimer *autopoll_timer;
    bool autopoll_armed;        /* boot-time 0x172 crutch active */
    bool autopoll_click;        /* synthetic mouse button held down */
    bool autopoll_saw172;       /* have observed 0x172 with bit7 set */
    bool egret_unsolicited;     /* the in-flight reply is an unsolicited
                                 * autopoll packet, not a host reply */

    /* VIA1 CA1 60Hz tick and CA2 one-second interrupts */
    QEMUTimer *sixty_hz_timer;
    QEMUTimer *vbl_off_timer;
    QEMUTimer *one_second_timer;

    /* SETUPTIMEK calibration hack (see mac_via.c) */
    int timer_hack_state;
};

struct MacLc550MachineClass {
    MachineClass parent_class;

    uint32_t machine_id;
    uint16_t monitor_sense;      /* raw Apple monitor-sense code */
};

#define TYPE_MACLC550_MACHINE MACHINE_TYPE_NAME("maclc550-common")
OBJECT_DECLARE_TYPE(MacLc550MachineState, MacLc550MachineClass, MACLC550_MACHINE)

typedef struct MacLc550MachineData {
    const char *desc;
    uint32_t machine_id;
    uint16_t monitor_sense;
} MacLc550MachineData;

/* machine ID register: read-only, per-class value, writes ignored */

static uint64_t machine_id_read(void *opaque, hwaddr addr, unsigned size)
{
    MacLc550MachineState *m = opaque;
    MacLc550MachineClass *mc = MACLC550_MACHINE_GET_CLASS(m);

    /*
     * Any access width works on the real register; the ROM uses long
     * reads early on (expects 0xA55Axxxx) and later narrower reads, so
     * narrow reads return the LOW part of the ID.
     */
    return mc->machine_id & ((size == 4) ? 0xffffffff :
                             (size == 2) ? 0xffff : 0xff);
}

static void machine_id_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
}

static const MemoryRegionOps machine_id_ops = {
    .read = machine_id_read,
    .write = machine_id_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* Catch-all logging stub for unmodelled registers in the 0x60000000
 * Sonora device window (the onboard VRAM below overlaps it). */
static uint64_t sonora_probe_read(void *opaque, hwaddr addr, unsigned size)
{
    static int count;
    if (count < 400) {
        count++;
        qemu_log_mask(LOG_UNIMP, "maclc550 sonora-probe: read  0x%08x (%d)"
                      " pc=0x%08x\n", (unsigned)(0x60000000 + addr), size,
                      current_cpu ? M68K_CPU(current_cpu)->env.pc : 0);
    }
    return 0;
}
static void sonora_probe_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    static int count;
    if (count < 400) {
        count++;
        qemu_log_mask(LOG_UNIMP, "maclc550 sonora-probe: write 0x%08x (%d)"
                      " <- 0x%08" PRIx64 " pc=0x%08x\n",
                      (unsigned)(0x60000000 + addr), size, val,
                      current_cpu ? M68K_CPU(current_cpu)->env.pc : 0);
    }
}
static const MemoryRegionOps sonora_probe_ops = {
    .read = sonora_probe_read,
    .write = sonora_probe_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static uint32_t maclc550_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

static uint64_t ramio_read(void *opaque, hwaddr addr, unsigned size)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "maclc550 ram-hole: read  0x%08x (%d) pc=0x%08x\n",
                      (unsigned)addr, size, maclc550_trace_pc());
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
                      "maclc550 ram-hole: write 0x%08x (%d) <- 0x%08" PRIx64
                      " pc=0x%08x\n",
                      (unsigned)addr, size, val, maclc550_trace_pc());
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

/*
 * A31-half accesses: only the low 24 bits reach the decoder (24-bit
 * tagged master pointers must alias onto RAM).  Although this is a
 * 32-bit-clean Sonora machine, classic MacOS boots the ROM in 24-bit
 * mode (see the XPRAM 0x8a comment) and dereferences 24-bit-tagged
 * Handle master pointers whose top byte carries the Handle-state flags
 * (lock/purge/resource -- e.g. 0xA0010730 = locked+resource Handle at
 * real address 0x00010730, seen in the post-rendezvous startup dispatch
 * loop's _PtInRgn at ROM 0x40802a76).  Our PMMU is a no-op, so forward
 * the whole A31 half to the low 16 MB.  Low priority (-2): the Valkyrie
 * window at 0xf9800000 keeps its own higher-priority decode.
 */
static MemTxResult maclc550_a31_read(void *opaque, hwaddr addr, uint64_t *data,
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

static MemTxResult maclc550_a31_write(void *opaque, hwaddr addr, uint64_t value,
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

static const MemoryRegionOps maclc550_a31_ops = {
    .read_with_attrs = maclc550_a31_read,
    .write_with_attrs = maclc550_a31_write,
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

static MemTxResult maclc550_escc_mirror_read(void *opaque, hwaddr addr,
                                            uint64_t *data, unsigned size,
                                            MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_read(opaque, addr, data, size, attrs);
}

static MemTxResult maclc550_escc_mirror_write(void *opaque, hwaddr addr,
                                             uint64_t val, unsigned size,
                                             MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_write(opaque, addr, val, size, attrs);
}

static const MemoryRegionOps maclc550_escc_mirror_ops = {
    .read_with_attrs = maclc550_escc_mirror_read,
    .write_with_attrs = maclc550_escc_mirror_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t maclc550_via1_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS6522MacLc550State *v1s = opaque;
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
        MacLc550MachineState *m = v1s->machine;
        bool xcvr = m->egret_xcvr_asserted && !m->egret_no_resp &&
                    m->egret_resp_len > 0;

        /*
         * Cold-start line handshake: /XCVR (PB3) tracks /VIA_FULL (PB4)
         * -- the Egret asserts /XCVR (PB3 low) while the host holds
         * /VIA_FULL asserted (PB4 low) and releases it when the host
         * does.  See the egret_coldstart comment.  Gated so it never
         * fires once a formal session, pseudo-command reply, or
         * unsolicited ADB packet is in play (all of which drive /XCVR
         * through the branches above/below).
         */
        if (m->egret_coldstart && !m->egret_session &&
            !m->egret_pseudo_active && m->egret_resp_len == 0) {
            xcvr = !(s->b & EGRET_VIA_FULL);
        }

        if (xcvr) {
            val &= ~(uint64_t)EGRET_XCVR;
        } else {
            val |= EGRET_XCVR;
        }
    }

    if (reg == VIA_REG_SR && v1s->machine) {
        maclc550_egret_sr_read(v1s);
    }

    return val;
}

static void maclc550_via1_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MOS6522MacLc550State *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;
    MacLc550MachineState *m = v1s->machine;

    /*
     * SETUPTIMEK calibration hack, as on q800/lc475/quadra630
     * (mac_via.c): under TCG the ROM's dbra-loop timer calibration can
     * produce garbage (TimeDBRA ends up zero, later causing a
     * divide-by-zero SysError in the Time Manager conversion).  Detect
     * the T2=0x30c calibration run and stuff known-good values at its
     * end.
     */
    if (m) {
        switch (m->timer_hack_state) {
        case 0:
            if (reg == VIA_REG_T2CL && val == 0xc) {
                m->timer_hack_state = 1;
            }
            break;
        case 1:
            if (reg == VIA_REG_T2CH && val == 0x3) {
                m->timer_hack_state = 2;
            } else if (reg == VIA_REG_T2CL || reg == VIA_REG_T2CH) {
                m->timer_hack_state = 0;
            }
            break;
        case 2:
            if (reg == VIA_REG_IER && val == 0x20) {
                stw_be_phys(&address_space_memory, 0xd00, 0x2a00 * 3);
                stw_be_phys(&address_space_memory, 0xd02, 0x079d * 3);
                m->timer_hack_state = 3;
            }
            break;
        default:
            break;
        }
    }

    mos6522_write(s, reg, val, size);

    if (reg == VIA_REG_SR || reg == VIA_REG_ACR) {
        qemu_log_mask(LOG_UNIMP,
                      "maclc550 via1: reg%d <- 0x%02" PRIx64 " pc=0x%08x\n",
                      (int)reg, val, maclc550_trace_pc());
    }

    if (reg == VIA_REG_SR && v1s->machine) {
        maclc550_egret_sr_written(v1s);
    }
    if (reg == VIA_REG_ACR && v1s->machine) {
        maclc550_egret_acr_changed(v1s);
    }

    if (reg == VIA_REG_B) {
        via1_rtc_update(v1s);
        v1s->last_b = s->b;
    }
}

static const MemoryRegionOps maclc550_via1_ops = {
    .read = maclc550_via1_read,
    .write = maclc550_via1_write,
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

static void maclc550_rbv_update_irq(MacLc550MachineState *m);

static uint64_t maclc550_rbv_read(void *opaque, hwaddr addr, unsigned size)
{
    MacLc550MachineState *m = opaque;
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

    qemu_log_mask(LOG_UNIMP, "maclc550 rbv: read  +0x%02x -> 0x%02" PRIx64
                  " pc=0x%08x\n", (unsigned)addr, val, maclc550_trace_pc());
    return val;
}

static void maclc550_rbv_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MacLc550MachineState *m = opaque;

    addr &= 0x1f;
    qemu_log_mask(LOG_UNIMP, "maclc550 rbv: write +0x%02x <- 0x%02" PRIx64
                  " pc=0x%08x\n", (unsigned)addr, val, maclc550_trace_pc());

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
    maclc550_rbv_update_irq(m);
}

static const MemoryRegionOps maclc550_rbv_ops = {
    .read = maclc550_rbv_read,
    .write = maclc550_rbv_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * VDAC stub at the classic RBV-family VDAC offset (+0x24000): ported
 * verbatim (store + log, no real DAC behaviour) from maciisi.c /
 * macclassicii.c.  Present-but-inert in case anything still probes
 * this address; the real CLUT/monitor-sense/video-mode registers for
 * this machine are the Valkyrie/CSC bank below (maclc550_valkyrie_*).
 */

static uint64_t maclc550_vdac_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    MacLc550MachineState *m = opaque;
    uint64_t val = m->vdac_regs[addr & 0x3f];

    qemu_log_mask(LOG_UNIMP, "maclc550 vdac: read  +0x%02x -> 0x%02"
                  PRIx64 "\n", (unsigned)(addr & 0x3f), val);
    return val;
}

static void maclc550_vdac_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    MacLc550MachineState *m = opaque;

    qemu_log_mask(LOG_UNIMP, "maclc550 vdac: write +0x%02x <- 0x%02"
                  PRIx64 "\n", (unsigned)(addr & 0x3f), val);
    m->vdac_regs[addr & 0x3f] = val;
}

static const MemoryRegionOps maclc550_vdac_ops = {
    .read = maclc550_vdac_read,
    .write = maclc550_vdac_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * Sonora ID/config register stub: see the machine_init comment where
 * this is mapped at I/O +0x28000, right after the RBV window.
 */

static uint64_t maclc550_sonoraid_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    MacLc550MachineState *m = opaque;
    uint64_t val = m->sonoraid_regs[addr & 0x3f];

    qemu_log_mask(LOG_UNIMP, "maclc550 sonora-id: read  +0x%02x -> 0x%02"
                  PRIx64 "\n", (unsigned)(addr & 0x3f), val);
    return val;
}

static void maclc550_sonoraid_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    MacLc550MachineState *m = opaque;

    qemu_log_mask(LOG_UNIMP, "maclc550 sonora-id: write +0x%02x <- 0x%02"
                  PRIx64 "\n", (unsigned)(addr & 0x3f), val);
    m->sonoraid_regs[addr & 0x3f] = val;
}

static const MemoryRegionOps maclc550_sonoraid_ops = {
    .read = maclc550_sonoraid_read,
    .write = maclc550_sonoraid_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * Ariel-class colour-DAC register block at I/O +0x18000 (0x50f18000).
 * This is a colour machine, so -- unlike the mono Classic II, which
 * strapped PA7 low to SKIP it (macclassicii.c) -- the LC 550 ROM's
 * kind-7 bring-up pokes the onboard colour DAC here (0x4080497e writes
 * an index/mode pair: 0x90 -> +0, 0xb7 -> +1).  With nothing decoded
 * the write bus-errors and the ROM loops in its fault handler.
 *
 * The Ariel is a 4-register Brooktree-style RAMDAC (MAME
 * devices/video/ariel.cpp): +0 CLUT write-address (resets the RGB
 * sub-index), +1 CLUT data (R,G,B triples, auto-incrementing the
 * address after B), +2 control/mode (reads back the last written
 * value), +3 pixel key.  The ROM only writes +0/+1 here during
 * bring-up and never reads the block back (verified by tracing), and
 * the machine's actual scan-out CLUT is the Valkyrie/CSC bank
 * (maclc550_valkyrie_*), so a store-and-read-back model of the four
 * registers -- with +2 returning its last-written byte, matching real
 * silicon -- is sufficient; no palette is driven through it.
 */
static uint64_t maclc550_ariel_read(void *opaque, hwaddr addr, unsigned size)
{
    MacLc550MachineState *m = opaque;

    return m->ariel_regs[addr & 0x3f];
}

static void maclc550_ariel_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    MacLc550MachineState *m = opaque;

    m->ariel_regs[addr & 0x3f] = val;
}

static const MemoryRegionOps maclc550_ariel_ops = {
    .read = maclc550_ariel_read,
    .write = maclc550_ariel_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};


/*
 * Valkyrie/CSC interrupt/status/CLUT registers (ported from lc475.c):
 *   +0x104 (long) written 4    -> interrupt enable mask
 *   +0x108 (long) polled       -> status; bit 2 = vertical blank
 *   +0x10c (long) written 0    -> VBL status clear
 *   +0x122 (word) read         -> live monitor-sense lines
 *   +0x200 (byte) write        -> CLUT address register
 *   +0x213 (byte) write x3     -> CLUT data (R, G, B triplets)
 * The 60Hz tick sets the VBL bit; writes to +0x10c clear it.
 */

static void maclc550_valkyrie_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    MacLc550MachineState *m = MACLC550_MACHINE(opaque);
    MacLc550RegBank *b = m->valkyrie_bank;

    if (addr == 0x10c || (addr <= 0x10c && addr + size > 0x10c)) {
        /* VBL status clear */
        m->valkyrie_regs[0x10b] &= ~0x04;
        return;
    }
    if (addr == 0x200) {
        /* CLUT address register */
        m->clut_addr = val & 0xff;
        m->clut_phase = 0;
    }
    if (addr == 0x213 && size == 1) {
        /* CLUT data: three writes (R, G, B) per entry */
        m->clut_rgb[m->clut_phase++] = val & 0xff;
        if (m->clut_phase == 3) {
            m->fb.palette[m->clut_addr] = 0xFF000000 |
                (m->clut_rgb[0] << 16) | (m->clut_rgb[1] << 8) |
                m->clut_rgb[2];
            m->fb.invalidate = 1;
            m->clut_phase = 0;
            m->clut_addr++;
        }
        return;
    }
    maclc550_regbank_write(b, addr, val, size);
}

static uint64_t maclc550_valkyrie_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    MacLc550MachineState *m = MACLC550_MACHINE(opaque);
    MacLc550MachineClass *mc = MACLC550_MACHINE_GET_CLASS(m);

    /* Monitor sense: word reads at +0x122 return the live sense lines. */
    if (addr == 0x122 && size == 2) {
        return mc->monitor_sense;
    }
    return maclc550_regbank_read(m->valkyrie_bank, addr, size);
}

static const MemoryRegionOps maclc550_valkyrie_ops = {
    .read = maclc550_valkyrie_read,
    .write = maclc550_valkyrie_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* 60.15Hz VBL tick on CA1 and one-second tick on CA2, as on mac_via */

#define VIA_60HZ_TIMER_PERIOD_NS   16625800

static void maclc550_rbv_update_irq(MacLc550MachineState *m)
{
    /*
     * All enabled slot lines summarise into IFR bit 1 → level 2,
     * INCLUDING the onboard-video VBL (slot $E, SIFR bit 6): the OS
     * runs its Vertical Retrace Manager slot tasks — among them the
     * cursor task that couples MTemp → RawMouse and redraws the mouse
     * pointer — off this interrupt (SIER reads 0x7F at the Finder).
     * The ROM-era boot polls the line before any handler exists; that
     * is safe because the line PULSES (1.3ms per frame, dropped by
     * maclc550_vbl_off) instead of latching, so a masked blank is
     * missed rather than serviced stale (the old dsBadSlotInt came
     * from the pre-pulse latch model, not from delivering bit 6).
     */
    uint8_t slot_cpu = m->rbv_sifr & m->rbv_sier & 0x7f;

    if (slot_cpu) {
        m->rbv_ifr |= RBV_IFR_SLOT;
    } else {
        m->rbv_ifr &= ~RBV_IFR_SLOT;
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&m->glue), MACLC550_GLUE_RBV),
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

static void maclc550_scsi_irq(void *opaque, int n, int level)
{
    MacLc550MachineState *m = opaque;

    if (level) {
        m->rbv_ifr |= RBV_IFR_SCSI_IRQ;
    } else {
        m->rbv_ifr &= ~RBV_IFR_SCSI_IRQ;
    }
    maclc550_rbv_update_irq(m);
}

static void maclc550_vbl_off(void *opaque)
{
    MacLc550MachineState *m = opaque;

    /*
     * End of vertical blank: the slot line and the level-triggered IFR
     * summary drop together, so an interrupt masked past the blank is
     * simply missed rather than serviced stale (dsBadSlotInt).
     */
    m->rbv_sifr &= ~RBV_SLOT_E_INT;
    maclc550_rbv_update_irq(m);
}

static void maclc550_sixty_hz(void *opaque)
{
    MacLc550MachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA1_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    /* onboard video vertical blank = slot $E line pulses via the RBV */
    m->rbv_sifr |= RBV_SLOT_E_INT;
    maclc550_rbv_update_irq(m);
    /* Valkyrie/CSC VBL status bit, polled directly by some video drivers */
    m->valkyrie_regs[0x10b] |= 0x04;
    timer_mod(m->vbl_off_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1300000);

    timer_mod(m->sixty_hz_timer,
              (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
               VIA_60HZ_TIMER_PERIOD_NS) /
              VIA_60HZ_TIMER_PERIOD_NS * VIA_60HZ_TIMER_PERIOD_NS);
}

static void maclc550_one_second(void *opaque)
{
    MacLc550MachineState *m = opaque;
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

static bool maclc550_egret_session_pseudo_reply(MacLc550MachineState *m);

static void maclc550_egret_set_xcvr(MOS6522MacLc550State *v1s,
                                        bool assert)
{
    MOS6522State *s = MOS6522(v1s);

    /*
     * PB3 is an Egret-driven input: the authoritative line state lives
     * in the machine struct so port-B reads report it regardless of the
     * ROM's ORB writes (see maclc550_via1_read).  Keep s->b in sync
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

static void maclc550_egret_schedule_int(MacLc550MachineState *m)
{
    timer_mod(m->egret_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * 1000);
}

static void maclc550_egret_timer_cb(void *opaque)
{
    MacLc550MachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), SR_INT_BIT);

    qemu_set_irq(irq, 1);
}

static void maclc550_egret_no_response(MacLc550MachineState *m)
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

static void maclc550_egret_process(MacLc550MachineState *m)
{
    ADBBusState *adb_bus = &m->via1.adb_bus;
    uint8_t *c = m->egret_cmd;
    int n = m->egret_cmd_len;

    m->egret_resp_len = 0;
    m->egret_resp_idx = 0;

    qemu_log_mask(LOG_UNIMP,
                  "maclc550 egret: cmd len=%d [%02x %02x %02x %02x]\n",
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
     * Egret power-on cold-start SYNC/self-test command.  During
     * bring-up (0x408d1b84) the ROM sends [01 0e <payload>] over the
     * cold-start byte-exchange and reads a 3-byte reply whose FIRST
     * byte it checks must equal 2 (0x408d1c2c: cmpib #2,d0 where d0 is
     * the first received byte); a mismatch makes it retry the whole
     * sync and eventually divert into the serial diagnostic monitor.
     * Answer with a reply led by 0x02.  (The other [01 xx ...] pseudo
     * session commands -- write-MCU/control -- want no response, see
     * below.)
     *
     * ONLY while the ROM drives the Egret by POLLING (VIA1 IER SR-interrupt
     * disabled): that is the pre-driver cold-start sync (0x408d1b84).  Once
     * the post-MMU interrupt-driven Egret driver is installed (IER SR-int
     * enabled) the System still occasionally issues an [01 22]/[01 0e]
     * control command over that ISR framing -- observed live: a single
     * [01 22 df 02] during the disk-System ADB/startup bring-up.  That
     * driver does NOT want the bare 3-byte [02 00 00] cold-start reply: it
     * discards one turnaround byte (0x408d1732) and its completion
     * (0x408d1838) computes d0 = stored_count - 4, so a 3-byte reply losing
     * the discard stores only 2, underflows d0 to 0xFFFE, and dbf-copies
     * ~64KB of the reply buffer through the request block's data pointer
     * (a0@8) across low memory -- wiping a dispatch/jump slot that a later
     * jmp (a0) then takes to address 0 (Illegal instruction 7fee @ 0x2e,
     * the terminal bomb).  Route it instead through the session-pseudo
     * generic-ACK path below (proper [00 00 00 cmd] header + the
     * turnaround-discard dummy byte), exactly as the [01 1b/1c] control
     * commands are handled -- see maclc550_egret_session_pseudo_reply.
     */
    if (c[0] == 0x01 && (c[1] == 0x0e || c[1] == 0x22) &&
        !(MOS6522(&m->via1)->ier & SR_INT)) {
        qemu_log_mask(LOG_UNIMP,
                      "maclc550 egret: cold-start sync cmd 0x%02x len=%d\n",
                      c[1], n);
        m->egret_resp[m->egret_resp_len++] = 0x02;
        m->egret_resp[m->egret_resp_len++] = 0x00;
        m->egret_resp[m->egret_resp_len++] = 0x00;
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
    /*
     * GET_PRAM over the formal-session framing: [01 07 addrHi addrLo].
     * The driver (ROM 0x408b3b6e) does a receive-turnaround and clocks
     * FOUR reply bytes through 0x408b3bea, keeping the LAST one as the
     * returned PRAM byte.  A plain no-response turnaround delivers only
     * junk there, so the ROM reads a corrupt XPRAM (the 'NuMc' validity
     * signature at 0x0C, the default-OS ddType at 0x76/0x77, etc. all
     * come back 0), decides parameter RAM is invalid, rewrites it to
     * 0xFF and bails to the serial diagnostic console.  Stage the real
     * value in the 4th slot -- [00 00 00 <value>] -- so the receive-
     * turnaround's ack_toggle chain lands it in the byte the driver
     * keeps.  Keep the no-response /XCVR-low signalling the transport
     * already drives for this turnaround; only the data changes.
     */
    if (c[0] == 0x01 && c[1] == 0x07 && n >= 4) {
        uint16_t addr = ((uint16_t)c[2] << 8) | c[3];
        uint8_t data = m->via1.PRAM[addr & 0xff];

        qemu_log_mask(LOG_UNIMP,
                      "maclc550 egret: session GET_PRAM addr=0x%04x"
                      " -> 0x%02x\n", addr, data);
        m->egret_no_resp = true;
        m->egret_resp_len = 0;
        m->egret_resp[m->egret_resp_len++] = 0x00;
        m->egret_resp[m->egret_resp_len++] = 0x00;
        m->egret_resp[m->egret_resp_len++] = 0x00;
        m->egret_resp[m->egret_resp_len++] = data;
        m->egret_resp_idx = 0;
        return;
    }

    if (c[0] == 0x01 && n >= 2) {
        qemu_log_mask(LOG_UNIMP,
                      "maclc550 egret: pseudo (session) cmd 0x%02x"
                      " len=%d\n", c[1], n);
        /*
         * Two different reply framings are in play for these session-
         * framed pseudo-commands, depending on the driver routine that
         * sent them:
         *
         *  - Data-returning READS (GET_PRAM 0x07, READ_MCU 0x02, GET_TIME
         *    0x03) are collected with the poll-cadence receive-turnaround:
         *    the driver re-opens a receive session and clocks the reply
         *    bytes out through maclc550_egret_ack_toggle.  The historical
         *    no-response staging drives that turnaround (the read value is
         *    currently junk, but the boot tolerates it), so leave it.
         *
         *  - WRITE/SET/control commands (SET_PRAM 0x0c, WRITE_MCU 0x08,
         *    SET_TIME 0x09, and the [01 1b/1c/0e ...] control settings)
         *    are read back by the driver over the /XCVR byte-handshake
         *    instead (ROM 0x408b3a80: release the session, spin until PB3
         *    goes low, then clock the reply straight out of SR).  That
         *    path needs a real staged reply delivered with /XCVR asserted
         *    -- the no-response turnaround never lowers PB3, so the driver
         *    hangs forever.  Build the proper reply (with side effects --
         *    SET_PRAM actually writes PRAM) and drive it out through the
         *    same byte-handshake delivery the direct pseudo path uses.
         */
        /*
         * 0x07 GET_PRAM is collected by the polled boot driver over its
         * own receive-turnaround (handled with real data just above).
         * Everything else here -- SET_PRAM 0x0c, WRITE_MCU 0x08, and the
         * post-MMU interrupt-driven driver's READ_MCU 0x02 / GET_TIME 0x03
         * -- is read back over the /XCVR byte-handshake and needs the real
         * reply driven out through the pseudo path (READ_MCU/GET_TIME as
         * data, WRITE_MCU/SET_PRAM as the bare ACK).
         */
        if (c[1] != 0x07 && maclc550_egret_session_pseudo_reply(m)) {
            return;
        }
        maclc550_egret_no_response(m);
        return;
    }

    {
        MOS6522MacLc550State *v1s = &m->via1;
        MOS6522State *s = MOS6522(v1s);
        uint8_t obuf[ADB_MAX_OUT_LEN];
        int olen;

        adb_autopoll_block(adb_bus);
        olen = adb_request(adb_bus, obuf, c, n);
        adb_autopoll_unblock(adb_bus);

        if (s->ier & SR_INT) {
            /*
             * The post-MMU interrupt-driven Egret driver sends PLAIN ADB
             * commands (not just [01 xx] pseudo-commands) over the same
             * formal-session framing and reads the reply back over the
             * /XCVR byte-handshake -- notably the SendReset [00 00] that
             * ADBReInit (ADBBase = lowmem 0xcf8) issues to start the ADB
             * bus rescan.  That driver expects the identical framing as
             * for a pseudo-command: a [00 00 00 cmd] 4-byte header (+ any
             * register data), one turnaround-discard byte eaten at
             * 0x408d1732, and a final completion shift interrupt whose
             * routine strips the header via `d0 = stored_count - 4`.
             * Delivering the raw 2-byte no-response turnaround instead
             * stores only one byte, so the completion is never satisfied
             * and the ADB Manager spins forever at 0x4080a870 waiting on
             * ADBBase[349] bit5 -- that bit only clears once the reset
             * completion advances the rescan.  Build the header (echoing
             * the ADB command byte) plus any device reply and drive it out
             * through the pseudo byte-handshake path, exactly as
             * maclc550_egret_session_pseudo_reply does.
             */
            m->egret_resp_len = 0;
            m->egret_resp[m->egret_resp_len++] = 0x00;
            m->egret_resp[m->egret_resp_len++] = 0x00;
            m->egret_resp[m->egret_resp_len++] = 0x00;
            m->egret_resp[m->egret_resp_len++] = c[0];
            if (olen > 0 &&
                m->egret_resp_len + olen <= (int)sizeof(m->egret_resp)) {
                memcpy(m->egret_resp + m->egret_resp_len, obuf, olen);
                m->egret_resp_len += olen;
            }
            /* dummy byte for the driver's 0x408d1732 turnaround discard */
            if (m->egret_resp_len + 1 <= (int)sizeof(m->egret_resp)) {
                memmove(m->egret_resp + 1, m->egret_resp, m->egret_resp_len);
                m->egret_resp[0] = 0x00;
                m->egret_resp_len++;
            }
            m->egret_pseudo_active = true;
            m->egret_session_pseudo = true;
            m->egret_no_resp = false;
            m->egret_session = false;
            m->egret_cmd_len = 0;
            s->sr = m->egret_resp[0];
            m->egret_resp_idx = 1;
            maclc550_egret_set_xcvr(v1s, true);
            maclc550_egret_schedule_int(m);
            return;
        }

        if (olen > 0) {
            /* reply: the raw register data */
            memcpy(m->egret_resp, obuf, olen);
            m->egret_resp_len = olen;
        } else {
            /*
             * Listen/no-data/absent device: ADB bus timeout -- the
             * Egret turns around without a response.
             */
            maclc550_egret_no_response(m);
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
static void maclc550_egret_pseudo_reply(MacLc550MachineState *m,
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
static uint8_t maclc550_egret_mcu_read(MacLc550MachineState *m,
                                           uint16_t addr)
{
    if (addr >= 0x100 && addr <= 0x1ff) {
        return m->via1.PRAM[addr - 0x100];
    }
    return m->egret_mcu_mem[addr & 0xff];
}

static void maclc550_egret_mcu_write(MacLc550MachineState *m,
                                         uint16_t addr, uint8_t data)
{
    if (addr >= 0x100 && addr <= 0x1ff) {
        m->via1.PRAM[addr - 0x100] = data;
    } else {
        m->egret_mcu_mem[addr & 0xff] = data;
    }
}

static bool maclc550_egret_pseudo_build(MacLc550MachineState *m)
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
                      "maclc550 egret: pseudo-framed ADB cmd 0x%02x"
                      " len=%d -> olen=%d\n", c[1], n - 1, olen);
        maclc550_egret_pseudo_reply(m, c[1]);
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
                          "maclc550 egret: pseudo GET_PRAM addr=0x%04x"
                          " -> 0x%02x\n", addr, data);
            maclc550_egret_pseudo_reply(m, EGRET_PSEUDO_GET_PRAM);
            m->egret_resp[m->egret_resp_len++] = data;
            return true;
        }
        break;
    case EGRET_PSEUDO_SET_PRAM:
        if (n >= 5) {
            uint16_t addr = ((uint16_t)c[2] << 8) | c[3];

            m->via1.PRAM[addr & 0xff] = c[4];
            qemu_log_mask(LOG_UNIMP,
                          "maclc550 egret: pseudo SET_PRAM addr=0x%04x"
                          " <- 0x%02x\n", addr, c[4]);
            maclc550_egret_pseudo_reply(m, EGRET_PSEUDO_SET_PRAM);
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
                maclc550_egret_mcu_write(m, addr + j, c[4 + j]);
            }
            qemu_log_mask(LOG_UNIMP,
                          "maclc550 egret: pseudo WRITE_MCU addr=0x%04x"
                          " len=%d\n", addr, n - 4);
            maclc550_egret_pseudo_reply(m, EGRET_PSEUDO_WRITE_MCU);
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
                          "maclc550 egret: pseudo GET_TIME -> 0x%08x\n", t);
            maclc550_egret_pseudo_reply(m, EGRET_PSEUDO_GET_TIME);
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
                          "maclc550 egret: pseudo SET_TIME <- 0x%08x\n", t);
            maclc550_egret_pseudo_reply(m, EGRET_PSEUDO_SET_TIME);
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
         * maclc550_egret_session_update.  Stage bytes to the end
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
                          "maclc550 egret: pseudo READ_MCU addr=0x%04x"
                          " (stream %d) first=0x%02x\n", addr, len,
                          maclc550_egret_mcu_read(m, addr));
            maclc550_egret_pseudo_reply(m, EGRET_PSEUDO_READ_MCU);
            for (j = 0; j < len; j++) {
                m->egret_resp[m->egret_resp_len++] =
                    maclc550_egret_mcu_read(m, addr + j);
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
                      "maclc550 egret: pseudo generic-ACK cmd 0x%02x"
                      " (len=%d)\n", c[1], n);
        maclc550_egret_pseudo_reply(m, c[1]);
        return true;
    }

    qemu_log_mask(LOG_UNIMP,
                  "maclc550 egret: unrecognised pseudo-command"
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
static bool maclc550_egret_pseudo_try_process(MOS6522MacLc550State *v1s)
{
    MacLc550MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (!maclc550_egret_pseudo_build(m)) {
        m->egret_pseudo_cmd_len = 0;
        return false;
    }

    m->egret_pseudo_cmd_len = 0;
    m->egret_pseudo_active = true;
    m->egret_session_pseudo = false;    /* direct byte-handshake, not session */
    m->egret_coldstart = false;         /* cold-start sync is over */
    m->egret_no_resp = false;

    /*
     * The first reply byte must already be in the shift register by
     * the time the ROM's receive wait loop sees the completion
     * interrupt and checks /XCVR -- stage it now.
     */
    s->sr = m->egret_resp[0];
    m->egret_resp_idx = 1;
    maclc550_egret_set_xcvr(v1s, true);
    maclc550_egret_schedule_int(m);
    return true;
}

/*
 * A write/set/control pseudo-command that arrived over the FORMAL-SESSION
 * framing (collected in egret_cmd[] by the send path, dispatched from
 * maclc550_egret_process) but whose reply the interrupt-driven driver
 * reads back over the /XCVR byte-handshake (ROM 0x408b3a80), not the
 * poll-cadence receive-turnaround.  Build the reply via the shared
 * pseudo-command builder (so SET_PRAM etc. take their real side effects)
 * and hand it to the same byte-handshake delivery maclc550_egret_sr_read
 * drives for the direct pseudo path: first byte staged in SR, /XCVR
 * asserted, completion interrupt scheduled.  Returns false (caller falls
 * back to the no-response turnaround) if the command is not a recognised
 * pseudo-command.
 */
static bool maclc550_egret_session_pseudo_reply(MacLc550MachineState *m)
{
    MOS6522MacLc550State *v1s = &m->via1;
    MOS6522State *s = MOS6522(v1s);

    if (m->egret_cmd_len < 2 ||
        m->egret_cmd_len > (int)sizeof(m->egret_pseudo_cmd)) {
        return false;
    }
    memcpy(m->egret_pseudo_cmd, m->egret_cmd, m->egret_cmd_len);
    m->egret_pseudo_cmd_len = m->egret_cmd_len;

    if (!maclc550_egret_pseudo_build(m)) {
        m->egret_pseudo_cmd_len = 0;
        return false;
    }
    m->egret_pseudo_cmd_len = 0;

    /*
     * READ_MCU (0x02) is a STREAMED read: the wire carries no length, so
     * maclc550_egret_pseudo_build stages the whole 240-byte MCU window and
     * the interrupt-driven driver (0x408d17xx) would keep clocking bytes
     * into its request-block buffers and over-run them at completion.
     * Snoop the request block instead: while the driver is shifting this
     * reply in, a2 (env->aregs[2]) points at it, and the two byte counts
     * it will read are declared there: a2@14 bytes into its buffer at
     * a2@24 (of which the first four are the [00 00 00 cmd] protocol
     * header the completion strips via d0 = a2@16-4) and a2@18 bytes into
     * its second buffer at a2@20 -- for READ_MCU the requested MCU data
     * lands in that a2@20 half (a2@14 is typically just the 4-byte
     * header).  Rebuild the reply to exactly those two lengths, filling
     * both data regions with the real MCU bytes, so the driver reads
     * precisely what it declared, closes on its own count, and gets the
     * autopoll/ADB parameters it read back rather than zeros (zeros there
     * made it later jump through a null table into low memory).  Low RAM
     * is identity-mapped, so a2 doubles as the physical address of the
     * request block.
     */
    if (m->egret_cmd_len >= 4 && m->egret_cmd[1] == 0x02 && current_cpu) {
        CPUM68KState *env = &M68K_CPU(current_cpu)->env;
        uint32_t rb = env->aregs[2] & 0x00ffffff;
        uint16_t ndata = lduw_be_phys(&address_space_memory, rb + 14);
        uint16_t nhdr = lduw_be_phys(&address_space_memory, rb + 18);
        uint16_t addr = ((uint16_t)m->egret_cmd[2] << 8) | m->egret_cmd[3];

        qemu_log_mask(LOG_UNIMP,
                      "maclc550 egret: READ_MCU snoop rb=0x%06x ndata=%u"
                      " nhdr=%u addr=0x%04x\n", rb, ndata, nhdr, addr);
        if (ndata >= 4 && (int)(ndata + nhdr) < (int)sizeof(m->egret_resp)) {
            int i, j = 0;

            m->egret_resp_len = 0;
            m->egret_resp[m->egret_resp_len++] = 0x00;
            m->egret_resp[m->egret_resp_len++] = 0x00;
            m->egret_resp[m->egret_resp_len++] = 0x00;
            m->egret_resp[m->egret_resp_len++] = 0x02;
            /* remaining a2@24-buffer bytes (past the 4-byte header) */
            for (i = 0; i + 4 < ndata; i++) {
                m->egret_resp[m->egret_resp_len++] =
                    maclc550_egret_mcu_read(m, addr + j++);
            }
            /* a2@20-buffer bytes: the bulk of the READ_MCU payload */
            for (i = 0; i < nhdr; i++) {
                m->egret_resp[m->egret_resp_len++] =
                    maclc550_egret_mcu_read(m, addr + j++);
            }
        }
    }

    /*
     * The post-MMU interrupt-driven Egret driver (0x408d17xx, used for the
     * WRITE_MCU autopoll-parameter uploads) discards ONE turnaround byte
     * when it flips the shifter to receive (the read+discard at
     * 0x408d1732) before it starts STORING reply bytes into its request
     * block.  Its completion routine (0x408d1838) then computes
     * `d0 = stored_count - 4` and dbf-copies d0+1 bytes, so if fewer than
     * the 4 header bytes actually land in the block d0 underflows to
     * ~0xFFFE and smashes the stack (observed: reaches _InitGraf at
     * 0x40802842 on a corrupted A7 -> double fault).  Prepend a dummy byte
     * so the turnaround discard eats it and the full [00 00 00 cmd] header
     * still gets stored (stored_count == 4, d0 == 0, no bad copy).  The
     * polled SET_PRAM driver (0x408b3a80) does NOT discard a turnaround
     * byte and reads only its first three status bytes, so leave its reply
     * unpadded (padding would also desync the stale-reply teardown that
     * keys off egret_resp_len).
     */
    /*
     * The turnaround-discard is a property of the post-MMU interrupt-driven
     * Egret driver (0x408d17xx), NOT of the specific command: every one of
     * its pseudo-commands (WRITE_MCU 0x08, READ_MCU 0x02, GET_TIME 0x03, and
     * the [01 1b/1c ...] control settings) is read back with the same
     * read+discard at 0x408d1732, so each needs one dummy byte prepended for
     * the discard to eat while the full [00 00 00 cmd] header still lands
     * (stored_count == 4, d0 == 0).  An unpadded control reply (e.g. the
     * [01 1b] autopoll-enable) loses the discard from its own 4 bytes,
     * stores only 3, and the completion's `d0 = stored_count - 4` underflows
     * to 0xFFFF -- dbf then copies ~64KB of the reply buffer across all of
     * low memory from destination 0, wiping the exception vectors and the
     * boot globals (MemTop/BufPtr) and double-faulting a few instructions
     * later.  Keying the pad on the command byte missed the control
     * commands; key it instead on the interrupt-driven driver being active,
     * which is exactly when VIA1's SR interrupt is enabled (IER bit 2).  The
     * polled boot-ROM SET_PRAM driver (0x408b3a80) runs with the SR
     * interrupt DISABLED and does not discard, so it stays unpadded.
     */
    if ((s->ier & SR_INT) &&
        m->egret_resp_len + 1 <= (int)sizeof(m->egret_resp)) {
        memmove(m->egret_resp + 1, m->egret_resp, m->egret_resp_len);
        m->egret_resp[0] = 0x00;
        m->egret_resp_len++;
    }
    m->egret_pseudo_active = true;
    m->egret_session_pseudo = true;
    m->egret_no_resp = false;
    /*
     * The reply is delivered over the byte-handshake (pseudo) path from
     * here on, not the formal session the command arrived in: close the
     * formal session so the driver's PB4//PB5 toggles while it clocks the
     * reply out of SR are absorbed by the pseudo branch of
     * maclc550_egret_session_update rather than mistaken for further
     * session command bytes, and so the NEXT command opens a clean new
     * session once this drained reply is torn down.
     */
    m->egret_session = false;
    m->egret_cmd_len = 0;

    s->sr = m->egret_resp[0];
    m->egret_resp_idx = 1;
    maclc550_egret_set_xcvr(v1s, true);
    maclc550_egret_schedule_int(m);
    return true;
}

/*
 * /XCVR_SESSION (PB3) as sampled by the ROM driver at each shift
 * interrupt: real responses keep PB3 high while bytes flow and drop it
 * as the end marker; a no-response exchange holds PB3 low throughout
 * (the driver still clocks two junk bytes through SR).
 */
static void maclc550_egret_ack_toggle(MOS6522MacLc550State *v1s)
{
    MacLc550MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (m->egret_resp_idx < m->egret_resp_len) {
        s->sr = m->egret_resp[m->egret_resp_idx++];
        maclc550_egret_set_xcvr(v1s, m->egret_no_resp);
        maclc550_egret_schedule_int(m);
        qemu_log_mask(LOG_UNIMP,
                      "maclc550 egret: feed 0x%02x (#%d/%d) pc=%08x b=%02x\n",
                      s->sr, m->egret_resp_idx, m->egret_resp_len,
                      maclc550_trace_pc(), s->b);
    } else {
        /* final ack: PB3 low = end-of-response, the exchange is over */
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
        m->egret_session = false;
        maclc550_egret_set_xcvr(v1s, true);
        maclc550_egret_schedule_int(m);
        qemu_log_mask(LOG_UNIMP,
                      "maclc550 egret: response complete pc=%08x b=%02x\n",
                      maclc550_trace_pc(), s->b);
    }
}

static void maclc550_egret_session_update(MOS6522MacLc550State *v1s)
{
    MacLc550MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);
    bool sys = !(s->b & EGRET_SYS_SESSION);
    uint8_t hs_change = (s->b ^ v1s->last_b) & (EGRET_SYS_SESSION |
                                                EGRET_VIA_FULL);

    /*
     * Pseudo-command reply delivery (byte-handshake framing) is driven
     * entirely from maclc550_egret_sr_read -- the /VIA_FULL (PB4)
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
    /*
     * Stale, fully-drained byte-handshake reply: a session-framed
     * write/set pseudo-command (SET_PRAM &c., dispatched through
     * maclc550_egret_session_pseudo_reply) stages a 4-byte
     * [00 00 00 cmd] reply, but its driver (ROM 0x408b3a80) reads only
     * the 3-byte [00 00 00] status and moves straight on, leaving the
     * exchange marked active with the cmd-echo byte undelivered.  When
     * the host then opens a fresh formal-session SEND (/SYS_SESSION
     * asserted, shifter in SR-OUT), that previous exchange is over: fall
     * out of the pseudo branch so the teardown just below runs and the
     * new session opens normally instead of being swallowed here.
     */
    if (m->egret_pseudo_active &&
        !(m->egret_resp_idx >= m->egret_resp_len && sys &&
          (s->acr & SR_OUT) && (hs_change & EGRET_SYS_SESSION))) {
        /*
         * Count-exhausted close of a STREAMED reply by the post-MMU
         * interrupt-driven driver (READ_MCU 0x02 over the formal-session
         * framing).  When its request-block byte count runs out that
         * driver deasserts BOTH handshake lines together (orib #48 ->
         * /SYS_SESSION and /VIA_FULL high at 0x408d17de) and then spins at
         * 0x408d15a2 waiting for /XCVR to RISE before it re-asserts
         * /SYS_SESSION for the next command.  A streamed reply stages far
         * more bytes than the host wants, so the last-byte close in
         * maclc550_egret_sr_read never fires and /XCVR would stay asserted
         * -> deadlock (the host never re-asserts /SYS_SESSION, so the
         * assert-edge early-close just below never fires either).  Detect
         * the both-lines-high close here and drop /XCVR so the spin
         * releases.  Clear egret_session_pseudo: this driver keys the end
         * of the exchange off /XCVR rising, not off a completion shift
         * interrupt (that is the WRITE_MCU/fixed-reply path, which drains
         * its whole reply and tears down in sr_read instead).
         */
        if ((hs_change & (EGRET_SYS_SESSION | EGRET_VIA_FULL)) &&
            (s->b & (EGRET_SYS_SESSION | EGRET_VIA_FULL)) ==
                (EGRET_SYS_SESSION | EGRET_VIA_FULL) &&
            m->egret_resp_idx > 4) {
            timer_del(m->egret_timer);
            maclc550_egret_set_xcvr(v1s, false);
            m->egret_pseudo_active = false;
            m->egret_pseudo_closing = true;
            m->egret_session_pseudo = false;
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
            return;
        }
        /*
         * Host-side early close of a STREAMED reply: the interrupt-
         * driven Egret driver's receive (ISR 0x40a14912) reads exactly
         * the number of data bytes its request block asked for and
         * then drops /SYS_SESSION (bclr #5 at 0x40a149c8) and waits
         * for /XCVR to rise as the end-of-exchange handshake.  A
         * streamed READ_MCU reply usually stages more bytes than the
         * host wants (the wire carries no length), so the last-byte
         * close in maclc550_egret_sr_read never triggers -- end
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
            maclc550_egret_set_xcvr(v1s, false);
            m->egret_pseudo_active = false;
            m->egret_pseudo_closing = true;
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
        }
        return;
    }

    if (m->egret_pseudo_active) {
        /*
         * Fell through the guard above: a fully-drained session-framed
         * write/set reply whose driver has now opened a fresh send.
         * Drop the leftover (undelivered cmd-echo byte + its pending
         * completion interrupt) and continue into the session-open
         * handling below so the new command is collected normally.
         */
        timer_del(m->egret_timer);
        maclc550_egret_set_xcvr(v1s, false);
        m->egret_pseudo_active = false;
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
    }

    /*
     * Teardown tail: the ROM clears /SYS_SESSION then /VIA_FULL
     * (0x4084a646/0x4084a650) after the last reply byte.  The first of
     * those writes is a /SYS_SESSION-released edge that would otherwise
     * match the formal-session-open test below; keep suppressing until
     * both lines have settled low (/VIA_FULL also clear).
     */
    if (m->egret_pseudo_closing) {
        /*
         * A pseudo-command that arrived over the formal-session framing
         * and was answered over the /XCVR byte-handshake (WRITE_MCU &c.
         * from the post-MMU interrupt-driven Egret driver at 0x408d17xx):
         * after draining the whole reply and seeing /XCVR rise, that
         * driver tears the session down (orib #48 -> both lines high at
         * 0x408d17de) and then spins at 0x408d180a for ONE final
         * "session-closed" shift interrupt to run its completion routine
         * (0x408d1838).  Raise exactly that single interrupt here, on the
         * teardown write.  Scheduling it any earlier (e.g. at the last
         * reply byte) makes the driver read a phantom extra SR byte and
         * fault; the polled boot-ROM's own byte-handshake receive keys off
         * /XCVR instead and never sets egret_session_pseudo, so it is
         * unaffected.
         */
        if (m->egret_session_pseudo && (hs_change & EGRET_SYS_SESSION) &&
            !sys) {
            /*
             * Only raise the completion interrupt when the driver is about
             * to CONSUME it by polling with interrupts MASKED (I:7): that
             * is the WRITE_MCU/SET_PRAM spin at 0x408d180a.  READ_MCU runs
             * its completion tail with interrupts ENABLED (I:1), where the
             * same raised SR line would instead re-enter the Egret ISR
             * against a request the driver has not re-armed and jump wild
             * into low memory.  In that case leave it to the driver's own
             * per-byte ISR flow to finish the transaction.
             */
            int ipl = current_cpu ?
                      ((M68K_CPU(current_cpu)->env.sr >> 8) & 7) : 7;

            m->egret_session_pseudo = false;
            m->egret_pseudo_closing = false;
            if (ipl == 7) {
                s->sr = 0;
                maclc550_egret_schedule_int(m);
            }
            return;
        }
        if (!(s->b & (EGRET_SYS_SESSION | EGRET_VIA_FULL))) {
            m->egret_pseudo_closing = false;
        }
        return;
    }

    /*
     * Egret power-on cold-start line handshake (see the egret_coldstart
     * field comment): while the pre-driver sync at 0x408d1ae6 is running
     * (no session, no pseudo/ADB traffic), the Egret clocks a shift-
     * register interrupt each time the host toggles /VIA_FULL (PB4) --
     * the /XCVR line itself is mirrored combinationally in the port-B
     * read above.  This carries both the line handshake and the busy-
     * waits on the SR-complete flag (VIA1 IFR bit 2) through to the end
     * of the sync, so the ROM proceeds to boot instead of timing out
     * into its serial diagnostic monitor.
     */
    if (m->egret_coldstart && !sys && !m->egret_session
        && !m->egret_pseudo_active && m->egret_resp_len == 0) {
        if (hs_change & EGRET_VIA_FULL) {
            maclc550_egret_schedule_int(m);
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
        /*
         * The Egret cold-start's own byte-exchange phase (0x408d1c38+)
         * opens a formal session too (it asserts /SYS_SESSION to shift
         * the sync bytes).  Latch cold-start handling OFF for the
         * duration so the per-/VIA_FULL-toggle interrupt below does not
         * fire during the byte shifting (it corrupts the SR timing).
         * Remember that THIS session was a cold-start one so it can be
         * re-armed on close -- the ROM runs the whole cold-start (line
         * handshake + byte exchange) more than once during bring-up, and
         * only cold-start-originated sessions should re-arm (a normal
         * driver session must not, or the toggle handler would interfere
         * with regular Egret traffic).
         */
        if (m->egret_coldstart) {
            m->egret_coldstart_session = true;
        }
        m->egret_coldstart = false;
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
            maclc550_egret_set_xcvr(v1s, false);
            maclc550_egret_sr_written(v1s);
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
            maclc550_egret_set_xcvr(v1s, m->egret_no_resp);
            maclc550_egret_schedule_int(m);
        } else {
            /*
             * Receive session with nothing to deliver: the Egret has
             * nothing to say -- run the no-response signature (/XCVR
             * low throughout, two junk bytes clocked and discarded).
             */
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
            maclc550_egret_no_response(m);
            s->sr = m->egret_resp[0];
            m->egret_resp_idx = 1;
            maclc550_egret_set_xcvr(v1s, true);
            maclc550_egret_schedule_int(m);
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
        maclc550_egret_ack_toggle(v1s);
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
            maclc550_egret_process(m);
        }
        /*
         * A session-framed write/set pseudo-command (e.g. SET_PRAM)
         * whose reply the interrupt-driven driver reads back over the
         * /XCVR byte-handshake: maclc550_egret_process has already staged
         * that reply and begun its pseudo delivery (first byte in SR,
         * /XCVR asserted, completion interrupt scheduled).  Do NOT run the
         * ordinary close teardown below -- it would drop the staged reply
         * (resp_idx>0) and deassert /XCVR, hanging the driver at
         * 0x408b3a9c.  Just finish closing the formal session.
         */
        if (m->egret_pseudo_active) {
            m->egret_session = false;
            m->egret_cmd_len = 0;
            m->egret_coldstart_session = false;
            return;
        }
        m->egret_session = false;
        m->egret_cmd_len = 0;
        /*
         * Re-arm the Egret cold-start line-handshake handling for the
         * next sync round, but only while the boot is still driving the
         * Egret by POLLING (VIA1 IER SR-interrupt disabled).  Once the
         * interrupt-driven Egret driver is installed (post-MMU, IER
         * SR-int enabled) the raw cold-start sync never runs again, and
         * re-arming there would let the per-/VIA_FULL-toggle handler
         * interfere with that driver's own transactions.
         */
        if (m->egret_coldstart_session && !(s->ier & SR_INT)) {
            m->egret_coldstart = true;
        }
        m->egret_coldstart_session = false;
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
        maclc550_egret_set_xcvr(v1s, false);
        /*
         * The Egret clocks one final shift-register interrupt when the
         * host releases /TIP: the "session closed" acknowledgement,
         * needed to advance the poll cadence's ORB ^= 0x20 reopen.
         */
        maclc550_egret_schedule_int(m);
    }
}

static void maclc550_egret_sr_written(MOS6522MacLc550State *v1s)
{
    MacLc550MachineState *m = v1s->machine;
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
                      "maclc550 egret: <- 0x%02x (#%d) pc=%08x b=%02x\n",
                      s->sr, m->egret_cmd_len, maclc550_trace_pc(), s->b);
    } else if ((s->acr & SR_CTRL) == SR_CTRL) {
        /*
         * Outside a formal session, with the shifter in the exact
         * external-clock SR-OUT mode the byte-handshake pseudo-command
         * send uses (ROM 0x4084a556 sets ACR to 0x1c before shifting
         * the first byte): collect the raw byte.  Whether this forms a
         * recognised pseudo-command is decided at the send->receive
         * turnaround (maclc550_egret_acr_changed); the cold-start
         * sync's own framing bytes share this same ACR pattern but are
         * simply discarded there when unrecognised.
         */
        if (m->egret_pseudo_cmd_len < (int)sizeof(m->egret_pseudo_cmd)) {
            m->egret_pseudo_cmd[m->egret_pseudo_cmd_len++] = s->sr;
        }
        qemu_log_mask(LOG_UNIMP,
                      "maclc550 egret: pseudo <- 0x%02x (#%d) pc=%08x"
                      " b=%02x\n", s->sr, m->egret_pseudo_cmd_len,
                      maclc550_trace_pc(), s->b);
    }

    /*
     * Regardless of session framing, the Egret provides the shift clock:
     * each byte the host shifts OUT in external-clock SR mode completes
     * and raises the SR interrupt.  This is what carries the ROM's Egret
     * cold-start byte-framing sync (0x40814cc8), whose send helpers clock
     * bytes with /SYS_SESSION released (no formal session open).
     */
    maclc550_egret_schedule_int(m);
}

static void maclc550_egret_acr_changed(MOS6522MacLc550State *v1s)
{
    MacLc550MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    qemu_log_mask(LOG_UNIMP,
                  "maclc550 egret: DBG acr=%02x sess=%d cmdlen=%d "
                  "resplen=%d respidx=%d pseudoact=%d pc=%08x\n",
                  s->acr, m->egret_session, m->egret_cmd_len,
                  m->egret_resp_len, m->egret_resp_idx,
                  m->egret_pseudo_active, maclc550_trace_pc());

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
        maclc550_egret_process(m);
        m->egret_cmd_len = 0;
        /*
         * A session-framed write/set pseudo-command (SET_PRAM &c.) may
         * have started a byte-handshake reply delivery inside
         * maclc550_egret_process (session_pseudo_reply): it already staged
         * the first byte and asserted /XCVR for the driver's PB3 wait at
         * ROM 0x408b3a9c.  Leave that alone -- driving /XCVR here from
         * egret_no_resp would deassert it and hang the read.
         */
        if (!m->egret_pseudo_active) {
            maclc550_egret_set_xcvr(v1s, m->egret_no_resp &&
                                        m->egret_resp_len > 0);
        }
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
        if (!maclc550_egret_pseudo_try_process(v1s)) {
            /*
             * Drop any stale, already-consumed response left over from
             * an earlier exchange: with the interrupt-driven Egret
             * driver's IER SR-int enabled, a leftover resp_idx <
             * resp_len would make the read-side chain in
             * maclc550_egret_sr_read feed its junk bytes into this
             * fresh turnaround's receive as if they were a reply
             * (observed as "feed 0x00 (#2/2)" garbage after an
             * unrecognised command, cascading into the ISR copying
             * stale SR bytes through an uninitialised buffer pointer).
             */
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
            maclc550_egret_schedule_int(m);
        }
    }
}

static void maclc550_egret_sr_read(MOS6522MacLc550State *v1s)
{
    MacLc550MachineState *m = v1s->machine;
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
                      "maclc550 egret: pseudo -> 0x%02x (#%d/%d)"
                      " pc=%08x b=%02x\n", s->sr, m->egret_resp_idx,
                      m->egret_resp_len, maclc550_trace_pc(), s->b);
        if (just_read >= m->egret_resp_len - 1) {
            /*
             * Unsolicited autopoll packet (Egret-initiated, not a reply to
             * a host command): the interrupt-driven transport's receive
             * (ROM 0x408d173e) stores this last byte and then, seeing /XCVR
             * risen (0x408d1792), completes the receive against the pending
             * autopoll op it re-arms after every transaction (a2@52 ==
             * a2@48 == 0x5a14, ROM 0x40814bd6) -- no host session teardown
             * follows, so finish clean without arming the formal-session
             * suppression (egret_pseudo_closing would then never clear,
             * since there is no host PB4/PB5 teardown write to end it).
             */
            if (m->egret_unsolicited) {
                maclc550_egret_set_xcvr(v1s, false);
                m->egret_pseudo_active = false;
                m->egret_unsolicited = false;
                m->egret_resp_len = 0;
                m->egret_resp_idx = 0;
                return;
            }
            /*
             * That was the last byte: drop /XCVR, exchange is over.
             * The ROM still has its own teardown writes coming
             * (0x4084a646/0x4084a650) -- keep suppressing the formal-
             * session detector through those (see egret_pseudo_closing
             * in maclc550_egret_session_update).
             */
            maclc550_egret_set_xcvr(v1s, false);
            m->egret_pseudo_active = false;
            m->egret_pseudo_closing = true;
            /*
             * egret_session_pseudo is deliberately LEFT set here: a reply
             * to a formal-session-framed pseudo-command (SET_PRAM,
             * WRITE_MCU &c.) needs one final "session-closed" shift
             * interrupt once the host tears the session down -- raised in
             * maclc550_egret_session_update's closing handler, not now
             * (raising it mid-read makes the ISR-driven driver at
             * 0x408d17xx read a phantom extra byte and fault).  The direct
             * byte-handshake path clears the flag in pseudo_try_process
             * and keys off /XCVR rising instead, so it is unaffected.
             */
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
        } else {
            s->sr = m->egret_resp[m->egret_resp_idx++];
            maclc550_egret_schedule_int(m);
        }
        return;
    }

    /*
     * Interrupt-driven Egret driver (post-MMU boot: IER SR-int enabled,
     * ISR 0x40814912): each reply byte is consumed by the ISR's SR
     * read, after which the Egret clocks the next byte in -- chain
     * delivery from the read side, exactly like the pseudo path above.
     * (The polled boot-ROM flows instead drive delivery from their
     * PB4//PB5 handshake edges via maclc550_egret_ack_toggle, so
     * this is gated on IER to avoid double-feeding them.)
     */
    if ((s->ier & SR_INT) && m->egret_resp_idx < m->egret_resp_len) {
        maclc550_egret_ack_toggle(v1s);
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
        maclc550_egret_schedule_int(m);
        return;
    }

    if (m->egret_resp_len == 0) {
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "maclc550 egret: -> 0x%02x (#%d/%d) pc=%08x b=%02x\n",
                  s->sr, m->egret_resp_idx, m->egret_resp_len,
                  maclc550_trace_pc(), s->b);
}

/*
 * Deliver an unsolicited Egret autopoll packet to the interrupt-driven ADB
 * driver.  The transport re-arms a receive for the registered autopoll op
 * after every transaction (ROM 0x40814bb4: a2@52 = a2@48, up to 12 bytes
 * into a2@24, with a2 = the transport request block at lowmem *(0xde0)); it
 * then waits for the Egret to assert /XCVR and clock the packet in.  When
 * the received packet's byte 1 is 0 (autopoll), the completion routes it to
 * the autopoll op's callback (0x408b2fcc), which takes byte 3 as the ADB
 * command byte (addr<<4 | Talk-R0), byte 2 as status and bytes 4.. as the
 * device register data, and dispatches the addressed device's completion.
 *
 * We only inject when the transport is genuinely idle AND that pending
 * receive is the registered autopoll op (a2@52 == a2@48, both non-zero),
 * so the packet lands on the autopoll path and never collides with a host
 * command.  `pkt` is the raw byte stream the transport stores, i.e.
 * [b0, 0x00, status, adbcmd, data...].
 */
static void maclc550_egret_deliver_unsolicited(MacLc550MachineState *m,
                                               const uint8_t *pkt, int len)
{
    MOS6522MacLc550State *v1s = &m->via1;
    MOS6522State *s = MOS6522(v1s);
    uint32_t rb;
    uint32_t op48, op52;
    int i;

    if (len <= 0 || len > (int)sizeof(m->egret_resp)) {
        return;
    }
    /* transport must be idle and the SR interrupt-driven driver installed */
    if (!(s->ier & SR_INT) || m->egret_session || m->egret_pseudo_active ||
        m->egret_pseudo_closing || m->egret_coldstart ||
        m->egret_resp_len != 0 || m->egret_resp_idx != 0) {
        return;
    }
    /* the pending receive must be the registered autopoll op */
    rb = ldl_be_phys(&address_space_memory, 0xde0) & 0x00ffffff;
    if (rb < 0x1000) {
        return;
    }
    op48 = ldl_be_phys(&address_space_memory, rb + 48);
    op52 = ldl_be_phys(&address_space_memory, rb + 52);
    if (op48 == 0 || op48 != op52) {
        return;
    }

    m->egret_resp_len = 0;
    for (i = 0; i < len; i++) {
        m->egret_resp[m->egret_resp_len++] = pkt[i];
    }
    m->egret_pseudo_active = true;
    m->egret_unsolicited = true;
    m->egret_session_pseudo = false;
    m->egret_no_resp = false;
    m->egret_session = false;
    s->sr = m->egret_resp[0];
    m->egret_resp_idx = 1;
    maclc550_egret_set_xcvr(v1s, true);
    maclc550_egret_schedule_int(m);

    qemu_log_mask(LOG_UNIMP,
                  "maclc550 egret: autopoll unsolicited pkt len=%d"
                  " [%02x %02x %02x %02x ..] op=0x%06x\n", len,
                  pkt[0], len > 1 ? pkt[1] : 0, len > 2 ? pkt[2] : 0,
                  len > 3 ? pkt[3] : 0, op48 & 0x00ffffff);
}

/*
 * Boot-time mouse-rendezvous crutch (see the autopoll_timer field comment
 * and hw/m68k/macse30.c).  Fires periodically; while the ROM is parked on
 * its 0x172 spin (0x40802a38) with 0x172 bit7 still set, hold the mouse
 * button DOWN, force the mouse to re-announce that held state (no cursor
 * movement) and deliver it as an unsolicited Egret autopoll packet so the
 * mouse's ROM completion fires and clears 0x172.  Release the button and
 * stop once 0x172 has cleared.
 */
static void maclc550_egret_autopoll(void *opaque)
{
    MacLc550MachineState *m = opaque;
    ADBBusState *adb_bus = &m->via1.adb_bus;
    uint8_t flag172;
    uint32_t pc;

    if (!m->autopoll_armed) {
        return;
    }

    flag172 = address_space_ldub(&address_space_memory, 0x172,
                                 MEMTXATTRS_UNSPECIFIED, NULL);
    pc = current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;

    if (flag172 & 0x80) {
        m->autopoll_saw172 = true;
    }

    /*
     * Rendezvous complete: 0x172 has been seen set and is now clear.
     * Release the synthetic button and disarm the crutch so no further
     * autopoll injection can land mid-way through whatever the ROM does
     * next (e.g. a later ADBReInit's enumeration).
     */
    if (m->autopoll_saw172 && flag172 == 0) {
        if (m->autopoll_click) {
            m->autopoll_click = false;
            qemu_input_queue_btn(NULL, INPUT_BUTTON_LEFT, false);
            qemu_input_event_sync();
        }
        m->autopoll_armed = false;
        return;
    }

    /*
     * Parked on the 0x172 startup spin (0x40802a38, tstb 0x172; bne) with
     * the flag still set: hold the button down and re-announce it, then
     * deliver the poll as an unsolicited autopoll packet.
     */
    if (m->autopoll_saw172 && (flag172 & 0x80) &&
        pc >= 0x40802a30 && pc <= 0x40802a60) {
        uint8_t obuf[ADB_MAX_OUT_LEN];
        uint8_t pkt[8];
        int olen;

        if (!m->autopoll_click) {
            m->autopoll_click = true;
            qemu_input_queue_btn(NULL, INPUT_BUTTON_LEFT, true);
            qemu_input_event_sync();
        }
        adb_mouse_force_report(adb_bus);

        adb_autopoll_block(adb_bus);
        olen = adb_poll(adb_bus, obuf, adb_bus->autopoll_mask);
        adb_autopoll_unblock(adb_bus);

        if (olen > 0) {
            int i;
            /* [b0, autopoll-type 0, status 0, adbcmd, regdata...] */
            pkt[0] = 0x00;
            pkt[1] = 0x00;
            pkt[2] = 0x00;
            pkt[3] = obuf[0];           /* ADB command byte (addr<<4|0x0c) */
            for (i = 1; i < olen && i + 3 < (int)sizeof(pkt); i++) {
                pkt[3 + i] = obuf[i];
            }
            maclc550_egret_deliver_unsolicited(m, pkt, 3 + olen);
        }
    }

    timer_mod(m->autopoll_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 20 * 1000000);
}

/*
 * The RBV also emulates a VIA2 at the classic VIA2 site (slice +0x2000):
 * registers are VIA-spaced (offset >> 9), low address bits don't care.
 * IFR (reg 13) and IER (reg 14) share the RBV interrupt state.
 */

static uint64_t maclc550_rbv_via2_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    MacLc550MachineState *m = opaque;
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

    qemu_log_mask(LOG_UNIMP, "maclc550 rbv-via2: read  reg%d -> 0x%02" PRIx64
                  " pc=0x%08x\n", reg, val, maclc550_trace_pc());
    return val;
}

static void maclc550_rbv_via2_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    MacLc550MachineState *m = opaque;
    int reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    qemu_log_mask(LOG_UNIMP, "maclc550 rbv-via2: write reg%d <- 0x%02" PRIx64
                  " pc=0x%08x\n", reg, val, maclc550_trace_pc());

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
    maclc550_rbv_update_irq(m);
}

static const MemoryRegionOps maclc550_rbv_via2_ops = {
    .read = maclc550_rbv_via2_read,
    .write = maclc550_rbv_via2_write,
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

static uint64_t maclc550_scsi_pdma_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    NCR5380State *s = opaque;

    return ncr5380_pdma_read(s);
}

static void maclc550_scsi_pdma_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    NCR5380State *s = opaque;

    ncr5380_pdma_write(s, val);
}

static const MemoryRegionOps maclc550_scsi_pdma_ops = {
    .read = maclc550_scsi_pdma_read,
    .write = maclc550_scsi_pdma_write,
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

static MemTxResult maclc550_scsi_hsk_read(void *opaque, hwaddr addr,
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

static MemTxResult maclc550_scsi_hsk_write(void *opaque, hwaddr addr,
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

static const MemoryRegionOps maclc550_scsi_hsk_ops = {
    .read_with_attrs = maclc550_scsi_hsk_read,
    .write_with_attrs = maclc550_scsi_hsk_write,
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

static void maclc550_irq_sink(void *opaque, int n, int level)
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
static bool maclc550_iotrace_is_asc_gap(hwaddr addr)
{
    return addr >= ASC_OFS && addr < ASC_OFS + 0x2000;
}

static MemTxResult maclc550_iotrace_read(void *opaque, hwaddr addr,
                                        uint64_t *data, unsigned size,
                                        MemTxAttrs attrs)
{
    if (maclc550_iotrace_is_asc_gap(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "maclc550 io: read  +0x%05x (%d) -> asc gap (0)\n",
                      (unsigned)addr, size);
        *data = 0;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "maclc550 io: read  +0x%05x (%d) -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, maclc550_trace_pc());
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult maclc550_iotrace_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size,
                                         MemTxAttrs attrs)
{
    if (maclc550_iotrace_is_asc_gap(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "maclc550 io: write +0x%05x (%d) <- 0x%08" PRIx64
                      " -> asc gap (ignored)\n",
                      (unsigned)addr, size, val);
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "maclc550 io: write +0x%05x (%d) <- 0x%08" PRIx64
                  " -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, val, maclc550_trace_pc());
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps maclc550_iotrace_ops = {
    .read_with_attrs = maclc550_iotrace_read,
    .write_with_attrs = maclc550_iotrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void maclc550_machine_init(MachineState *machine)
{
    MacLc550MachineState *m = MACLC550_MACHINE(machine);
    ram_addr_t ram_size = machine->ram_size;
    const char *bios_name = machine->firmware ?: MACLC550_ROM_FILENAME;
    char *filename;
    int bios_size;
    uint8_t *ptr;
    CPUState *cs;
    DeviceState *dev;
    SysBusDevice *sysbus;
    int i;

    if (ram_size > 0x10000000) {
        error_report("Too much memory for this machine: %" PRId64 " MiB, "
                     "maximum 256 MiB", ram_size / MiB);
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
     * A31-half (24-bit tagged master pointer) forwarding.  Although this
     * is a 32-bit-clean Sonora machine, MacOS boots the ROM in 24-bit
     * mode and dereferences 24-bit-tagged Handle master pointers whose
     * top byte carries the Handle-state flags -- boot testing turned up
     * exactly the case the original "32-bit-clean, needs none" note
     * anticipated: the post-mouse-rendezvous startup dispatch loop's
     * _PtInRgn (ROM 0x40802a76) faulted on region handle 0xA0010730
     * (locked+resource Handle, real address 0x00010730).  Forward the
     * whole top half to the low 16 MB, at low priority so the Valkyrie
     * window at 0xf9800000 keeps its own decode.
     */
    memory_region_init_io(&m->ramio_a31, OBJECT(machine), &maclc550_a31_ops,
                          m, "maclc550.ram-a31", 0x80000000);
    memory_region_add_subregion_overlap(get_system_memory(), 0x80000000,
                                        &m->ramio_a31, -2);

    /* machine ID register */
    memory_region_init_io(&m->machine_id, NULL, &machine_id_ops, m,
                          "Machine ID", 4);
    memory_region_add_subregion(get_system_memory(), 0x5ffffffc,
                                &m->machine_id);

    /* IRQ glue */
    object_initialize_child(OBJECT(machine), "glue", &m->glue,
                            TYPE_MACLC550_GLUE);
    object_property_set_link(OBJECT(&m->glue), "cpu", OBJECT(&m->cpu),
                             &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&m->glue), &error_fatal);

    /* I/O container + repeating alias */
    memory_region_init(&m->macio, OBJECT(machine), "mac-io", IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE, &m->macio);

    memory_region_init_io(&m->macio_alias, OBJECT(machine), &macio_alias_ops,
                          &m->macio, "mac-io.alias", IO_SIZE - IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE + IO_SLICE,
                                &m->macio_alias);

    /* catch-all trace region behind the devices */
    memory_region_init_io(&m->iotrace, OBJECT(machine), &maclc550_iotrace_ops,
                          m, "mac-io.trace", IO_SLICE);
    memory_region_add_subregion_overlap(&m->macio, 0, &m->iotrace, -1);

    /* VIA1 */
    object_initialize_child(OBJECT(machine), "via1", &m->via1,
                            TYPE_MOS6522_MACLC550);
    qdev_prop_set_uint64(DEVICE(&m->via1), "frequency", VIA_TIMER_FREQ);
    /*
     * Machine identification is the dedicated ID register above, not a
     * VIA1 port-A decoder-kind strap dance (see the machine-ID-register
     * comment near the top of the file).  Apple's own tech notes note
     * that this register scheme's bit 10 signals "additional ID bits
     * elsewhere (e.g. VIA1)" for some boxes; no such fine-grained strap
     * requirement is known for this ROM, so leave port A at its
     * pulled-up idle level (no straps asserted) until boot testing
     * shows otherwise.
     */
    qdev_prop_set_uint8(DEVICE(&m->via1), "pins-a", 0xff);
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
     * Default OS / startup-device XPRAM bytes, as the Egret MCU's own
     * firmware defaults provide on real hardware (the ROM's XPRAM
     * default-rebuild pass covers 0x01-0x6d but NOT this range, so
     * with a zeroed store the boot-driver installer's expected ddType
     * -- GetOSDefault = XPRAM[0x76..0x77], compared at 0x40a07264
     * against the disk's driver ddType -- reads 0, matches no driver
     * on any disk, and the boot rescans the SCSI bus forever at the
     * blinking "?".  0x0001 = the standard Mac SCSI driver ddType this
     * (and every stock) disk carries; startup-device record 0x78-0x7b
     * = 0xff.. = no preference, scan the bus (the working -M quadra700
     * oracle reads exactly 0x0001/0xffff here).
     */
    m->via1.PRAM[0x76] = 0x00;
    m->via1.PRAM[0x77] = 0x01;      /* default OS: ddType 1 (Mac SCSI) */
    m->via1.PRAM[0x78] = 0xff;      /* default startup device: none */
    m->via1.PRAM[0x79] = 0xff;
    m->via1.PRAM[0x7a] = 0xff;
    m->via1.PRAM[0x7b] = 0xff;
    m->via1.machine = m;
    m->egret_coldstart = true;          /* Egret power-on line handshake */
    m->egret_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maclc550_egret_timer_cb,
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
    /* boot-time mouse-rendezvous autopoll crutch (see field comment) */
    m->autopoll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     maclc550_egret_autopoll, m);
    m->autopoll_armed = true;
    timer_mod(m->autopoll_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 20 * 1000000);

    m->vbl_off_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maclc550_vbl_off, m);
    m->sixty_hz_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maclc550_sixty_hz, m);
    timer_mod(m->sixty_hz_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VIA_60HZ_TIMER_PERIOD_NS);
    m->one_second_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, maclc550_one_second,
                                       m);
    timer_mod(m->one_second_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), MACLC550_GLUE_VIA1));
    memory_region_init_io(&m->via1mem, OBJECT(machine), &maclc550_via1_ops,
                          &m->via1, "via1", VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, VIA1_OFS, &m->via1mem);

    /* RBV's VIA2-emulation window at the classic VIA2 site */
    memory_region_init_io(&m->rbv_via2mem, OBJECT(machine),
                          &maclc550_rbv_via2_ops, m, "rbv-via2",
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
                                           MACLC550_GLUE_SCC));
    memory_region_add_subregion(&m->macio, SCC_OFS,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->escc),
                                                       0));

    /* EASC, as on lc475.c/maciisi.c (LC-family, not the IIci's discrete ASC) */
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
                       qemu_allocate_irq(maclc550_irq_sink, m, 0));

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
                          &maclc550_escc_mirror_ops, NULL, "escc-mirror",
                          0x2000);
    memory_region_add_subregion(&m->macio, SCC_OFS | 0x8000, &m->escc_mirror);

    memory_region_init_alias(&m->swim_mirror, OBJECT(machine), "swim-mirror",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->swim),
                                                    0),
                             0, 0x2000);
    memory_region_add_subregion(&m->macio, SWIM_OFS | 0x8000, &m->swim_mirror);

    /* RBV-style VIA2/interrupt register block (no video here: see fb below) */
    memory_region_init_io(&m->rbvmem, OBJECT(machine), &maclc550_rbv_ops, m,
                          "rbv", 0x2000);
    memory_region_add_subregion(&m->macio, RBV_OFS, &m->rbvmem);

    /* VDAC stub: present-but-inert, see maclc550_vdac_ops comment */
    memory_region_init_io(&m->vdacmem, OBJECT(machine), &maclc550_vdac_ops,
                          m, "vdac", 0x40);
    memory_region_add_subregion(&m->macio, VDAC_OFS, &m->vdacmem);

    /*
     * Sonora ID/config register bank at +0x28000 (right after the RBV
     * window): the ROM's early bring-up (0x40804b4e) reads a byte here
     * and masks/compares it (family/sub-model detection, empirically --
     * this address is otherwise unmapped in the real RBV/VDAC decode).
     * Traced live: with this address BUS-ERRORING (unmapped), the probe's
     * fault handler aborts the whole bring-up path taken afterwards ends
     * up dereferencing an uninitialised low-memory family-record jump
     * vector and crashes into address 0.  Stubbing it present (all bits
     * set, like idle/pulled-up unmapped-but-decoded space) avoids the
     * fault and lets bring-up continue to build that table properly.
     * Exact real bit meaning not identified yet -- see CCLASSIC-NOTES.md.
     */

    memset(m->sonoraid_regs, 0xff, sizeof(m->sonoraid_regs));
    /*
     * +0x02 is the Sonora video controller's MONITOR-SENSE register:
     * the ROM's kind-7 capability search (0x40804b4e) reads it, masks
     * bits 4-6 (& 0x70), and uses the Apple 3-bit sense code there to
     * pick the built-in display's video-mode capability record --
     * sense code 2 (0x20) = 12" RGB 512x384, code 6 (0x60) = 13"/14"
     * RGB 640x480.  The LC 550 has a built-in 640x480 display (code 6);
     * the Color Classic II a 512x384 one (code 2).  Drive it from the
     * per-machine monitor_sense so each selects its own record (the
     * LC 550's +18=0x4a record, the Color Classic II's +18 record) --
     * a wrong code selects the wrong mode and the boot diverts into the
     * serial console instead of continuing.
     */
    m->sonoraid_regs[2] = (MACLC550_MACHINE_GET_CLASS(m)->monitor_sense & 7)
                          << 4;
    memory_region_init_io(&m->sonoraidmem, OBJECT(machine),
                          &maclc550_sonoraid_ops, m, "sonora-id", 0x40);
    memory_region_add_subregion(&m->macio, 0x28000, &m->sonoraidmem);

    /* Ariel-class colour DAC at +0x18000 (see maclc550_ariel_ops) */
    memory_region_init_io(&m->arielmem, OBJECT(machine), &maclc550_ariel_ops,
                          m, "ariel", 0x2000);
    memory_region_add_subregion(&m->macio, 0x18000, &m->arielmem);

    /* NCR5380 SCSI controller and its pseudo-DMA aperture (+0x2000) */
    object_initialize_child(OBJECT(machine), "scsi", &m->scsi, TYPE_NCR5380);
    qdev_prop_set_uint8(DEVICE(&m->scsi), "reg-shift", 4);
    sysbus = SYS_BUS_DEVICE(&m->scsi);
    sysbus_realize(sysbus, &error_fatal);
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(maclc550_scsi_irq, m, 0));
    memory_region_add_subregion(&m->macio, SCSI_OFS,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_init_io(&m->scsi_pdma, OBJECT(machine),
                          &maclc550_scsi_pdma_ops, &m->scsi, "scsi-pdma", 0x2000);
    memory_region_add_subregion(&m->macio, SCSI_OFS + 0x2000, &m->scsi_pdma);

    /* handshake pseudo-DMA aperture ("SCSI+DRQ", 0x50F06000) */
    memory_region_init_io(&m->scsi_hsk, OBJECT(machine),
                          &maclc550_scsi_hsk_ops, &m->scsi, "scsi-hsk", 0x2000);
    memory_region_add_subregion(&m->macio, 0x6000, &m->scsi_hsk);

    scsi_bus_legacy_handle_cmdline(&m->scsi.bus);

    /*
     * Valkyrie/CSC video control registers (CLUT + monitor sense),
     * ported from lc475.c -- see maclc550_valkyrie_ops.
     */
    {
        static MacLc550RegBank valkyrie_bank;

        valkyrie_bank = (MacLc550RegBank){ "valkyrie", m->valkyrie_regs,
                                           sizeof(m->valkyrie_regs), 8000 };
        m->valkyrie_bank = &valkyrie_bank;
        memory_region_init_io(&m->valkyrie_mem, OBJECT(machine),
                              &maclc550_valkyrie_ops, m,
                              "valkyrie", sizeof(m->valkyrie_regs));
        memory_region_add_subregion(get_system_memory(), 0xf9800000,
                                    &m->valkyrie_mem);
    }

    /*
     * Low-priority catch-all logging stub covering the whole 0x60000000
     * Sonora device window; the real VRAM below overlaps it at higher
     * priority.  Keeps any as-yet-unmodelled register probes elsewhere in
     * the window from bus-faulting the config POST.
     */
    memory_region_init_io(&m->sonora_probe, OBJECT(machine),
                          &sonora_probe_ops, m, "sonora-probe", 0x01000000);
    memory_region_add_subregion_overlap(get_system_memory(), 0x60000000,
                                        &m->sonora_probe, 0);

    /* Onboard video framebuffer at its physical decode */
    object_initialize_child(OBJECT(machine), "fb", &m->fb, TYPE_MACLC550_FB);
    sysbus = SYS_BUS_DEVICE(&m->fb);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(), MACLC550_VRAM_BASE,
                                        sysbus_mmio_get_region(sysbus, 0), 1);
    /*
     * The VRAM aperture doesn't decode the upper address bits: the
     * ROM's video driver may size VRAM by looking for aliasing (a BERR
     * there would be fatal), so wrap the VRAM through the whole 8 MiB
     * window below the control registers (cf. lc475.c).
     */
    for (i = 0; i < ARRAY_SIZE(m->vram_aliases); i++) {
        memory_region_init_alias(&m->vram_aliases[i], NULL,
                                 "maclc550.vram-alias",
                                 sysbus_mmio_get_region(sysbus, 0),
                                 0, MACLC550_VRAM_SIZE);
        memory_region_add_subregion_overlap(get_system_memory(),
                                    MACLC550_VRAM_BASE +
                                    (i + 1) * MACLC550_VRAM_SIZE,
                                    &m->vram_aliases[i], 1);
    }

    /* ROM */
    memory_region_init_rom(&m->rom, NULL, "maclc550.rom", MACLC550_ROM_SIZE,
                           &error_abort);
    memory_region_add_subregion(get_system_memory(), MACLC550_ROM_ADDR,
                                &m->rom);

    memory_region_init_alias(&m->rom_alias, NULL, "maclc550.rom-alias",
                             &m->rom, 0, MACLC550_ROM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x40000000,
                                &m->rom_alias);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, MACLC550_ROM_ADDR,
                                        MACLC550_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }

    if (!qtest_enabled()) {
        if (bios_size <= 0 || bios_size > MACLC550_ROM_SIZE) {
            error_report("could not load MacROM '%s'", bios_name);
            exit(1);
        }

        cs = CPU(&m->cpu);
        ptr = rom_ptr(MACLC550_ROM_ADDR, bios_size);
        assert(ptr != NULL);
        stl_phys(cs->as, 0, ldl_be_p(ptr));    /* reset initial SP */
        stl_phys(cs->as, 4,
                 MACLC550_ROM_ADDR + ldl_be_p(ptr + 4)); /* reset initial PC */

        /*
         * Relocation-base fixup, same bug family as macclassicii.c's
         * device_table[0] patch.  Very early bring-up (ROM 0x40802e00)
         * locates a small header by scanning backward from a fixed
         * code address (0x40803d52) for a self-referential longword
         * matching that address -- this finds a table entry that is
         * valid regardless of where the ROM actually runs, since the
         * comparison value is computed PC-relative at runtime.  Having
         * found it, the code steps back 2 entries (8 bytes) to fetch a
         * SECOND value used as `d3 = *that - a2` (a2 = actual running
         * base, 0x40800000 here), then jumps to (0x40802e3e + d3).
         *
         * Traced live (-d in_asm,cpu register dumps): the matched entry
         * at ROM file offset 0x3d4a holds 0x40803d52, the actual
         * running address of the scan's anchor point -- consistent, so
         * the SAME entry's own "canonical" pairing is self-consistent
         * by construction.  The entry 2 slots back, file offset 0x3d42,
         * is a DIFFERENT variant used for the delta and holds
         * 0x00a03d52 -- a 24-BIT-position address (canonical base
         * 0x00a00000), not the 32-bit in-place one.  d3 computes to a
         * huge negative delta and the jump lands at raw address
         * 0x00a06b90 -- unbacked RAM, which the CPU runs through
         * forever as an endless stream of decoded-as-zero
         * `ori.b #0,d0` words (never bus faults, so nothing else about
         * the boot ever shows up in the logs).  Patch the 24-bit
         * variant to the 32-bit in-place equivalent (0x40800000 +
         * 0x3d52) so the delta comes out the same small, in-ROM value
         * (0x3d52) the matched entry's own pairing already implies,
         * landing the jump back inside the ROM instead of into RAM.
         */
        if (bios_size > 0x3d42 + 4 &&
            ldl_be_p(ptr + 0x3d42) == 0x00a03d52 &&
            ldl_be_p(ptr + 0x3d4a) == MACLC550_ROM_ADDR + 0x3d52) {
            stl_be_p(ptr + 0x3d42, MACLC550_ROM_ADDR + 0x3d52);
            /*
             * Re-sign the ROM after the relocation patch.  The power-on
             * self-test at ROM 0x40847a24 sums the ROM as 16-bit words
             * (into a 32-bit accumulator) starting at ROM+4 and compares
             * the result against the stored 32-bit checksum longword at
             * ROM+0 (0xede66cbd); a mismatch sets d6=0xffff and the boot
             * diverts to the serial diagnostic console (via 0x408472d0 ->
             * 0x4084a6f6 -> 0x408b989a) instead of continuing to StartBoot.
             * The relocation patch just above raised the high word at
             * offset 0x3d42 from 0x00a0 to 0x4080, i.e. it added
             * (0x4080-0x00a0)=0x3fe0 to that word-sum, so bump the stored
             * checksum by the same amount to keep the self-test passing.
             * (ROM+0 is read as the comparison target *before* the sum
             * loop starts at ROM+4, so it is itself outside the summed
             * range and this adjustment does not perturb the sum.)
             */
            stl_be_p(ptr, ldl_be_p(ptr) + (0x4080 - 0x00a0));
        }
    }
}

static GlobalProperty hw_compat_maclc550[] = {
    { "scsi-hd", "quirk_mode_page_vendor_specific_apple", "on" },
    { "scsi-hd", "vendor", " SEAGATE" },
    { "scsi-hd", "product", "          ST225N" },
    { "scsi-hd", "ver", "1.0 " },
    { "scsi-cd", "quirk_mode_page_apple_vendor", "on" },
    { "scsi-cd", "quirk_mode_sense_rom_use_dbd", "on" },
    { "scsi-cd", "quirk_mode_page_vendor_specific_apple", "on" },
    { "scsi-cd", "quirk_mode_page_truncated", "on" },
    { "scsi-cd", "vendor", "MATSHITA" },
    { "scsi-cd", "product", "CD-ROM CR-8005" },
    { "scsi-cd", "ver", "1.0k" },
};
static const size_t hw_compat_maclc550_len = G_N_ELEMENTS(hw_compat_maclc550);

static void maclc550_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68030"),
        NULL
    };
    const MacLc550MachineData *md = data;
    MacLc550MachineClass *lmc = MACLC550_MACHINE_CLASS(oc);
    MachineClass *mc = MACHINE_CLASS(oc);

    lmc->machine_id = md->machine_id;
    lmc->monitor_sense = md->monitor_sense;
    mc->desc = md->desc;
    mc->init = maclc550_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->block_default_type = IF_SCSI;
    mc->default_ram_size = 16 * MiB;
    mc->default_ram_id = "maclc550.ram";
    machine_add_audiodev_property(mc);
    compat_props_add(mc->compat_props, hw_compat_maclc550,
                     hw_compat_maclc550_len);
}

static const MacLc550MachineData maclc550_data = {
    .desc = "Macintosh LC 550",
    .machine_id = MACLC550_MACHINE_ID,
    .monitor_sense = 6,          /* Apple sense code 6 = 13" 640x480 RGB */
};

static const MacLc550MachineData colorclassicii_data = {
    .desc = "Macintosh Color Classic II",
    .machine_id = MACCCLASSICII_MACHINE_ID,
    /*
     * Built-in fixed 512x384 colour CRT = Apple monitor sense code 2
     * (12" RGB), which the kind-7 capability search (ROM 0x40804b4e)
     * matches against the Color Classic's video-mode record.  (The
     * LC 550 uses code 6 = 640x480.)
     */
    .monitor_sense = 2,
};

static void maclc550_machine_alias_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    maclc550_machine_class_init(oc, data);
    /*
     * Performa 550/560 share the LC 550's Gestalt machine ID and
     * hardware on real machines (Linux bootinfo-mac.h: MAC_MODEL_P550
     * covers all three) -- register as a plain alias rather than a
     * separate machine-ID value.
     */
    mc->alias = "performa550";
}

static const TypeInfo maclc550_machine_typeinfo[] = {
    {
        .name       = TYPE_MACLC550_GLUE,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacLc550GlueState),
        .instance_init = maclc550_glue_init,
        .class_init = maclc550_glue_class_init,
    },
    {
        .name       = TYPE_MOS6522_MACLC550,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522MacLc550State),
        .instance_init = mos6522_maclc550_init,
        .class_init = mos6522_maclc550_class_init,
    },
    {
        .name       = TYPE_MACLC550_FB,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacLc550FbState),
        .class_init = maclc550_fb_class_init,
    },
    {
        .name       = TYPE_MACLC550_MACHINE,
        .parent     = TYPE_MACHINE,
        .abstract   = true,
        .instance_size = sizeof(MacLc550MachineState),
        .class_size = sizeof(MacLc550MachineClass),
    },
    {
        .name       = MACHINE_TYPE_NAME("maclc550"),
        .parent     = TYPE_MACLC550_MACHINE,
        .class_init = maclc550_machine_alias_init,
        .class_data = &maclc550_data,
    },
    {
        .name       = MACHINE_TYPE_NAME("colorclassicii"),
        .parent     = TYPE_MACLC550_MACHINE,
        .class_init = maclc550_machine_class_init,
        .class_data = &colorclassicii_data,
    },
};

DEFINE_TYPES(maclc550_machine_typeinfo)
