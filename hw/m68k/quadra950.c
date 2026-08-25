/*
 * QEMU Macintosh Quadra 950 / Quadra 900 hardware system emulator
 *
 * The Quadra 900/950 are the Quadra 700's big towers (q800-family:
 * 68040, VIA1 + VIA2, MCU memory controller at 0x50F0E000, ESP 53C96
 * SCSI at 0x50F0F000 with the DAFB "TurboSCSI" pseudo-DMA handshake,
 * SONIC Ethernet, ESCC serial, SWIM floppy, EASC sound, NuBus with the
 * built-in DAFB video in pseudo-slot 9) with two crucial deltas:
 *
 *  - serial and floppy are fronted by IOPs (6502-based I/O processors)
 *    at 0x50F0C000 (SCC IOP) and 0x50F1E000 (SWIM/ISM IOP), spoken to
 *    through a shared-RAM mailbox (Linux asm/mac_iop.h layout); the
 *    underlying SCC is still reachable through the bypass window at
 *    0x50F0C020;
 *  - ADB/PRAM/RTC live behind an Egret MCU on the VIA1 shift register
 *    (Cuda packet format, but the host asserts TIP/TACK active-HIGH,
 *    per Linux via-cuda.c), not the classic RTC/ADB-II pair.
 *
 * Machine identification (ROM 3DC27823 universal table, entries at
 * +0x387c/+0x38bc): decoder kind 8 (successful long read of the MCU at
 * 0x5000E000) plus VIA1 port A straps (PA & 0x56): 0x50 = Quadra 900
 * (box 0x0e), 0x10 = Quadra 950 (box 0x14).
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
#include "qemu/guest-random.h"
#include "exec/target_page.h"
#include "system/system.h"
#include "target/m68k/cpu.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "elf.h"
#include "hw/core/loader.h"
#include "ui/console.h"
#include "hw/char/escc.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/scsi/esp.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "standard-headers/asm-m68k/bootinfo-mac.h"
#include "bootinfo.h"
#include "hw/m68k/q800-glue.h"
#include "hw/misc/mos6522.h"
#include "hw/misc/mac_via.h"
#include "hw/input/adb.h"
#include "hw/audio/asc.h"
#include "hw/nubus/mac-nubus-bridge.h"
#include "hw/display/macfb.h"
#include "hw/block/swim.h"
#include "hw/net/dp8393x.h"
#include "net/net.h"
#include "net/util.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "system/qtest.h"
#include "system/runstate.h"
#include "system/reset.h"
#include "system/rtc.h"
#include "qemu/cutils.h"
#include "migration/vmstate.h"

#define MACROM_ADDR     0x40800000
#define MACROM_SIZE     0x00100000

#define MACROM_FILENAME "quadra950.rom"

#define IO_BASE               0x50000000
#define IO_SLICE              0x00040000
#define IO_SLICE_MASK         (IO_SLICE - 1)
#define IO_SIZE               0x04000000

#define VIA_BASE              (IO_BASE + 0x00000)
#define SONIC_PROM_BASE       (IO_BASE + 0x08000)
#define SONIC_BASE            (IO_BASE + 0x0a000)
#define SCC_IOP_BASE          (IO_BASE + 0x0c000)
#define SCC_BASE              (IO_BASE + 0x0c020)
#define ESP_BASE              (IO_BASE + 0x0f000)
#define ESP_PDMA              (IO_BASE + 0x0f100)
#define ASC_BASE              (IO_BASE + 0x14000)
#define SWIM_IOP_BASE         (IO_BASE + 0x1E000)
#define SWIM_BASE             (IO_BASE + 0x1E000)

#define SONIC_PROM_SIZE       0x1000

/*
 * The DAFB (slot 9) register space contains the "TurboSCSI" pseudo-DMA
 * handshake register: 32-bit reg at +0x24, bit 9 = live SCSI DRQ
 * (MAC_SCSI_QUADRA2, as on the Quadra 700).
 */
#define DAFB_REGS_BASE        0xf9800000
#define TURBOSCSI_BASE        (DAFB_REGS_BASE + 0x20)
#define TURBOSCSI_SIZE        0x10
#define TURBOSCSI_DRQ         0x200

#define VIDEO_BASE            0xf9000000

#define MAC_CLOCK  3686418

#define VIA_TIMER_FREQ        783360
#define VIA_60HZ_TIMER_PERIOD_NS   16625800

/* Size of whole RAM area */
#define RAM_SIZE              0x40000000

/*
 * Slot 0x9 is the in-built framebuffer; the towers have five physical
 * NuBus slots 0xa..0xe
 */
#define Q950_NUBUS_SLOTS_AVAILABLE    (BIT(0x9) | BIT(0xa) | BIT(0xb) | \
                                       BIT(0xc) | BIT(0xd) | BIT(0xe))

/* Same ID register value as the other Quadra-class machines */
#define Q950_MACHINE_ID    0xa55a2bad

/*
 * VIA1 port A machine-ID straps (mask 0x56: PA1, PA2, PA4, PA6), from
 * the ROM's universal table: (PA & 0x56) == 0x10 -> box 0x14 = Quadra
 * 950, == 0x50 -> box 0x0e = Quadra 900.  PA0 high (low = ROM burn-in
 * mode), PA7 high as on the Quadra 700.
 */
#define Q950_VIA1_PINS_A    0x91        /* PA7 | PA4 | PA0 */
#define Q900_VIA1_PINS_A    0xd1        /* PA7 | PA6 | PA4 | PA0 */

/* VIA returns time offset from Jan 1, 1904, not 1970 */
#define RTC_OFFSET 2082844800

static int q950_log_budget = 400000;

#define q950_log(...) do {                          \
        if (q950_log_budget > 0) {                  \
            q950_log_budget--;                      \
            qemu_log_mask(LOG_UNIMP, __VA_ARGS__);  \
        }                                           \
    } while (0)

/*
 * Egret handshake lines on VIA1 port B.  Per Linux via-cuda.c the
 * packet protocol is the Cuda's, but the host asserts TIP/TACK
 * active-HIGH on an Egret (idle low); the Egret asserts /TREQ LOW.
 */
#define EGRET_TREQ       0x08    /* in:  Egret transfer request (act. low) */
#define EGRET_TACK       0x10    /* out: byte acknowledge (active high) */
#define EGRET_TIP        0x20    /* out: transfer in progress (act. high) */

/* Cuda/Egret packet type bytes */
#define EGRET_PKT_ADB     0
#define EGRET_PKT_PSEUDO  1
#define EGRET_PKT_ERROR   2

/*
 * pseudo commands.  The ROM's _EgretDispatch trap builds packets with
 * cmd 0x02 = block read / 0x08 = block write (address space: 0-3 the
 * clock, 0x100-0x1ff the PRAM); the polled boot driver uses 0x07 with
 * flat addresses for the same block-read semantics.
 */
#define EGRET_CMD_AUTOPOLL       0x01
#define EGRET_CMD_READ_PRAM      0x02
#define EGRET_CMD_GET_TIME       0x03
#define EGRET_CMD_GET_PRAM       0x07
#define EGRET_CMD_WRITE_PRAM     0x08
#define EGRET_CMD_SET_TIME       0x09
#define EGRET_CMD_SET_PRAM       0x0c

/* classic 343-0042 RTC bit-bang lines on VIA1 port B (fallback) */
#define VIA1B_vRTCEnb  0x04
#define VIA1B_vRTCClk  0x02
#define VIA1B_vRTCData 0x01

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

#define TYPE_MOS6522_Q950 "mos6522-q950"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522Q950State, MOS6522_Q950)

struct Q950MachineState;

struct MOS6522Q950State {
    MOS6522State parent_obj;

    struct Q950MachineState *machine;
    ADBBusState adb_bus;
    uint8_t last_b;
    uint8_t pins_a;

    /* PRAM (accessed via Egret pseudo commands and the RTC fallback) */
    uint8_t PRAM[256];
    uint32_t tick_offset;

    /* classic RTC bit-bang engine state */
    uint8_t data_out;
    int data_out_cnt;
    uint8_t data_in;
    int data_in_cnt;
    uint8_t cmd;
    uint8_t alt;
    uint8_t wprotect;
};

/*
 * IOP host interface (Linux asm/mac_iop.h): +0/+1 ram_addr hi/lo,
 * +4..+7 status_ctrl, +8..+0x1f ram_data (autoincrementing).  32K of
 * shared RAM.  No 6502 core: the minimum liveness the ROM demands is
 * modelled instead (alive flag on RUN, mailbox send-channel
 * completion on IRQ).
 */

#define IOP_BYPASS       0x01
#define IOP_AUTOINC      0x02
#define IOP_RUN          0x04
#define IOP_IRQ          0x08
#define IOP_INT0         0x10
#define IOP_INT1         0x20
#define IOP_HWINT        0x40
#define IOP_DMAINACTIVE  0x80

#define IOP_ADDR_MAX_SEND_CHAN 0x0200
#define IOP_ADDR_MAX_RECV_CHAN 0x0300
#define IOP_ADDR_SEND_STATE   0x0201
#define IOP_ADDR_SEND_MSG     0x0220
#define IOP_ADDR_RECV_STATE   0x0301
#define IOP_ADDR_ALIVE        0x031f
#define IOP_ADDR_RECV_MSG     0x0320
#define IOP_MSG_LEN           32
#define NUM_IOP_CHAN          7

#define IOP_MSG_IDLE          0
#define IOP_MSG_NEW           1
#define IOP_MSG_RCVD          2
#define IOP_MSG_COMPLETE      3

/* ADB-over-IOP message flags (Linux asm/adb_iop.h) */
#define ADB_CHAN              2
#define ADB_IOP_EXPLICIT      0x80
#define ADB_IOP_AUTOPOLL      0x40
#define ADB_IOP_SET_AUTOPOLL  0x20
#define ADB_IOP_SRQ           0x04
#define ADB_IOP_TIMEOUT       0x02

typedef struct Q950IOP {
    struct Q950MachineState *machine;
    const char *name;
    bool is_ism;                /* the SWIM/ISM IOP carries ADB */
    MemoryRegion mem;
    uint8_t ram[0x8000];
    uint16_t addr;
    uint8_t ctrl;
    qemu_irq irq;               /* asserted while INT0/INT1 pending */
    QEMUTimer *timer;           /* defers message processing */

    /*
     * Snapshot of the last content WE posted into the ADB channel's
     * RECEIVE message slot.  The ROM's ADB-explicit-command driver
     * does not use a fresh SEND_MSG slot for follow-up commands in a
     * request/reply "conversation": after consuming a reply (RECV_STATE
     * walked NEW->RCVD->COMPLETE, i.e. back to IDLE), it stages the
     * NEXT outgoing adb_iopmsg IN PLACE in that same RECV_MSG slot and
     * kicks -- there is no corresponding SEND_STATE transition to
     * NEW.  We detect this by diffing the slot's content against what
     * we last wrote there (see q950_iop_adb_check_staged()).
     */
    uint8_t adb_last_recv[IOP_MSG_LEN];
    bool adb_last_recv_valid;
} Q950IOP;

typedef struct {
    const char *name;
    uint8_t *regs;
    uint32_t size;
    int log_budget;
} Q950RegBank;

struct Q950MachineState {
    MachineState parent_obj;

    bool easc;
    uint8_t pins_a;
    M68kCPU cpu;
    MemoryRegion rom;
    MemoryRegion rom_alias;
    GLUEState glue;
    MOS6522Q950State via1;
    MOS6522Q800VIA2State via2;
    dp8393xState dp8393x;
    MemoryRegion dp8393x_prom;
    ESCCState escc;
    OrIRQState escc_orgate;
    SysBusESPState esp;
    Swim swim;
    MacNubusBridge mac_nubus_bridge;
    MacfbNubusState macfb;
    ASCState asc;
    MemoryRegion ramio;
    MemoryRegion macio;
    MemoryRegion macio_alias;
    MemoryRegion machine_id;
    MemoryRegion via1mem;
    MemoryRegion turboscsi_mem;
    MemoryRegion mcu_mem;
    uint8_t mcu_regs[0x2000];
    Q950IOP scc_iop;
    Q950IOP swim_iop;
    MemoryRegion iotrace;
    MemoryRegion bgtrace;

    /* TurboSCSI pseudo-DMA handshake */
    uint32_t turboscsi_ctrl;
    int esp_drq;
    qemu_irq esp_drq_via2;

    /* VIA1 CA1 60Hz tick and CA2 one-second tick */
    QEMUTimer *sixty_hz_timer;
    QEMUTimer *one_second_timer;

    /* Egret MCU engine on the VIA1 shift register */
    QEMUTimer *egret_timer;
    uint8_t egret_cmd[272 + ADB_MAX_OUT_LEN];
    int egret_cmd_len;
    uint8_t egret_resp[288 + ADB_MAX_OUT_LEN];
    int egret_resp_len;
    int egret_resp_idx;
    bool egret_session;
    bool egret_sync;
    /*
     * Transport mode latch: false = the boot ROM's polled drivers
     * (Cuda packet framing, active-HIGH TIP/TACK sessions, SR-read
     * driven delivery, /TREQ low while a response flows); true = the
     * interrupt-driven OS ADB driver (raw ADB bytes, active-LOW
     * TIP/TACK, preloaded SR, toggle-acknowledged delivery, /TREQ
     * high while a response flows — the maciisi.c protocol).
     */
    bool egret_osmode;
    bool egret_no_resp;

    /* ADB autopoll routed through the ISM IOP mailbox */
    bool iop_autopoll;

    /* SETUPTIMEK calibration hack (see mac_via.c) */
    int timer_hack_state;
};

struct Q950MachineClass {
    MachineClass parent_class;

    uint8_t pins_a;
    int mac_model;
};

#define TYPE_Q950_MACHINE MACHINE_TYPE_NAME("quadra950-common")
OBJECT_DECLARE_TYPE(Q950MachineState, Q950MachineClass, Q950_MACHINE)

typedef struct Q950MachineData {
    const char *desc;
    uint8_t pins_a;
    int mac_model;
} Q950MachineData;

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static uint32_t q950_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

static void rerandomize_rng_seed(void *opaque)
{
    struct bi_record *rng_seed = opaque;
    qemu_guest_getrandom_nofail((void *)rng_seed->data + 2,
                                be16_to_cpu(*(uint16_t *)rng_seed->data));
}

/* ------------------------------------------------------------------ */
/* classic 343-0042 RTC bit-bang engine (fallback; ported from lc475) */

static int via1_rtc_compact_cmd(uint8_t value)
{
    uint8_t read = value & 0x80;

    value &= 0x7f;

    if ((value & 0x78) == 0x38) {
        return read | (REG_PRAM_SECT + (value & 0x07));
    }
    if ((value & 0x03) == 0x01) {
        value >>= 2;
        if ((value & 0x18) == 0) {
            return read | (REG_0 + (value & 0x03));
        } else if ((value == 0x0c) && !read) {
            return REG_TEST;
        } else if ((value == 0x0d) && !read) {
            return REG_WPROTECT;
        } else if ((value & 0x1c) == 0x08) {
            return read | (REG_PRAM_ADDR + 0x10 + (value & 0x03));
        } else if ((value & 0x10) == 0x10) {
            return read | (REG_PRAM_ADDR + (value & 0x0f));
        }
    }
    return REG_INVALID;
}

static void via1_rtc_update(MOS6522Q950State *v1s)
{
    MOS6522State *s = MOS6522(v1s);
    int cmd, sector, addr;
    uint32_t time;

    if (s->b & VIA1B_vRTCEnb) {
        return;
    }

    if (s->dirb & VIA1B_vRTCData) {
        if (!(v1s->last_b & VIA1B_vRTCClk) && (s->b & VIA1B_vRTCClk)) {
            v1s->data_out <<= 1;
            v1s->data_out |= s->b & VIA1B_vRTCData;
            v1s->data_out_cnt++;
        }
    } else {
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

    if (v1s->cmd == REG_EMPTY) {
        cmd = via1_rtc_compact_cmd(v1s->data_out);

        if (cmd == REG_INVALID) {
            return;
        }

        if (cmd & 0x80) { /* this is a read command */
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
            case REG_PRAM_SECT...REG_PRAM_SECT_LAST:
                v1s->cmd = cmd;
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

    if (v1s->alt == REG_EMPTY) {
        switch (v1s->cmd & 0x7f) {
        case REG_0...REG_3:
        case REG_TEST:
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
                v1s->data_in = v1s->PRAM[sector * 32 + addr];
                v1s->data_in_cnt = 8;
                v1s->cmd = REG_EMPTY;
            } else {
                v1s->alt = addr;
            }
            break;
        default:
            g_assert_not_reached();
        }
        return;
    }

    g_assert(REG_PRAM_SECT <= v1s->cmd && v1s->cmd <= REG_PRAM_SECT_LAST);
    sector = v1s->cmd - REG_PRAM_SECT;
    v1s->PRAM[sector * 32 + v1s->alt] = v1s->data_out;
    v1s->alt = REG_EMPTY;
    v1s->cmd = REG_EMPTY;
}

/* ------------------------------------------------------------------ */
/* VIA1 subclass fronting the Egret */

static void q950_egret_session_update(MOS6522Q950State *v1s);
static void q950_egret_sr_written(MOS6522Q950State *v1s);
static void q950_egret_acr_changed(MOS6522Q950State *v1s);
static void q950_egret_idle_treq(MOS6522Q950State *v1s);

static void q950_via1_portA_write(MOS6522State *s)
{
}

static void q950_via1_portB_write(MOS6522State *s)
{
    MOS6522Q950State *v1s = MOS6522_Q950(s);

    if (v1s->machine) {
        q950_egret_session_update(v1s);
        q950_egret_idle_treq(v1s);
    }
}

static void mos6522_q950_init(Object *obj)
{
    MOS6522Q950State *v1s = MOS6522_Q950(obj);

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;

    /* the ADB bus hangs off the Egret; the VIA1 device stands in for it */
    qbus_init(&v1s->adb_bus, sizeof(v1s->adb_bus), TYPE_ADB_BUS,
              DEVICE(obj), "adb.0");
}

static void mos6522_q950_reset_hold(Object *obj, ResetType type)
{
    MOS6522Q950State *v1s = MOS6522_Q950(obj);
    MOS6522State *ms = MOS6522(v1s);
    MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(ms);

    if (mdc->parent_phases.hold) {
        mdc->parent_phases.hold(obj, type);
    }

    /*
     * Board-specific idle state of the input pins: port A straps;
     * port B has /TREQ (PB3) deasserted HIGH, TIP/TACK idle low
     * (Egret polarity) but the pull-ups leave undriven lines high.
     */
    ms->a = v1s->pins_a;
    ms->b = 0xff;

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;
    v1s->data_out_cnt = 0;
    v1s->data_in_cnt = 0;
}

static void mos6522_q950_class_init(ObjectClass *klass, const void *data)
{
    MOS6522DeviceClass *mdc = MOS6522_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    mdc->portA_write = q950_via1_portA_write;
    mdc->portB_write = q950_via1_portB_write;
    resettable_class_set_parent_phases(rc, NULL, mos6522_q950_reset_hold,
                                       NULL, &mdc->parent_phases);
}

/* ------------------------------------------------------------------ */
/* generic machine plumbing (as on quadra700) */

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

static uint64_t machine_id_read(void *opaque, hwaddr addr, unsigned size)
{
    /* long reads want 0xA55Axxxx; word reads compare the low word */
    return Q950_MACHINE_ID & ((size == 4) ? 0xffffffff :
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

static uint64_t ramio_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0x0;
}

static void ramio_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
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
 * DAFB TurboSCSI pseudo-DMA handshake: 32-bit register at DAFB + 0x24,
 * bit 9 reads back the live ESP DRQ.
 */

static uint64_t q950_turboscsi_read(void *opaque, hwaddr addr, unsigned size)
{
    Q950MachineState *m = opaque;
    uint32_t reg = 0;
    uint64_t val;
    int i;

    if ((addr & ~3) == 4) {     /* 0xf9800024 */
        reg = (m->turboscsi_ctrl & ~TURBOSCSI_DRQ) |
              (m->esp_drq ? TURBOSCSI_DRQ : 0);
    }

    /* slice the 32-bit register by byte lane */
    val = 0;
    for (i = 0; i < size; i++) {
        val = (val << 8) | ((reg >> (8 * (3 - ((addr + i) & 3)))) & 0xff);
    }
    return val;
}

static void q950_turboscsi_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    Q950MachineState *m = opaque;

    if ((addr & ~3) == 4) {
        m->turboscsi_ctrl = val;
    }
}

static const MemoryRegionOps q950_turboscsi_ops = {
    .read = q950_turboscsi_read,
    .write = q950_turboscsi_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* ------------------------------------------------------------------ */
/* IOPs */

static void q950_iop_update_irq(Q950IOP *iop)
{
    if (iop->irq) {
        qemu_set_irq(iop->irq, (iop->ctrl & (IOP_INT0 | IOP_INT1)) != 0);
    }
}

static void q950_iop_adb_msg(Q950IOP *iop, uint8_t *msg);

/* post a message on an IOP receive channel (IOP -> host direction) */
static void q950_iop_post_recv(Q950IOP *iop, int chan, const uint8_t *msg,
                               int len)
{
    uint8_t *dst = &iop->ram[IOP_ADDR_RECV_MSG + chan * IOP_MSG_LEN];

    memset(dst, 0, IOP_MSG_LEN);
    memcpy(dst, msg, MIN(len, IOP_MSG_LEN));
    iop->ram[IOP_ADDR_RECV_STATE + chan] = IOP_MSG_NEW;
    iop->ctrl |= IOP_INT1;
    q950_iop_update_irq(iop);
    q950_log("q950 iop %s: recv chan %d posted [%02x %02x %02x ...]\n",
             iop->name, chan, msg[0], msg[1], msg[2]);

    if (iop->is_ism && chan == ADB_CHAN) {
        memcpy(iop->adb_last_recv, dst, IOP_MSG_LEN);
        iop->adb_last_recv_valid = true;
    }
}

/*
 * Check whether the ROM has staged a follow-up explicit ADB command
 * directly in the ADB channel's RECEIVE message slot (see the comment
 * on Q950IOP::adb_last_recv).  This only makes sense once the host has
 * fully consumed our previous reply there (RECV_STATE walked back to
 * IDLE) and the slot's content has changed to something with the
 * EXPLICIT flag set that isn't just an echo of our own last reply.
 */
static bool q950_iop_adb_check_staged(Q950IOP *iop)
{
    uint8_t *slot;

    if (!iop->is_ism || !iop->adb_last_recv_valid) {
        return false;
    }
    if (iop->ram[IOP_ADDR_RECV_STATE + ADB_CHAN] != IOP_MSG_IDLE) {
        return false;               /* host hasn't consumed our reply yet */
    }

    slot = &iop->ram[IOP_ADDR_RECV_MSG + ADB_CHAN * IOP_MSG_LEN];
    if (!(slot[0] & ADB_IOP_EXPLICIT)) {
        return false;
    }
    if (memcmp(slot, iop->adb_last_recv, IOP_MSG_LEN) == 0) {
        return false;               /* unchanged: still our own old reply */
    }

    q950_log("q950 iop %s: DBG staged follow-up ADB cmd in recv slot "
             "[%02x %02x %02x ...]\n", iop->name, slot[0], slot[1], slot[2]);
    q950_iop_adb_msg(iop, slot);
    return true;
}

/*
 * ADB service on the ISM IOP, channel 2 (Linux adb-iop.c protocol):
 * struct adb_iopmsg { flags, count, cmd, data[8], spare[21] }.
 * Explicit commands are answered with a reply message on the RECEIVE
 * channel ([flags, count=data len, cmd, data...]; TIMEOUT flag when
 * the device has nothing); SET_AUTOPOLL carries a 16-bit device
 * bitmap in data[0..1].
 */
static void q950_iop_adb_msg(Q950IOP *iop, uint8_t *msg)
{
    Q950MachineState *m = iop->machine;
    ADBBusState *adb_bus = &m->via1.adb_bus;
    uint8_t flags = msg[0], count = msg[1], cmd = msg[2];

    if (flags & ADB_IOP_SET_AUTOPOLL) {
        uint16_t mask = (msg[3] << 8) | msg[4];
        bool enable = (flags & ADB_IOP_AUTOPOLL) && mask != 0;

        adb_set_autopoll_mask(adb_bus, mask);
        adb_set_autopoll_enabled(adb_bus, enable);
        m->iop_autopoll = enable;
        q950_log("q950 iop adb: autopoll mask 0x%04x %s\n", mask,
                 enable ? "on" : "off");
        return;
    }

    if (flags & ADB_IOP_EXPLICIT) {
        uint8_t req[1 + 8];
        uint8_t obuf[ADB_MAX_OUT_LEN];
        int dlen = MIN(count, 8);
        int olen, reqlen;

        req[0] = cmd;
        memcpy(req + 1, msg + 3, dlen);
        /* only Listen commands carry data on the bus */
        reqlen = ((cmd & 0x0c) == 0x08) ? 1 + dlen : 1;

        adb_autopoll_block(adb_bus);
        olen = adb_request(adb_bus, obuf, req, reqlen);
        adb_autopoll_unblock(adb_bus);

        /*
         * The send message completes fire-and-forget (its request has
         * no completion routine); the ADB REPLY is delivered as a
         * RECEIVE-channel message [flags, count, cmd, data...].  The
         * ROM's recv-listener completion (0x4080a566) checks flags
         * bit7: SET = reply to an explicit command (-> advance the
         * command queue, 0x4080a45e), CLEAR = autopolled data
         * (-> 0x4080a494); bit1 = timeout, bit2 = SRQ — the Linux
         * adb_iop.h convention.
         */
        {
            uint8_t reply[3 + 8];

            memset(reply, 0, sizeof(reply));
            reply[2] = cmd;
            if (olen > 0) {
                reply[0] = ADB_IOP_EXPLICIT;
                reply[1] = MIN(olen, 8);
                memcpy(reply + 3, obuf, MIN(olen, 8));
            } else {
                reply[0] = ADB_IOP_EXPLICIT | ADB_IOP_TIMEOUT;
                reply[1] = 0;
            }
            /* mirror into the send message too (harmless) */
            memcpy(msg, reply, 3 + 8);
            q950_log("q950 iop adb: cmd %02x len %d -> %s len %d\n",
                     cmd, dlen, olen > 0 ? "data" : "timeout", olen);
            q950_iop_post_recv(iop, ADB_CHAN, reply,
                               3 + (olen > 0 ? MIN(olen, 8) : 0));
        }
        return;
    }

    q950_log("q950 iop adb: unhandled flags 0x%02x cmd 0x%02x\n",
             flags, cmd);
}

static void q950_iop_process(Q950IOP *iop)
{
    int chan, i;
    bool found = false;

    for (chan = 0; chan < NUM_IOP_CHAN; chan++) {
        if (iop->ram[IOP_ADDR_SEND_STATE + chan] == IOP_MSG_NEW) {
            found = true;
        }
    }
    if (!found && q950_iop_adb_check_staged(iop)) {
        /*
         * A follow-up explicit ADB command was staged in place in the
         * receive slot rather than sent via a fresh SEND_MSG (see
         * q950_iop_adb_check_staged()); it has already been serviced
         * and its reply posted.  Nothing else to do this kick.
         */
        found = true;
    }
    if (!found) {
        q950_log("q950 iop %s: kick with no NEW msg; send states "
                 "%02x %02x %02x %02x %02x %02x %02x recv "
                 "%02x %02x %02x %02x %02x %02x %02x\n", iop->name,
                 iop->ram[0x201], iop->ram[0x202], iop->ram[0x203],
                 iop->ram[0x204], iop->ram[0x205], iop->ram[0x206],
                 iop->ram[0x207],
                 iop->ram[0x301], iop->ram[0x302], iop->ram[0x303],
                 iop->ram[0x304], iop->ram[0x305], iop->ram[0x306],
                 iop->ram[0x307]);
    }

    for (chan = 0; chan < NUM_IOP_CHAN; chan++) {
        if (iop->ram[IOP_ADDR_SEND_STATE + chan] == IOP_MSG_NEW) {
            uint8_t *msg = &iop->ram[IOP_ADDR_SEND_MSG + chan * IOP_MSG_LEN];
            char buf[3 * IOP_MSG_LEN + 1];

            for (i = 0; i < IOP_MSG_LEN; i++) {
                sprintf(buf + i * 3, " %02x", msg[i]);
            }
            q950_log("q950 iop %s: send chan %d msg%s pc=0x%08x\n",
                     iop->name, chan, buf, q950_trace_pc());
            /* service what we understand, then complete the message */
            if (iop->is_ism && chan == ADB_CHAN) {
                q950_iop_adb_msg(iop, msg);
            }
            iop->ram[IOP_ADDR_SEND_STATE + chan] = IOP_MSG_COMPLETE;
        }
    }
    /*
     * Always raise INT0 on a kick, even with nothing newly completed:
     * the ROM's completed-request queue is drained by a deferred task
     * that only gets (re)installed from the IOP ISR epilogue
     * (0x408050f4: tstl completed-queue -> jsr [jDTInstall]).  The
     * final kick of a chain (e.g. the receive-acknowledge) otherwise
     * never raises the interrupt that would drain the last completion
     * and the ADB startup starves.  A spurious INT0 scan finds no
     * COMPLETE channels and is harmless.
     */
    iop->ctrl |= IOP_INT0;
    q950_iop_update_irq(iop);
}

static void q950_iop_ctrl_write(Q950IOP *iop, uint8_t val)
{
    uint8_t old = iop->ctrl;
    uint8_t ctrl;

    q950_log("q950 iop %s: ctrl <- 0x%02x (was 0x%02x) pc=0x%08x\n",
             iop->name, val, old, q950_trace_pc());

    /* mode bits are stored; writing INT0/INT1 clears those flags */
    ctrl = (old & (IOP_INT0 | IOP_INT1)) |
           (val & (IOP_BYPASS | IOP_AUTOINC | IOP_RUN));
    ctrl &= ~(val & (IOP_INT0 | IOP_INT1));
    iop->ctrl = ctrl;

    if ((val & IOP_RUN) && !(old & IOP_RUN)) {
        /*
         * Firmware start: the loaded IOP code would announce itself by
         * setting the alive flag and publishing its channel counts
         * (the host message sender validates the channel number
         * against MAX_SEND/RECV_CHAN, 0x40804e00 area).
         */
        iop->ram[IOP_ADDR_ALIVE] = 0xff;
        if (iop->ram[IOP_ADDR_MAX_SEND_CHAN] == 0) {
            iop->ram[IOP_ADDR_MAX_SEND_CHAN] = NUM_IOP_CHAN;
        }
        if (iop->ram[IOP_ADDR_MAX_RECV_CHAN] == 0) {
            iop->ram[IOP_ADDR_MAX_RECV_CHAN] = NUM_IOP_CHAN;
        }
        q950_log("q950 iop %s: started (alive; max chans %d/%d)\n",
                 iop->name, iop->ram[IOP_ADDR_MAX_SEND_CHAN],
                 iop->ram[IOP_ADDR_MAX_RECV_CHAN]);
    }
    if (val & IOP_IRQ) {
        /*
         * Host attention: scan the send channels after a short virtual
         * delay.  The reply window is tight in BOTH directions: a
         * synchronous completion interrupts the ROM inside the
         * sender's IPL-raised dispatch and the completion chain loses
         * the request, while a late one misses the driver's
         * TimeDBRA-calibrated dbra reply waits (gdb/exec-log-slowed
         * runs got further than free-running ones).  100us of virtual
         * time lands in between; run with -icount shift=7 so virtual
         * time tracks guest instructions, not the host wallclock.
         */
        timer_mod(iop->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * 1000);
    }
    q950_iop_update_irq(iop);
}

static void q950_iop_timer_cb(void *opaque)
{
    Q950IOP *iop = opaque;

    q950_iop_process(iop);
}

static uint64_t q950_iop_read(void *opaque, hwaddr addr, unsigned size)
{
    Q950IOP *iop = opaque;
    uint8_t val;

    switch (addr >> 1) {
    case 0:
        val = iop->addr >> 8;
        break;
    case 1:
        val = iop->addr & 0xff;
        break;
    case 2:
    case 3:
        /* no DMA request pending */
        val = iop->ctrl | IOP_DMAINACTIVE;
        break;
    default:
        val = iop->ram[iop->addr & 0x7fff];
        if ((iop->addr & 0x7fff) >= 0x200 && (iop->addr & 0x7fff) < 0x340) {
            q950_log("q950 iop %s: DBG ram[0x%03x] -> 0x%02x pc=0x%08x\n",
                     iop->name, iop->addr & 0x7fff, val, q950_trace_pc());
        }
        if (iop->ctrl & IOP_AUTOINC) {
            iop->addr++;
        }
        break;
    }
    return val;
}

static void q950_iop_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    Q950IOP *iop = opaque;

    switch (addr >> 1) {
    case 0:
        iop->addr = (iop->addr & 0x00ff) | ((val & 0xff) << 8);
        break;
    case 1:
        iop->addr = (iop->addr & 0xff00) | (val & 0xff);
        break;
    case 2:
    case 3:
        q950_iop_ctrl_write(iop, val);
        break;
    default:
        iop->ram[iop->addr & 0x7fff] = val;
        if ((iop->addr & 0x7fff) >= 0x200 && (iop->addr & 0x7fff) < 0x340) {
            q950_log("q950 iop %s: DBG ram[0x%03x] <- 0x%02x pc=0x%08x\n",
                     iop->name, iop->addr & 0x7fff, (unsigned)val,
                     q950_trace_pc());
        }
        /*
         * Host acknowledging a receive-channel message: writing
         * IOP_MSG_COMPLETE to a RECV_STATE byte makes the IOP reset
         * the channel to idle.
         */
        if ((iop->addr & 0x7fff) >= IOP_ADDR_RECV_STATE &&
            (iop->addr & 0x7fff) < IOP_ADDR_RECV_STATE + NUM_IOP_CHAN &&
            val == IOP_MSG_COMPLETE) {
            iop->ram[iop->addr & 0x7fff] = IOP_MSG_IDLE;
        }
        if (iop->ctrl & IOP_AUTOINC) {
            iop->addr++;
        }
        break;
    }
}

static const MemoryRegionOps q950_iop_ops = {
    .read = q950_iop_read,
    .write = q950_iop_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        /* multi-byte accesses hit the byte registers lane by lane */
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * MCU (Quadra 700/900 memory controller) at 0x50F0E000: the ROM's
 * machine identification requires a successful long read here (decoder
 * kind 8) and the memory sizing writes bank registers.
 */

static uint64_t q950_mcu_read(void *opaque, hwaddr addr, unsigned size)
{
    Q950MachineState *m = opaque;
    uint64_t val = 0;
    int i;

    for (i = 0; i < size; i++) {
        val = (val << 8) | m->mcu_regs[(addr + i) & (sizeof(m->mcu_regs) - 1)];
    }
    return val;
}

static void q950_mcu_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    Q950MachineState *m = opaque;
    int i;

    for (i = size - 1; i >= 0; i--) {
        m->mcu_regs[(addr + i) & (sizeof(m->mcu_regs) - 1)] = val & 0xff;
        val >>= 8;
    }
}

static const MemoryRegionOps q950_mcu_ops = {
    .read = q950_mcu_read,
    .write = q950_mcu_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * ESP DRQ: latch for the TurboSCSI register only.  Unlike the
 * q800/q700, VIA2 CA2 is NOT the SCSI DRQ on the towers — it is the
 * ISM IOP interrupt (Level2 vector 0xd70 -> ROM 0x40809d70: clear
 * VIA2 IFR bit0, then the IOP manager ISR for IOP #1).
 */
static void q950_esp_drq(void *opaque, int n, int level)
{
    Q950MachineState *m = opaque;

    m->esp_drq = level;
}

/* unmapped I/O space bus-errors on the real machine; log the probes */

static MemTxResult q950_iotrace_read(void *opaque, hwaddr addr,
                                     uint64_t *data, unsigned size,
                                     MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q950 io: read  +0x%05x (%d) -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, q950_trace_pc());
    }
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult q950_iotrace_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned size,
                                      MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q950 io: write +0x%05x (%d) <- 0x%08" PRIx64
                      " -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, val, q950_trace_pc());
    }
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps q950_iotrace_ops = {
    .read_with_attrs = q950_iotrace_read,
    .write_with_attrs = q950_iotrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* background catch-all outside the I/O slice: log + BERR */

static MemTxResult q950_bgtrace_read(void *opaque, hwaddr addr,
                                     uint64_t *data, unsigned size,
                                     MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q950 bus: read  0x%08x (%d) -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, q950_trace_pc());
    }
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult q950_bgtrace_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned size,
                                      MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q950 bus: write 0x%08x (%d) <- 0x%08" PRIx64
                      " -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, val, q950_trace_pc());
    }
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps q950_bgtrace_ops = {
    .read_with_attrs = q950_bgtrace_read,
    .write_with_attrs = q950_bgtrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* ------------------------------------------------------------------ */
/*
 * Egret MCU engine.  Packet format = the Cuda's (quadra630.c engine);
 * transport polarity per Linux via-cuda.c: the host asserts TIP/TACK
 * HIGH (idle low), the Egret asserts /TREQ LOW.
 */

static void q950_egret_set_treq(MOS6522Q950State *v1s, bool assert)
{
    MOS6522State *s = MOS6522(v1s);

    if (assert) {
        s->b &= ~EGRET_TREQ;            /* active low */
    } else {
        s->b |= EGRET_TREQ;
    }
}

/*
 * Host probes sometimes drive all of port B as outputs, clobbering the
 * stored /TREQ input state.  Restore the idle level whenever no
 * exchange is in progress.
 */
static void q950_egret_idle_treq(MOS6522Q950State *v1s)
{
    Q950MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (!m->egret_session && m->egret_resp_len == 0
        && (s->b & (EGRET_TIP | EGRET_TACK)) == 0) {
        q950_egret_set_treq(v1s, false);
    }
}

static void q950_egret_schedule_int(Q950MachineState *m)
{
    timer_mod(m->egret_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * 1000);
}

static void q950_egret_timer_cb(void *opaque)
{
    Q950MachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), SR_INT_BIT);

    qemu_set_irq(irq, 1);
}

/*
 * OS-mode (interrupt driver) command processing: raw ADB command bytes
 * with no Cuda type byte, as on the Mac IIsi (maciisi.c).
 */
static void q950_egret_os_no_response(Q950MachineState *m)
{
    /*
     * No response: run the interrupt cadence with two junk bytes but
     * with /TREQ asserted (low) from the turnaround on, which makes
     * the driver set its discard flag (0xa624) and zero the byte
     * count (0xa646).
     */
    m->egret_no_resp = true;
    m->egret_resp[m->egret_resp_len++] = 0x00;
    m->egret_resp[m->egret_resp_len++] = 0x00;
}

static void q950_egret_process_os(Q950MachineState *m)
{
    ADBBusState *adb_bus = &m->via1.adb_bus;
    uint8_t *c = m->egret_cmd;
    int n = m->egret_cmd_len;

    m->egret_resp_len = 0;
    m->egret_resp_idx = 0;
    m->egret_no_resp = false;

    q950_log("q950 egret-os: cmd len=%d [%02x %02x %02x %02x]\n",
             n, n > 0 ? c[0] : 0, n > 1 ? c[1] : 0, n > 2 ? c[2] : 0,
             n > 3 ? c[3] : 0);

    if (n == 0) {
        return;
    }

    if (n == 1 && c[0] == 0x00) {
        /*
         * ADB SendReset: reset the bus but answer the two status
         * bytes (Egret 341S0851 reports firmware 1.01), as on maciisi
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
            /* Listen/no-data/absent device: no response */
            q950_egret_os_no_response(m);
        }
    }
}

static void q950_egret_process(Q950MachineState *m)
{
    ADBBusState *adb_bus = &m->via1.adb_bus;
    MOS6522Q950State *v1s = &m->via1;
    uint8_t *c = m->egret_cmd;
    uint8_t *r = m->egret_resp;
    int n = m->egret_cmd_len;
    uint32_t now, val;

    if (m->egret_osmode) {
        q950_egret_process_os(m);
        return;
    }

    m->egret_resp_len = 0;
    m->egret_resp_idx = 0;
    m->egret_no_resp = false;

    q950_log("q950 egret: cmd len=%d [%02x %02x %02x %02x %02x]\n",
             n, n > 0 ? c[0] : 0, n > 1 ? c[1] : 0, n > 2 ? c[2] : 0,
             n > 3 ? c[3] : 0, n > 4 ? c[4] : 0);

    if (n == 0) {
        return;
    }

    /*
     * Polled-mode response format (established against three ROM
     * drivers, see Q950-NOTES): a FOUR byte header [type, status,
     * flags, cmd-echo] followed by data.  status 0 = OK, 2 = error
     * (the trap dispatcher turns it into paramErr, 0x40814a18);
     * flags bit6 = no-data.  The type byte must be preloaded into SR
     * at the turnaround: drivers 1/2 read-and-discard it once before
     * collecting (0x4b33c / 0x15446), the trap dispatcher's first
     * interrupt read collects it directly (0x40814970).
     */
    switch (c[0]) {
    case EGRET_PKT_ADB:
        if (n < 2) {
            /* bare [00]: ADB SendReset sync */
            uint8_t scratch[ADB_MAX_OUT_LEN];
            uint8_t rst = 0x00;

            adb_autopoll_block(adb_bus);
            adb_request(adb_bus, scratch, &rst, 1);
            adb_autopoll_unblock(adb_bus);
            r[0] = EGRET_PKT_ADB;
            r[1] = 0x00;
            r[2] = 0x00;
            r[3] = 0x00;
            m->egret_resp_len = 4;
            break;
        }
        {
            uint8_t obuf[ADB_MAX_OUT_LEN];
            int olen;

            adb_autopoll_block(adb_bus);
            olen = adb_request(adb_bus, obuf, c + 1, n - 1);
            adb_autopoll_unblock(adb_bus);

            r[0] = EGRET_PKT_ADB;
            r[1] = 0x00;
            r[3] = c[1];
            if (olen > 0) {
                r[2] = 0x00;
                memcpy(r + 4, obuf, olen);
                m->egret_resp_len = 4 + olen;
            } else {
                /* timeout/no data */
                r[2] = 0x40;
                m->egret_resp_len = 4;
            }
        }
        break;

    case EGRET_PKT_PSEUDO:
        /* generic ACK header; commands append data / have side effects */
        r[0] = EGRET_PKT_PSEUDO;
        r[1] = 0x00;
        r[2] = 0x00;
        r[3] = n > 1 ? c[1] : 0;
        m->egret_resp_len = 4;
        if (n < 2) {
            break;
        }
        switch (c[1]) {
        case EGRET_CMD_AUTOPOLL:
            if (n >= 3) {
                adb_set_autopoll_enabled(adb_bus, c[2] != 0);
            }
            break;
        case EGRET_CMD_GET_TIME:
            now = v1s->tick_offset +
                  (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                   / NANOSECONDS_PER_SECOND);
            r[4] = now >> 24;
            r[5] = now >> 16;
            r[6] = now >> 8;
            r[7] = now;
            m->egret_resp_len = 8;
            break;
        case EGRET_CMD_SET_TIME:
            if (n >= 6) {
                val = (c[2] << 24) | (c[3] << 16) | (c[4] << 8) | c[5];
                v1s->tick_offset = val -
                    (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                     / NANOSECONDS_PER_SECOND);
            }
            break;
        case EGRET_CMD_READ_PRAM:
            /*
             * Trap-dispatcher single-byte read [01 02 addr_hi addr_lo]:
             * the response is Egret-terminated — /TREQ deasserts after
             * exactly one data byte, and the trap driver computes the
             * data length from the bytes received before /TREQ rose
             * (0x40814a58: received_count - 4).
             */
            if (n >= 4) {
                int addr = (c[2] << 8) | c[3];

                if (addr < 4) {
                    now = v1s->tick_offset +
                          (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                           / NANOSECONDS_PER_SECOND);
                    r[4] = now >> ((3 - addr) * 8);
                } else {
                    r[4] = v1s->PRAM[addr & 0xff];
                }
                m->egret_resp_len = 5;
                q950_log("q950 egret: read 0x%03x -> 0x%02x\n", addr, r[4]);
            }
            break;
        case EGRET_CMD_GET_PRAM:
            /*
             * Block read: [01 07 addr_hi addr_lo]; the Egret then
             * streams bytes for as long as the host keeps clocking
             * them (it ends the read by dropping TIP with TACK high).
             * The ROM reads 1 byte for XPRAM scans (0x4b258), 32 bytes
             * for the PRAM-validity read at addr 0 (0x4081528e), etc.
             * Address space: 0-3 = the RTC clock, PRAM bytes both at
             * 0x00-0xff (flat, XPRAM scans) and 0x100-0x1ff.
             */
            if (n >= 4) {
                int addr = (c[2] << 8) | c[3];
                int i;

                now = v1s->tick_offset +
                      (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                       / NANOSECONDS_PER_SECOND);
                for (i = 0; i < 260; i++) {
                    int a = addr + i;

                    if (a < 4) {
                        r[4 + i] = now >> ((3 - a) * 8);
                    } else {
                        r[4 + i] = v1s->PRAM[a & 0xff];
                    }
                }
                m->egret_resp_len = 4 + 260;
                q950_log("q950 egret: get pram 0x%03x -> 0x%02x ...\n",
                         addr, r[4]);
            }
            break;
        case EGRET_CMD_WRITE_PRAM:
            /*
             * Block write: [01 08 addr_hi addr_lo] + data bytes (the
             * PRAM-init at 0x408152c4 writes (0x10,16), (8,4), (12,4),
             * (0x20,224), (0x76,19) through helper 0x15392; addresses
             * 0x100-0x1ff = PRAM proper via the trap dispatcher).
             */
            if (n >= 4) {
                int addr = (c[2] << 8) | c[3];
                int i;
                bool clock_set = false;

                now = v1s->tick_offset +
                      (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                       / NANOSECONDS_PER_SECOND);
                for (i = 4; i < n; i++) {
                    int a = addr + i - 4;

                    if (a < 4) {
                        now &= ~(0xffu << ((3 - a) * 8));
                        now |= (uint32_t)c[i] << ((3 - a) * 8);
                        clock_set = true;
                    } else {
                        v1s->PRAM[a & 0xff] = c[i];
                    }
                }
                if (clock_set) {
                    v1s->tick_offset = now -
                        (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                         / NANOSECONDS_PER_SECOND);
                }
                q950_log("q950 egret: write 0x%03x len %d [%02x ...]\n",
                         addr, n - 4, n > 4 ? c[4] : 0);
            }
            break;
        case EGRET_CMD_SET_PRAM:
            if (n >= 5) {
                v1s->PRAM[((c[2] << 8) | c[3]) & 0xff] = c[4];
                q950_log("q950 egret: set pram 0x%02x <- 0x%02x\n",
                         c[3], c[4]);
            }
            break;
        default:
            /* unknown pseudo commands are generically acknowledged */
            break;
        }
        break;

    default:
        /* unknown packet type: status 2 = error */
        r[0] = EGRET_PKT_ERROR;
        r[1] = 0x02;
        r[2] = 0x00;
        r[3] = c[0];
        m->egret_resp_len = 4;
        break;
    }
}

/*
 * Stage a freshly built polled-mode response for collection: /TREQ
 * low, the type byte preloaded into SR, and the shift interrupt that
 * the turnaround wait loops on (0x4b32e, 0x1543e).
 */
static void q950_egret_polled_stage_resp(MOS6522Q950State *v1s)
{
    Q950MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (m->egret_resp_len > 0) {
        s->sr = m->egret_resp[0];
        m->egret_resp_idx = 1;
        q950_egret_set_treq(v1s, true);
        q950_egret_schedule_int(m);
    } else {
        q950_egret_set_treq(v1s, false);
    }
}

/*
 * Egret-initiated transfer: autopolled ADB data asserts /TREQ while
 * the bus is idle; the host then opens a session and clocks the packet
 * out like a command response.
 */
static void q950_egret_adb_poll(void *opaque)
{
    Q950MachineState *m = opaque;
    MOS6522Q950State *v1s = &m->via1;
    MOS6522State *s = MOS6522(v1s);
    uint8_t obuf[ADB_MAX_OUT_LEN + 3];
    int olen;

    /* only when no exchange is in progress and the shifter is inbound */
    if (m->egret_session || m->egret_resp_len > 0 || (s->acr & SR_OUT)) {
        return;
    }

    olen = adb_poll(&v1s->adb_bus, obuf + 2, v1s->adb_bus.autopoll_mask);
    if (olen <= 0) {
        return;
    }
    if (m->iop_autopoll) {
        /*
         * ADB goes through the ISM IOP on this machine: deliver
         * autopolled data as a receive-channel message (flags
         * AUTOPOLL, cmd = the Talk R0 command byte adb_poll tagged).
         */
        Q950IOP *iop = &m->swim_iop;
        uint8_t reply[3 + 8];

        if (iop->ram[IOP_ADDR_RECV_STATE + ADB_CHAN] != IOP_MSG_IDLE) {
            return;             /* previous message not consumed yet */
        }
        memset(reply, 0, sizeof(reply));
        reply[0] = ADB_IOP_EXPLICIT | ADB_IOP_AUTOPOLL;
        reply[1] = MIN(olen - 1, 8);
        reply[2] = obuf[2];
        memcpy(reply + 3, obuf + 3, MIN(olen - 1, 8));
        q950_iop_post_recv(iop, ADB_CHAN, reply, 3 + MIN(olen - 1, 8));
        return;
    }
    if (m->egret_osmode) {
        /*
         * OS transport (maciisi semantics): raw data tagged with the
         * Talk R0 command byte; /TREQ asserted signals the packet, the
         * host opens a receive session and the first byte must be in
         * SR already.
         */
        memcpy(m->egret_resp, obuf + 2, olen);
        m->egret_resp_len = olen;
        m->egret_resp_idx = 1;
        m->egret_no_resp = false;
        m->egret_session = true;
        s->sr = m->egret_resp[0];
        q950_egret_set_treq(v1s, true);
        q950_egret_schedule_int(m);
        q950_log("q950 egret-os: unsol len=%d [%02x %02x %02x]\n",
                 m->egret_resp_len, obuf[2], obuf[3], obuf[4]);
        return;
    }
    /* adb_poll tags the data with the Talk R0 command byte at obuf[2] */
    obuf[0] = EGRET_PKT_ADB;
    obuf[1] = 0x40;                     /* autopolled data flag */
    memcpy(m->egret_resp, obuf, olen + 2);
    m->egret_resp_len = olen + 2;
    m->egret_resp_idx = 1;
    s->sr = m->egret_resp[0];
    q950_egret_set_treq(v1s, true);
    q950_egret_schedule_int(m);
    q950_log("q950 egret: unsol len=%d [%02x %02x %02x %02x]\n",
             m->egret_resp_len, obuf[0], obuf[1], obuf[2],
             m->egret_resp_len > 3 ? obuf[3] : 0);
}

/*
 * OS-mode transport: the maciisi.c Egret engine, active-low TIP/TACK.
 * "sys" = /TIP asserted (low); mid-exchange toggles keep exactly one
 * of TIP/TACK asserted; both released = transient release or close.
 */

static void q950_egret_os_ack_toggle(MOS6522Q950State *v1s)
{
    Q950MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (m->egret_resp_idx < m->egret_resp_len) {
        s->sr = m->egret_resp[m->egret_resp_idx++];
        q950_egret_set_treq(v1s, m->egret_no_resp);
        q950_egret_schedule_int(m);
        q950_log("q950 egret-os: feed 0x%02x (#%d/%d) pc=%08x b=%02x\n",
                 s->sr, m->egret_resp_idx, m->egret_resp_len,
                 q950_trace_pc(), s->b);
    } else {
        /* final ack: /TREQ low = end-of-response, the exchange is over */
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
        m->egret_session = false;
        q950_egret_set_treq(v1s, true);
        q950_egret_schedule_int(m);
        q950_log("q950 egret-os: response complete pc=%08x b=%02x\n",
                 q950_trace_pc(), s->b);
    }
}

static void q950_egret_os_session_update(MOS6522Q950State *v1s)
{
    Q950MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);
    bool sys = !(s->b & EGRET_TIP);
    uint8_t hs_change = (s->b ^ v1s->last_b) & (EGRET_TIP | EGRET_TACK);

    /*
     * Session opens on a /TIP assert edge, or a /TACK assert edge with
     * /TIP already held and the shifter outbound (chained send:
     * ORB &= 0xCF straight after the previous exchange, 0xa656).
     */
    if (sys && !m->egret_session &&
        ((hs_change & EGRET_TIP) ||
         ((hs_change & EGRET_TACK) && !(s->b & EGRET_TACK) &&
          (s->acr & SR_OUT)))) {
        m->egret_session = true;
        m->egret_cmd_len = 0;
        if (s->acr & SR_OUT) {
            /*
             * Send session: any response still held from the previous
             * exchange is stale; the driver preloads the first byte
             * into SR before asserting the session, so collect it now.
             */
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
            q950_egret_set_treq(v1s, false);
            q950_egret_sr_written(v1s);
        } else if (m->egret_resp_len > 0 && m->egret_resp_idx == 0) {
            /*
             * Receive session with a held response (the receive
             * reopen: eor 0x30 released both lines, eor 0x20
             * re-asserts /TIP with the shifter inbound, 0xa5fc/0xa622)
             * — the first byte must already be in SR when the opening
             * shift interrupt is dispatched (0xa624 reads it via
             * 0xa68e at the NEXT interrupt).
             */
            s->sr = m->egret_resp[0];
            m->egret_resp_idx = 1;
            q950_egret_set_treq(v1s, m->egret_no_resp);
            q950_egret_schedule_int(m);
        } else {
            /* receive session with nothing to deliver: no-response */
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
            q950_egret_os_no_response(m);
            s->sr = m->egret_resp[0];
            m->egret_resp_idx = 1;
            q950_egret_set_treq(v1s, true);
            q950_egret_schedule_int(m);
        }
        return;
    }

    /*
     * TIP/TACK toggles during the receive phase acknowledge the byte
     * in SR and clock the next one (eor 0x30 keeps exactly one of
     * them asserted); BOTH released is the transient release/close.
     */
    if (m->egret_session && !(s->acr & SR_OUT) && hs_change
        && m->egret_resp_len > 0
        && (s->b & (EGRET_TIP | EGRET_TACK)) !=
           (EGRET_TIP | EGRET_TACK)) {
        q950_egret_os_ack_toggle(v1s);
        return;
    }

    /* /TIP released: the session is closed (edge-triggered) */
    if (!sys && (hs_change & EGRET_TIP)) {
        if (m->egret_session && m->egret_cmd_len > 0
            && m->egret_resp_len == 0 && m->egret_resp_idx == 0) {
            /* Listen commands sent without a receive turnaround */
            q950_egret_process(m);
        }
        m->egret_session = false;
        m->egret_cmd_len = 0;
        /*
         * A staged but undelivered response SURVIVES the close (the
         * poll cadence closes and immediately reopens to collect it);
         * one being delivered was abandoned mid-read — drop it.
         */
        if (m->egret_resp_idx > 0) {
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
        }
        if (m->egret_resp_len == 0) {
            q950_egret_set_treq(v1s, false);
        }
        /* the "session closed" acknowledgement interrupt */
        q950_egret_schedule_int(m);
    }
}

static void q950_egret_polled_session_update(MOS6522Q950State *v1s)
{
    Q950MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);
    /* Egret polarity: TIP/TACK asserted HIGH */
    bool sys = (s->b & (EGRET_TIP | EGRET_TACK)) != 0;
    uint8_t hs_change = (s->b ^ v1s->last_b) & (EGRET_TIP | EGRET_TACK);

    if (sys && !m->egret_session) {
        m->egret_session = true;
        m->egret_sync = !(s->b & EGRET_TIP) && (s->b & EGRET_TACK);
        /*
         * A session opened with the shifter INBOUND while a response
         * is staged is the collection session for that response
         * (driver 2's post-turnaround TIP assert at 0x1544e, driver
         * 1's unsolicited read at 0x4b42e): keep the staged bytes.
         */
        if (!(s->acr & SR_OUT) && m->egret_resp_len > 0) {
            return;
        }
        m->egret_cmd_len = 0;
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
        /*
         * NOTE: unlike the lc475/q630 Cuda drivers, this ROM's Egret
         * driver does NOT preload the first byte into SR before
         * asserting the session (observed: session open at 0x4084b29e,
         * first SR write at 0x4084b2a2) — collecting the stale SR here
         * would prepend a junk byte to every command.
         */
        if (m->egret_sync) {
            /* polled sync: /TREQ low is the session/byte acknowledge */
            q950_egret_set_treq(v1s, true);
        }
        return;
    }

    /*
     * Receive phase: response delivery is driven by SR READS
     * (q950_egret_sr_read) — the ROM's 0x4b3ea helper does TACK low ->
     * wait SR int -> TACK high -> read SR, but on the first call TACK
     * is already low (no edge), so the edges cannot be the byte
     * clock: each SR read hands the next byte over and re-raises the
     * interrupt.  Handshake toggles only matter to keep the
     * transiently released session alive (the turnaround drops TIP
     * with TACK already low, 0x4b32a).
     */
    if (m->egret_session && !(s->acr & SR_OUT) && m->egret_resp_len > 0) {
        /*
         * The host ends a (block) read by dropping TIP with TACK still
         * high (0x4b37c / 0x15420) — it may stop before consuming
         * everything staged (GET_PRAM streams up to 260 bytes; XPRAM
         * scans read just one).  The turnaround's transient TIP drop
         * (0x4b32a) has TACK low and nothing consumed yet, so gate on
         * TACK high + at least one byte delivered.
         */
        if ((hs_change & EGRET_TIP) && !(s->b & EGRET_TIP)
            && (s->b & EGRET_TACK) && m->egret_resp_idx > 0) {
            m->egret_resp_len = 0;
            m->egret_resp_idx = 0;
            q950_egret_set_treq(v1s, false);
            q950_egret_schedule_int(m);
            q950_log("q950 egret: read ended by host pc=%08x b=%02x\n",
                     q950_trace_pc(), s->b);
            return;
        }
        if (!sys) {
            q950_egret_schedule_int(m);
        }
        return;
    }

    if (!sys && m->egret_session) {
        /*
         * Host released the handshake lines: end of the send phase.
         * The Egret processes the command now; a response it builds
         * STAYS staged with /TREQ asserted — the host collects it
         * either through the ACR turnaround that directly follows
         * (driver 2 at 0x15438 flips ACR with the lines released) or
         * through the "Egret wants to send" path after a close
         * (driver 1 checks /TREQ at 0x4b41e after every exchange).
         */
        if (m->egret_cmd_len > 0 && m->egret_resp_len == 0
            && m->egret_resp_idx == 0) {
            q950_egret_process(m);
            m->egret_cmd_len = 0;
            q950_egret_polled_stage_resp(v1s);
        } else if (m->egret_resp_len == 0
                   || m->egret_resp_idx >= m->egret_resp_len) {
            /* /TREQ returns to idle (deasserted) */
            q950_egret_set_treq(v1s, false);
        }
        q950_egret_schedule_int(m);
        m->egret_session = false;
        m->egret_sync = false;
        m->egret_cmd_len = 0;
    }
}

static void q950_egret_session_update(MOS6522Q950State *v1s)
{
    Q950MachineState *m = v1s->machine;

    if (m->egret_osmode) {
        q950_egret_os_session_update(v1s);
    } else {
        q950_egret_polled_session_update(v1s);
    }
}

/*
 * SR read during the receive phase (POLLED mode only): the byte just
 * consumed frees the next one — load it and re-raise the shift
 * interrupt.  After the last byte, deassert /TREQ: the driver
 * busy-waits for /TREQ high as the end-of-response marker (0x4b380 /
 * the unsolicited loop 0x4b43a).  In OS mode delivery is clocked by
 * the handshake toggles instead (q950_egret_os_ack_toggle).
 */
static void q950_egret_sr_read(MOS6522Q950State *v1s)
{
    Q950MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    /*
     * Active whenever a response is staged and the shifter is inbound
     * — including with the session lines released (driver 2's
     * turnaround discard read at 0x15446 happens before it re-asserts
     * TIP).
     */
    if (m->egret_osmode || (s->acr & SR_OUT) || m->egret_resp_len == 0) {
        return;
    }

    if (m->egret_resp_idx < m->egret_resp_len) {
        s->sr = m->egret_resp[m->egret_resp_idx++];
        q950_egret_set_treq(v1s, true);
        q950_egret_schedule_int(m);
        q950_log("q950 egret: feed 0x%02x (#%d/%d) pc=%08x b=%02x\n",
                 s->sr, m->egret_resp_idx, m->egret_resp_len,
                 q950_trace_pc(), s->b);
    } else {
        /* last byte was just read: end of response */
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
        q950_egret_set_treq(v1s, false);
        q950_egret_schedule_int(m);
        q950_log("q950 egret: response complete pc=%08x b=%02x\n",
                 q950_trace_pc(), s->b);
    }
}

static void q950_egret_sr_written(MOS6522Q950State *v1s)
{
    Q950MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (!(s->acr & SR_OUT)) {
        return;
    }

    if (!m->egret_session) {
        /*
         * A first command byte written with no session open selects
         * the transport: the polled drivers assert TIP (high) BEFORE
         * writing SR, so if a session-open edge was missed TIP is
         * high here; the OS driver preloads SR while the lines still
         * sit at the polled-idle low level (0xa656: ACR |= 0x1c,
         * SR = cmd, THEN ORB &= 0xCF — no edge if already low).
         */
        bool osmode = !(s->b & EGRET_TIP);

        if (osmode != m->egret_osmode) {
            q950_log("q950 egret: transport -> %s (b=%02x pc=%08x)\n",
                     osmode ? "os" : "polled", s->b, q950_trace_pc());
        }
        m->egret_osmode = osmode;
        m->egret_session = true;
        m->egret_sync = false;
        m->egret_cmd_len = 0;
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
    }

    if (m->egret_cmd_len < (int)sizeof(m->egret_cmd)) {
        m->egret_cmd[m->egret_cmd_len++] = s->sr;
    }
    /*
     * /TREQ stays HIGH during host sends (low = collision signal),
     * except in a polled sync exchange where /TREQ low is the ack.
     */
    q950_egret_set_treq(v1s, m->egret_sync);
    q950_log("q950 egret: <- 0x%02x (#%d) pc=%08x b=%02x\n", s->sr,
             m->egret_cmd_len, q950_trace_pc(), s->b);
    q950_egret_schedule_int(m);
}

static void q950_egret_acr_changed(MOS6522Q950State *v1s)
{
    Q950MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (m->egret_osmode) {
        /*
         * OS-mode turnaround (maciisi semantics): the host flips the
         * shifter to receive while holding the session; /TREQ stays
         * HIGH for a real response and goes LOW for no-response.
         */
        if (m->egret_session && !(s->acr & SR_OUT)
            && m->egret_resp_len == 0 && m->egret_cmd_len > 0) {
            q950_egret_process(m);
            m->egret_cmd_len = 0;
            q950_egret_set_treq(v1s, m->egret_no_resp &&
                                     m->egret_resp_len > 0);
        } else if (m->egret_session && !(s->acr & SR_OUT)
                   && m->egret_resp_len > 0 && m->egret_cmd_len > 0) {
            /* collision: the host receives our staged packet first */
            m->egret_cmd_len = 0;
        }
        return;
    }

    /*
     * Host turns the shifter to OUTPUT: it is starting a new command.
     * Drop any stale staged response.
     */
    if ((s->acr & SR_OUT) && m->egret_resp_len > 0) {
        m->egret_resp_len = 0;
        m->egret_resp_idx = 0;
        q950_egret_set_treq(v1s, false);
    }

    /*
     * The host turns the shifter around to receive while still holding
     * the session: treat the bytes collected so far as the command and
     * build the response; the host's following handshake toggle clocks
     * the first byte out.  /TREQ LOW = response data pending.
     */
    if (m->egret_session && !(s->acr & SR_OUT) && m->egret_resp_len == 0
        && m->egret_cmd_len > 0) {
        q950_egret_process(m);
        m->egret_cmd_len = 0;
        q950_egret_polled_stage_resp(v1s);
    } else if (m->egret_session && !(s->acr & SR_OUT)
               && m->egret_resp_len > 0 && m->egret_cmd_len > 0) {
        /*
         * Turnaround with an Egret-initiated packet staged: the host
         * collided with us mid-send and is receiving our packet first;
         * it re-sends its command afterwards, so drop the partial one.
         */
        m->egret_cmd_len = 0;
    } else if (!m->egret_session && !(s->acr & SR_OUT)
               && m->egret_resp_len > 0) {
        /*
         * Turnaround with the session lines released and a response
         * staged from the send-phase processing (driver 2, 0x15438):
         * make sure the IFR wait that follows is satisfied.
         */
        q950_egret_schedule_int(m);
    }
}

/*
 * VIA1 window: VIA-spaced registers with the board straps merged into
 * port A input reads and the Egret/RTC engine hooks.
 */

static uint64_t q950_via1_read(void *opaque, hwaddr addr, unsigned size)
{
    Q950MachineState *m = opaque;
    MOS6522State *ms = MOS6522(&m->via1);
    hwaddr reg = (addr >> 9) & 0xf;
    uint64_t val;

    val = mos6522_read(ms, reg, size);

    /* input pins on port A read the board straps, not the last output */
    if (reg == VIA_REG_A || reg == VIA_REG_ANH) {
        val = (val & ms->dira) | (m->pins_a & ~ms->dira);
    }

    /* SR reads clock the Egret response delivery */
    if (reg == VIA_REG_SR && m->via1.machine) {
        q950_egret_sr_read(&m->via1);
    }

    return val;
}

static void q950_via1_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    Q950MachineState *m = opaque;
    MOS6522Q950State *v1s = &m->via1;
    MOS6522State *ms = MOS6522(v1s);
    hwaddr reg = (addr >> 9) & 0xf;

    /*
     * SETUPTIMEK calibration hack, as on q800 (mac_via.c): under TCG
     * the ROM's dbra-loop timer calibration produces garbage (TimeDBRA
     * at 0xd00 ends up zero, later causing a divide-by-zero SysError in
     * the Time Manager).  Detect the T2=0x30c calibration run and stuff
     * known-good values at its end.
     */
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
            /* end of SETUPTIMEK: force sane calibration results */
            stw_be_phys(&address_space_memory, 0xd00, 0x2a00 * 3);
            stw_be_phys(&address_space_memory, 0xd02, 0x079d * 3);
            m->timer_hack_state = 3;
        }
        break;
    default:
        break;
    }

    if (reg == VIA_REG_B || reg == VIA_REG_SR || reg == VIA_REG_ACR ||
        reg == VIA_REG_DIRB) {
        static int count;

        if (count < 4000) {
            count++;
            qemu_log_mask(LOG_UNIMP,
                          "q950 via1: reg%d <- 0x%02" PRIx64 " pc=0x%08x\n",
                          (int)reg, val, q950_trace_pc());
        }
    }
    mos6522_write(ms, reg, val, size);

    if (reg == VIA_REG_SR && v1s->machine) {
        q950_egret_sr_written(v1s);
    }
    if (reg == VIA_REG_ACR && v1s->machine) {
        q950_egret_acr_changed(v1s);
    }
    if (reg == VIA_REG_B) {
        via1_rtc_update(v1s);
        v1s->last_b = ms->b;
    }
}

static const MemoryRegionOps q950_via1_ops = {
    .read = q950_via1_read,
    .write = q950_via1_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* 60.15Hz tick on VIA1 CA1 and one-second tick on CA2 */

static void q950_sixty_hz(void *opaque)
{
    Q950MachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA1_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->sixty_hz_timer,
              (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
               VIA_60HZ_TIMER_PERIOD_NS) /
              VIA_60HZ_TIMER_PERIOD_NS * VIA_60HZ_TIMER_PERIOD_NS);
}

static void q950_one_second(void *opaque)
{
    Q950MachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA2_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->one_second_timer,
              (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000) / 1000 * 1000);
}

/* ------------------------------------------------------------------ */

static void q950_machine_init(MachineState *machine)
{
    Q950MachineState *m = Q950_MACHINE(machine);
    Q950MachineClass *qmc = Q950_MACHINE_GET_CLASS(machine);
    int linux_boot;
    int32_t kernel_size;
    uint64_t elf_entry;
    char *filename;
    int bios_size;
    ram_addr_t initrd_base;
    int32_t initrd_size;
    uint8_t *prom;
    int i, checksum;
    const MacFbMode *macfb_mode;
    ram_addr_t ram_size = machine->ram_size;
    const char *kernel_filename = machine->kernel_filename;
    const char *initrd_filename = machine->initrd_filename;
    const char *kernel_cmdline = machine->kernel_cmdline;
    const char *bios_name = machine->firmware ?: MACROM_FILENAME;
    hwaddr parameters_base;
    CPUState *cs;
    DeviceState *dev;
    SysBusESPState *sysbus_esp;
    ESPState *esp;
    SysBusDevice *sysbus;
    BusState *adb_bus;
    NubusBus *nubus;
    NICInfo *nd;
    MACAddr mac;
    uint8_t rng_seed[32];

    linux_boot = (kernel_filename != NULL);
    m->pins_a = qmc->pins_a;

    if (ram_size > 1 * GiB) {
        error_report("Too much memory for this machine: %" PRId64 " MiB, "
                     "maximum 1024 MiB", ram_size / MiB);
        exit(1);
    }

    /* init CPUs */
    object_initialize_child(OBJECT(machine), "cpu", &m->cpu, machine->cpu_type);
    qdev_realize(DEVICE(&m->cpu), NULL, &error_fatal);
    qemu_register_reset(main_cpu_reset, &m->cpu);

    /* RAM */
    memory_region_init_io(&m->ramio, OBJECT(machine), &ramio_ops, &m->ramio,
                          "ram", RAM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x0, &m->ramio);

    memory_region_add_subregion(&m->ramio, 0, machine->ram);

    /* background catch-all: everything unclaimed bus-errors, logged */
    memory_region_init_io(&m->bgtrace, OBJECT(machine), &q950_bgtrace_ops,
                          m, "q950.bus-trace", 0xffffffffull + 1);
    memory_region_add_subregion_overlap(get_system_memory(), 0,
                                        &m->bgtrace, -3);

    /*
     * Create container for all IO devices
     */
    memory_region_init(&m->macio, OBJECT(machine), "mac-io", IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE, &m->macio);

    /*
     * Memory from IO_BASE to IO_BASE + IO_SLICE is repeated
     * from IO_BASE + IO_SLICE to IO_BASE + IO_SIZE
     */
    memory_region_init_io(&m->macio_alias, OBJECT(machine), &macio_alias_ops,
                          &m->macio, "mac-io.alias", IO_SIZE - IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE + IO_SLICE,
                                &m->macio_alias);

    /* catch-all trace region behind the devices in the I/O slice */
    memory_region_init_io(&m->iotrace, OBJECT(machine), &q950_iotrace_ops,
                          m, "mac-io.trace", IO_SLICE);
    memory_region_add_subregion_overlap(&m->macio, 0, &m->iotrace, -1);

    /* MCU memory controller regbank at 0x50F0E000 */
    memory_region_init_io(&m->mcu_mem, OBJECT(machine), &q950_mcu_ops, m,
                          "mcu", sizeof(m->mcu_regs));
    memory_region_add_subregion(&m->macio, 0xe000, &m->mcu_mem);

    memory_region_init_io(&m->machine_id, NULL, &machine_id_ops, NULL,
                          "Machine ID", 4);
    memory_region_add_subregion(get_system_memory(), 0x5ffffffc,
                                &m->machine_id);

    /* IRQ Glue */
    object_initialize_child(OBJECT(machine), "glue", &m->glue, TYPE_GLUE);
    object_property_set_link(OBJECT(&m->glue), "cpu", OBJECT(&m->cpu),
                             &error_abort);
    /*
     * Pin the classic interrupt mapping (VIA1=1, VIA2=2, SCC=4): our
     * VIA1 is the Egret's, which has no auxmode output to drive the
     * GLUE (as on lc475).
     */
    qdev_prop_set_uint8(DEVICE(&m->glue), "auxmode-default", 1);
    sysbus_realize(SYS_BUS_DEVICE(&m->glue), &error_fatal);

    /* VIA 1 (fronting the Egret) */
    object_initialize_child(OBJECT(machine), "via1", &m->via1,
                            TYPE_MOS6522_Q950);
    qdev_prop_set_uint64(DEVICE(&m->via1), "frequency", VIA_TIMER_FREQ);
    m->via1.pins_a = m->pins_a;
    sysbus = SYS_BUS_DEVICE(&m->via1);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_init_io(&m->via1mem, OBJECT(machine), &q950_via1_ops,
                          m, "via1", VIA_SIZE);
    memory_region_add_subregion(&m->macio, VIA_BASE - IO_BASE, &m->via1mem);
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), GLUE_IRQ_IN_VIA1));
    {
        struct tm tm;

        qemu_get_timedate(&tm, 0);
        m->via1.tick_offset = (uint32_t)mktimegm(&tm) + RTC_OFFSET;
    }
    /*
     * PRAM is left invalid (all zeroes, no signature): the ROM then
     * rebuilds it with proper defaults, which the SCSI boot scan needs
     * (see LC475 notes finding 13).
     */
    m->via1.machine = m;
    m->egret_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q950_egret_timer_cb, m);

    /* ADB devices behind the Egret */
    adb_bus = qdev_get_child_bus(DEVICE(&m->via1), "adb.0");
    dev = qdev_new(TYPE_ADB_KEYBOARD);
    qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    dev = qdev_new(TYPE_ADB_MOUSE);
    qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    adb_register_autopoll_callback(&m->via1.adb_bus, q950_egret_adb_poll, m);

    m->sixty_hz_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q950_sixty_hz, m);
    timer_mod(m->sixty_hz_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VIA_60HZ_TIMER_PERIOD_NS);
    m->one_second_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, q950_one_second,
                                       m);
    timer_mod(m->one_second_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);

    /* VIA 2 */
    object_initialize_child(OBJECT(machine), "via2", &m->via2,
                            TYPE_MOS6522_Q800_VIA2);
    sysbus = SYS_BUS_DEVICE(&m->via2);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, VIA_BASE - IO_BASE + VIA_SIZE,
                                sysbus_mmio_get_region(sysbus, 1));
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), GLUE_IRQ_IN_VIA2));

    /* MACSONIC */

    object_initialize_child(OBJECT(machine), "dp8393x", &m->dp8393x,
                            TYPE_DP8393X);
    dev = DEVICE(&m->dp8393x);
    nd = qemu_find_nic_info(TYPE_DP8393X, true, "dp83932");
    if (nd) {
        qdev_set_nic_properties(dev, nd);
        memcpy(mac.a, nd->macaddr.a, sizeof(mac.a));
    } else {
        qemu_macaddr_default_if_unset(&mac);
    }
    mac.a[0] = 0x08;
    mac.a[1] = 0x00;
    mac.a[2] = 0x07;
    qdev_prop_set_macaddr(dev, "mac", mac.a);

    qdev_prop_set_uint8(dev, "it_shift", 2);
    qdev_prop_set_bit(dev, "big_endian", true);
    object_property_set_link(OBJECT(dev), "dma_mr",
                             OBJECT(get_system_memory()), &error_abort);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, SONIC_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), GLUE_IRQ_IN_SONIC));

    memory_region_init_rom(&m->dp8393x_prom, NULL, "dp8393x-q950.prom",
                           SONIC_PROM_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), SONIC_PROM_BASE,
                                &m->dp8393x_prom);

    /* Add MAC address with valid checksum to PROM */
    prom = memory_region_get_ram_ptr(&m->dp8393x_prom);
    checksum = 0;
    for (i = 0; i < 6; i++) {
        prom[i] = revbit8(mac.a[i]);
        checksum ^= prom[i];
    }
    prom[7] = 0xff - checksum;

    /* SCC (in the IOP bypass window at +0x20) */

    object_initialize_child(OBJECT(machine), "escc", &m->escc,
                            TYPE_ESCC);
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

    /*
     * Logically OR both ESCC IRQs and the SCC IOP interrupt together:
     * the SCC IOP interrupts on the GLUE's level-4 ("SCC") input (the
     * ROM's level-4 autovector handler 0x40809b10 dispatches straight
     * into the IOP manager's ISR for IOP #0, 0x40809d60 -> 0x40805036)
     */
    object_initialize_child(OBJECT(machine), "escc_orgate", &m->escc_orgate,
                            TYPE_OR_IRQ);
    object_property_set_int(OBJECT(&m->escc_orgate), "num-lines", 3,
                            &error_fatal);
    dev = DEVICE(&m->escc_orgate);
    qdev_realize(dev, NULL, &error_fatal);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(dev, 0));
    sysbus_connect_irq(sysbus, 1, qdev_get_gpio_in(dev, 1));
    qdev_connect_gpio_out(dev, 0,
                          qdev_get_gpio_in(DEVICE(&m->glue),
                                           GLUE_IRQ_IN_ESCC));
    memory_region_add_subregion(&m->macio, SCC_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));

    /* SCC IOP host registers below the SCC bypass window */
    m->scc_iop.machine = m;
    m->scc_iop.name = "scc";
    m->scc_iop.irq = qdev_get_gpio_in(dev, 2);
    m->scc_iop.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q950_iop_timer_cb,
                                    &m->scc_iop);
    memory_region_init_io(&m->scc_iop.mem, OBJECT(machine), &q950_iop_ops,
                          &m->scc_iop, "scc-iop", 0x20);
    memory_region_add_subregion(&m->macio, SCC_IOP_BASE - IO_BASE,
                                &m->scc_iop.mem);

    /* SCSI */

    object_initialize_child(OBJECT(machine), "esp", &m->esp,
                            TYPE_SYSBUS_ESP);
    sysbus_esp = SYSBUS_ESP(&m->esp);
    esp = &sysbus_esp->esp;
    esp->dma_memory_read = NULL;
    esp->dma_memory_write = NULL;
    esp->dma_opaque = NULL;
    sysbus_esp->it_shift = 4;
    esp->dma_enabled = 1;

    sysbus = SYS_BUS_DEVICE(&m->esp);
    sysbus_realize(sysbus, &error_fatal);
    /* SCSI IRQ is negative edge triggered */
    sysbus_connect_irq(sysbus, 0,
                       qemu_irq_invert(
                           qdev_get_gpio_in(DEVICE(&m->via2),
                                                   VIA2_IRQ_SCSI_BIT)));
    /*
     * SCSI DRQ: latched into the DAFB TurboSCSI handshake register
     * (MAC_SCSI_QUADRA2); VIA2 CA2 belongs to the ISM IOP here
     */
    sysbus_connect_irq(sysbus, 1, qemu_allocate_irq(q950_esp_drq, m, 0));
    memory_region_add_subregion(&m->macio, ESP_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_add_subregion(&m->macio, ESP_PDMA - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 1));

    scsi_bus_legacy_handle_cmdline(&esp->bus);

    /* Apple Sound Chip */

    object_initialize_child(OBJECT(machine), "asc", &m->asc, TYPE_ASC);
    qdev_prop_set_uint8(DEVICE(&m->asc), "asctype", m->easc ? ASC_TYPE_EASC
                                                            : ASC_TYPE_ASC);
    if (machine->audiodev) {
        qdev_prop_set_string(DEVICE(&m->asc), "audiodev", machine->audiodev);
    }
    sysbus = SYS_BUS_DEVICE(&m->asc);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, ASC_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(DEVICE(&m->glue),
                                                   GLUE_IRQ_IN_ASC));

    /* Wire ASC IRQ via GLUE for use in classic mode */
    qdev_connect_gpio_out(DEVICE(&m->glue), GLUE_IRQ_ASC,
                          qdev_get_gpio_in(DEVICE(&m->via2),
                                           VIA2_IRQ_ASC_BIT));

    /* SWIM floppy controller (behind the SWIM IOP) */

    object_initialize_child(OBJECT(machine), "swim", &m->swim,
                            TYPE_SWIM);
    sysbus = SYS_BUS_DEVICE(&m->swim);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, SWIM_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));

    /*
     * SWIM/ISM IOP host registers: the first 0x20 bytes of the SWIM
     * window (only SWIM register 0 collides, the others are 0x200
     * apart)
     */
    m->swim_iop.machine = m;
    m->swim_iop.name = "swim";
    m->swim_iop.is_ism = true;
    /*
     * ISM IOP interrupt = VIA2 CA2 (IFR bit 0), active low (the VIA
     * PCR is programmed for negative edges as usual on Macs)
     */
    m->swim_iop.irq = qemu_irq_invert(
        qdev_get_gpio_in(DEVICE(&m->via2), VIA2_IRQ_SCSI_DATA_BIT));
    m->swim_iop.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q950_iop_timer_cb,
                                     &m->swim_iop);
    memory_region_init_io(&m->swim_iop.mem, OBJECT(machine), &q950_iop_ops,
                          &m->swim_iop, "swim-iop", 0x20);
    memory_region_add_subregion_overlap(&m->macio, SWIM_IOP_BASE - IO_BASE,
                                        &m->swim_iop.mem, 1);

    /* NuBus */

    object_initialize_child(OBJECT(machine), "mac-nubus-bridge",
                            &m->mac_nubus_bridge,
                            TYPE_MAC_NUBUS_BRIDGE);
    sysbus = SYS_BUS_DEVICE(&m->mac_nubus_bridge);
    dev = DEVICE(&m->mac_nubus_bridge);
    qdev_prop_set_uint32(DEVICE(&m->mac_nubus_bridge), "slot-available-mask",
                         Q950_NUBUS_SLOTS_AVAILABLE);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                MAC_NUBUS_FIRST_SLOT * NUBUS_SUPER_SLOT_SIZE,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_add_subregion(get_system_memory(),
                                NUBUS_SLOT_BASE +
                                MAC_NUBUS_FIRST_SLOT * NUBUS_SLOT_SIZE,
                                sysbus_mmio_get_region(sysbus, 1));
    qdev_connect_gpio_out(dev, 9,
                          qdev_get_gpio_in_named(DEVICE(&m->via2), "nubus-irq",
                          VIA2_NUBUS_IRQ_INTVIDEO));
    for (i = 1; i < VIA2_NUBUS_IRQ_NB; i++) {
        qdev_connect_gpio_out(dev, 9 + i,
                              qdev_get_gpio_in_named(DEVICE(&m->via2),
                                                     "nubus-irq",
                                                     VIA2_NUBUS_IRQ_9 + i));
    }

    /*
     * Since the framebuffer in slot 0x9 uses a separate IRQ, wire the unused
     * IRQ via GLUE for use by SONIC Ethernet in classic mode
     */
    qdev_connect_gpio_out(DEVICE(&m->glue), GLUE_IRQ_NUBUS_9,
                          qdev_get_gpio_in_named(DEVICE(&m->via2), "nubus-irq",
                                                 VIA2_NUBUS_IRQ_9));

    nubus = NUBUS_BUS(qdev_get_child_bus(dev, "nubus-bus.0"));

    /* framebuffer in nubus slot #9 */

    object_initialize_child(OBJECT(machine), "macfb", &m->macfb,
                            TYPE_NUBUS_MACFB);
    dev = DEVICE(&m->macfb);
    qdev_prop_set_uint32(dev, "slot", 9);
    /*
     * The Q900/950 DAFB revision is the Quadra 700's: framebuffer base
     * from the VADDR register (VADDR1 * 0x200); default to the 21"
     * 1152x870 display
     */
    qdev_prop_set_bit(dev, "vaddr-base", true);
    qdev_prop_set_uint32(dev, "width", graphic_width ?: 1152);
    qdev_prop_set_uint32(dev, "height", graphic_height ?: 870);
    qdev_prop_set_uint8(dev, "depth", graphic_depth ?: 8);
    qdev_realize(dev, BUS(nubus), &error_fatal);

    macfb_mode = (NUBUS_MACFB(dev)->macfb).mode;

    /* TurboSCSI handshake register shadowing the DAFB register space */
    memory_region_init_io(&m->turboscsi_mem, OBJECT(machine),
                          &q950_turboscsi_ops, m, "turboscsi",
                          TURBOSCSI_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(), TURBOSCSI_BASE,
                                        &m->turboscsi_mem, 1);

    cs = CPU(&m->cpu);
    if (linux_boot) {
        uint64_t high;
        void *param_blob, *param_ptr, *param_rng_seed;

        if (kernel_cmdline) {
            param_blob = g_malloc(strlen(kernel_cmdline) + 1024);
        } else {
            param_blob = g_malloc(1024);
        }

        kernel_size = load_elf(kernel_filename, NULL, NULL, NULL,
                               &elf_entry, NULL, &high, NULL, ELFDATA2MSB,
                               EM_68K, 0, 0);
        if (kernel_size < 0) {
            error_report("could not load kernel '%s'", kernel_filename);
            exit(1);
        }
        stl_phys(cs->as, 4, elf_entry); /* reset initial PC */
        parameters_base = (high + 1) & ~1;
        param_ptr = param_blob;

        BOOTINFO1(param_ptr, BI_MACHTYPE, MACH_MAC);
        BOOTINFO1(param_ptr, BI_FPUTYPE, FPU_68040);
        BOOTINFO1(param_ptr, BI_MMUTYPE, MMU_68040);
        BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68040);
        BOOTINFO1(param_ptr, BI_MAC_CPUID, CPUB_68040);
        BOOTINFO1(param_ptr, BI_MAC_MODEL, qmc->mac_model);
        BOOTINFO1(param_ptr,
                  BI_MAC_MEMSIZE, ram_size >> 20); /* in MB */
        BOOTINFO2(param_ptr, BI_MEMCHUNK, 0, ram_size);
        BOOTINFO1(param_ptr, BI_MAC_VADDR,
                  VIDEO_BASE + macfb_mode->offset);
        BOOTINFO1(param_ptr, BI_MAC_VDEPTH, macfb_mode->depth);
        BOOTINFO1(param_ptr, BI_MAC_VDIM,
                  (macfb_mode->height << 16) | macfb_mode->width);
        BOOTINFO1(param_ptr, BI_MAC_VROW, macfb_mode->stride);
        BOOTINFO1(param_ptr, BI_MAC_SCCBASE, SCC_BASE);

        if (kernel_cmdline) {
            BOOTINFOSTR(param_ptr, BI_COMMAND_LINE,
                        kernel_cmdline);
        }

        /* Pass seed to RNG. */
        param_rng_seed = param_ptr;
        qemu_guest_getrandom_nofail(rng_seed, sizeof(rng_seed));
        BOOTINFODATA(param_ptr, BI_RNG_SEED,
                     rng_seed, sizeof(rng_seed));

        /* load initrd */
        if (initrd_filename) {
            initrd_size = get_image_size(initrd_filename, NULL);
            if (initrd_size < 0) {
                error_report("could not load initial ram disk '%s'",
                             initrd_filename);
                exit(1);
            }

            initrd_base = (ram_size - initrd_size) & TARGET_PAGE_MASK;
            load_image_targphys(initrd_filename, initrd_base,
                                ram_size - initrd_base, &error_fatal);
            BOOTINFO2(param_ptr, BI_RAMDISK, initrd_base,
                      initrd_size);
        } else {
            initrd_base = 0;
            initrd_size = 0;
        }
        BOOTINFO0(param_ptr, BI_LAST);
        rom_add_blob_fixed_as("bootinfo", param_blob, param_ptr - param_blob,
                              parameters_base, cs->as);
        qemu_register_reset_nosnapshotload(rerandomize_rng_seed,
                            rom_ptr_for_as(cs->as, parameters_base,
                                           param_ptr - param_blob) +
                            (param_rng_seed - param_blob));
        g_free(param_blob);
    } else {
        uint8_t *ptr;
        /* allocate and load BIOS */
        memory_region_init_rom(&m->rom, NULL, "m68k_mac.rom", MACROM_SIZE,
                               &error_abort);
        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
        memory_region_add_subregion(get_system_memory(), MACROM_ADDR, &m->rom);

        memory_region_init_alias(&m->rom_alias, NULL, "m68k_mac.rom-alias",
                                 &m->rom, 0, MACROM_SIZE);
        memory_region_add_subregion(get_system_memory(), 0x40000000,
                                    &m->rom_alias);

        /* Load MacROM binary */
        if (filename) {
            bios_size = load_image_targphys(filename, MACROM_ADDR, MACROM_SIZE,
                                            NULL);
            g_free(filename);
        } else {
            bios_size = -1;
        }

        /* Remove qtest_enabled() check once firmware files are in the tree */
        if (!qtest_enabled()) {
            if (bios_size <= 0 || bios_size > MACROM_SIZE) {
                error_report("could not load MacROM '%s'", bios_name);
                exit(1);
            }

            ptr = rom_ptr(MACROM_ADDR, bios_size);
            assert(ptr != NULL);
            stl_phys(cs->as, 0, ldl_be_p(ptr));    /* reset initial SP */
            stl_phys(cs->as, 4,
                     MACROM_ADDR + ldl_be_p(ptr + 4)); /* reset initial PC */

            /*
             * Make POST failures non-fatal, as on the Quadra 700 (the
             * same dispatcher/branch exists at the same offset in this
             * ROM): the ROM's timing tests measure VIA-timer periods
             * against CPU busy loops, ratios TCG cannot honour.  Turn
             * the dispatcher's single failure branch at ROM offset
             * 0x46f7c ("tstl %d6; beq next") into a bra and fix up the
             * header checksum for the changed word.
             */
            if (bios_size >= 0x46f80 && ldl_be_p(ptr + 0x46f7c) == 0x4a86671a) {
                uint8_t *p = rom_ptr(MACROM_ADDR + 0x46f7e, 1);
                if (p) {
                    *p = 0x60;      /* beq.s -> bra.s */
                    stl_be_p(ptr, ldl_be_p(ptr) - 0x700);
                }
            }
        }
    }
}

static bool q950_get_easc(Object *obj, Error **errp)
{
    Q950MachineState *ms = Q950_MACHINE(obj);

    return ms->easc;
}

static void q950_set_easc(Object *obj, bool value, Error **errp)
{
    Q950MachineState *ms = Q950_MACHINE(obj);

    ms->easc = value;
}

static void q950_init(Object *obj)
{
    Q950MachineState *ms = Q950_MACHINE(obj);

    /* Default to EASC */
    ms->easc = true;
}

static GlobalProperty hw_compat_q950[] = {
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
static const size_t hw_compat_q950_len = G_N_ELEMENTS(hw_compat_q950);

static void q950_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68040"),
        NULL
    };
    const Q950MachineData *md = data;
    Q950MachineClass *qmc = Q950_MACHINE_CLASS(oc);
    MachineClass *mc = MACHINE_CLASS(oc);

    qmc->pins_a = md->pins_a;
    qmc->mac_model = md->mac_model;
    mc->desc = md->desc;
    mc->init = q950_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->block_default_type = IF_SCSI;
    /* an empty auto-created scsi-cd makes MacOS loop a locked-disk alert */
    mc->no_cdrom = true;
    mc->default_ram_id = "m68k_mac.ram";
    machine_add_audiodev_property(mc);
    compat_props_add(mc->compat_props, hw_compat_q950, hw_compat_q950_len);

    object_class_property_add_bool(oc, "easc", q950_get_easc, q950_set_easc);
    object_class_property_set_description(oc, "easc",
        "Set to off to use ASC rather than EASC");
}

static const Q950MachineData q950_data = {
    .desc = "Macintosh Quadra 950",
    .pins_a = Q950_VIA1_PINS_A,
    .mac_model = MAC_MODEL_Q950,
};

static const Q950MachineData q900_data = {
    .desc = "Macintosh Quadra 900",
    .pins_a = Q900_VIA1_PINS_A,
    .mac_model = MAC_MODEL_Q900,
};

static const TypeInfo q950_machine_typeinfo[] = {
    {
        .name       = TYPE_MOS6522_Q950,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522Q950State),
        .instance_init = mos6522_q950_init,
        .class_init = mos6522_q950_class_init,
    },
    {
        .name       = TYPE_Q950_MACHINE,
        .parent     = TYPE_MACHINE,
        .abstract   = true,
        .instance_init = q950_init,
        .instance_size = sizeof(Q950MachineState),
        .class_size = sizeof(Q950MachineClass),
    },
    {
        .name       = MACHINE_TYPE_NAME("quadra950"),
        .parent     = TYPE_Q950_MACHINE,
        .class_init = q950_machine_class_init,
        .class_data = &q950_data,
    },
    {
        .name       = MACHINE_TYPE_NAME("quadra900"),
        .parent     = TYPE_Q950_MACHINE,
        .class_init = q950_machine_class_init,
        .class_data = &q900_data,
    },
};

DEFINE_TYPES(q950_machine_typeinfo)
