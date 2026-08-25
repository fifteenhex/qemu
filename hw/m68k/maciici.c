/*
 * QEMU Macintosh IIci hardware system emulator
 *
 * 68030 machine with RBV (RAM-Based Video), classic VIA1 ADB
 * transceiver, 343-0042 RTC/PRAM, ASC sound, SWIM floppy, NCR5380 SCSI
 * and Z8530 SCC.  Derived from maciisi.c (the IIsi is the IIci's
 * cost-reduced sibling: same RBV chipset, but the IIsi moved ADB/RTC
 * behind an Egret MCU while the IIci keeps the discrete transceiver
 * and RTC chip).  The IIci ROM (368CADFE, version $067C) identifies
 * the machine with the same decoder-kind-5 fingerprint and VIA1 port A
 * straps ((PA & 0x56) == 0x46) as the IIsi ROM does the IIsi.
 * ADB transceiver model ported from hw/misc/mac_via.c (q800).
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

#define MACIICI_ROM_ADDR      0x40800000
#define MACIICI_ROM_SIZE      0x00080000
#define MACIICI_ROM_FILENAME  "maciici.rom"

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
#define TYPE_MACIICI_GLUE "maciici-glue"
OBJECT_DECLARE_SIMPLE_TYPE(MacIIciGlueState, MACIICI_GLUE)

struct MacIIciGlueState {
    SysBusDevice parent_obj;
    M68kCPU *cpu;
    uint8_t ipr;
};

#define MACIICI_GLUE_VIA1     0       /* level 1 */
#define MACIICI_GLUE_RBV      1       /* level 2 */
#define MACIICI_GLUE_SCC      3       /* level 4 */
#define MACIICI_GLUE_NMI      6      /* level 7 */

static void maciici_glue_set_irq(void *opaque, int irq, int level)
{
    MacIIciGlueState *s = opaque;
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

static void maciici_glue_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    MacIIciGlueState *s = MACIICI_GLUE(dev);

    qdev_init_gpio_in(dev, maciici_glue_set_irq, 8);
    s->ipr = 0;
}

static const Property maciici_glue_properties[] = {
    DEFINE_PROP_LINK("cpu", MacIIciGlueState, cpu, TYPE_M68K_CPU, M68kCPU *),
};

static void maciici_glue_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, maciici_glue_properties);
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

#define TYPE_MOS6522_MACIICI "mos6522-maciici"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522MacIIciState, MOS6522_MACIICI)

struct MOS6522MacIIciState {
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

static void via1_rtc_update(MOS6522MacIIciState *v1s)
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

    qemu_log_mask(LOG_UNIMP, "maciici rtc: byte 0x%02x (cmd=%02x alt=%02x)\n",
                  v1s->data_out, v1s->cmd, v1s->alt);

    /* first byte: it's a command */
    if (v1s->cmd == REG_EMPTY) {
        cmd = via1_rtc_compact_cmd(v1s->data_out);

        if (cmd == REG_INVALID) {
            qemu_log_mask(LOG_UNIMP, "maciici rtc: invalid cmd 0x%02x\n",
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

static void maciici_adb_poll(void *opaque)
{
    MOS6522MacIIciState *v1s = MOS6522_MACIICI(opaque);
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

static int maciici_adb_send_len(uint8_t data)
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

static void maciici_adb_send(MOS6522MacIIciState *v1s, int state, uint8_t data)
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
    if (v1s->adb_data_out_index == maciici_adb_send_len(v1s->adb_data_out[0])) {
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

static void maciici_adb_receive(MOS6522MacIIciState *v1s, int state,
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

static void maciici_adb_update(MOS6522MacIIciState *v1s)
{
    MOS6522State *s = MOS6522(v1s);
    int oldstate, state;

    oldstate = (v1s->last_b & VIA1B_vADB_StateMask) >> VIA1B_vADB_StateShift;
    state = (s->b & VIA1B_vADB_StateMask) >> VIA1B_vADB_StateShift;

    if (state != oldstate) {
        if (s->acr & SR_OUT) {
            /* output mode */
            maciici_adb_send(v1s, state, s->sr);
        } else {
            /* input mode */
            maciici_adb_receive(v1s, state, &s->sr);
        }
    }
}

static void maciici_via1_portA_write(MOS6522State *s)
{
}

static void maciici_via1_portB_write(MOS6522State *s)
{
}

static void mos6522_maciici_init(Object *obj)
{
    MOS6522MacIIciState *v1s = MOS6522_MACIICI(obj);

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;

    /* the ADB bus hangs off the VIA1 shift-register transceiver */
    qbus_init(&v1s->adb_bus, sizeof(v1s->adb_bus), TYPE_ADB_BUS,
              DEVICE(obj), "adb.0");
}

static void mos6522_maciici_reset_hold(Object *obj, ResetType type)
{
    MOS6522MacIIciState *v1s = MOS6522_MACIICI(obj);
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

static const Property mos6522_maciici_properties[] = {
    DEFINE_PROP_UINT8("pins-a", MOS6522MacIIciState, pins_a, 0xff),
    DEFINE_PROP_UINT8("pins-b", MOS6522MacIIciState, pins_b, 0xff),
};

static void mos6522_maciici_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    MOS6522DeviceClass *mdc = MOS6522_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_props(dc, mos6522_maciici_properties);
    mdc->portA_write = maciici_via1_portA_write;
    mdc->portB_write = maciici_via1_portB_write;
    resettable_class_set_parent_phases(rc, NULL, mos6522_maciici_reset_hold,
                                       NULL, &mdc->parent_phases);
}

/*
 * Onboard video framebuffer: 640x480 1-bit, rowbytes 80.
 *
 * The IIci's RBV video has no dedicated VRAM: the frame buffer is the
 * BOTTOM of RAM bank A.  When a monitor is sensed the ROM reserves
 * physical 0x0000..0x4FFFF (320 KiB, enough for 8-bit 640x480) for
 * video and builds its MMU maps with logical RAM relocated up by
 * 0x50000 (level-A entries 0-6 -> frames 0x050000, 0x150000, ... in
 * the 24-bit map at CRP [0x7FF920]) while the video windows (24-bit
 * 0xB08000, 32-bit virtual 0xFBB08000, slot views) translate down to
 * physical 0x0000.  So the display scans system RAM at offset 0; the
 * CPU never touches a separate VRAM device.  (The old model's fake
 * VRAM at physical 0xFBB08000 + 24-bit aliases only ever worked
 * because a monitor-sense bug made the ROM build no-video identity
 * maps that happened to land on it.)
 */

#define TYPE_MACIICI_FB "maciici-fb"
OBJECT_DECLARE_SIMPLE_TYPE(MacIIciFbState, MACIICI_FB)

#define MACIICI_FB_WIDTH   640
#define MACIICI_FB_HEIGHT  480
#define MACIICI_FB_ROWBYTES 80

struct MacIIciFbState {
    SysBusDevice parent_obj;

    MemoryRegion *ram;          /* scanout source: system RAM, offset 0 */
    MemoryRegionSection fbsection;
    QemuConsole *con;
    int invalidate;
};

static void maciici_fb_draw_line(void *opaque, uint8_t *d, const uint8_t *s,
                                 int width, int pitch)
{
    uint32_t *buf = (uint32_t *)d;
    int i, b;

    for (i = 0; i < MACIICI_FB_WIDTH / 8; i++) {
        uint8_t src = s[i];

        for (b = 0; b < 8; b++) {
            buf[i * 8 + b] = (src & 0x80) ? 0xFF000000 : 0xFFFFFFFF;
            src <<= 1;
        }
    }
}

static bool maciici_fb_update(void *opaque)
{
    MacIIciFbState *s = MACIICI_FB(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first = 0, last = 0;

    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection, s->ram, 0,
                                          MACIICI_FB_HEIGHT,
                                          MACIICI_FB_ROWBYTES);
        s->invalidate = 0;
    }

    framebuffer_update_display(surface, &s->fbsection,
                               MACIICI_FB_WIDTH, MACIICI_FB_HEIGHT,
                               MACIICI_FB_ROWBYTES, MACIICI_FB_WIDTH * 4,
                               0, 1, maciici_fb_draw_line, s,
                               &first, &last);
    qemu_console_update(s->con, 0, 0, MACIICI_FB_WIDTH, MACIICI_FB_HEIGHT);
    return true;
}

static void maciici_fb_invalidate(void *opaque)
{
    MacIIciFbState *s = MACIICI_FB(opaque);

    s->invalidate = 1;
}

static const GraphicHwOps maciici_fb_ops = {
    .invalidate = maciici_fb_invalidate,
    .gfx_update = maciici_fb_update,
};

static void maciici_fb_realize(DeviceState *dev, Error **errp)
{
    MacIIciFbState *s = MACIICI_FB(dev);

    if (!s->ram) {
        error_setg(errp, "maciici-fb: scanout RAM region not set");
        return;
    }

    s->invalidate = 1;
    s->con = qemu_graphic_console_create(dev, 0, &maciici_fb_ops, s);
    qemu_console_resize(s->con, MACIICI_FB_WIDTH, MACIICI_FB_HEIGHT);
}

static void maciici_fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->realize = maciici_fb_realize;
}

/* machine */

struct MacIIciMachineState {
    MachineState parent_obj;

    M68kCPU cpu;
    MacIIciGlueState glue;
    MOS6522MacIIciState via1;
    ESCCState escc;
    OrIRQState escc_orgate;
    ASCState asc;
    Swim swim;
    NCR5380State scsi;
    MacNubusBridge mac_nubus_bridge;

    MacIIciFbState fb;
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
    MemoryRegion vdacmem;
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
};

#define TYPE_MACIICI_MACHINE MACHINE_TYPE_NAME("maciici")
OBJECT_DECLARE_SIMPLE_TYPE(MacIIciMachineState, MACIICI_MACHINE)

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static uint32_t maciici_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

static uint64_t ramio_read(void *opaque, hwaddr addr, unsigned size)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "maciici ram-hole: read  0x%08x (%d) pc=0x%08x\n",
                      (unsigned)addr, size, maciici_trace_pc());
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
                      "maciici ram-hole: write 0x%08x (%d) <- 0x%08" PRIx64
                      " pc=0x%08x\n",
                      (unsigned)addr, size, val, maciici_trace_pc());
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

static MemTxResult maciici_a31_read(void *opaque, hwaddr addr, uint64_t *data,
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

static MemTxResult maciici_a31_write(void *opaque, hwaddr addr, uint64_t value,
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

static const MemoryRegionOps maciici_a31_ops = {
    .read_with_attrs = maciici_a31_read,
    .write_with_attrs = maciici_a31_write,
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

static MemTxResult maciici_escc_mirror_read(void *opaque, hwaddr addr,
                                            uint64_t *data, unsigned size,
                                            MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_read(opaque, addr, data, size, attrs);
}

static MemTxResult maciici_escc_mirror_write(void *opaque, hwaddr addr,
                                             uint64_t val, unsigned size,
                                             MemTxAttrs attrs)
{
    addr = IO_BASE + SCC_OFS + (addr & 7);
    return macio_alias_write(opaque, addr, val, size, attrs);
}

static const MemoryRegionOps maciici_escc_mirror_ops = {
    .read_with_attrs = maciici_escc_mirror_read,
    .write_with_attrs = maciici_escc_mirror_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t maciici_via1_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS6522MacIIciState *v1s = opaque;
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

static void maciici_via1_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MOS6522MacIIciState *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    mos6522_write(s, reg, val, size);

    if (reg == VIA_REG_SR || reg == VIA_REG_ACR) {
        qemu_log_mask(LOG_UNIMP,
                      "maciici via1: reg%d <- 0x%02" PRIx64 " pc=0x%08x\n",
                      (int)reg, val, maciici_trace_pc());
    }

    if (reg == VIA_REG_B) {
        via1_rtc_update(v1s);
        maciici_adb_update(v1s);
        v1s->last_b = s->b;
    }
}

static const MemoryRegionOps maciici_via1_ops = {
    .read = maciici_via1_read,
    .write = maciici_via1_write,
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

static void maciici_rbv_update_irq(MacIIciMachineState *m);

static uint64_t maciici_rbv_read(void *opaque, hwaddr addr, unsigned size)
{
    MacIIciMachineState *m = opaque;
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

    qemu_log_mask(LOG_UNIMP, "maciici rbv: read  +0x%02x -> 0x%02" PRIx64
                  " pc=0x%08x\n", (unsigned)addr, val, maciici_trace_pc());
    return val;
}

static void maciici_rbv_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MacIIciMachineState *m = opaque;

    addr &= 0x1f;
    qemu_log_mask(LOG_UNIMP, "maciici rbv: write +0x%02x <- 0x%02" PRIx64
                  " pc=0x%08x\n", (unsigned)addr, val, maciici_trace_pc());

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
    maciici_rbv_update_irq(m);
}

static const MemoryRegionOps maciici_rbv_ops = {
    .read = maciici_rbv_read,
    .write = maciici_rbv_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* 60.15Hz VBL tick on CA1 and one-second tick on CA2, as on mac_via */

#define VIA_60HZ_TIMER_PERIOD_NS   16625800

static void maciici_rbv_update_irq(MacIIciMachineState *m)
{
    /*
     * All enabled slot lines summarise into IFR bit 1 → level 2,
     * INCLUDING the onboard-video VBL (slot $E, SIFR bit 6): the OS
     * runs its Vertical Retrace Manager slot tasks — among them the
     * cursor task that couples MTemp → RawMouse and redraws the mouse
     * pointer — off this interrupt (SIER reads 0x7F at the Finder).
     * The ROM-era boot polls the line before any handler exists; that
     * is safe because the line PULSES (1.3ms per frame, dropped by
     * maciici_vbl_off) instead of latching, so a masked blank is
     * missed rather than serviced stale (the old dsBadSlotInt came
     * from the pre-pulse latch model, not from delivering bit 6).
     */
    uint8_t slot_cpu = m->rbv_sifr & m->rbv_sier & 0x7f;

    if (slot_cpu) {
        m->rbv_ifr |= RBV_IFR_SLOT;
    } else {
        m->rbv_ifr &= ~RBV_IFR_SLOT;
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&m->glue), MACIICI_GLUE_RBV),
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

static void maciici_scsi_irq(void *opaque, int n, int level)
{
    MacIIciMachineState *m = opaque;

    if (level) {
        m->rbv_ifr |= RBV_IFR_SCSI_IRQ;
    } else {
        m->rbv_ifr &= ~RBV_IFR_SCSI_IRQ;
    }
    maciici_rbv_update_irq(m);
}

static void maciici_vbl_off(void *opaque)
{
    MacIIciMachineState *m = opaque;

    /*
     * End of vertical blank: the slot line and the level-triggered IFR
     * summary drop together, so an interrupt masked past the blank is
     * simply missed rather than serviced stale (dsBadSlotInt).
     */
    m->rbv_sifr &= ~RBV_SLOT_E_INT;
    maciici_rbv_update_irq(m);
}

static void maciici_sixty_hz(void *opaque)
{
    MacIIciMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA1_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    /* onboard video vertical blank = slot $E line pulses via the RBV */
    m->rbv_sifr |= RBV_SLOT_E_INT;
    maciici_rbv_update_irq(m);
    timer_mod(m->vbl_off_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1300000);

    timer_mod(m->sixty_hz_timer,
              (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
               VIA_60HZ_TIMER_PERIOD_NS) /
              VIA_60HZ_TIMER_PERIOD_NS * VIA_60HZ_TIMER_PERIOD_NS);
}

static void maciici_one_second(void *opaque)
{
    MacIIciMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA2_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->one_second_timer,
              (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000) / 1000 * 1000);
}

/*
 * The RBV also emulates a VIA2 at the classic VIA2 site (slice +0x2000):
 * registers are VIA-spaced (offset >> 9), low address bits don't care.
 * IFR (reg 13) and IER (reg 14) share the RBV interrupt state.
 */

static uint64_t maciici_rbv_via2_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    MacIIciMachineState *m = opaque;
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

    qemu_log_mask(LOG_UNIMP, "maciici rbv-via2: read  reg%d -> 0x%02" PRIx64
                  " pc=0x%08x\n", reg, val, maciici_trace_pc());
    return val;
}

static void maciici_rbv_via2_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    MacIIciMachineState *m = opaque;
    int reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    qemu_log_mask(LOG_UNIMP, "maciici rbv-via2: write reg%d <- 0x%02" PRIx64
                  " pc=0x%08x\n", reg, val, maciici_trace_pc());

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
    maciici_rbv_update_irq(m);
}

static const MemoryRegionOps maciici_rbv_via2_ops = {
    .read = maciici_rbv_via2_read,
    .write = maciici_rbv_via2_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* VDAC (CLUT): store + log */

static uint64_t maciici_vdac_read(void *opaque, hwaddr addr, unsigned size)
{
    MacIIciMachineState *m = opaque;
    uint64_t val = m->vdac_regs[addr & 0x3f];

    qemu_log_mask(LOG_UNIMP, "maciici vdac: read  +0x%02x -> 0x%02" PRIx64 "\n",
                  (unsigned)(addr & 0x3f), val);
    return val;
}

static void maciici_vdac_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MacIIciMachineState *m = opaque;

    qemu_log_mask(LOG_UNIMP, "maciici vdac: write +0x%02x <- 0x%02" PRIx64 "\n",
                  (unsigned)(addr & 0x3f), val);
    m->vdac_regs[addr & 0x3f] = val;
}

static const MemoryRegionOps maciici_vdac_ops = {
    .read = maciici_vdac_read,
    .write = maciici_vdac_write,
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

static uint64_t maciici_scsi_pdma_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    NCR5380State *s = opaque;

    return ncr5380_pdma_read(s);
}

static void maciici_scsi_pdma_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    NCR5380State *s = opaque;

    ncr5380_pdma_write(s, val);
}

static const MemoryRegionOps maciici_scsi_pdma_ops = {
    .read = maciici_scsi_pdma_read,
    .write = maciici_scsi_pdma_write,
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

static MemTxResult maciici_scsi_hsk_read(void *opaque, hwaddr addr,
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

static MemTxResult maciici_scsi_hsk_write(void *opaque, hwaddr addr,
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

static const MemoryRegionOps maciici_scsi_hsk_ops = {
    .read_with_attrs = maciici_scsi_hsk_read,
    .write_with_attrs = maciici_scsi_hsk_write,
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

static void maciici_irq_sink(void *opaque, int n, int level)
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
static bool maciici_iotrace_is_asc_gap(hwaddr addr)
{
    return addr >= ASC_OFS && addr < ASC_OFS + 0x2000;
}

static MemTxResult maciici_iotrace_read(void *opaque, hwaddr addr,
                                        uint64_t *data, unsigned size,
                                        MemTxAttrs attrs)
{
    if (maciici_iotrace_is_asc_gap(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "maciici io: read  +0x%05x (%d) -> asc gap (0)\n",
                      (unsigned)addr, size);
        *data = 0;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "maciici io: read  +0x%05x (%d) -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, maciici_trace_pc());
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult maciici_iotrace_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size,
                                         MemTxAttrs attrs)
{
    if (maciici_iotrace_is_asc_gap(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "maciici io: write +0x%05x (%d) <- 0x%08" PRIx64
                      " -> asc gap (ignored)\n",
                      (unsigned)addr, size, val);
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "maciici io: write +0x%05x (%d) <- 0x%08" PRIx64
                  " -> BERR pc=0x%08x\n",
                  (unsigned)addr, size, val, maciici_trace_pc());
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps maciici_iotrace_ops = {
    .read_with_attrs = maciici_iotrace_read,
    .write_with_attrs = maciici_iotrace_write,
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
static void maciici_nubus_irq(void *opaque, int n, int level)
{
    MacIIciMachineState *m = opaque;
    uint8_t bit = 1 << ((n & 0xf) - 8);

    if (level) {
        m->rbv_sifr |= bit;
    } else {
        m->rbv_sifr &= ~bit;
    }
    maciici_rbv_update_irq(m);
}

static void maciici_machine_init(MachineState *machine)
{
    MacIIciMachineState *m = MACIICI_MACHINE(machine);
    ram_addr_t ram_size = machine->ram_size;
    const char *bios_name = machine->firmware ?: MACIICI_ROM_FILENAME;
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
                          &maciici_a31_ops, m, "maciici.ram-a31",
                          0x80000000);
    memory_region_add_subregion_overlap(get_system_memory(), 0x80000000,
                                        &m->ramio_a31, -2);

    /* IRQ glue */
    object_initialize_child(OBJECT(machine), "glue", &m->glue,
                            TYPE_MACIICI_GLUE);
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
    memory_region_init_io(&m->iotrace, OBJECT(machine), &maciici_iotrace_ops,
                          m, "mac-io.trace", IO_SLICE);
    memory_region_add_subregion_overlap(&m->macio, 0, &m->iotrace, -1);

    /* VIA1 */
    object_initialize_child(OBJECT(machine), "via1", &m->via1,
                            TYPE_MOS6522_MACIICI);
    qdev_prop_set_uint64(DEVICE(&m->via1), "frequency", VIA_TIMER_FREQ);
    /*
     * Machine-ID straps on port A (read with DDRA all-input during
     * identification): the ROM requires (PA & 0x56) == 0x46 — PA4 low —
     * to select the Mac IIci configuration; PA4 high selects a sibling
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

        adb_register_autopoll_callback(adb_bus, maciici_adb_poll, &m->via1);
        m->via1.adb_data_ready = qdev_get_gpio_in(DEVICE(&m->via1),
                                                  SR_INT_BIT);

        dev = qdev_new(TYPE_ADB_KEYBOARD);
        qdev_realize_and_unref(dev, BUS(adb_bus), &error_fatal);
        dev = qdev_new(TYPE_ADB_MOUSE);
        qdev_realize_and_unref(dev, BUS(adb_bus), &error_fatal);

        adb_set_autopoll_enabled(adb_bus, true);
    }
    m->vbl_off_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maciici_vbl_off, m);
    m->sixty_hz_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maciici_sixty_hz, m);
    timer_mod(m->sixty_hz_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VIA_60HZ_TIMER_PERIOD_NS);
    m->one_second_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, maciici_one_second,
                                       m);
    timer_mod(m->one_second_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), MACIICI_GLUE_VIA1));
    memory_region_init_io(&m->via1mem, OBJECT(machine), &maciici_via1_ops,
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
                          &maciici_rbv_via2_ops, m, "rbv-via2",
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
                                           MACIICI_GLUE_SCC));
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
    /* TODO: route via VIA2 interrupt inputs; direct CPU wiring storms */
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(maciici_irq_sink, m, 0));

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
                          &maciici_escc_mirror_ops, NULL, "escc-mirror",
                          0x2000);
    memory_region_add_subregion(&m->macio, SCC_OFS | 0x8000, &m->escc_mirror);

    memory_region_init_alias(&m->swim_mirror, OBJECT(machine), "swim-mirror",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->swim),
                                                    0),
                             0, 0x2000);
    memory_region_add_subregion(&m->macio, SWIM_OFS | 0x8000, &m->swim_mirror);

    /* RBV / VDAC / SCSI stubs */
    memory_region_init_io(&m->rbvmem, OBJECT(machine), &maciici_rbv_ops, m,
                          "rbv", 0x2000);
    memory_region_add_subregion(&m->macio, RBV_OFS, &m->rbvmem);

    memory_region_init_io(&m->vdacmem, OBJECT(machine), &maciici_vdac_ops, m,
                          "vdac", 0x40);
    memory_region_add_subregion(&m->macio, VDAC_OFS, &m->vdacmem);

    /* NCR5380 SCSI controller and its pseudo-DMA aperture (+0x2000) */
    object_initialize_child(OBJECT(machine), "scsi", &m->scsi, TYPE_NCR5380);
    qdev_prop_set_uint8(DEVICE(&m->scsi), "reg-shift", 4);
    sysbus = SYS_BUS_DEVICE(&m->scsi);
    sysbus_realize(sysbus, &error_fatal);
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(maciici_scsi_irq, m, 0));
    memory_region_add_subregion(&m->macio, SCSI_OFS,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_init_io(&m->scsi_pdma, OBJECT(machine),
                          &maciici_scsi_pdma_ops, &m->scsi, "scsi-pdma", 0x2000);
    memory_region_add_subregion(&m->macio, SCSI_OFS + 0x2000, &m->scsi_pdma);

    /* handshake pseudo-DMA aperture ("SCSI+DRQ", 0x50F06000) */
    memory_region_init_io(&m->scsi_hsk, OBJECT(machine),
                          &maciici_scsi_hsk_ops, &m->scsi, "scsi-hsk", 0x2000);
    memory_region_add_subregion(&m->macio, 0x6000, &m->scsi_hsk);

    scsi_bus_legacy_handle_cmdline(&m->scsi.bus);

    /* ROM */
    memory_region_init_rom(&m->rom, NULL, "maciici.rom", MACIICI_ROM_SIZE,
                           &error_abort);
    memory_region_add_subregion(get_system_memory(), MACIICI_ROM_ADDR,
                                &m->rom);

    memory_region_init_alias(&m->rom_alias, NULL, "maciici.rom-alias",
                             &m->rom, 0, MACIICI_ROM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x40000000,
                                &m->rom_alias);

    /*
     * NOTE: the onboard-video Declaration ROM (top 0x1400 bytes of the
     * system ROM) must NOT be aliased into slot $E standard space.  The
     * ROM registers the motherboard DeclROM itself as pseudo-slot 0
     * (sInfo siDirPtr = 0x4087EC1A, read straight out of the system
     * ROM), and the family PrimaryInit sExec in the DeclROM prunes the
     * slot-0 Slot Resource Table records down to the machine- and
     * monitor-appropriate subset with spSlot hardcoded to 0.  An alias
     * at 0xFEF80000 (tried earlier) makes the generic slot scan register
     * a SECOND, never-pruned copy of the whole family DeclROM as slot
     * $E; the System then opens every video sResource in it, including
     * the Mac LC's .Display_Video_Apple_VISA driver whose stub scans
     * the LC ROM location 0x00A44000 for its 'Dave'-tagged body and
     * bus-errors fatally on a IIci.
     */

    /*
     * Onboard video: the display scans the bottom of RAM (see the
     * maciici-fb comment).  All CPU-side framebuffer addresses
     * (ScrnBase 0xFBB08000, the 24-bit window 0xB08000, slot views)
     * are VIRTUAL and resolve through the ROM's PMMU maps to physical
     * RAM 0x0000+; there is no separate VRAM decode to model.
     */
    object_initialize_child(OBJECT(machine), "fb", &m->fb, TYPE_MACIICI_FB);
    m->fb.ram = machine->ram;
    sysbus = SYS_BUS_DEVICE(&m->fb);
    sysbus_realize(sysbus, &error_fatal);

    /*
     * NuBus card bus.  The IIci has only the 030 PDS, but a PDS->NuBus
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

        slot_irq = qemu_allocate_irqs(maciici_nubus_irq, m,
                                      MAC_NUBUS_LAST_SLOT + 1);
        for (slot = MAC_NUBUS_FIRST_SLOT; slot <= MAC_NUBUS_LAST_SLOT; slot++) {
            qdev_connect_gpio_out(DEVICE(&m->mac_nubus_bridge), slot,
                                  slot_irq[slot]);
        }
    }

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, MACIICI_ROM_ADDR,
                                        MACIICI_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }

    if (!qtest_enabled()) {
        if (bios_size <= 0 || bios_size > MACIICI_ROM_SIZE) {
            error_report("could not load MacROM '%s'", bios_name);
            exit(1);
        }

        uint32_t entry;

        cs = CPU(&m->cpu);
        ptr = rom_ptr(MACIICI_ROM_ADDR, bios_size);
        assert(ptr != NULL);
        stl_phys(cs->as, 0, ldl_be_p(ptr));    /* reset initial SP */
        /*
         * Reset initial PC: the IIci ROM stores the ABSOLUTE entry
         * address (0x4080002A) at +4, unlike the IIsi/Classic II ROMs
         * which store a base-relative offset (0x2A).  Handle both.
         */
        entry = ldl_be_p(ptr + 4);
        if (entry < MACIICI_ROM_SIZE) {
            entry += MACIICI_ROM_ADDR;
        }
        stl_phys(cs->as, 4, entry);
    }
}

static void maciici_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68030"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Macintosh IIci";
    mc->init = maciici_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->block_default_type = IF_SCSI;
    mc->default_ram_size = 8 * MiB;
    mc->default_ram_id = "maciici.ram";
    machine_add_audiodev_property(mc);
}

static const TypeInfo maciici_machine_typeinfo[] = {
    {
        .name       = TYPE_MACIICI_GLUE,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacIIciGlueState),
        .instance_init = maciici_glue_init,
        .class_init = maciici_glue_class_init,
    },
    {
        .name       = TYPE_MOS6522_MACIICI,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522MacIIciState),
        .instance_init = mos6522_maciici_init,
        .class_init = mos6522_maciici_class_init,
    },
    {
        .name       = TYPE_MACIICI_FB,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MacIIciFbState),
        .class_init = maciici_fb_class_init,
    },
    {
        .name       = TYPE_MACIICI_MACHINE,
        .parent     = TYPE_MACHINE,
        .instance_size = sizeof(MacIIciMachineState),
        .class_init = maciici_machine_class_init,
    },
};

DEFINE_TYPES(maciici_machine_typeinfo)
