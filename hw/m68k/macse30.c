/*
 * QEMU Macintosh SE/30 hardware system emulator
 *
 * 68030-16MHz compact Mac: classic VIA1 ADB transceiver (same as the
 * IIci -- Linux's adb_type MAC_ADB_II covers both), a REAL discrete
 * VIA2 (Linux via_type MAC_VIA_II -- the older Mac II/IIx/SE30-style
 * two-chip VIA pair, NOT the IIci's RBV-emulated VIA2 site) combined
 * with VIA1/SCC/NMI into the CPU's autovector IPL lines by the "GLUE"
 * ASIC, ASC sound, SWIM floppy, NCR5380 SCSI, Z8530 SCC, and a FIXED
 * 512x342 1-bit onboard framebuffer (no RBV video, no NuBus -- this
 * machine has only the 030 PDS, modelled minimally as absent).
 *
 * Derived from maciici.c (the classic VIA1 ADB transceiver -- proven
 * working there -- plus the RTC engine, SCC/ASC/SWIM/SCSI layout and
 * address map, all unchanged) with the RBV-emulated VIA2/video swapped
 * for a plain discrete TYPE_MOS6522 VIA2 (GLUE combines its IRQ output
 * at level 2, same as the RBV summary did for the IIci) and video
 * replaced by mac128k.c's compact-Mac fixed-framebuffer model (see
 * macclassicii.c, which ports the same thing).  ROM: macIIx.rom (the
 * shared II/IIx/IIcx/SE30 ROM, version $0178); its reset vector is an
 * ABSOLUTE address, already handled by the entry-detection logic below
 * (kept from maciici.c, which handles both forms).
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

#define MACSE30_ROM_ADDR      0x40800000
#define MACSE30_ROM_SIZE      0x00040000
#define MACSE30_ROM_FILENAME  "macIIx.rom"

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
#define TYPE_MACSE30_GLUE "macse30-glue"
OBJECT_DECLARE_SIMPLE_TYPE(MacSE30GlueState, MACSE30_GLUE)

struct MacSE30GlueState {
    SysBusDevice parent_obj;
    M68kCPU *cpu;
    uint8_t ipr;
};

#define MACSE30_GLUE_VIA1     0       /* level 1 */
#define MACSE30_GLUE_VIA2     1       /* level 2 */
#define MACSE30_GLUE_SCC      3       /* level 4 */
#define MACSE30_GLUE_NMI      6      /* level 7 */

static void macse30_glue_set_irq(void *opaque, int irq, int level)
{
    MacSE30GlueState *s = opaque;
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

static void macse30_glue_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    MacSE30GlueState *s = MACSE30_GLUE(dev);

    qdev_init_gpio_in(dev, macse30_glue_set_irq, 8);
    s->ipr = 0;
}

static const Property macse30_glue_properties[] = {
    DEFINE_PROP_LINK("cpu", MacSE30GlueState, cpu, TYPE_M68K_CPU, M68kCPU *),
};

static void macse30_glue_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, macse30_glue_properties);
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

#define TYPE_MOS6522_MACSE30 "mos6522-macse30"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522MacSE30State, MOS6522_MACSE30)

struct MOS6522MacSE30State {
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
};

/*
 * Real discrete VIA2 (Mac II/IIx/SE30/IIcx GLUE era): TYPE_MOS6522
 * itself is abstract, so a trivial concrete subclass with no
 * board-specific behaviour is enough to instantiate it.
 */
#define TYPE_MOS6522_MACSE30_VIA2 "mos6522-macse30-via2"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522MacSE30VIA2State, MOS6522_MACSE30_VIA2)

struct MOS6522MacSE30VIA2State {
    MOS6522State parent_obj;
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

static void via1_rtc_update(MOS6522MacSE30State *v1s)
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

    qemu_log_mask(LOG_UNIMP, "macse30 rtc: byte 0x%02x (cmd=%02x alt=%02x)\n",
                  v1s->data_out, v1s->cmd, v1s->alt);

    /* first byte: it's a command */
    if (v1s->cmd == REG_EMPTY) {
        cmd = via1_rtc_compact_cmd(v1s->data_out);

        if (cmd == REG_INVALID) {
            qemu_log_mask(LOG_UNIMP, "macse30 rtc: invalid cmd 0x%02x\n",
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

static void macse30_adb_poll(void *opaque)
{
    MOS6522MacSE30State *v1s = MOS6522_MACSE30(opaque);
    MOS6522State *s = MOS6522(v1s);
    ADBBusState *adb_bus = &v1s->adb_bus;
    uint8_t obuf[9];
    uint8_t *data = &s->sr;
    int olen;

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

static int macse30_adb_send_len(uint8_t data)
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

static void macse30_adb_send(MOS6522MacSE30State *v1s, int state, uint8_t data)
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
        return;
    }

    /* If the command is complete, execute it */
    if (v1s->adb_data_out_index == macse30_adb_send_len(v1s->adb_data_out[0])) {
        /* (some ROMs start commands on an EVEN/ODD transition) */
        if (!adb_bus->autopoll_blocked) {
            adb_autopoll_block(adb_bus);
        }
        v1s->adb_data_in_size = adb_request(adb_bus, v1s->adb_data_in,
                                            v1s->adb_data_out,
                                            v1s->adb_data_out_index);
        v1s->adb_data_in_index = 0;

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
         * the autopoll mask accordingly
         */
        if ((v1s->adb_data_out[0] & 0xc) == 0xc) {
            v1s->adb_autopoll_cmd = v1s->adb_data_out[0];

            autopoll_mask = 1 << (v1s->adb_autopoll_cmd >> 4);
            adb_set_autopoll_mask(adb_bus, autopoll_mask);
        }
    }
}

static void macse30_adb_receive(MOS6522MacSE30State *v1s, int state,
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

static void macse30_adb_update(MOS6522MacSE30State *v1s)
{
    MOS6522State *s = MOS6522(v1s);
    int oldstate, state;

    oldstate = (v1s->last_b & VIA1B_vADB_StateMask) >> VIA1B_vADB_StateShift;
    state = (s->b & VIA1B_vADB_StateMask) >> VIA1B_vADB_StateShift;

    if (state != oldstate) {
        if (s->acr & SR_OUT) {
            /* output mode */
            macse30_adb_send(v1s, state, s->sr);
        } else {
            /* input mode */
            macse30_adb_receive(v1s, state, &s->sr);
        }
    }
}

static void macse30_via1_portA_write(MOS6522State *s)
{
}

static void macse30_via1_portB_write(MOS6522State *s)
{
}

static void mos6522_macse30_init(Object *obj)
{
    MOS6522MacSE30State *v1s = MOS6522_MACSE30(obj);

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;

    /* the ADB bus hangs off the VIA1 shift-register transceiver */
    qbus_init(&v1s->adb_bus, sizeof(v1s->adb_bus), TYPE_ADB_BUS,
              DEVICE(obj), "adb.0");
}

static void mos6522_macse30_reset_hold(Object *obj, ResetType type)
{
    MOS6522MacSE30State *v1s = MOS6522_MACSE30(obj);
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

static const Property mos6522_macse30_properties[] = {
    DEFINE_PROP_UINT8("pins-a", MOS6522MacSE30State, pins_a, 0xff),
    DEFINE_PROP_UINT8("pins-b", MOS6522MacSE30State, pins_b, 0xff),
};

static void mos6522_macse30_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    MOS6522DeviceClass *mdc = MOS6522_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_props(dc, mos6522_macse30_properties);
    mdc->portA_write = macse30_via1_portA_write;
    mdc->portB_write = macse30_via1_portB_write;
    resettable_class_set_parent_phases(rc, NULL, mos6522_macse30_reset_hold,
                                       NULL, &mdc->parent_phases);
}

/*
 * Onboard video framebuffer: FIXED 512x342 1-bit, rowbytes 64, white =
 * 0.  The SE/30 is a one-piece Mac with a single built-in screen and
 * no RBV video/monitor sense: scanned out of the TOP of main RAM at
 * (ram size - 0x5900), same "compact Mac" placement/formula as
 * mac128k.c and macclassicii.c.
 */

#define TYPE_MACSE30_FB "macse30-fb"
OBJECT_DECLARE_SIMPLE_TYPE(MacSE30FbState, MACSE30_FB)

#define MACSE30_FB_WIDTH    512
#define MACSE30_FB_HEIGHT   342
#define MACSE30_FB_ROWBYTES 64
#define MACSE30_FB_MAIN_OFS 0x5900

struct MacSE30FbState {
    SysBusDevice parent_obj;

    MemoryRegion *ram;
    uint32_t base;               /* offset into RAM of the screen buffer */
    MemoryRegionSection fbsection;
    QemuConsole *con;
    int invalidate;
};

static void macse30_fb_draw_line(void *opaque, uint8_t *d, const uint8_t *s,
                                 int width, int pitch)
{
    uint32_t *buf = (uint32_t *)d;
    int i, b;

    for (i = 0; i < MACSE30_FB_WIDTH / 8; i++) {
        uint8_t src = s[i];

        for (b = 0; b < 8; b++) {
            buf[i * 8 + b] = (src & 0x80) ? 0xFF000000 : 0xFFFFFFFF;
            src <<= 1;
        }
    }
}

static bool macse30_fb_update(void *opaque)
{
    MacSE30FbState *s = MACSE30_FB(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first = 0, last = 0;

    if (s->invalidate) {
        s->fbsection = (MemoryRegionSection) {
            .mr = s->ram,
            .offset_within_region = s->base,
            .size = int128_make64((uint64_t)MACSE30_FB_HEIGHT *
                                  MACSE30_FB_ROWBYTES),
        };
        s->invalidate = 0;
    }

    framebuffer_update_display(surface, &s->fbsection,
                               MACSE30_FB_WIDTH, MACSE30_FB_HEIGHT,
                               MACSE30_FB_ROWBYTES, MACSE30_FB_WIDTH * 4,
                               0, 1, macse30_fb_draw_line, s,
                               &first, &last);
    qemu_console_update(s->con, 0, 0, MACSE30_FB_WIDTH, MACSE30_FB_HEIGHT);
    return true;
}

static void macse30_fb_invalidate(void *opaque)
{
    MacSE30FbState *s = MACSE30_FB(opaque);

    s->invalidate = 1;
}

static const GraphicHwOps macse30_fb_ops = {
    .invalidate = macse30_fb_invalidate,
    .gfx_update = macse30_fb_update,
};

static void macse30_fb_realize(DeviceState *dev, Error **errp)
{
    MacSE30FbState *s = MACSE30_FB(dev);

    if (!s->ram) {
        error_setg(errp, "macse30-fb: scanout RAM region not set");
        return;
    }

    s->invalidate = 1;
    s->con = qemu_graphic_console_create(dev, 0, &macse30_fb_ops, s);
    qemu_console_resize(s->con, MACSE30_FB_WIDTH, MACSE30_FB_HEIGHT);
}

static void macse30_fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->realize = macse30_fb_realize;
}

/* machine */

struct MacSE30MachineState {
    MachineState parent_obj;

    M68kCPU cpu;
    MacSE30GlueState glue;
    MOS6522MacSE30State via1;
    MOS6522MacSE30VIA2State via2; /* real discrete VIA2 (Mac II/SE30 GLUE era) */
    ESCCState escc;
    OrIRQState escc_orgate;
    ASCState asc;
    Swim swim;
    NCR5380State scsi;

    MacSE30FbState fb;
    MemoryRegion rom;
    MemoryRegion rom_alias;
    MemoryRegion rom_alias24;
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
};

#define TYPE_MACSE30_MACHINE MACHINE_TYPE_NAME("macse30")
OBJECT_DECLARE_SIMPLE_TYPE(MacSE30MachineState, MACSE30_MACHINE)

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static uint32_t macse30_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

static uint64_t ramio_read(void *opaque, hwaddr addr, unsigned size)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "macse30 ram-hole: read  0x%08x (%d) pc=0x%08x\n",
                      (unsigned)addr, size, macse30_trace_pc());
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
                      "macse30 ram-hole: write 0x%08x (%d) <- 0x%08" PRIx64
                      " pc=0x%08x\n",
                      (unsigned)addr, size, val, macse30_trace_pc());
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

static MemTxResult macse30_a31_read(void *opaque, hwaddr addr, uint64_t *data,
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

static MemTxResult macse30_a31_write(void *opaque, hwaddr addr, uint64_t value,
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

static const MemoryRegionOps macse30_a31_ops = {
    .read_with_attrs = macse30_a31_read,
    .write_with_attrs = macse30_a31_write,
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

static MemTxResult macse30_escc_mirror_read(void *opaque, hwaddr addr,
                                            uint64_t *data, unsigned size,
                                            MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_read(opaque, addr, data, size, attrs);
}

static MemTxResult macse30_escc_mirror_write(void *opaque, hwaddr addr,
                                             uint64_t val, unsigned size,
                                             MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_write(opaque, addr, val, size, attrs);
}

static const MemoryRegionOps macse30_escc_mirror_ops = {
    .read_with_attrs = macse30_escc_mirror_read,
    .write_with_attrs = macse30_escc_mirror_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t macse30_via1_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS6522MacSE30State *v1s = opaque;
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

static void macse30_via1_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MOS6522MacSE30State *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    mos6522_write(s, reg, val, size);

    if (reg == VIA_REG_SR || reg == VIA_REG_ACR) {
        qemu_log_mask(LOG_UNIMP,
                      "macse30 via1: reg%d <- 0x%02" PRIx64 " pc=0x%08x\n",
                      (int)reg, val, macse30_trace_pc());
    }

    if (reg == VIA_REG_B) {
        via1_rtc_update(v1s);
        macse30_adb_update(v1s);
        v1s->last_b = s->b;
    }
}

static const MemoryRegionOps macse30_via1_ops = {
    .read = macse30_via1_read,
    .write = macse30_via1_write,
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

static void mos6522_macse30_via2_class_init(ObjectClass *klass,
                                            const void *data)
{
}

static void macse30_irq_sink(void *opaque, int n, int level)
{
}

static uint64_t macse30_via2_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;
    uint64_t val = mos6522_read(s, reg, size);

    if (reg == VIA_REG_A || reg == VIA_REG_ANH) {
        val |= ~s->dira & 0xff;
    }
    return val;
}

static void macse30_via2_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    mos6522_write(s, reg, val, size);
}

static const MemoryRegionOps macse30_via2_ops = {
    .read = macse30_via2_read,
    .write = macse30_via2_write,
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

static void macse30_sixty_hz(void *opaque)
{
    MacSE30MachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA1_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->sixty_hz_timer,
              (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
               VIA_60HZ_TIMER_PERIOD_NS) /
              VIA_60HZ_TIMER_PERIOD_NS * VIA_60HZ_TIMER_PERIOD_NS);
}

static void macse30_one_second(void *opaque)
{
    MacSE30MachineState *m = opaque;
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

static uint64_t macse30_scsi_pdma_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    NCR5380State *s = opaque;

    return ncr5380_pdma_read(s);
}

static void macse30_scsi_pdma_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    NCR5380State *s = opaque;

    ncr5380_pdma_write(s, val);
}

static const MemoryRegionOps macse30_scsi_pdma_ops = {
    .read = macse30_scsi_pdma_read,
    .write = macse30_scsi_pdma_write,
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

static MemTxResult macse30_scsi_hsk_read(void *opaque, hwaddr addr,
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

static MemTxResult macse30_scsi_hsk_write(void *opaque, hwaddr addr,
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

static const MemoryRegionOps macse30_scsi_hsk_ops = {
    .read_with_attrs = macse30_scsi_hsk_read,
    .write_with_attrs = macse30_scsi_hsk_write,
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
static bool macse30_iotrace_is_asc_gap(hwaddr addr)
{
    return addr >= ASC_OFS && addr < ASC_OFS + 0x2000;
}

static MemTxResult macse30_iotrace_read(void *opaque, hwaddr addr,
                                        uint64_t *data, unsigned size,
                                        MemTxAttrs attrs)
{
    if (macse30_iotrace_is_asc_gap(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "macse30 io: read  +0x%05x (%d) -> asc gap (0)\n",
                      (unsigned)addr, size);
        *data = 0;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "macse30 io: read  +0x%05x (%d) -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, macse30_trace_pc());
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult macse30_iotrace_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size,
                                         MemTxAttrs attrs)
{
    if (macse30_iotrace_is_asc_gap(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "macse30 io: write +0x%05x (%d) <- 0x%08" PRIx64
                      " -> asc gap (ignored)\n",
                      (unsigned)addr, size, val);
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "macse30 io: write +0x%05x (%d) <- 0x%08" PRIx64
                  " -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, val, macse30_trace_pc());
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps macse30_iotrace_ops = {
    .read_with_attrs = macse30_iotrace_read,
    .write_with_attrs = macse30_iotrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};


static void macse30_machine_init(MachineState *machine)
{
    MacSE30MachineState *m = MACSE30_MACHINE(machine);
    ram_addr_t ram_size = machine->ram_size;
    const char *bios_name = machine->firmware ?: MACSE30_ROM_FILENAME;
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
                          &macse30_a31_ops, m, "macse30.ram-a31",
                          0x80000000);
    memory_region_add_subregion_overlap(get_system_memory(), 0x80000000,
                                        &m->ramio_a31, -2);

    /* IRQ glue */
    object_initialize_child(OBJECT(machine), "glue", &m->glue,
                            TYPE_MACSE30_GLUE);
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
    memory_region_init_io(&m->iotrace, OBJECT(machine), &macse30_iotrace_ops,
                          m, "mac-io.trace", IO_SLICE);
    memory_region_add_subregion_overlap(&m->macio, 0, &m->iotrace, -1);

    /* VIA1 */
    object_initialize_child(OBJECT(machine), "via1", &m->via1,
                            TYPE_MOS6522_MACSE30);
    qdev_prop_set_uint64(DEVICE(&m->via1), "frequency", VIA_TIMER_FREQ);
    /*
     * Machine-ID straps on port A (read with DDRA all-input during
     * identification).  macIIx.rom is the shared II/IIx/IIcx/SE30 ROM
     * (decoder kind 4, "Mac II class VIA2 machines" per the ROM
     * analysis notes) rather than the $067C universal family the IIci/
     * IIsi/Classic II share -- its own machine-entry table has not yet
     * been extracted, so reuse the IIci/IIsi 0xEF strap as a starting
     * guess pending boot-test iteration.
     */
    qdev_prop_set_uint8(DEVICE(&m->via1), "pins-a", 0xef);
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

        adb_register_autopoll_callback(adb_bus, macse30_adb_poll, &m->via1);
        m->via1.adb_data_ready = qdev_get_gpio_in(DEVICE(&m->via1),
                                                  SR_INT_BIT);

        dev = qdev_new(TYPE_ADB_KEYBOARD);
        qdev_realize_and_unref(dev, BUS(adb_bus), &error_fatal);
        dev = qdev_new(TYPE_ADB_MOUSE);
        qdev_realize_and_unref(dev, BUS(adb_bus), &error_fatal);

        adb_set_autopoll_enabled(adb_bus, true);
    }
    m->sixty_hz_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, macse30_sixty_hz, m);
    timer_mod(m->sixty_hz_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VIA_60HZ_TIMER_PERIOD_NS);
    m->one_second_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, macse30_one_second,
                                       m);
    timer_mod(m->one_second_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), MACSE30_GLUE_VIA1));
    memory_region_init_io(&m->via1mem, OBJECT(machine), &macse30_via1_ops,
                          &m->via1, "via1", VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, VIA1_OFS, &m->via1mem);

    /* real discrete VIA2 at the classic VIA2 site, IRQ into GLUE level 2 */
    object_initialize_child(OBJECT(machine), "via2", &m->via2,
                            TYPE_MOS6522_MACSE30_VIA2);
    qdev_prop_set_uint64(DEVICE(&m->via2), "frequency", VIA_TIMER_FREQ);
    sysbus_realize(SYS_BUS_DEVICE(&m->via2), &error_fatal);
    /* DIAGNOSTIC: leave VIA2 IRQ disconnected to isolate the storm */
    memory_region_init_io(&m->via2mem, OBJECT(machine), &macse30_via2_ops,
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
                                           MACSE30_GLUE_SCC));
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
                       qemu_allocate_irq(macse30_irq_sink, m, 0));

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
                          &macse30_escc_mirror_ops, NULL, "escc-mirror",
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
     * TODO: route via VIA2 CA1 once the handshake is right -- wiring it
     * straight through storms the CPU (VIA2 IER starts masked, but
     * something about the 5380's idle IRQ level plus our minimal VIA2
     * model re-triggers immediately on every RTE).  Sink it for now,
     * same caution as the IIci's ASC comment.
     */
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(macse30_irq_sink, m, 0));
    memory_region_add_subregion(&m->macio, SCSI_OFS,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_init_io(&m->scsi_pdma, OBJECT(machine),
                          &macse30_scsi_pdma_ops, &m->scsi, "scsi-pdma", 0x2000);
    memory_region_add_subregion(&m->macio, SCSI_OFS + 0x2000, &m->scsi_pdma);

    /* handshake pseudo-DMA aperture ("SCSI+DRQ", 0x50F06000) */
    memory_region_init_io(&m->scsi_hsk, OBJECT(machine),
                          &macse30_scsi_hsk_ops, &m->scsi, "scsi-hsk", 0x2000);
    memory_region_add_subregion(&m->macio, 0x6000, &m->scsi_hsk);

    scsi_bus_legacy_handle_cmdline(&m->scsi.bus);

    /* ROM */
    memory_region_init_rom(&m->rom, NULL, "macse30.rom", MACSE30_ROM_SIZE,
                           &error_abort);
    memory_region_add_subregion(get_system_memory(), MACSE30_ROM_ADDR,
                                &m->rom);

    memory_region_init_alias(&m->rom_alias, NULL, "macse30.rom-alias",
                             &m->rom, 0, MACSE30_ROM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x40000000,
                                &m->rom_alias);

    /*
     * Onboard video: fixed 512x342 1-bit compact-Mac framebuffer, no
     * RBV video and no NuBus (this machine has only the 030 PDS).
     */
    object_initialize_child(OBJECT(machine), "fb", &m->fb, TYPE_MACSE30_FB);
    m->fb.ram = machine->ram;
    m->fb.base = ram_size - MACSE30_FB_MAIN_OFS;
    sysbus = SYS_BUS_DEVICE(&m->fb);
    sysbus_realize(sysbus, &error_fatal);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, MACSE30_ROM_ADDR,
                                        MACSE30_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }

    if (!qtest_enabled()) {
        if (bios_size <= 0 || bios_size > MACSE30_ROM_SIZE) {
            error_report("could not load MacROM '%s'", bios_name);
            exit(1);
        }

        uint32_t entry;

        cs = CPU(&m->cpu);
        ptr = rom_ptr(MACSE30_ROM_ADDR, bios_size);
        assert(ptr != NULL);
        stl_phys(cs->as, 0, ldl_be_p(ptr));    /* reset initial SP */
        /*
         * Reset initial PC: macIIx.rom stores the ABSOLUTE entry
         * address (0x4080002A) at +4, like the IIci ROM (some $067C
         * family siblings, e.g. the IIsi/Classic II, store a
         * base-relative offset instead).  Handle both.
         */
        entry = ldl_be_p(ptr + 4);
        if (entry < MACSE30_ROM_SIZE) {
            entry += MACSE30_ROM_ADDR;
        }
        stl_phys(cs->as, 4, entry);
    }
}

static void macse30_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68030"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Macintosh SE/30";
    mc->init = macse30_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->block_default_type = IF_SCSI;
    mc->default_ram_size = 4 * MiB;
    mc->default_ram_id = "macse30.ram";
    machine_add_audiodev_property(mc);
}

static const TypeInfo macse30_machine_typeinfo[] = {
    {
        .name       = TYPE_MACSE30_GLUE,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacSE30GlueState),
        .instance_init = macse30_glue_init,
        .class_init = macse30_glue_class_init,
    },
    {
        .name       = TYPE_MOS6522_MACSE30,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522MacSE30State),
        .instance_init = mos6522_macse30_init,
        .class_init = mos6522_macse30_class_init,
    },
    {
        .name       = TYPE_MOS6522_MACSE30_VIA2,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522MacSE30VIA2State),
        .class_init = mos6522_macse30_via2_class_init,
    },
    {
        .name       = TYPE_MACSE30_FB,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacSE30FbState),
        .class_init = macse30_fb_class_init,
    },
    {
        .name       = TYPE_MACSE30_MACHINE,
        .parent     = TYPE_MACHINE,
        .instance_size = sizeof(MacSE30MachineState),
        .class_init = macse30_machine_class_init,
    },
};

DEFINE_TYPES(macse30_machine_typeinfo)
