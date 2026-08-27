/*
 * QEMU Macintosh II / IIx / IIcx hardware system emulator
 *
 * The original open-architecture Macs: classic VIA1 ADB transceiver, a
 * REAL discrete VIA2 (Linux via_type MAC_VIA_II -- the two-chip VIA
 * pair) combined with VIA1/SCC/NMI into the CPU's autovector IPL lines
 * by the "GLUE" ASIC, ASC sound, SWIM floppy, NCR5380 SCSI, Z8530 SCC,
 * and -- unlike every compact Mac -- NO onboard video: these machines
 * need a NuBus video card.  Mac II = 68020 with six NuBus slots
 * ($9-$E); IIx = the same box with a 68030; IIcx = a compact 68030
 * three-slot ($9-$B) variant.
 *
 * Derived from macse30.c (the SE/30 is electrically this machine in a
 * compact case: same ROM, same VIA1 ADB transceiver + RTC engine, same
 * discrete VIA2/GLUE, same SCC/ASC/SWIM/SCSI layout and address map,
 * all kept unchanged) with the SE/30's pseudo-slot-$E onboard video
 * replaced by a real NuBus bus (mac-nubus-bridge, as q800/maciisi):
 * a card's slot IRQ pulls VIA2 PA0-PA5 low (active-low, slot $9-$E)
 * and edges VIA2 CA1 ("any slot"), the exact scheme the SE/30's
 * pseudo-slot VBL already proved out against this ROM's slot-interrupt
 * dispatcher.  Video comes from a NuBus card, e.g.:
 *
 *   -device radius-24xp,slot=9,romfile=<24Xp declaration bus ROM>
 *
 * ROM: macIIx.rom (the shared II-FDHD/IIx/IIcx/SE30 ROM, version
 * $0178); its reset vector is an ABSOLUTE address, handled by the
 * entry-detection logic below.  Machine-ID straps on VIA1 port A:
 * bit 6 clear = II/IIx, set = IIcx/SE30 (cf. MAME apple/macii.cpp);
 * II vs IIx are told apart by CPU type.
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
#include "system/runstate.h"
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
#include "ui/input.h"
#include "hw/nubus/mac-nubus-bridge.h"
#include "system/rtc.h"
#include "system/qtest.h"
#include "system/reset.h"

#define MACII_ROM_ADDR      0x40800000
#define MACII_ROM_SIZE      0x00040000
#define MACII_ROM_FILENAME  "macIIx.rom"

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
#define TYPE_MACII_GLUE "macii-glue"
OBJECT_DECLARE_SIMPLE_TYPE(MacIIGlueState, MACII_GLUE)

struct MacIIGlueState {
    SysBusDevice parent_obj;
    M68kCPU *cpu;
    uint8_t ipr;
};

#define MACII_GLUE_VIA1     0       /* level 1 */
#define MACII_GLUE_VIA2     1       /* level 2 */
#define MACII_GLUE_SCC      3       /* level 4 */
#define MACII_GLUE_NMI      6      /* level 7 */

static void macii_glue_set_irq(void *opaque, int irq, int level)
{
    MacIIGlueState *s = opaque;
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

static void macii_glue_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    MacIIGlueState *s = MACII_GLUE(dev);

    qdev_init_gpio_in(dev, macii_glue_set_irq, 8);
    s->ipr = 0;
}

static const Property macii_glue_properties[] = {
    DEFINE_PROP_LINK("cpu", MacIIGlueState, cpu, TYPE_M68K_CPU, M68kCPU *),
};

static void macii_glue_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, macii_glue_properties);
}

/*
 * VIA1 subclass.  The IIci has the real 343-0042 RTC/PRAM chip on PB0
 * (data), PB1 (clock), PB2 (/enable), and the classic ADB transceiver
 * on the shift register with PB4/PB5 as the transaction state lines
 * and PB3 as the transceiver interrupt.  RTC engine and ADB
 * transceiver ported from hw/misc/mac_via.c.
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

#define TYPE_MOS6522_MACII "mos6522-macii"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522MacIIState, MOS6522_MACII)

struct MOS6522MacIIState {
    MOS6522State parent_obj;

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

    /* classic ADB transceiver (cf. hw/misc/mac_via.c) */
    qemu_irq adb_data_ready;
    int adb_data_in_size;
    int adb_data_in_index;
    int adb_data_out_index;
    uint8_t adb_autopoll_cmd;
    uint8_t adb_data_in[128];
    uint8_t adb_data_out[16];

    /* boot-time autopoll crutch is armed only on the decl-ROM (video)
     * path and is torn back down (autopoll disabled) at the ROM's next
     * ADBReInit once the mouse rendezvous has completed -- see
     * macii_adb_autopoll_kick(), macii_adb_poll() and the BusReset
     * hook in macii_adb_send() */
    bool adb_quiesce_armed;
    bool adb_saw_172_set;
    bool adb_saw_172_cleared;
    bool adb_click_pressed;
};

/*
 * Real discrete VIA2 (Mac II/IIx/SE30/IIcx GLUE era): TYPE_MOS6522
 * itself is abstract, so a trivial concrete subclass with no
 * board-specific behaviour is enough to instantiate it.
 */
#define TYPE_MOS6522_MACII_VIA2 "mos6522-macii-via2"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522MacIIVIA2State, MOS6522_MACII_VIA2)

struct MOS6522MacIIVIA2State {
    MOS6522State parent_obj;

    /*
     * Live external level of the port A pins (slot IRQ lines $9-$E on
     * PA0-PA5, active LOW, pulled high when idle).  Input-configured
     * bits read this instead of the stale output latch.
     */
    uint8_t pins_a;
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

static void via1_rtc_update(MOS6522MacIIState *v1s)
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

    qemu_log_mask(LOG_UNIMP, "macii rtc: byte 0x%02x (cmd=%02x alt=%02x)\n",
                  v1s->data_out, v1s->cmd, v1s->alt);

    /* first byte: it's a command */
    if (v1s->cmd == REG_EMPTY) {
        cmd = via1_rtc_compact_cmd(v1s->data_out);

        if (cmd == REG_INVALID) {
            qemu_log_mask(LOG_UNIMP, "macii rtc: invalid cmd 0x%02x\n",
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
 * Classic ADB transceiver on VIA1 (as on the Mac II/SE/Quadra family):
 *   PB5/PB4 out  ST1/ST0 transaction state (0 new cmd, 1 even byte,
 *                2 odd byte, 3 idle)
 *   PB3 in       transceiver interrupt (autopoll data present / bus
 *                timeout / SRQ, depending on transaction phase)
 * Data moves through the VIA shift register; each shifted byte raises
 * the SR interrupt.  Model ported from hw/misc/mac_via.c (q800).
 */

#define VIA1B_vADBInt        0x08
#define VIA1B_vADB_StateMask 0x30
#define VIA1B_vADB_StateShift 4

#define ADB_STATE_NEW       0
#define ADB_STATE_EVEN      1
#define ADB_STATE_ODD       2
#define ADB_STATE_IDLE      3

static uint32_t macii_trace_pc(void); /* defined below */

static void macii_adb_poll(void *opaque)
{
    MOS6522MacIIState *v1s = MOS6522_MACII(opaque);
    MOS6522State *s = MOS6522(v1s);
    ADBBusState *adb_bus = &v1s->adb_bus;
    uint8_t obuf[9];
    uint8_t *data = &s->sr;
    int olen;

    /*
     * The boot-time autopoll crutch (macii_adb_autopoll_kick()) exists
     * ONLY to complete the ROM's post-video mouse rendezvous -- the spin
     * on low-mem flag 0x172 at ROM 0x40802432 (a loop that rendezvouses
     * every enumerated ADB device in turn, re-arming 0x172 for each).
     * Observe that rendezvous here (0x172, first seen SET by the ROM's
     * own init store at 0x4080038a, later cleared once the mouse
     * completion callback fires) but do NOT tear the crutch down yet:
     * that happens in macii_adb_send() on the ROM's next ADBReInit (the
     * BusReset that begins it), by which point every device in the loop
     * has completed -- see the comment there for why the crutch must be
     * gone before ADBReInit's enumeration runs.  We require having seen
     * 0x172 with bit 7 set before honouring a later 0, since our kick can
     * run before the ROM's init store and freshly-cleared RAM reads 0.
     */
    if (v1s->adb_quiesce_armed) {
        uint8_t flag172 = address_space_ldub(&address_space_memory, 0x172,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t pc = macii_trace_pc();

        if (flag172 & 0x80) {
            v1s->adb_saw_172_set = true;
        }
        /*
         * When the ROM is actually parked on its 0x172 rendezvous spin
         * (interrupted PC in [0x40802400,0x40802460]) and 0x172 is still
         * set, hold the synthetic mouse button DOWN and force the mouse to
         * re-report that held state on every autopoll.  Each such reply
         * carries byte-0 bit 7 = 0 ("button down"), the exact edge the
         * ROM's device-3 completion callback needs to clear 0x172.  Two
         * subtleties, both learned the hard way here:
         *   - a static button hold is reported by an ADB mouse only ONCE
         *     (it answers Talk R0 only on a state change), and a single
         *     reply is not reliably caught by the one poll the ROM happens
         *     to service, so we must re-arm a report every poll -- but via
         *     adb_mouse_force_report() (which just makes the device
         *     re-announce its current state), NOT via synthetic cursor
         *     movement: the ROM's immediate post-rendezvous startup
         *     compares the live cursor position (low-mem 'Mouse', 0x830)
         *     against screen regions and hangs elsewhere if it has moved.
         *   - toggling the button instead would inject "button up" replies
         *     too, which re-SET 0x172, so the spin might never observe the
         *     0 -- holding it down keeps every reply a clearing one.
         * Tying this to the live spin PC (not a fixed delay from machine
         * start) is what makes it reliable under -icount, where the spin
         * is reached only tens of virtual seconds in at a run-dependent
         * moment.
         */
        if (v1s->adb_saw_172_set && (flag172 & 0x80) &&
            pc >= 0x40802400 && pc <= 0x40802460) {
            if (!v1s->adb_click_pressed) {
                v1s->adb_click_pressed = true;
                qemu_input_queue_btn(NULL, INPUT_BUTTON_LEFT, true);
                qemu_input_event_sync();
            }
            adb_mouse_force_report(adb_bus);
        }
        if (v1s->adb_saw_172_set && flag172 == 0 &&
            !v1s->adb_saw_172_cleared) {
            /* rendezvous done -- release the button and go quiet */
            v1s->adb_saw_172_cleared = true;
            if (v1s->adb_click_pressed) {
                v1s->adb_click_pressed = false;
                qemu_input_queue_btn(NULL, INPUT_BUTTON_LEFT, false);
                qemu_input_event_sync();
            }
        }
    }

    /*
     * Setting vADBInt below indicates that an autopoll reply has been
     * received, however we must block autopoll until the point where
     * the entire reply has been read back to the host
     */
    adb_autopoll_block(adb_bus);

    if (v1s->adb_data_in_size > 0 && v1s->adb_data_in_index == 0) {
        /*
         * An existing response is still pending: report it as a "fake"
         * autopoll reply (cf. mac_via.c for the Linux motivation)
         */
        *data = v1s->adb_data_out[0];
        olen = v1s->adb_data_in_size;

        s->b &= ~VIA1B_vADBInt;
        qemu_irq_raise(v1s->adb_data_ready);
    } else {
        /* Otherwise poll as normal */
        v1s->adb_data_in_index = 0;
        v1s->adb_data_out_index = 0;
        olen = adb_poll(adb_bus, obuf, adb_bus->autopoll_mask);

        if (olen > 0) {
            /* Autopoll response */
            *data = obuf[0];
            olen--;
            memcpy(v1s->adb_data_in, &obuf[1], olen);
            v1s->adb_data_in_size = olen;

            s->b &= ~VIA1B_vADBInt;
            qemu_irq_raise(v1s->adb_data_ready);
        } else {
            *data = v1s->adb_autopoll_cmd;
            obuf[0] = 0xff;
            obuf[1] = 0xff;
            olen = 2;

            memcpy(v1s->adb_data_in, obuf, olen);
            v1s->adb_data_in_size = olen;

            s->b &= ~VIA1B_vADBInt;
            qemu_irq_raise(v1s->adb_data_ready);
        }
    }
}

static int macii_adb_send_len(uint8_t data)
{
    /* Determine the send length from the given ADB command */
    uint8_t cmd = data & 0xc;
    uint8_t reg = data & 0x3;

    switch (cmd) {
    case 0x8:
        /* Listen command */
        switch (reg) {
        case 2:
            /* Register 2 is only used for the keyboard */
            return 3;
        case 3:
            /*
             * Fortunately our devices only implement writes
             * to register 3 which is fixed at 2 bytes
             */
            return 3;
        default:
            qemu_log_mask(LOG_UNIMP, "ADB unknown length for register %d\n",
                          reg);
            return 1;
        }
    default:
        /* Talk, BusReset */
        return 1;
    }
}

static void macii_adb_send(MOS6522MacIIState *v1s, int state, uint8_t data)
{
    MOS6522State *ms = MOS6522(v1s);
    ADBBusState *adb_bus = &v1s->adb_bus;
    uint16_t autopoll_mask;

    switch (state) {
    case ADB_STATE_NEW:
        /*
         * Command byte: vADBInt tells host autopoll data already present
         * in VIA shift register and ADB transceiver
         */
        adb_autopoll_block(adb_bus);

        if (adb_bus->status & ADB_STATUS_POLLREPLY) {
            /* Tell the host the existing data is from autopoll */
            ms->b &= ~VIA1B_vADBInt;
        } else {
            ms->b |= VIA1B_vADBInt;
            v1s->adb_data_out_index = 0;
            v1s->adb_data_out[v1s->adb_data_out_index++] = data;
        }

        qemu_irq_raise(v1s->adb_data_ready);
        break;

    case ADB_STATE_EVEN:
    case ADB_STATE_ODD:
        ms->b |= VIA1B_vADBInt;
        v1s->adb_data_out[v1s->adb_data_out_index++] = data;
        qemu_irq_raise(v1s->adb_data_ready);
        break;

    case ADB_STATE_IDLE:
        ms->b |= VIA1B_vADBInt;
        adb_autopoll_unblock(adb_bus);
        /*
         * Whatever reply was pending is now moot -- the state machine is
         * back to idle, whether or not the guest actually read it back
         * byte-by-byte (this ROM's low-level ADB ISR skips straight to
         * IDLE without draining EVEN/ODD at all when it decides, from
         * vADBInt alone, that a Talk got no reply -- see
         * macii_adb_send()'s ADB_STATE_NEW case).  Without this,
         * macii_adb_poll()'s "an existing response is still pending"
         * fast path (adb_data_in_size > 0 && adb_data_in_index == 0)
         * would keep matching this leftover reply FOREVER, since
         * nothing else ever clears adb_data_in_size to 0 -- silently
         * replaying the same stale bytes on every future autopoll cycle
         * instead of ever calling adb_poll() to ask devices for their
         * actual current state again.
         */
        v1s->adb_data_in_size = 0;
        return;
    }

    /* If the command is complete, execute it */
    if (v1s->adb_data_out_index == macii_adb_send_len(v1s->adb_data_out[0])) {
        /* (some ROMs start commands on an EVEN/ODD transition) */
        if (!adb_bus->autopoll_blocked) {
            adb_autopoll_block(adb_bus);
        }
        v1s->adb_data_in_size = adb_request(adb_bus, v1s->adb_data_in,
                                            v1s->adb_data_out,
                                            v1s->adb_data_out_index);
        v1s->adb_data_in_index = 0;

        /*
         * Tear down the boot-time autopoll crutch (see macii_adb_poll())
         * exactly when the ROM begins its post-video ADBReInit -- signalled
         * by an ADB BusReset (command low nibble 0) issued AFTER the mouse
         * rendezvous has already completed (0x172 seen set then cleared).
         * ADBReInit runs a device-enumeration loop that installs each
         * device's completion vector into a table on a tiny boot stack;
         * were our autopoll timer still injecting mouse-data replies, a
         * VIA1 SR interrupt could land mid-enumeration and dispatch through
         * a half-written vector, sending the CPU to a garbage low address
         * (~0x1a) and the ROM into its SysError serial MicroBug monitor.
         * Disabling autopoll here restores the quiescent, autopoll-off ADB
         * the ROM's enumeration expects (the same state a no-video boot,
         * which never arms the crutch, is always in).  The earlier
         * PRE-video ADBReInit's BusReset is not affected: 0x172 has not
         * been cleared by then, so adb_saw_172_cleared is still false.
         */
        if ((v1s->adb_data_out[0] & 0x0f) == 0 && v1s->adb_quiesce_armed &&
            v1s->adb_saw_172_cleared) {
            v1s->adb_quiesce_armed = false;
            adb_set_autopoll_enabled(adb_bus, false);
        }

        if (adb_bus->status & ADB_STATUS_BUSTIMEOUT) {
            /*
             * Bus timeout (but allow first EVEN and ODD byte to indicate
             * timeout via vADBInt and SRQ status)
             */
            v1s->adb_data_in[0] = 0xff;
            v1s->adb_data_in[1] = 0xff;
            v1s->adb_data_in_size = 2;
        }

        /*
         * If last command is TALK, store it for use by autopoll and adjust
         * the autopoll mask accordingly.
         *
         * NOTE: this ORs the newly-talked device into the existing mask
         * rather than replacing it outright (as hw/misc/mac_via.c's
         * otherwise-identical q800 original does).  Replacing meant
         * whichever device the ROM happened to explicitly Talk to LAST
         * became the ONLY device autopoll would ever revisit -- on this
         * ROM's boot sequence that is the keyboard (a housekeeping Talk
         * from RAM-resident code right after the video/cursor-init
         * sequence's one-shot, explicit mouse Talk R0), silently
         * dropping the mouse back out of the autopoll set forever.  The
         * mouse's own registered ADB completion callback (which clears
         * low-mem flag 0x172 and lets QuickDraw's first desktop draw
         * proceed) is only ever invoked by THIS ROM through a reply that
         * arrives via the autopoll/POLLREPLY path -- an explicit
         * one-shot Talk's reply is, by this ROM's own design, treated as
         * inconclusive/no-op when the addressed device is merely idle
         * (the normal case for a mouse nobody is touching), so without
         * autopoll continuing to cover device 3 the completion can never
         * fire and the boot hangs forever before the first desktop draw.
         */
        if ((v1s->adb_data_out[0] & 0xc) == 0xc) {
            v1s->adb_autopoll_cmd = v1s->adb_data_out[0];

            autopoll_mask = adb_bus->autopoll_mask |
                             (1 << (v1s->adb_autopoll_cmd >> 4));
            adb_set_autopoll_mask(adb_bus, autopoll_mask);
        }
    }
}

static void macii_adb_receive(MOS6522MacIIState *v1s, int state,
                               uint8_t *data)
{
    MOS6522State *ms = MOS6522(v1s);
    ADBBusState *adb_bus = &v1s->adb_bus;
    uint16_t pending;

    switch (state) {
    case ADB_STATE_NEW:
        ms->b |= VIA1B_vADBInt;
        return;

    case ADB_STATE_IDLE:
        ms->b |= VIA1B_vADBInt;
        adb_autopoll_unblock(adb_bus);
        /* see the matching comment in macii_adb_send()'s IDLE case */
        v1s->adb_data_in_size = 0;
        break;

    case ADB_STATE_EVEN:
    case ADB_STATE_ODD:
        switch (v1s->adb_data_in_index) {
        case 0:
            /* First EVEN byte: vADBInt indicates bus timeout */
            *data = v1s->adb_data_in[v1s->adb_data_in_index];
            if (adb_bus->status & ADB_STATUS_BUSTIMEOUT) {
                ms->b &= ~VIA1B_vADBInt;
            } else {
                ms->b |= VIA1B_vADBInt;
            }
            v1s->adb_data_in_index++;
            break;

        case 1:
            /* First ODD byte: vADBInt indicates SRQ */
            *data = v1s->adb_data_in[v1s->adb_data_in_index];
            pending = adb_bus->pending & ~(1 << (v1s->adb_autopoll_cmd >> 4));
            if (pending) {
                ms->b &= ~VIA1B_vADBInt;
            } else {
                ms->b |= VIA1B_vADBInt;
            }
            v1s->adb_data_in_index++;
            break;

        default:
            /*
             * Otherwise vADBInt indicates end of data. Note that Linux
             * specifically checks for the sequence 0x0 0xff to confirm the
             * end of the poll reply, so provide these extra bytes below to
             * keep it happy
             */
            if (v1s->adb_data_in_index < v1s->adb_data_in_size) {
                /* Next data byte */
                *data = v1s->adb_data_in[v1s->adb_data_in_index];
                ms->b |= VIA1B_vADBInt;
            } else if (v1s->adb_data_in_index == v1s->adb_data_in_size) {
                if (adb_bus->status & ADB_STATUS_BUSTIMEOUT) {
                    /* Bus timeout (no more data) */
                    *data = 0xff;
                } else {
                    /* Return 0x0 after reply */
                    *data = 0;
                }
                ms->b &= ~VIA1B_vADBInt;
            } else {
                /* Bus timeout (no more data) */
                *data = 0xff;
                ms->b &= ~VIA1B_vADBInt;
                adb_bus->status = 0;
                adb_autopoll_unblock(adb_bus);
            }

            if (v1s->adb_data_in_index <= v1s->adb_data_in_size) {
                v1s->adb_data_in_index++;
            }
            break;
        }

        qemu_irq_raise(v1s->adb_data_ready);
        break;
    }
}

static void macii_adb_update(MOS6522MacIIState *v1s)
{
    MOS6522State *s = MOS6522(v1s);
    int oldstate, state;

    oldstate = (v1s->last_b & VIA1B_vADB_StateMask) >> VIA1B_vADB_StateShift;
    state = (s->b & VIA1B_vADB_StateMask) >> VIA1B_vADB_StateShift;

    if (state != oldstate) {
        if (s->acr & SR_OUT) {
            /* output mode */
            macii_adb_send(v1s, state, s->sr);
        } else {
            /* input mode */
            macii_adb_receive(v1s, state, &s->sr);
        }
    }
}

static void macii_via1_portA_write(MOS6522State *s)
{
}

static void macii_via1_portB_write(MOS6522State *s)
{
    /*
     * PB4/PB5 (the ADB transceiver ST1/ST0 state lines, cf.
     * VIA1B_vADB_StateMask) are host-driven outputs with no other
     * source modeled here; mos6522's generic ORB/IRB handling only
     * updates bits that DDRB currently claims as outputs and silently
     * retains whatever was last latched for the rest (see
     * mos6522_write's `s->b = (s->b & ~dirb) | (val & dirb)`).  Real
     * VIA1 hardware reads floating/undriven state-line pins as the
     * idle bus level (ADB_STATE_IDLE == 3, all ones) rather than a
     * stale zero.  Without this, an early ROM VIA self-test that
     * walks DDRB through an all-output/all-input pattern (observed on
     * the II/IIx/SE30 ROM before the ADB Manager ever touches this
     * VIA) can leave the ST1/ST0 latch bits stuck at 0 ("new
     * transaction") once DDRB reverts those two bits to inputs; the
     * ADB Manager's very first real transition to ADB_STATE_NEW (also
     * 0) then reads as *no* state change at all (old == new == 0),
     * macii_adb_update() never calls macii_adb_send(), the ADB
     * SR interrupt never fires, and the ROM's ADBReInit wait loop
     * (btst of its busy flag) spins forever -- this was traced live
     * with gdb + a temporary trace and confirmed zero adb send/receive
     * calls ever fired.  Force state-line bits that DDRB currently
     * marks as inputs back to the idle level on every ORB write, so
     * the latch can never carry a stale non-idle value across a DDRB
     * direction change.
     */
    s->b |= VIA1B_vADB_StateMask & ~s->dirb;
}

static void mos6522_macii_init(Object *obj)
{
    MOS6522MacIIState *v1s = MOS6522_MACII(obj);

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;

    /* the ADB bus hangs off the VIA1 shift-register transceiver */
    qbus_init(&v1s->adb_bus, sizeof(v1s->adb_bus), TYPE_ADB_BUS,
              DEVICE(obj), "adb.0");
}

static void mos6522_macii_reset_hold(Object *obj, ResetType type)
{
    MOS6522MacIIState *v1s = MOS6522_MACII(obj);
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

    v1s->adb_data_in_size = 0;
    v1s->adb_data_in_index = 0;
    v1s->adb_data_out_index = 0;
}

static const Property mos6522_macii_properties[] = {
    DEFINE_PROP_UINT8("pins-a", MOS6522MacIIState, pins_a, 0xff),
    DEFINE_PROP_UINT8("pins-b", MOS6522MacIIState, pins_b, 0xff),
};

static void mos6522_macii_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    MOS6522DeviceClass *mdc = MOS6522_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_props(dc, mos6522_macii_properties);
    mdc->portA_write = macii_via1_portA_write;
    mdc->portB_write = macii_via1_portB_write;
    resettable_class_set_parent_phases(rc, NULL, mos6522_macii_reset_hold,
                                       NULL, &mdc->parent_phases);
}

/* machine */

struct MacIIMachineState {
    MachineState parent_obj;

    M68kCPU cpu;
    MacIIGlueState glue;
    MOS6522MacIIState via1;
    MOS6522MacIIVIA2State via2; /* real discrete VIA2 (Mac II/SE30 GLUE era) */
    ESCCState escc;
    OrIRQState escc_orgate;
    ASCState asc;
    Swim swim;
    NCR5380State scsi;
    MacNubusBridge mac_nubus_bridge;

    MemoryRegion rom;
    MemoryRegion rom_alias;
    MemoryRegion rom_alias24;
    /* 68020 HMMU 24-bit map (macii only): 0x800000-0xFFFFFF window */
    MemoryRegion hmmu24;
    /* 68020 only: low-24 decode for bit31-clear tagged pointers */
    MemoryRegion tag24_2x;
    MemoryRegion tag24_4x;
    MemoryRegion tag24_6x;
    MemoryRegion tag24_ram[4];
    /* per-slot 24-bit-region mirror at standard-slot offset (slot << 20) */
    MemoryRegion slot24_mirror[MAC_NUBUS_SLOT_NB];
    MemoryRegion ramio;
    MemoryRegion ramio_a31;
    MemoryRegion macio;
    MemoryRegion macio_alias;
    MemoryRegion macio_alias24;
    MemoryRegion via1mem;
    MemoryRegion via2mem;
    MemoryRegion escc_mirror;
    MemoryRegion swim_mirror;
    MemoryRegion scsi_pdma;
    MemoryRegion scsi_hsk;
    MemoryRegion iotrace;

    /* VIA1 CA1 60Hz tick and CA2 one-second interrupts */
    QEMUTimer *sixty_hz_timer;
    QEMUTimer *one_second_timer;
    /* one-shot: re-assert ADB autopoll-enabled after reset (see
     * macii_adb_autopoll_kick()) */
    QEMUTimer *adb_autopoll_kick_timer;
};

typedef struct MacIIMachineState MacIIMachineState;

struct MacIIMachineClass {
    MachineClass parent_class;

    uint8_t via1_pins_a;        /* machine-ID straps (bit6: IIcx/SE30) */
    uint16_t nubus_slot_mask;   /* II/IIx: $9-$E; IIcx: $9-$B */
    bool hmmu;                  /* 68020 Mac II: Apple HMMU, not a PMMU */
};
typedef struct MacIIMachineClass MacIIMachineClass;

#define TYPE_MACII_MACHINE MACHINE_TYPE_NAME("macii-common")
DECLARE_OBJ_CHECKERS(MacIIMachineState, MacIIMachineClass,
                     MACII_MACHINE, TYPE_MACII_MACHINE)

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

/*
 * Kick the ADB engine once, right after boot, so this ROM's post-video
 * cursor-init code (which spins on low-mem byte 0x172 forever otherwise,
 * blocking QuickDraw's very first desktop draw) can actually complete.
 * Three independent things are needed here, all discovered by live
 * tracing/disassembly against the ROM's own low-level ADB ISR
 * (0x40807002 on this ROM build):
 *
 * 1. autopoll must actually be enabled once the guest starts running.
 *    ADBBusState's own qdev/resettable reset_hold (adb_bus_reset_hold(),
 *    in hw/input/adb.c) unconditionally sets autopoll_enabled = false on
 *    every system reset -- including the one QEMU runs automatically
 *    right after machine init/realize, which happens AFTER our one-time
 *    adb_set_autopoll_enabled(adb_bus, true) call at device-creation
 *    time, silently undoing it before the guest CPU ever executes a
 *    single instruction.  (Confirmed live via temporary tracing: a
 *    `qemu_register_reset()` handler tried first actually runs BEFORE
 *    adb_bus_reset_hold(), not after -- legacy `qemu_register_reset`
 *    callbacks and the nested qdev tree's own resettable reset_hold
 *    phases are not simply ordered by registration order, so
 *    re-asserting from another reset hook loses the same race.)  A
 *    one-shot QEMU_CLOCK_VIRTUAL timer fired at machine start
 *    unconditionally runs strictly after the whole reset sequence
 *    (timers are only serviced once the main loop starts running, which
 *    is after qemu_system_reset() has fully completed), so it reliably
 *    wins the race no reset-time hook can.
 *
 * 2. the mouse (ADB device 3) must stay covered by the autopoll mask.
 *    macii_adb_send() narrows adb_bus->autopoll_mask to whichever
 *    device was most recently the target of an explicit Talk (see the
 *    comment there) -- on this ROM's boot sequence that ends up being
 *    the keyboard, not the mouse, dropping device 3 out of the autopoll
 *    set right after the ROM's one-shot explicit mouse Talk R0.  Fixed
 *    at the source in macii_adb_send().
 *
 * 3. the ROM's low-mem flag 0x172 is only ever updated by an *edge* on
 *    the mouse reply's button-state bit (bit 7 of the reply's first
 *    byte): the completion callback the ROM registers for device 3 XORs
 *    the incoming reply's bit 7 against 0x172's current value and does
 *    nothing at all unless they differ.  0x172 is initialised to 0x80
 *    (bit 7 set, "button up") long before ADB even starts, and an idle
 *    ADB mouse's replies (real data or our timeout placeholder alike)
 *    always report bit 7 set ("button up") too -- so on a boot where
 *    nobody ever touches the mouse, that bit never actually changes and
 *    0x172 can never clear no matter how many times the mouse is
 *    autopolled.  macii_adb_poll() therefore holds a synthetic
 *    left-button DOWN and forces the mouse to re-announce that held
 *    state on every poll -- but only while the ROM is actually parked on
 *    its 0x172 spin (PC-gated) and WITHOUT injecting any cursor
 *    movement; see the long comment there for why both constraints
 *    matter.  Once 0x172 clears, the button is released and the mouse
 *    goes back to reporting "no button", so this has no lasting effect
 *    beyond unblocking this one ROM-level rendezvous.
 */
static void macii_adb_autopoll_kick(void *opaque)
{
    MacIIMachineState *m = opaque;
    ADBBusState *adb_bus = &m->via1.adb_bus;

    adb_set_autopoll_enabled(adb_bus, true);
    m->via1.adb_quiesce_armed = true;
    /* the synthetic button toggling is done in macii_adb_poll() only
     * once the ROM is observed parked on its 0x172 rendezvous spin */
}

static uint32_t macii_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

static uint64_t ramio_read(void *opaque, hwaddr addr, unsigned size)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "macii ram-hole: read  0x%08x (%d) pc=0x%08x\n",
                      (unsigned)addr, size, macii_trace_pc());
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
                      "macii ram-hole: write 0x%08x (%d) <- 0x%08" PRIx64
                      " pc=0x%08x\n",
                      (unsigned)addr, size, val, macii_trace_pc());
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

static MemTxResult macii_a31_read(void *opaque, hwaddr addr, uint64_t *data,
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

static MemTxResult macii_a31_write(void *opaque, hwaddr addr, uint64_t value,
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

static const MemoryRegionOps macii_a31_ops = {
    .read_with_attrs = macii_a31_read,
    .write_with_attrs = macii_a31_write,
    .endianness = DEVICE_BIG_ENDIAN,
    /*
     * .valid.max = 8: QuickDraw's bitfield blitters (bfextu/bfins on
     * 24-bit-tagged bitmap pointers, e.g. 0x8000447x reads at ROM
     * 0x4081ade0) reach this region as single 8-byte accesses on the
     * 020 (no PMMU to strip the tag first); with max 4 the memory core
     * rejected them (txn fail size=8) = sad mac 0F/0001.  .impl stays
     * 4 so the core splits them.
     */
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* SCC mirror at +0xC000: the 8 SCC bytes repeat through the window */

static MemTxResult macii_escc_mirror_read(void *opaque, hwaddr addr,
                                            uint64_t *data, unsigned size,
                                            MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_read(opaque, addr, data, size, attrs);
}

static MemTxResult macii_escc_mirror_write(void *opaque, hwaddr addr,
                                             uint64_t val, unsigned size,
                                             MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_write(opaque, addr, val, size, attrs);
}

static const MemoryRegionOps macii_escc_mirror_ops = {
    .read_with_attrs = macii_escc_mirror_read,
    .write_with_attrs = macii_escc_mirror_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t macii_via1_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS6522MacIIState *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;
    uint64_t val;

    val = mos6522_read(s, reg, size);

    /* input pins on port A read the board straps, not the last output */
    if (reg == VIA_REG_A || reg == VIA_REG_ANH) {
        val = (val & s->dira) | (v1s->pins_a & ~s->dira);
    }

    return val;
}

static void macii_via1_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MOS6522MacIIState *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    mos6522_write(s, reg, val, size);

    if (reg == VIA_REG_SR || reg == VIA_REG_ACR) {
        qemu_log_mask(LOG_UNIMP,
                      "macii via1: reg%d <- 0x%02" PRIx64 " pc=0x%08x\n",
                      (int)reg, val, macii_trace_pc());
    }

    if (reg == VIA_REG_B) {
        via1_rtc_update(v1s);
        macii_adb_update(v1s);
        v1s->last_b = s->b;
    }
}

static const MemoryRegionOps macii_via1_ops = {
    .read = macii_via1_read,
    .write = macii_via1_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};


/*
 * Real discrete VIA2 (the Mac II/IIx/SE30/IIcx family GLUE era, NOT
 * the IIci's RBV-emulated VIA2 site): a second 6522 VIA chip at the
 * classic VIA2 site (slice +0x2000), VIA-spaced (offset >> 9) like
 * VIA1.  Its own IRQ output feeds the GLUE at level 2, summarising
 * SCSI and ASC (the only two interrupt sources modelled here -- no
 * NuBus slots on this machine).  Port A input bits read as pulled
 * HIGH when configured as inputs (NuBus-style pull-ups; the
 * accept-test machine-ID dance also samples a VIA2-position PA bit).
 * TYPE_MOS6522 itself is abstract, so a trivial concrete subclass with
 * no board-specific behaviour is enough to instantiate it.
 */

static void mos6522_macii_via2_class_init(ObjectClass *klass,
                                            const void *data)
{
}

static void macii_irq_sink(void *opaque, int n, int level)
{
}

static uint64_t macii_via2_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS6522MacIIVIA2State *v2s = opaque;
    MOS6522State *s = &v2s->parent_obj;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;
    uint64_t val = mos6522_read(s, reg, size);

    if (reg == VIA_REG_A || reg == VIA_REG_ANH) {
        /*
         * Input-configured pins read the live external level: the slot
         * IRQ lines ($9-$E on PA0-PA5, active low; all idle-high pull-ups
         * unless the pseudo-slot $E video VBL is asserting PA5).
         */
        val = (val & s->dira) | (v2s->pins_a & ~s->dira);
    }
    return val;
}

static void macii_via2_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    mos6522_write(s, reg, val, size);
}

static const MemoryRegionOps macii_via2_ops = {
    .read = macii_via2_read,
    .write = macii_via2_write,
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

/* 60.15Hz tick on VIA1 CA1 and one-second tick on VIA1 CA2 */

#define VIA_60HZ_TIMER_PERIOD_NS   16625800

static void macii_sixty_hz(void *opaque)
{
    MacIIMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA1_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->sixty_hz_timer,
              (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
               VIA_60HZ_TIMER_PERIOD_NS) /
              VIA_60HZ_TIMER_PERIOD_NS * VIA_60HZ_TIMER_PERIOD_NS);
}

static void macii_one_second(void *opaque)
{
    MacIIMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA2_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->one_second_timer,
              (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000) / 1000 * 1000);
}

/*
 * Pseudo-DMA aperture for the NCR5380 at slice +0x12000: each access
 * moves one byte to/from the current SCSI data phase.
 */

static uint64_t macii_scsi_pdma_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    NCR5380State *s = opaque;

    return ncr5380_pdma_read(s);
}

static void macii_scsi_pdma_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    NCR5380State *s = opaque;

    ncr5380_pdma_write(s, val);
}

static const MemoryRegionOps macii_scsi_pdma_ops = {
    .read = macii_scsi_pdma_read,
    .write = macii_scsi_pdma_write,
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

static MemTxResult macii_scsi_hsk_read(void *opaque, hwaddr addr,
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

static MemTxResult macii_scsi_hsk_write(void *opaque, hwaddr addr,
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

static const MemoryRegionOps macii_scsi_hsk_ops = {
    .read_with_attrs = macii_scsi_hsk_read,
    .write_with_attrs = macii_scsi_hsk_write,
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


/* unmapped I/O space bus-errors on the real machine */

/*
 * The discrete ASC chip fully decodes its 0x2000 window (unlike the
 * genuinely-absent NuBus slot space elsewhere in this catch-all): register
 * offsets the ASC model doesn't implement (beyond the FIFO/regs/extregs
 * sub-ranges) should read back quietly instead of bus-erroring, or the
 * ROM's ASC probe (which pokes speculatively past the known register set)
 * takes a SysError -> debug-nub detour instead of continuing boot.
 */
static bool macii_iotrace_is_asc_gap(hwaddr addr)
{
    return addr >= ASC_OFS && addr < ASC_OFS + 0x2000;
}

static MemTxResult macii_iotrace_read(void *opaque, hwaddr addr,
                                        uint64_t *data, unsigned size,
                                        MemTxAttrs attrs)
{
    if (macii_iotrace_is_asc_gap(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "macii io: read  +0x%05x (%d) -> asc gap (0)\n",
                      (unsigned)addr, size);
        *data = 0;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "macii io: read  +0x%05x (%d) -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, macii_trace_pc());
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult macii_iotrace_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size,
                                         MemTxAttrs attrs)
{
    if (macii_iotrace_is_asc_gap(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "macii io: write +0x%05x (%d) <- 0x%08" PRIx64
                      " -> asc gap (ignored)\n",
                      (unsigned)addr, size, val);
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "macii io: write +0x%05x (%d) <- 0x%08" PRIx64
                  " -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, val, macii_trace_pc());
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps macii_iotrace_ops = {
    .read_with_attrs = macii_iotrace_read,
    .write_with_attrs = macii_iotrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};


/*
 * NuBus slot interrupts: on the II/IIx/IIcx (GLUE-era discrete VIA2) a
 * card in slot $9-$E asserts its (active-low, open-collector) slot IRQ
 * line into VIA2 PA0-PA5, and any asserted slot also edges VIA2 CA1
 * ("any slot" SLOTS interrupt).  The OS's slot dispatcher takes the
 * CA1 interrupt, reads port A to see WHICH slots are asserting, and
 * runs each slot's sInt queue; the card's driver then clears the
 * card-side condition, deasserting the line.  This is the same scheme
 * the SE/30's pseudo-slot $E VBL used, driven here from the NuBus
 * bridge's per-slot IRQ outputs.
 */
static void macii_nubus_irq(void *opaque, int n, int level)
{
    MacIIMachineState *m = opaque;
    uint8_t bit = 1 << ((n & 0xf) - MAC_NUBUS_FIRST_SLOT);

    if (level) {
        qemu_irq irq2 = qdev_get_gpio_in(DEVICE(&m->via2), CA1_INT_BIT);

        m->via2.pins_a &= ~bit;
        qemu_irq_lower(irq2);
        qemu_irq_raise(irq2);
    } else {
        m->via2.pins_a |= bit;
    }
}

/*
 * Apple HMMU (68020 Mac II only): the classic 24-bit map's top half,
 * 0x800000-0xFFFFFF -- ROM at 0x800000 (mirrored through the megabyte,
 * writes ignored like real ROM), the six 1MB NuBus slot windows at
 * 0x900000-0xEFFFFF (mapped to the bottom megabyte of each slot's
 * standard space, exactly MAME's hmmu_translate_addr_ii), and the
 * mac-io slice at 0xF00000-0xFFFFFF (24-bit 0xF0xxxx = 0x50F0xxxx).
 *
 * Deliberately modelled as one ALWAYS-ON forwarder region with
 * mode-independent semantics instead of PB3-toggled mappings (PB3 is
 * the real HMMU 24/32-bit switch, cf. MAME hmmu_via2_out_b): MacOS
 * calls SwapMMUMode around every 32-bit QuickDraw operation, and
 * remapping memory regions at that rate (observed ~7000/s) melts into
 * flatview-rebuild overhead.  Always-on is safe because nothing in the
 * 32-bit map ever decodes 0x800000-0xFFFFFF (RAM is capped at 8MB
 * here), and the 24-bit behaviours are POST-compatible: the ROM's
 * 32-bit-mode RAM probe walk (0x4083f8e4, 1MB steps to 0x07F00000)
 * reads ROM data / empty-slot zeroes / VIA bytes instead of faulting,
 * and its alias-test magic write at 0x800000 is discarded exactly as
 * real ROM would.  Empty-slot reads return 0 (no bus error) -- the
 * Slot Manager's real card probing happens at 0xFsFFFFFF where errors
 * still surface.
 */
static MemTxResult macii_hmmu24_read(void *opaque, hwaddr addr,
                                     uint64_t *data, unsigned size,
                                     MemTxAttrs attrs)
{
    MacIIMachineState *m = opaque;
    hwaddr full = 0x800000 + addr;
    MemTxResult r;
    uint64_t val;

    if (full < 0x900000) {
        /* ROM, mirrored */
        uint8_t *p = memory_region_get_ram_ptr(&m->rom);

        *data = p[(full - 0x800000) & (MACII_ROM_SIZE - 1)];
        return MEMTX_OK;
    }
    if (full < 0xF00000) {
        /* NuBus slot window -> bottom 1MB of standard slot space */
        hwaddr target = 0xF0000000 | ((full & 0xF00000) << 4) |
                        (full & 0xFFFFF);

        val = address_space_ldub(&address_space_memory, target, attrs, &r);
        *data = (r == MEMTX_OK) ? val : 0;
        return MEMTX_OK;
    }
    /* mac-io */
    val = address_space_ldub(&address_space_memory,
                             0x50F00000 | (full & 0xFFFFF), attrs, &r);
    *data = (r == MEMTX_OK) ? val : 0;
    return MEMTX_OK;
}

static MemTxResult macii_hmmu24_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    hwaddr full = 0x800000 + addr;
    MemTxResult r;

    if (full < 0x900000) {
        /* ROM: writes ignored */
        return MEMTX_OK;
    }
    if (full < 0xF00000) {
        hwaddr target = 0xF0000000 | ((full & 0xF00000) << 4) |
                        (full & 0xFFFFF);

        address_space_stb(&address_space_memory, target, value, attrs, &r);
        return MEMTX_OK;
    }
    address_space_stb(&address_space_memory,
                      0x50F00000 | (full & 0xFFFFF), value, attrs, &r);
    return MEMTX_OK;
}

static const MemoryRegionOps macii_hmmu24_ops = {
    .read_with_attrs = macii_hmmu24_read,
    .write_with_attrs = macii_hmmu24_write,
    .endianness = DEVICE_BIG_ENDIAN,
    /* valid.max 8: bitfield blitters issue 8-byte accesses */
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void macii_machine_init(MachineState *machine)
{
    MacIIMachineState *m = MACII_MACHINE(machine);
    ram_addr_t ram_size = machine->ram_size;
    const char *bios_name = machine->firmware ?: MACII_ROM_FILENAME;
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
                          &macii_a31_ops, m, "macii.ram-a31",
                          0x80000000);
    memory_region_add_subregion_overlap(get_system_memory(), 0x80000000,
                                        &m->ramio_a31, -2);

    /* IRQ glue */
    object_initialize_child(OBJECT(machine), "glue", &m->glue,
                            TYPE_MACII_GLUE);
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
    memory_region_init_io(&m->iotrace, OBJECT(machine), &macii_iotrace_ops,
                          m, "mac-io.trace", IO_SLICE);
    memory_region_add_subregion_overlap(&m->macio, 0, &m->iotrace, -1);

    /* VIA1 */
    object_initialize_child(OBJECT(machine), "via1", &m->via1,
                            TYPE_MOS6522_MACII);
    qdev_prop_set_uint64(DEVICE(&m->via1), "frequency", VIA_TIMER_FREQ);
    /*
     * Machine-ID straps on port A (read with DDRA all-input during
     * identification): bit 6 SET selects the IIcx/SE30 pair, CLEAR the
     * II/IIx pair (cf. MAME apple/macii.cpp: PA reads 0x81 on the
     * II/IIx and 0x81|0x40 on the IIcx/SE30); II vs IIx are then told
     * apart by CPU type.  The other bits keep the SE/30's proven 0xEF
     * idle-high values (bits 1,2,3,5 evidently ignored by this ROM;
     * bit 4 low, bit 0/7 high).
     */
    qdev_prop_set_uint8(DEVICE(&m->via1), "pins-a",
                        MACII_MACHINE_GET_CLASS(machine)->via1_pins_a);
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

    /* ADB devices on the classic VIA1 transceiver (as on q800) */
    {
        ADBBusState *adb_bus = &m->via1.adb_bus;

        adb_register_autopoll_callback(adb_bus, macii_adb_poll, &m->via1);
        m->via1.adb_data_ready = qdev_get_gpio_in(DEVICE(&m->via1),
                                                  SR_INT_BIT);

        dev = qdev_new(TYPE_ADB_KEYBOARD);
        qdev_realize_and_unref(dev, BUS(adb_bus), &error_fatal);
        dev = qdev_new(TYPE_ADB_MOUSE);
        qdev_realize_and_unref(dev, BUS(adb_bus), &error_fatal);

        /*
         * The autopoll crutch (and its synthetic mouse-click nudge)
         * completes the ROM's post-video mouse rendezvous (the 0x172
         * spin at 0x40802432), which is part of every boot now that the
         * pseudo-slot $E video declaration ROM is built in by default.
         */
        m->adb_autopoll_kick_timer =
            timer_new_ns(QEMU_CLOCK_VIRTUAL, macii_adb_autopoll_kick, m);
        timer_mod(m->adb_autopoll_kick_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
    }
    m->sixty_hz_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, macii_sixty_hz, m);
    timer_mod(m->sixty_hz_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VIA_60HZ_TIMER_PERIOD_NS);
    m->one_second_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, macii_one_second,
                                       m);
    timer_mod(m->one_second_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), MACII_GLUE_VIA1));
    memory_region_init_io(&m->via1mem, OBJECT(machine), &macii_via1_ops,
                          &m->via1, "via1", VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, VIA1_OFS, &m->via1mem);

    /* real discrete VIA2 at the classic VIA2 site, IRQ into GLUE level 2 */
    object_initialize_child(OBJECT(machine), "via2", &m->via2,
                            TYPE_MOS6522_MACII_VIA2);
    qdev_prop_set_uint64(DEVICE(&m->via2), "frequency", VIA_TIMER_FREQ);
    m->via2.pins_a = 0xff;      /* slot IRQ lines idle high (pull-ups) */
    sysbus_realize(SYS_BUS_DEVICE(&m->via2), &error_fatal);
    /*
     * The earlier VIA2 longword-access interrupt storm (fixed above via
     * macii_via2_ops' .valid/.impl split) was root-caused to the wide
     * access itself, not to VIA2's IRQ output; disconnecting the IRQ
     * during that debugging session was a diagnostic dead end that was
     * never reconnected.  Wire VIA2's own summary IRQ into GLUE level 2
     * now, matching the RBV summary wiring the IIci uses for its VIA2
     * site.
     */
    sysbus_connect_irq(SYS_BUS_DEVICE(&m->via2), 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), MACII_GLUE_VIA2));
    memory_region_init_io(&m->via2mem, OBJECT(machine), &macii_via2_ops,
                          &m->via2, "via2", VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, VIA1_OFS + VIA_REGION_SIZE,
                                &m->via2mem);

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
                                           MACII_GLUE_SCC));
    memory_region_add_subregion(&m->macio, SCC_OFS,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->escc),
                                                       0));

    /* ASC (the IIci has the original ASC, not the IIsi's EASC) */
    object_initialize_child(OBJECT(machine), "asc", &m->asc, TYPE_ASC);
    qdev_prop_set_uint8(DEVICE(&m->asc), "asctype", ASC_TYPE_ASC);
    if (machine->audiodev) {
        qdev_prop_set_string(DEVICE(&m->asc), "audiodev", machine->audiodev);
    }
    sysbus = SYS_BUS_DEVICE(&m->asc);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, ASC_OFS,
                                sysbus_mmio_get_region(sysbus, 0));
    /* TODO: route via VIA2 CB1; see the SCSI IRQ comment below on storms */
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(macii_irq_sink, m, 0));

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
                          &macii_escc_mirror_ops, NULL, "escc-mirror",
                          0x2000);
    memory_region_add_subregion(&m->macio, SCC_OFS | 0x8000, &m->escc_mirror);

    memory_region_init_alias(&m->swim_mirror, OBJECT(machine), "swim-mirror",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->swim),
                                                    0),
                             0, 0x2000);
    memory_region_add_subregion(&m->macio, SWIM_OFS | 0x8000, &m->swim_mirror);

    /* NCR5380 SCSI controller and its pseudo-DMA aperture (+0x2000) */
    object_initialize_child(OBJECT(machine), "scsi", &m->scsi, TYPE_NCR5380);
    qdev_prop_set_uint8(DEVICE(&m->scsi), "reg-shift", 4);
    sysbus = SYS_BUS_DEVICE(&m->scsi);
    sysbus_realize(sysbus, &error_fatal);
    /*
     * Real Mac II-class GLUE wiring: the NCR5380's IRQ output feeds
     * VIA2 CA1 (cf. Linux arch/m68k/mac/via.c and MAME's mac.cpp GLUE
     * model -- SCSI_IRQ is a VIA2 CA1 edge on the II/IIx/SE30 family,
     * NOT a direct GLUE/CPU line and NOT RBV-summarised as on the
     * IIci).  A large (192-sector) blind SCSI READ was observed (gdb +
     * -trace ncr5380_*) to stall forever after the ROM's PIO copy loop
     * pulled exactly one 512-byte block and fell into an outer tail
     * poll of CSB/BSR (0x40806dd8-style "wait for a flag an interrupt
     * handler clears" idiom) that never resolves with SCSI IRQ sunk --
     * consistent with (not yet proven) that outer dispatcher being
     * interrupt-driven for multi-block transfers.  The previous
     * session's storm concern here was root-caused instead to the
     * VIA2 longword-access bug (fixed above); revisit if a genuine
     * storm reappears now that access-size splitting is correct.
     */
    /*
     * Unlike the SE/30 model (which put the 5380 on VIA2 CA1), use the
     * REAL Mac II wiring: SCSI IRQ = VIA2 CB2 (Linux macints.h
     * IRQ_MAC_SCSI = IRQ_VIA2_3), because CA1 here is the live "any
     * slot" NuBus interrupt (Linux IRQ_MAC_NUBUS = IRQ_VIA2_1) --
     * macii_nubus_irq()'s slot pulses leave CA1 high, so a 5380 level
     * assert on the same line could arrive edge-less and be lost,
     * hanging synchronous File Manager I/O.
     */
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->via2), CB2_INT_BIT));
    memory_region_add_subregion(&m->macio, SCSI_OFS,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_init_io(&m->scsi_pdma, OBJECT(machine),
                          &macii_scsi_pdma_ops, &m->scsi, "scsi-pdma", 0x2000);
    memory_region_add_subregion(&m->macio, SCSI_OFS + 0x2000, &m->scsi_pdma);

    /* handshake pseudo-DMA aperture ("SCSI+DRQ", 0x50F06000) */
    memory_region_init_io(&m->scsi_hsk, OBJECT(machine),
                          &macii_scsi_hsk_ops, &m->scsi, "scsi-hsk", 0x2000);
    memory_region_add_subregion(&m->macio, 0x6000, &m->scsi_hsk);

    scsi_bus_legacy_handle_cmdline(&m->scsi.bus);

    /* ROM */
    memory_region_init_rom(&m->rom, NULL, "macii.rom", MACII_ROM_SIZE,
                           &error_abort);
    memory_region_add_subregion(get_system_memory(), MACII_ROM_ADDR,
                                &m->rom);

    memory_region_init_alias(&m->rom_alias, NULL, "macii.rom-alias",
                             &m->rom, 0, MACII_ROM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x40000000,
                                &m->rom_alias);


    /*
     * Real NuBus (the structural difference from the SE/30): super-slot
     * space at 0x90000000 and standard slot space at 0xF9000000, both
     * at priority -1 -- above the A31 catch-all (-2), which keeps
     * providing the low-24-bit RAM decode everywhere no card answers.
     * Slot IRQs are routed into VIA2 port A + CA1 (see
     * macii_nubus_irq()).  Video is a plugged-in card (e.g. -device
     * radius-24xp,slot=9,romfile=...): these machines have no onboard
     * video at all.
     */
    {
        MacIIMachineClass *mmc = MACII_MACHINE_GET_CLASS(machine);
        qemu_irq *slot_irq;
        SysBusDevice *nb;
        int slot;

        object_initialize_child(OBJECT(machine), "mac-nubus-bridge",
                                &m->mac_nubus_bridge, TYPE_MAC_NUBUS_BRIDGE);
        nb = SYS_BUS_DEVICE(&m->mac_nubus_bridge);
        qdev_prop_set_uint16(DEVICE(nb), "slot-available-mask",
                             mmc->nubus_slot_mask);
        sysbus_realize(nb, &error_fatal);
        /*
         * Super-slot space (0x90000000-0xEFFFFFFF) goes BELOW the A31
         * low-24 catch-all (-2): MacOS dereferences 24-bit-tagged
         * master pointers with the tag bits still set (observed:
         * 0xA082xxxx locked-handle reads from the Process Manager
         * bring-up at ROM 0x40806a0a, = sad mac 0F/0001 when they bus
         * -error), and on this family those must resolve through the
         * low-24 decode -- the 030 machines' PMMU only strips the top
         * byte for LOGICAL 24-bit accesses, not for these physical
         * ones.  No card modelled here decodes super-slot space, so
         * nothing is lost by hiding it.  Standard slot space
         * (0xF9000000+, decl ROMs + card registers) keeps -1, above
         * the catch-all.
         */
        memory_region_add_subregion_overlap(get_system_memory(),
                            MAC_NUBUS_FIRST_SLOT * NUBUS_SUPER_SLOT_SIZE,
                            sysbus_mmio_get_region(nb, 0), -3);
        memory_region_add_subregion_overlap(get_system_memory(),
                            NUBUS_SLOT_BASE +
                            MAC_NUBUS_FIRST_SLOT * NUBUS_SLOT_SIZE,
                            sysbus_mmio_get_region(nb, 1), -1);

        slot_irq = qemu_allocate_irqs(macii_nubus_irq, m,
                                      MAC_NUBUS_LAST_SLOT + 1);
        for (slot = MAC_NUBUS_FIRST_SLOT; slot <= MAC_NUBUS_LAST_SLOT;
             slot++) {
            qdev_connect_gpio_out(DEVICE(&m->mac_nubus_bridge), slot,
                                  slot_irq[slot]);
        }

        /*
         * 24-bit-region mirrors: MacOS' slot 24->32-bit address formula
         * on this ROM's 020 path is 0xF0000000 | (slot << 24) | 24-bit
         * address, so the 32-bit twin of a 24-bit slot-window pointer
         * ($9XXXXX on slot 9) is 0xF99XXXXX -- the card is expected to
         * decode its 24-bit-accessible megabyte AGAIN at standard-slot
         * offset (slot << 20), as real 24-bit-compatible NuBus cards
         * do.  (Observed: 32-bit QuickDraw blitting to ScrnBase's
         * 32-bit twin 0xF9900460 bus-erroring into sad mac 0F/0001,
         * while the same boot's 24-bit-window accesses were fine.)
         * Alias each slot's first megabyte there.  The 030 machines'
         * PMMU pages 24-bit slot windows to the BOTTOM of standard
         * slot space instead, so those never hit this; the mirror is
         * still harmless there.
         */
        for (slot = MAC_NUBUS_FIRST_SLOT; slot <= MAC_NUBUS_LAST_SLOT;
             slot++) {
            int i = slot - MAC_NUBUS_FIRST_SLOT;
            g_autofree char *name = g_strdup_printf("macii.slot24-mirror-%x",
                                                    slot);

            memory_region_init_alias(&m->slot24_mirror[i], OBJECT(machine),
                                     name, sysbus_mmio_get_region(nb, 1),
                                     i * NUBUS_SLOT_SIZE, 0x100000);
            memory_region_add_subregion_overlap(get_system_memory(),
                                                NUBUS_SLOT_BASE +
                                                (hwaddr)slot * NUBUS_SLOT_SIZE
                                                + ((hwaddr)slot << 20),
                                                &m->slot24_mirror[i], 0);
        }
    }

    /*
     * 68020 Mac II only: the Apple HMMU's 24-bit map.  The 020 has no
     * PMMU, so unlike the IIx/IIcx (whose ROM programs the 030 PMMU
     * with an IS=8 24-bit map) the classic 24-bit address map must
     * exist in hardware: only the low 24 address bits reach the
     * decoder, with ROM at 0x800000, the six 1MB NuBus slot windows at
     * 0x900000-0xEFFFFF (the bottom 1MB of each slot's standard space)
     * and the I/O slice mirrors at 0xF00000-0xFFFFFF (24-bit 0xF0xxxx
     * = 0x50F0xxxx, i.e. the mac-io slice repeating every 0x40000).
     * Modelled as the single always-on macii.hmmu24 forwarder region
     * (see macii_hmmu24_ops above for why it is not PB3-toggled),
     * priority 1 above the RAM container.  Tagged 24-bit master
     * pointers with A31 set (0x80/0xA0/0xC0/0xE0 prefixes) resolve
     * through the existing A31 low-24 forwarder into this map --
     * 0xA082xxxx-style locked-handle dereferences land in the ROM
     * window exactly as on real HMMU hardware (this was the macii
     * sad-mac 0F/0001 blocker: they used to fall into NuBus super-slot
     * space and bus-error).
     */
    if (MACII_MACHINE_GET_CLASS(machine)->hmmu) {
        memory_region_init_io(&m->hmmu24, OBJECT(machine), &macii_hmmu24_ops,
                              m, "macii.hmmu24", 0x800000);
        memory_region_add_subregion_overlap(get_system_memory(), 0x00800000,
                                            &m->hmmu24, 1);

        /*
         * With no PMMU to strip them, 24-bit-tagged master pointers
         * with A31 CLEAR (Memory Manager flag prefixes 0x20/0x40/0x60:
         * purgeable/resource bits without the lock bit) also arrive
         * here physically un-masked; only the A31-set half is covered
         * by macii.ram-a31.  Observed: System code at RAM 0x001fd0a4
         * word-reading 0x6000EFD0 forever (fault-retry loop, boot
         * crawls and SysErrors).  Give the 0x20000000-0x3FFFFFFF and
         * 0x60000000-0x7FFFFFFF ranges the same low-24 forwarding.
         * (The 0x40000000 prefix range is left alone: it is the ROM's
         * real 32-bit home, and for ROM-range low-24 addresses the
         * identity decode already matches the 24-bit map.)
         */
        memory_region_init_io(&m->tag24_2x, OBJECT(machine), &macii_a31_ops,
                              m, "macii.tag24-2x", 0x20000000);
        memory_region_add_subregion_overlap(get_system_memory(), 0x20000000,
                                            &m->tag24_2x, 1);
        memory_region_init_io(&m->tag24_6x, OBJECT(machine), &macii_a31_ops,
                              m, "macii.tag24-6x", 0x20000000);
        memory_region_add_subregion_overlap(get_system_memory(), 0x60000000,
                                            &m->tag24_6x, -2);
        /*
         * 0x40 prefix (resource tag): the 0x40000000-0x40FFFFFF
         * megabyte-space is mostly the ROM's 32-bit home, where the
         * identity decode already equals the 24-bit map for ROM-range
         * low-24 addresses -- but tagged RAM pointers like 0x40091E68
         * (observed bus-error bomb during extension loading) land in
         * the gaps between rom/rom-alias.  Backfill those gaps, below
         * the ROM regions, with the same low-24 forwarding (the region
         * base is 16MB-aligned so the offset masking works out).
         */
        memory_region_init_io(&m->tag24_4x, OBJECT(machine), &macii_a31_ops,
                              m, "macii.tag24-4x", 0x01000000);
        memory_region_add_subregion_overlap(get_system_memory(), 0x40000000,
                                            &m->tag24_4x, -1);

        /*
         * Fast path for LOCKED-tagged pointers (prefixes 0x80, 0xA0,
         * 0xC0, 0xE0 -- lock bit 31 plus purgeable/resource): 24-bit
         * MacOS executes code straight out of locked handles, so
         * instruction fetch through the A31 io forwarder means
         * MMIO-exec with no TB caching -- observed as the Finder
         * "booting" for virtual HOURS with the PC sampled inside
         * 0x800Fxxxx.  Alias guest RAM directly at each locked-prefix
         * base (above the io forwarders) so tagged code and data in
         * RAM run at native speed; low-24 addresses beyond RAM still
         * fall through to the forwarders for ROM/slot/IO windows.
         */
        {
            static const hwaddr tag_bases[4] = {
                0x80000000, 0xA0000000, 0xC0000000, 0xE0000000,
            };
            int i;

            for (i = 0; i < 4; i++) {
                g_autofree char *name =
                    g_strdup_printf("macii.tag24-ram-%d", i);

                memory_region_init_alias(&m->tag24_ram[i], OBJECT(machine),
                                         name, machine->ram, 0,
                                         machine->ram_size);
                memory_region_add_subregion_overlap(get_system_memory(),
                                                    tag_bases[i],
                                                    &m->tag24_ram[i], -1);
            }
        }
    }

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, MACII_ROM_ADDR,
                                        MACII_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }

    if (!qtest_enabled()) {
        if (bios_size <= 0 || bios_size > MACII_ROM_SIZE) {
            error_report("could not load MacROM '%s'", bios_name);
            exit(1);
        }

        uint32_t entry;

        cs = CPU(&m->cpu);
        ptr = rom_ptr(MACII_ROM_ADDR, bios_size);
        assert(ptr != NULL);
        stl_phys(cs->as, 0, ldl_be_p(ptr));    /* reset initial SP */
        /*
         * Reset initial PC: macIIx.rom stores the ABSOLUTE entry
         * address (0x4080002A) at +4, like the IIci ROM (some $067C
         * family siblings, e.g. the IIsi/Classic II, store a
         * base-relative offset instead).  Handle both.
         */
        entry = ldl_be_p(ptr + 4);
        if (entry < MACII_ROM_SIZE) {
            entry += MACII_ROM_ADDR;
        }
        stl_phys(cs->as, 4, entry);
    }
}

static void macii_machine_common_class_init(ObjectClass *oc,
                                            const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = macii_machine_init;
    mc->max_cpus = 1;
    mc->block_default_type = IF_SCSI;
    /*
     * 4 MiB, like the SE/30: this ROM's RAM alias-sizing test faults
     * the boot (POST error -> serial MicroBug) with an 8 MiB bank A --
     * the 24-bit bank layout puts ROM at 0x800000, and the sizing
     * write/read-back at the 8 MiB mark reads our unbacked 0 instead
     * of a ROM/bus-error mismatch it can classify.  Verified by in_asm
     * trace diff at 0x4080397e (magic 0xA5B4C3D2 alias probe).
     */
    mc->default_ram_size = 4 * MiB;
    mc->default_ram_id = "macii.ram";
    machine_add_audiodev_property(mc);
}

#define MACII_SLOT_MASK_9_E 0x7e00      /* six slots, $9-$E */
#define MACII_SLOT_MASK_9_B 0x0e00      /* three slots, $9-$B */

static void macii_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68020"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);
    MacIIMachineClass *mmc = MACII_MACHINE_CLASS(oc);

    mc->desc = "Macintosh II (FDHD)";
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68020");
    mc->valid_cpu_types = valid_cpu_types;
    mmc->via1_pins_a = 0xaf;            /* bit 6 clear: II/IIx */
    mmc->nubus_slot_mask = MACII_SLOT_MASK_9_E;
    mmc->hmmu = true;
}

static void maciix_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68030"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);
    MacIIMachineClass *mmc = MACII_MACHINE_CLASS(oc);

    mc->desc = "Macintosh IIx";
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->valid_cpu_types = valid_cpu_types;
    mmc->via1_pins_a = 0xaf;            /* bit 6 clear: II/IIx */
    mmc->nubus_slot_mask = MACII_SLOT_MASK_9_E;
}

static void maciicx_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68030"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);
    MacIIMachineClass *mmc = MACII_MACHINE_CLASS(oc);

    mc->desc = "Macintosh IIcx";
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->valid_cpu_types = valid_cpu_types;
    mmc->via1_pins_a = 0xef;            /* bit 6 set: IIcx/SE30 */
    mmc->nubus_slot_mask = MACII_SLOT_MASK_9_B;
}

static const TypeInfo macii_machine_typeinfo[] = {
    {
        .name       = TYPE_MACII_GLUE,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacIIGlueState),
        .instance_init = macii_glue_init,
        .class_init = macii_glue_class_init,
    },
    {
        .name       = TYPE_MOS6522_MACII,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522MacIIState),
        .instance_init = mos6522_macii_init,
        .class_init = mos6522_macii_class_init,
    },
    {
        .name       = TYPE_MOS6522_MACII_VIA2,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522MacIIVIA2State),
        .class_init = mos6522_macii_via2_class_init,
    },
    {
        .name       = TYPE_MACII_MACHINE,
        .parent     = TYPE_MACHINE,
        .instance_size = sizeof(MacIIMachineState),
        .class_size = sizeof(MacIIMachineClass),
        .class_init = macii_machine_common_class_init,
        .abstract   = true,
    },
    {
        .name       = MACHINE_TYPE_NAME("macii"),
        .parent     = TYPE_MACII_MACHINE,
        .class_init = macii_machine_class_init,
    },
    {
        .name       = MACHINE_TYPE_NAME("maciix"),
        .parent     = TYPE_MACII_MACHINE,
        .class_init = maciix_machine_class_init,
    },
    {
        .name       = MACHINE_TYPE_NAME("maciicx"),
        .parent     = TYPE_MACII_MACHINE,
        .class_init = maciicx_machine_class_init,
    },
};

DEFINE_TYPES(macii_machine_typeinfo)
