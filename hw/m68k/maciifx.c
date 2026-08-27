/*
 * QEMU Macintosh IIfx hardware system emulator
 *
 * 68030 @ 40MHz machine with the OSS interrupt controller (in place of
 * VIA2: 16 interrupt sources, each mapped to a programmable 68030 IPL
 * level), SCC and SWIM fronted by IOP I/O coprocessors (shared-RAM
 * mailboxes, Linux asm/mac_iop.h layout — model lifted from
 * quadra950.c), ADB behind the SWIM/ISM IOP (Linux adb_iop protocol),
 * the SCSIDMA ASIC (343S0064: embedded NCR 53C80 plus handshake/DMA
 * windows), ASC sound, classic 343-0042 RTC bit-banged on VIA1 port B,
 * and NuBus slots 9-E (no onboard video: a macfb NuBus card provides
 * the display).
 *
 * Hardware references: MAME src/mame/apple/maciifx.cpp + scsidma.cpp,
 * Linux arch/m68k/mac/oss.c + asm/mac_oss.h + asm/mac_iop.h.
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
#include "system/rtc.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/runstate.h"

#define MACIIFX_ROM_ADDR      0x40800000
#define MACIIFX_ROM_SIZE      0x00080000
#define MACIIFX_ROM_FILENAME  "maciifx.rom"

/*
 * I/O: one 1MB decoded slice at 0x50000000, mirrored through the first
 * 16MB (address bits 20-23 are don't-care: MAME's mirror 0x00f00000;
 * Linux uses the 0x50F0xxxx names).
 */
#define IO_BASE               0x50000000
#define IO_SLICE              0x00100000
#define IO_SLICE_MASK         (IO_SLICE - 1)
#define IO_SIZE               0x01000000

/* offsets within the I/O slice */
#define VIA1_OFS              0x00000
#define VIA1_MIRROR_OFS       0x40000
#define SCC_IOP_OFS           0x04000
#define SCC_OFS               0x04020    /* IOP bypass window */
#define SCSIDMA_OFS           0x08000
#define ASC_OFS               0x10000
#define SWIM_IOP_OFS          0x12000
#define SWIM_OFS              0x12000
#define BIU_OFS               0x18000
#define OSS_OFS               0x1a000
#define EXP0_OFS              0x1c000

#define VIA_SPACING_SHIFT     9       /* VIA regs every 0x200 */
#define VIA_REGION_SIZE       0x2000
#define VIA_TIMER_FREQ        783360
#define VIA_60HZ_TIMER_PERIOD_NS   16625800

#define MAC_CLOCK             3686418

/* Size of whole RAM area (unbacked reads return 0 for RAM sizing) */
#define RAM_SIZE              0x40000000

/* VIA returns time offset from Jan 1, 1904, not 1970 */
#define RTC_OFFSET 2082844800

/*
 * VIA1 port A machine-ID straps: MAME's maciifx returns 0xd3
 * (PA7|PA6|PA4|PA1|PA0).
 */
#define MACIIFX_VIA1_PINS_A   0xd3

static int maciifx_log_budget = 400000;

#define maciifx_log(...) do {                                   \
        if (maciifx_log_budget > 0) {                           \
            maciifx_log_budget--;                               \
            qemu_log_mask(LOG_UNIMP, __VA_ARGS__);              \
        }                                                       \
    } while (0)

/* ------------------------------------------------------------------ */
/* OSS interrupt controller (Linux mac_oss.h, MAME maciifx.cpp)        */

#define OSS_NUBUS0     0        /* NuBus slot 9 */
#define OSS_IOPISM     6
#define OSS_IOPSCC     7
#define OSS_SOUND      8
#define OSS_SCSI       9
#define OSS_60HZ       10
#define OSS_VIA1       11
#define OSS_PARITY     14
#define OSS_NUM_SOURCES 16

#define OSS_REG_STATUS    0x200  /* bit7 = interrupt active */
#define OSS_REG_PENDING_H 0x202  /* sources 8-15 */
#define OSS_REG_PENDING_L 0x203  /* sources 0-7 */
#define OSS_REG_ROM_CTRL  0x204  /* write 0x80 = poweroff */
#define OSS_REG_ACK_60HZ  0x207

/* 343-0042 RTC command decode (as on maciisi/lc475) */
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

#define VIA1B_vRTCEnb  0x04
#define VIA1B_vRTCClk  0x02
#define VIA1B_vRTCData 0x01

/* ------------------------------------------------------------------ */
/* VIA1 subclass: RTC/PRAM bit-bang on port B; carries the ADB bus     */
/* (the ADB devices actually hang off the ISM IOP)                     */

#define TYPE_MOS6522_MACIIFX "mos6522-maciifx"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522MacIIfxState, MOS6522_MACIIFX)

struct MacIIfxMachineState;

struct MOS6522MacIIfxState {
    MOS6522State parent_obj;

    struct MacIIfxMachineState *machine;
    ADBBusState adb_bus;
    uint8_t pins_a;
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

static void via1_rtc_update(MOS6522MacIIfxState *v1s)
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
                v1s->data_in = v1s->PRAM[(cmd & 0x7f) - REG_PRAM_ADDR];
                v1s->data_in_cnt = 8;
                break;
            case REG_PRAM_SECT...REG_PRAM_SECT_LAST:
                /* the only two-byte read command */
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
                /* it's a read */
                v1s->data_in = v1s->PRAM[sector * 32 + addr];
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

static void maciifx_via1_portA_write(MOS6522State *s)
{
}

static void maciifx_via1_portB_write(MOS6522State *s)
{
    /* RTC engine runs from the MMIO write hook (needs last_b ordering) */
}

static void mos6522_maciifx_init(Object *obj)
{
    MOS6522MacIIfxState *v1s = MOS6522_MACIIFX(obj);

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;

    /* the ADB bus hangs off the ISM IOP; the VIA1 device stands in for it */
    qbus_init(&v1s->adb_bus, sizeof(v1s->adb_bus), TYPE_ADB_BUS,
              DEVICE(obj), "adb.0");
}

static void mos6522_maciifx_reset_hold(Object *obj, ResetType type)
{
    MOS6522MacIIfxState *v1s = MOS6522_MACIIFX(obj);
    MOS6522State *ms = MOS6522(v1s);
    MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(ms);

    if (mdc->parent_phases.hold) {
        mdc->parent_phases.hold(obj, type);
    }

    /* board-specific idle state of the input pins */
    ms->a = v1s->pins_a;
    ms->b = 0xff;

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;
    v1s->data_out_cnt = 0;
    v1s->data_in_cnt = 0;
}

static const Property mos6522_maciifx_properties[] = {
    DEFINE_PROP_UINT8("pins-a", MOS6522MacIIfxState, pins_a, 0xff),
};

static void mos6522_maciifx_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    MOS6522DeviceClass *mdc = MOS6522_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_props(dc, mos6522_maciifx_properties);
    mdc->portA_write = maciifx_via1_portA_write;
    mdc->portB_write = maciifx_via1_portB_write;
    resettable_class_set_parent_phases(rc, NULL, mos6522_maciifx_reset_hold,
                                       NULL, &mdc->parent_phases);
}

/* ------------------------------------------------------------------ */
/* IOP host interface + mailbox HLE (lifted from quadra950.c)          */

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

typedef struct MacIIfxIOP {
    struct MacIIfxMachineState *machine;
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
     * RECEIVE message slot: the ROM stages follow-up explicit ADB
     * commands IN PLACE in that slot (see quadra950.c).
     */
    uint8_t adb_last_recv[IOP_MSG_LEN];
    bool adb_last_recv_valid;
} MacIIfxIOP;

/* ------------------------------------------------------------------ */
/* machine state                                                       */

struct MacIIfxMachineState {
    MachineState parent_obj;

    M68kCPU cpu;
    MOS6522MacIIfxState via1;
    ESCCState escc;
    OrIRQState escc_orgate;
    ASCState asc;
    Swim swim;
    NCR5380State scsi;
    MacNubusBridge mac_nubus_bridge;
    MacIIfxIOP scc_iop;
    MacIIfxIOP swim_iop;

    MemoryRegion rom;
    MemoryRegion rom_alias;
    MemoryRegion ramio;
    MemoryRegion ramio_a31;
    MemoryRegion macio;
    MemoryRegion macio_alias;
    MemoryRegion via1mem;
    MemoryRegion via1mem_mirror;
    MemoryRegion biumem;
    MemoryRegion ossmem;
    MemoryRegion asc_mirror;
    MemoryRegion exp0mem;
    uint8_t exp0_regs[0x40];
    MemoryRegion scsi_hskd;
    MemoryRegion scsi_hskr;
    MemoryRegion scsi_ctrlmem;
    MemoryRegion iotrace;

    /* OSS */
    uint8_t oss_level[OSS_NUM_SOURCES];
    uint16_t oss_pending;       /* bit n = source n */
    uint8_t oss_rom_ctrl;
    uint8_t oss_regs[0x400];    /* raw storage for the unmodelled rest */
    int last_ipl;

    /* SCSIDMA control */
    uint32_t scsidma_ctrl;
    uint32_t scsidma_count;
    uint32_t scsidma_addr;
    bool scsi_irq_state;

    /* VIA1 CA1 60Hz tick and CA2 one-second tick */
    QEMUTimer *sixty_hz_timer;
    QEMUTimer *one_second_timer;

    /* ADB autopoll routed through the ISM IOP mailbox */
    bool iop_autopoll;
};

#define TYPE_MACIIFX_MACHINE MACHINE_TYPE_NAME("maciifx")
OBJECT_DECLARE_SIMPLE_TYPE(MacIIfxMachineState, MACIIFX_MACHINE)

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static uint32_t maciifx_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

/* ------------------------------------------------------------------ */
/* OSS                                                                 */

static void maciifx_oss_update(MacIIfxMachineState *m)
{
    int level = 0;
    int i;

    for (i = 0; i < OSS_NUM_SOURCES; i++) {
        if ((m->oss_pending & (1 << i)) && m->oss_level[i] > level) {
            level = m->oss_level[i] & 7;
        }
    }

    if (level != m->last_ipl) {
        maciifx_log("maciifx oss: IPL %d -> %d (pending 0x%04x levels "
                    "%d%d%d%d%d%d%d%d.%d%d%d%d%d%d%d%d)\n",
                    m->last_ipl, level, m->oss_pending,
                    m->oss_level[0], m->oss_level[1], m->oss_level[2],
                    m->oss_level[3], m->oss_level[4], m->oss_level[5],
                    m->oss_level[6], m->oss_level[7], m->oss_level[8],
                    m->oss_level[9], m->oss_level[10], m->oss_level[11],
                    m->oss_level[12], m->oss_level[13], m->oss_level[14],
                    m->oss_level[15]);
    }
    m->last_ipl = level;

    if (level > 0) {
        m68k_set_irq_level(&m->cpu, level, 24 + level);
    } else {
        m68k_set_irq_level(&m->cpu, 0, 0);
    }
}

/* a source's interrupt line changed (60Hz is latched separately) */
static void maciifx_oss_set_source(MacIIfxMachineState *m, int src,
                                   int level)
{
    if (level) {
        m->oss_pending |= 1 << src;
    } else {
        m->oss_pending &= ~(1 << src);
    }
    maciifx_oss_update(m);
}

static uint64_t maciifx_oss_read(void *opaque, hwaddr addr, unsigned size)
{
    MacIIfxMachineState *m = opaque;
    uint64_t val;

    addr &= 0x1fff;
    if (addr < OSS_NUM_SOURCES) {
        val = m->oss_level[addr];
    } else {
        switch (addr) {
        case OSS_REG_STATUS:
            val = m->last_ipl ? 0x80 : 0x00;
            break;
        case OSS_REG_PENDING_H:
            val = (m->oss_pending >> 8) & 0xff;
            break;
        case OSS_REG_PENDING_L:
            val = m->oss_pending & 0xff;
            break;
        case OSS_REG_ROM_CTRL:
            val = m->oss_rom_ctrl;
            break;
        default:
            val = addr < sizeof(m->oss_regs) ? m->oss_regs[addr] : 0;
            break;
        }
    }

    maciifx_log("maciifx oss: read  +0x%03x -> 0x%02" PRIx64 " pc=0x%08x\n",
                (unsigned)addr, val, maciifx_trace_pc());
    return val;
}

static void maciifx_oss_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MacIIfxMachineState *m = opaque;

    addr &= 0x1fff;
    maciifx_log("maciifx oss: write +0x%03x <- 0x%02" PRIx64 " pc=0x%08x\n",
                (unsigned)addr, val, maciifx_trace_pc());

    if (addr < OSS_NUM_SOURCES) {
        m->oss_level[addr] = val & 7;
        maciifx_oss_update(m);
        return;
    }

    switch (addr) {
    case OSS_REG_ACK_60HZ:
        m->oss_pending &= ~(1 << OSS_60HZ);
        maciifx_oss_update(m);
        break;
    case OSS_REG_PENDING_H:
        m->oss_pending = (m->oss_pending & 0x00ff) | ((val & 0xff) << 8);
        maciifx_oss_update(m);
        break;
    case OSS_REG_PENDING_L:
        m->oss_pending = (m->oss_pending & 0xff00) | (val & 0xff);
        maciifx_oss_update(m);
        break;
    case OSS_REG_ROM_CTRL:
        m->oss_rom_ctrl = val;
        if (val & 0x80) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
        break;
    default:
        if (addr < sizeof(m->oss_regs)) {
            m->oss_regs[addr] = val;
        }
        break;
    }
}

static const MemoryRegionOps maciifx_oss_ops = {
    .read = maciifx_oss_read,
    .write = maciifx_oss_write,
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

/* interrupt line handlers feeding the OSS */

static void maciifx_via1_irq(void *opaque, int n, int level)
{
    MacIIfxMachineState *m = opaque;

    maciifx_oss_set_source(m, OSS_VIA1, level);
}

static void maciifx_asc_irq(void *opaque, int n, int level)
{
    MacIIfxMachineState *m = opaque;

    maciifx_oss_set_source(m, OSS_SOUND, level);
}

static void maciifx_scc_irq(void *opaque, int n, int level)
{
    MacIIfxMachineState *m = opaque;

    maciifx_oss_set_source(m, OSS_IOPSCC, level);
}

static void maciifx_ism_irq(void *opaque, int n, int level)
{
    MacIIfxMachineState *m = opaque;

    maciifx_oss_set_source(m, OSS_IOPISM, level);
}

/*
 * A NuBus card in slot 9+n interrupts through OSS source n.
 */
static void maciifx_nubus_irq(void *opaque, int n, int level)
{
    MacIIfxMachineState *m = opaque;
    int src = (n & 0xf) - 9;

    if (src >= 0 && src <= 5) {
        maciifx_oss_set_source(m, OSS_NUBUS0 + src, level);
    }
}

/*
 * SCSIDMA interrupt: the 53C80 IRQ is a status bit (SCIRQEN) in the
 * control register and reaches the OSS only while IRQEN is set.
 */
#define SCSIDMA_CTRL_DMAEN     0x0001
#define SCSIDMA_CTRL_IRQEN     0x0002
#define SCSIDMA_CTRL_HNDSHK    0x0008
#define SCSIDMA_CTRL_SCIRQEN   0x0040
#define SCSIDMA_CTRL_WDIRQ     0x0080
#define SCSIDMA_CTRL_DMABERR   0x0100
#define SCSIDMA_CTRL_ARBEN     0x1000
#define SCSIDMA_CTRL_WONARB    0x2000
#define SCSIDMA_CTRL_STATUS_MASK  (SCSIDMA_CTRL_SCIRQEN | \
                                   SCSIDMA_CTRL_WDIRQ | \
                                   SCSIDMA_CTRL_DMABERR | \
                                   SCSIDMA_CTRL_WONARB)

static void maciifx_scsidma_update_irq(MacIIfxMachineState *m)
{
    m->scsidma_ctrl &= ~SCSIDMA_CTRL_SCIRQEN;
    if (m->scsi_irq_state) {
        m->scsidma_ctrl |= SCSIDMA_CTRL_SCIRQEN;
    }
    maciifx_oss_set_source(m, OSS_SCSI,
                           m->scsi_irq_state &&
                           (m->scsidma_ctrl & SCSIDMA_CTRL_IRQEN));
}

static void maciifx_scsi_irq(void *opaque, int n, int level)
{
    MacIIfxMachineState *m = opaque;

    m->scsi_irq_state = level;
    maciifx_scsidma_update_irq(m);
}

/* ------------------------------------------------------------------ */
/* 60.15Hz tick (VIA1 CA1 + latched OSS source 10) and 1Hz on CA2      */

static void maciifx_iop_adb_poll(void *opaque);

static void maciifx_sixty_hz(void *opaque)
{
    MacIIfxMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA1_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    /* the OSS 60Hz source latches until acked at +0x207 */
    m->oss_pending |= 1 << OSS_60HZ;
    maciifx_oss_update(m);

    timer_mod(m->sixty_hz_timer,
              (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
               VIA_60HZ_TIMER_PERIOD_NS) /
              VIA_60HZ_TIMER_PERIOD_NS * VIA_60HZ_TIMER_PERIOD_NS);
}

static void maciifx_one_second(void *opaque)
{
    MacIIfxMachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA2_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->one_second_timer,
              (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000) / 1000 * 1000);
}

/* ------------------------------------------------------------------ */
/* IOP mailbox engine (see quadra950.c for the protocol archaeology)   */

static void maciifx_iop_update_irq(MacIIfxIOP *iop)
{
    if (iop->irq) {
        qemu_set_irq(iop->irq, (iop->ctrl & (IOP_INT0 | IOP_INT1)) != 0);
    }
}

static void maciifx_iop_adb_msg(MacIIfxIOP *iop, uint8_t *msg);

/* post a message on an IOP receive channel (IOP -> host direction) */
static void maciifx_iop_post_recv(MacIIfxIOP *iop, int chan,
                                  const uint8_t *msg, int len)
{
    uint8_t *dst = &iop->ram[IOP_ADDR_RECV_MSG + chan * IOP_MSG_LEN];

    memset(dst, 0, IOP_MSG_LEN);
    memcpy(dst, msg, MIN(len, IOP_MSG_LEN));
    iop->ram[IOP_ADDR_RECV_STATE + chan] = IOP_MSG_NEW;
    iop->ctrl |= IOP_INT1;
    maciifx_iop_update_irq(iop);
    maciifx_log("maciifx iop %s: recv chan %d posted [%02x %02x %02x ...]\n",
                iop->name, chan, msg[0], msg[1], msg[2]);

    if (iop->is_ism && chan == ADB_CHAN) {
        memcpy(iop->adb_last_recv, dst, IOP_MSG_LEN);
        iop->adb_last_recv_valid = true;
    }
}

/*
 * The ROM stages follow-up explicit ADB commands in place in the ADB
 * receive slot instead of using a fresh SEND_MSG (see quadra950.c).
 */
static bool maciifx_iop_adb_check_staged(MacIIfxIOP *iop)
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

    maciifx_log("maciifx iop %s: staged follow-up ADB cmd in recv slot "
                "[%02x %02x %02x %02x %02x %02x]\n", iop->name, slot[0],
                slot[1], slot[2], slot[3], slot[4], slot[5]);
    maciifx_iop_adb_msg(iop, slot);
    return true;
}

/*
 * ADB service on the ISM IOP, channel 2 (Linux adb-iop.c protocol):
 * struct adb_iopmsg { flags, count, cmd, data[8], spare[21] }.
 */
static void maciifx_iop_adb_msg(MacIIfxIOP *iop, uint8_t *msg)
{
    MacIIfxMachineState *m = iop->machine;
    ADBBusState *adb_bus = &m->via1.adb_bus;
    uint8_t flags = msg[0], count = msg[1], cmd = msg[2];

    if (flags & ADB_IOP_SET_AUTOPOLL) {
        uint16_t mask = (msg[3] << 8) | msg[4];
        bool enable = (flags & ADB_IOP_AUTOPOLL) && mask != 0;

        adb_set_autopoll_mask(adb_bus, mask);
        adb_set_autopoll_enabled(adb_bus, enable);
        m->iop_autopoll = enable;
        maciifx_log("maciifx iop adb: autopoll mask 0x%04x %s\n", mask,
                    enable ? "on" : "off");
        return;
    }

    /*
     * Anything else is an explicit ADB command.  The ROM-era driver
     * tags them ADB_IOP_EXPLICIT (0x80) but the OS-era RAM driver
     * sends flags 0x00 — those must be serviced too (dropping them
     * wedges the OS ADB manager in a resend loop).
     */
    {
        uint8_t req[1 + 8];
        uint8_t obuf[ADB_MAX_OUT_LEN];
        int dlen = MIN(count, 8);
        int olen, reqlen;
        bool is_listen = (cmd & 0x0c) == 0x08;

        req[0] = cmd;
        memcpy(req + 1, msg + 3, dlen);
        /* only Listen commands carry data on the bus */
        reqlen = (is_listen && dlen > 0) ? 1 + dlen : 1;

        if (is_listen && dlen == 0) {
            /*
             * Listen with no data bytes: a no-op on the wire.  Do NOT
             * forward it — QEMU's ADB devices parse the (absent) first
             * data byte as a new device address and would hop to a
             * garbage address (observed: keyboard parked at address 0,
             * autopolled Talk R0 tagged 0x0c, input dead).
             */
            olen = 0;
        } else {
            adb_autopoll_block(adb_bus);
            olen = adb_request(adb_bus, obuf, req, reqlen);
            adb_autopoll_unblock(adb_bus);
        }

        {
            uint8_t reply[3 + 8];

            memset(reply, 0, sizeof(reply));
            reply[2] = cmd;
            if (olen >= 0) {
                /*
                 * olen == 0 is a SUCCESSFUL exchange with no data bytes
                 * (Listen commands, SendReset): the IIfx ROM's ADBReInit
                 * relocation dance treats a TIMEOUT-flagged Listen reply
                 * as "no device answered" and retries the collision loop
                 * forever.  Only a negative return (nothing on the bus)
                 * is a real ADB timeout.
                 */
                reply[0] = ADB_IOP_EXPLICIT;
                reply[1] = MIN(olen, 8);
                memcpy(reply + 3, obuf, MIN(olen, 8));
            } else {
                reply[0] = ADB_IOP_EXPLICIT | ADB_IOP_TIMEOUT;
                reply[1] = 0;
            }
            /* mirror into the send message too (harmless) */
            memcpy(msg, reply, 3 + 8);
            maciifx_log("maciifx iop adb: cmd %02x len %d -> %s len %d\n",
                        cmd, dlen, olen >= 0 ? "ok" : "timeout", olen);
            maciifx_iop_post_recv(iop, ADB_CHAN, reply,
                                  3 + (olen > 0 ? MIN(olen, 8) : 0));
        }
    }
}

/* autopolled ADB data (the ADB bus autopoll timer callback) */
static void maciifx_iop_adb_poll(void *opaque)
{
    MacIIfxMachineState *m = opaque;
    MacIIfxIOP *iop = &m->swim_iop;
    ADBBusState *adb_bus = &m->via1.adb_bus;
    uint8_t obuf[ADB_MAX_OUT_LEN + 2];
    uint8_t reply[3 + 8];
    int olen;

    if (!m->iop_autopoll) {
        return;
    }
    if (iop->ram[IOP_ADDR_RECV_STATE + ADB_CHAN] != IOP_MSG_IDLE) {
        return;             /* previous message not consumed yet */
    }

    olen = adb_poll(adb_bus, obuf, adb_bus->autopoll_mask);
    if (olen <= 0) {
        return;
    }

    /*
     * obuf[0] = the Talk R0 command byte adb_poll tagged.  Flags bit7
     * (EXPLICIT) must be CLEAR: the OS ADB manager's listener
     * dispatches on it (SET = reply to an explicit command -> it tries
     * to complete a request that was never issued; delivering autopoll
     * data with bit7 set bombed the Finder the moment the mouse moved).
     */
    memset(reply, 0, sizeof(reply));
    reply[0] = ADB_IOP_AUTOPOLL;
    reply[1] = MIN(olen - 1, 8);
    reply[2] = obuf[0];
    memcpy(reply + 3, obuf + 1, MIN(olen - 1, 8));
    maciifx_iop_post_recv(iop, ADB_CHAN, reply, 3 + MIN(olen - 1, 8));
}

static void maciifx_iop_process(MacIIfxIOP *iop)
{
    int chan, i;
    bool found = false;

    for (chan = 0; chan < NUM_IOP_CHAN; chan++) {
        if (iop->ram[IOP_ADDR_SEND_STATE + chan] == IOP_MSG_NEW) {
            found = true;
        }
    }
    if (!found && maciifx_iop_adb_check_staged(iop)) {
        found = true;
    }

    for (chan = 0; chan < NUM_IOP_CHAN; chan++) {
        if (iop->ram[IOP_ADDR_SEND_STATE + chan] == IOP_MSG_NEW) {
            uint8_t *msg = &iop->ram[IOP_ADDR_SEND_MSG + chan * IOP_MSG_LEN];
            char buf[3 * IOP_MSG_LEN + 1];

            for (i = 0; i < IOP_MSG_LEN; i++) {
                sprintf(buf + i * 3, " %02x", msg[i]);
            }
            maciifx_log("maciifx iop %s: send chan %d msg%s pc=0x%08x\n",
                        iop->name, chan, buf, maciifx_trace_pc());
            /* service what we understand, then complete the message */
            if (iop->is_ism && chan == ADB_CHAN) {
                maciifx_iop_adb_msg(iop, msg);
            }
            iop->ram[IOP_ADDR_SEND_STATE + chan] = IOP_MSG_COMPLETE;
        }
    }
    /*
     * Always raise INT0 on a kick, even with nothing newly completed
     * (see quadra950.c: the completed-request queue drain is installed
     * from the IOP ISR epilogue).
     */
    iop->ctrl |= IOP_INT0;
    maciifx_iop_update_irq(iop);
}

static void maciifx_iop_ctrl_write(MacIIfxIOP *iop, uint8_t val)
{
    uint8_t old = iop->ctrl;
    uint8_t ctrl;

    maciifx_log("maciifx iop %s: ctrl <- 0x%02x (was 0x%02x) pc=0x%08x\n",
                iop->name, val, old, maciifx_trace_pc());

    /* mode bits are stored; writing INT0/INT1 clears those flags */
    ctrl = (old & (IOP_INT0 | IOP_INT1)) |
           (val & (IOP_BYPASS | IOP_AUTOINC | IOP_RUN));
    ctrl &= ~(val & (IOP_INT0 | IOP_INT1));
    iop->ctrl = ctrl;

    if ((val & IOP_RUN) && !(old & IOP_RUN)) {
        iop->ram[IOP_ADDR_ALIVE] = 0xff;
        if (iop->ram[IOP_ADDR_MAX_SEND_CHAN] == 0) {
            iop->ram[IOP_ADDR_MAX_SEND_CHAN] = NUM_IOP_CHAN;
        }
        if (iop->ram[IOP_ADDR_MAX_RECV_CHAN] == 0) {
            iop->ram[IOP_ADDR_MAX_RECV_CHAN] = NUM_IOP_CHAN;
        }
        maciifx_log("maciifx iop %s: started (alive; max chans %d/%d)\n",
                    iop->name, iop->ram[IOP_ADDR_MAX_SEND_CHAN],
                    iop->ram[IOP_ADDR_MAX_RECV_CHAN]);
    }
    if (val & IOP_IRQ) {
        /*
         * Host attention: scan the send channels after a short virtual
         * delay (100us; see quadra950.c for why neither synchronous
         * nor long works; run with -icount shift=7).
         */
        timer_mod(iop->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * 1000);
    }
    maciifx_iop_update_irq(iop);
}

static void maciifx_iop_timer_cb(void *opaque)
{
    MacIIfxIOP *iop = opaque;

    maciifx_iop_process(iop);
}

static uint64_t maciifx_iop_read(void *opaque, hwaddr addr, unsigned size)
{
    MacIIfxIOP *iop = opaque;
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
        if (iop->ctrl & IOP_AUTOINC) {
            iop->addr++;
        }
        break;
    }
    return val;
}

static void maciifx_iop_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MacIIfxIOP *iop = opaque;

    switch (addr >> 1) {
    case 0:
        iop->addr = (iop->addr & 0x00ff) | ((val & 0xff) << 8);
        break;
    case 1:
        iop->addr = (iop->addr & 0xff00) | (val & 0xff);
        break;
    case 2:
    case 3:
        maciifx_iop_ctrl_write(iop, val);
        break;
    default:
        iop->ram[iop->addr & 0x7fff] = val;
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

static const MemoryRegionOps maciifx_iop_ops = {
    .read = maciifx_iop_read,
    .write = maciifx_iop_write,
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

/* ------------------------------------------------------------------ */
/* RAM-hole container (unbacked reads return 0 for the sizing probes)  */

static uint64_t ramio_read(void *opaque, hwaddr addr, unsigned size)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "maciifx ram-hole: read  0x%08x (%d) pc=0x%08x\n",
                      (unsigned)addr, size, maciifx_trace_pc());
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
                      "maciifx ram-hole: write 0x%08x (%d) <- 0x%08" PRIx64
                      " pc=0x%08x\n",
                      (unsigned)addr, size, val, maciifx_trace_pc());
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

/* I/O slice mirror (address bits 20-23 don't care) */

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
 * tagged master pointers must alias onto RAM), as on the IIsi.
 */

static MemTxResult maciifx_a31_read(void *opaque, hwaddr addr, uint64_t *data,
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

static MemTxResult maciifx_a31_write(void *opaque, hwaddr addr, uint64_t value,
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

static const MemoryRegionOps maciifx_a31_ops = {
    .read_with_attrs = maciifx_a31_read,
    .write_with_attrs = maciifx_a31_write,
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

/* VIA1 MMIO (regs every 0x200; port A input pins read the straps) */

static uint64_t maciifx_via1_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS6522MacIIfxState *v1s = opaque;
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

static void maciifx_via1_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MOS6522MacIIfxState *v1s = opaque;
    MOS6522State *s = opaque;
    hwaddr reg = (addr >> VIA_SPACING_SHIFT) & 0xf;

    mos6522_write(s, reg, val, size);

    if (reg == VIA_REG_B) {
        via1_rtc_update(v1s);
        v1s->last_b = s->b;
    }
}

static const MemoryRegionOps maciifx_via1_ops = {
    .read = maciifx_via1_read,
    .write = maciifx_via1_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* ------------------------------------------------------------------ */
/* SCSIDMA handshake windows and control registers                     */

/*
 * Handshake data window at +0x00-0x03 (blind MOVE.L transfers move a
 * byte per byte-lane, MSB first).  Our data phases are synchronous: a
 * byte is either ready now or never will be, so the no-byte case takes
 * the bus error the ROM's blind loops use as the end-of-phase signal.
 * Non-data accesses fall through to the plain 53C80 register 0.
 */

static MemTxResult maciifx_scsi_hskd_read(void *opaque, hwaddr addr,
                                          uint64_t *data, unsigned size,
                                          MemTxAttrs attrs)
{
    MacIIfxMachineState *m = opaque;
    NCR5380State *s = &m->scsi;

    if (ncr5380_pdma_ready(s, false)) {
        *data = ncr5380_pdma_read(s);
        return MEMTX_OK;
    }
    if (m->scsidma_ctrl & SCSIDMA_CTRL_HNDSHK) {
        *data = 0;
        return MEMTX_DECODE_ERROR;
    }
    /*
     * Plain CSD read (5380 register 0; inlined — going through the
     * address space would recurse into this overlay): during an
     * initiator-input phase the current data lines carry the last
     * latched byte, otherwise our own ODR drive.
     */
    *data = (m->scsi.phase & 0x04 /* CSB_IO */) ? m->scsi.last_data
                                                : m->scsi.odr;
    return MEMTX_OK;
}

static MemTxResult maciifx_scsi_hskd_write(void *opaque, hwaddr addr,
                                           uint64_t val, unsigned size,
                                           MemTxAttrs attrs)
{
    MacIIfxMachineState *m = opaque;
    NCR5380State *s = &m->scsi;

    if (ncr5380_pdma_ready(s, true)) {
        ncr5380_pdma_write(s, val);
        return MEMTX_OK;
    }
    if (m->scsidma_ctrl & SCSIDMA_CTRL_HNDSHK) {
        return MEMTX_DECODE_ERROR;
    }
    /* plain ODR write (5380 register 0 is a bare latch; inlined) */
    m->scsi.odr = val;
    return MEMTX_OK;
}

static const MemoryRegionOps maciifx_scsi_hskd_ops = {
    .read_with_attrs = maciifx_scsi_hskd_read,
    .write_with_attrs = maciifx_scsi_hskd_write,
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

/* handshake read window at +0x60-0x63 */

static MemTxResult maciifx_scsi_hskr_read(void *opaque, hwaddr addr,
                                          uint64_t *data, unsigned size,
                                          MemTxAttrs attrs)
{
    MacIIfxMachineState *m = opaque;
    NCR5380State *s = &m->scsi;

    if (ncr5380_pdma_ready(s, false)) {
        *data = ncr5380_pdma_read(s);
        return MEMTX_OK;
    }
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult maciifx_scsi_hskr_write(void *opaque, hwaddr addr,
                                           uint64_t val, unsigned size,
                                           MemTxAttrs attrs)
{
    MacIIfxMachineState *m = opaque;
    NCR5380State *s = &m->scsi;

    if (ncr5380_pdma_ready(s, true)) {
        ncr5380_pdma_write(s, val);
        return MEMTX_OK;
    }
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps maciifx_scsi_hskr_ops = {
    .read_with_attrs = maciifx_scsi_hskr_read,
    .write_with_attrs = maciifx_scsi_hskr_write,
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

/* control registers at +0x80 (u32: +0x80 ctrl, +0xc0 count, +0x100 addr) */

static uint64_t maciifx_scsi_ctrl_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    MacIIfxMachineState *m = opaque;
    uint64_t val = 0;

    switch (addr + 0x80) {
    case 0x80:
        val = m->scsidma_ctrl;
        break;
    case 0xc0:
        val = m->scsidma_count;
        break;
    case 0x100:
        val = m->scsidma_addr;
        break;
    default:
        break;
    }
    maciifx_log("maciifx scsidma: read  +0x%03x -> 0x%08" PRIx64
                " pc=0x%08x\n", (unsigned)addr + 0x80, val,
                maciifx_trace_pc());
    return val;
}

static void maciifx_scsi_ctrl_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    MacIIfxMachineState *m = opaque;

    maciifx_log("maciifx scsidma: write +0x%03x <- 0x%08" PRIx64
                " pc=0x%08x\n", (unsigned)addr + 0x80, val,
                maciifx_trace_pc());

    switch (addr + 0x80) {
    case 0x80:
        m->scsidma_ctrl = (m->scsidma_ctrl & SCSIDMA_CTRL_STATUS_MASK) |
                          (val & ~SCSIDMA_CTRL_STATUS_MASK);
        if (val & SCSIDMA_CTRL_ARBEN) {
            qemu_log_mask(LOG_UNIMP,
                          "maciifx scsidma: auto-arbitration unimplemented\n");
        }
        maciifx_scsidma_update_irq(m);
        break;
    case 0xc0:
        m->scsidma_count = val;
        break;
    case 0x100:
        m->scsidma_addr = val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps maciifx_scsi_ctrl_ops = {
    .read = maciifx_scsi_ctrl_read,
    .write = maciifx_scsi_ctrl_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* BIU at +0x18000: reads as zero */

static uint64_t maciifx_biu_read(void *opaque, hwaddr addr, unsigned size)
{
    maciifx_log("maciifx biu: read  +0x%04x pc=0x%08x\n", (unsigned)addr,
                maciifx_trace_pc());
    return 0;
}

static void maciifx_biu_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    maciifx_log("maciifx biu: write +0x%04x <- 0x%08" PRIx64 " pc=0x%08x\n",
                (unsigned)addr, val, maciifx_trace_pc());
}

static const MemoryRegionOps maciifx_biu_ops = {
    .read = maciifx_biu_read,
    .write = maciifx_biu_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * EXP0 (PDS expansion pseudo-slot registers) at +0x1c000: the ROM's
 * hardware-init writes a 17-byte pattern here (0x40802e5c: the 0xF3FF
 * shift loop) under a bus-error catcher whose continuation RETRIES the
 * whole device init — so unlike EXP1 (+0x1e000, probed and expected
 * absent) these writes must succeed.  Modelled as scratch registers.
 */

static uint64_t maciifx_exp0_read(void *opaque, hwaddr addr, unsigned size)
{
    MacIIfxMachineState *m = opaque;
    uint64_t val = m->exp0_regs[addr & 0x3f];

    maciifx_log("maciifx exp0: read  +0x%04x -> 0x%02" PRIx64 " pc=0x%08x\n",
                (unsigned)addr, val, maciifx_trace_pc());
    return val;
}

static void maciifx_exp0_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MacIIfxMachineState *m = opaque;

    maciifx_log("maciifx exp0: write +0x%04x <- 0x%02" PRIx64 " pc=0x%08x\n",
                (unsigned)addr, val, maciifx_trace_pc());
    m->exp0_regs[addr & 0x3f] = val;
}

static const MemoryRegionOps maciifx_exp0_ops = {
    .read = maciifx_exp0_read,
    .write = maciifx_exp0_write,
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

static MemTxResult maciifx_iotrace_read(void *opaque, hwaddr addr,
                                        uint64_t *data, unsigned size,
                                        MemTxAttrs attrs)
{
    maciifx_log("maciifx io: read  +0x%05x (%d) -> BERR pc=0x%08x\n",
                (unsigned)addr, size, maciifx_trace_pc());
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult maciifx_iotrace_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size,
                                         MemTxAttrs attrs)
{
    maciifx_log("maciifx io: write +0x%05x (%d) <- 0x%08" PRIx64
                " -> BERR pc=0x%08x\n",
                (unsigned)addr, size, val, maciifx_trace_pc());
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps maciifx_iotrace_ops = {
    .read_with_attrs = maciifx_iotrace_read,
    .write_with_attrs = maciifx_iotrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* ------------------------------------------------------------------ */
/* machine init                                                        */

static void maciifx_machine_init(MachineState *machine)
{
    MacIIfxMachineState *m = MACIIFX_MACHINE(machine);
    const char *bios_name = machine->firmware ?: MACIIFX_ROM_FILENAME;
    char *filename;
    int bios_size;
    uint8_t *ptr;
    CPUState *cs;
    DeviceState *dev;
    SysBusDevice *sysbus;
    NubusBus *nubus;
    int i;

    /* CPU */
    object_initialize_child(OBJECT(machine), "cpu", &m->cpu,
                            machine->cpu_type);
    qdev_realize(DEVICE(&m->cpu), NULL, &error_fatal);
    qemu_register_reset(main_cpu_reset, &m->cpu);

    /* RAM inside a container whose unbacked reads return 0 (RAM sizing) */
    memory_region_init_io(&m->ramio, OBJECT(machine), &ramio_ops, &m->ramio,
                          "ram", RAM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x0, &m->ramio);
    memory_region_add_subregion(&m->ramio, 0, machine->ram);

    /* A31-half physical decode -> low 24 bits (24-bit tagged pointers) */
    memory_region_init_io(&m->ramio_a31, OBJECT(machine),
                          &maciifx_a31_ops, m, "maciifx.ram-a31",
                          0x80000000);
    memory_region_add_subregion_overlap(get_system_memory(), 0x80000000,
                                        &m->ramio_a31, -2);

    /* I/O container + mirror alias */
    memory_region_init(&m->macio, OBJECT(machine), "mac-io", IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE, &m->macio);

    memory_region_init_io(&m->macio_alias, OBJECT(machine), &macio_alias_ops,
                          &m->macio, "mac-io.alias", IO_SIZE - IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE + IO_SLICE,
                                &m->macio_alias);

    /* catch-all bus-error trace region behind the devices */
    memory_region_init_io(&m->iotrace, OBJECT(machine), &maciifx_iotrace_ops,
                          m, "mac-io.trace", IO_SLICE);
    memory_region_add_subregion_overlap(&m->macio, 0, &m->iotrace, -1);

    /* OSS interrupt controller */
    m->last_ipl = 0;
    memory_region_init_io(&m->ossmem, OBJECT(machine), &maciifx_oss_ops, m,
                          "oss", VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, OSS_OFS, &m->ossmem);

    /* VIA1 (+ full mirror at +0x40000) */
    object_initialize_child(OBJECT(machine), "via1", &m->via1,
                            TYPE_MOS6522_MACIIFX);
    qdev_prop_set_uint64(DEVICE(&m->via1), "frequency", VIA_TIMER_FREQ);
    qdev_prop_set_uint8(DEVICE(&m->via1), "pins-a", MACIIFX_VIA1_PINS_A);
    sysbus = SYS_BUS_DEVICE(&m->via1);
    sysbus_realize(sysbus, &error_fatal);
    {
        struct tm tm;

        qemu_get_timedate(&tm, 0);
        m->via1.tick_offset = (uint32_t)mktimegm(&tm) + RTC_OFFSET;
    }
    /* PRAM stays all-zero = invalid: the ROM rebuilds it (LC475 note) */
    m->via1.machine = m;
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(maciifx_via1_irq, m, 0));
    memory_region_init_io(&m->via1mem, OBJECT(machine), &maciifx_via1_ops,
                          &m->via1, "via1", VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, VIA1_OFS, &m->via1mem);
    memory_region_init_alias(&m->via1mem_mirror, OBJECT(machine),
                             "via1-mirror", &m->via1mem, 0, VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, VIA1_MIRROR_OFS,
                                &m->via1mem_mirror);

    /* ADB devices (they sit behind the ISM IOP) */
    {
        BusState *adb_bus = qdev_get_child_bus(DEVICE(&m->via1), "adb.0");

        dev = qdev_new(TYPE_ADB_KEYBOARD);
        qdev_realize_and_unref(dev, adb_bus, &error_fatal);
        dev = qdev_new(TYPE_ADB_MOUSE);
        qdev_realize_and_unref(dev, adb_bus, &error_fatal);

        /*
         * Apple's ISM IOP firmware autopolls the configured ADB devices
         * autonomously; MacOS never sends a Linux-style SET_AUTOPOLL and
         * never falls back to host-driven Talk R0 polling (observed: the
         * ADBReInit sweeps run 50x at boot and then the channel goes
         * quiet).  Drive the bus autopoll timer and post AUTOPOLL
         * messages on the ADB mailbox channel.  The callback MUST be
         * registered: a guest SET_AUTOPOLL would otherwise arm the
         * timer with a NULL callback (segfault on first input).
         */
        adb_register_autopoll_callback(&m->via1.adb_bus,
                                       maciifx_iop_adb_poll, m);
        adb_set_autopoll_mask(&m->via1.adb_bus, 0xffff);
        adb_set_autopoll_enabled(&m->via1.adb_bus, true);
        m->iop_autopoll = true;
    }

    m->sixty_hz_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maciifx_sixty_hz, m);
    timer_mod(m->sixty_hz_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VIA_60HZ_TIMER_PERIOD_NS);
    m->one_second_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, maciifx_one_second,
                                       m);
    timer_mod(m->one_second_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);

    /* SCC (in the SCC IOP bypass window at +0x20) */
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

    /* ESCC IRQs + SCC IOP interrupt all reach OSS source 7 */
    object_initialize_child(OBJECT(machine), "escc_orgate", &m->escc_orgate,
                            TYPE_OR_IRQ);
    object_property_set_int(OBJECT(&m->escc_orgate), "num-lines", 3,
                            &error_fatal);
    dev = DEVICE(&m->escc_orgate);
    qdev_realize(dev, NULL, &error_fatal);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(dev, 0));
    sysbus_connect_irq(sysbus, 1, qdev_get_gpio_in(dev, 1));
    qdev_connect_gpio_out(dev, 0,
                          qemu_allocate_irq(maciifx_scc_irq, m, 0));
    memory_region_add_subregion(&m->macio, SCC_OFS,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->escc),
                                                       0));

    /* SCC IOP host registers below the SCC bypass window */
    m->scc_iop.machine = m;
    m->scc_iop.name = "scc";
    m->scc_iop.irq = qdev_get_gpio_in(DEVICE(&m->escc_orgate), 2);
    m->scc_iop.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maciifx_iop_timer_cb,
                                    &m->scc_iop);
    memory_region_init_io(&m->scc_iop.mem, OBJECT(machine), &maciifx_iop_ops,
                          &m->scc_iop, "scc-iop", 0x20);
    memory_region_add_subregion(&m->macio, SCC_IOP_OFS, &m->scc_iop.mem);

    /* SCSIDMA: NCR5380 core + handshake windows + control registers */
    object_initialize_child(OBJECT(machine), "scsi", &m->scsi, TYPE_NCR5380);
    qdev_prop_set_uint8(DEVICE(&m->scsi), "reg-shift", 4);
    sysbus = SYS_BUS_DEVICE(&m->scsi);
    sysbus_realize(sysbus, &error_fatal);
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(maciifx_scsi_irq, m, 0));
    memory_region_add_subregion(&m->macio, SCSIDMA_OFS,
                                sysbus_mmio_get_region(sysbus, 0));

    memory_region_init_io(&m->scsi_hskd, OBJECT(machine),
                          &maciifx_scsi_hskd_ops, m, "scsidma-hskd", 4);
    memory_region_add_subregion_overlap(&m->macio, SCSIDMA_OFS,
                                        &m->scsi_hskd, 2);
    memory_region_init_io(&m->scsi_hskr, OBJECT(machine),
                          &maciifx_scsi_hskr_ops, m, "scsidma-hskr", 4);
    memory_region_add_subregion_overlap(&m->macio, SCSIDMA_OFS + 0x60,
                                        &m->scsi_hskr, 2);
    memory_region_init_io(&m->scsi_ctrlmem, OBJECT(machine),
                          &maciifx_scsi_ctrl_ops, m, "scsidma-ctrl", 0x180);
    memory_region_add_subregion(&m->macio, SCSIDMA_OFS + 0x80,
                                &m->scsi_ctrlmem);

    scsi_bus_legacy_handle_cmdline(&m->scsi.bus);

    /* ASC (plain ASC on the IIfx) */
    object_initialize_child(OBJECT(machine), "asc", &m->asc, TYPE_ASC);
    qdev_prop_set_uint8(DEVICE(&m->asc), "asctype", ASC_TYPE_ASC);
    if (machine->audiodev) {
        qdev_prop_set_string(DEVICE(&m->asc), "audiodev", machine->audiodev);
    }
    sysbus = SYS_BUS_DEVICE(&m->asc);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, ASC_OFS,
                                sysbus_mmio_get_region(sysbus, 0));
    /*
     * The IIfx decodes the ASC as a 4K device mirrored through its 8K
     * window: the ROM's ASC POST (0x40842026) does its cleanup register
     * writes at base+0x1001.. (a0 already advanced by 0x800, offsets
     * 0x801..) and a bus error there vectors straight into the death
     * nub (d7 code 0x100 | test 0x89).
     */
    memory_region_init_alias(&m->asc_mirror, OBJECT(machine), "asc-mirror",
                             sysbus_mmio_get_region(sysbus, 0), 0, 0x1000);
    memory_region_add_subregion_overlap(&m->macio, ASC_OFS + 0x1000,
                                        &m->asc_mirror, 1);
    sysbus_connect_irq(sysbus, 0,
                       qemu_allocate_irq(maciifx_asc_irq, m, 0));

    /* SWIM floppy controller (behind the ISM IOP) */
    object_initialize_child(OBJECT(machine), "swim", &m->swim, TYPE_SWIM);
    sysbus = SYS_BUS_DEVICE(&m->swim);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, SWIM_OFS,
                                sysbus_mmio_get_region(sysbus, 0));

    /* ISM IOP host registers over the first 0x20 bytes of the window */
    m->swim_iop.machine = m;
    m->swim_iop.name = "ism";
    m->swim_iop.is_ism = true;
    m->swim_iop.irq = qemu_allocate_irq(maciifx_ism_irq, m, 0);
    m->swim_iop.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, maciifx_iop_timer_cb,
                                     &m->swim_iop);
    memory_region_init_io(&m->swim_iop.mem, OBJECT(machine), &maciifx_iop_ops,
                          &m->swim_iop, "ism-iop", 0x20);
    memory_region_add_subregion_overlap(&m->macio, SWIM_IOP_OFS,
                                        &m->swim_iop.mem, 1);

    /* BIU */
    memory_region_init_io(&m->biumem, OBJECT(machine), &maciifx_biu_ops, m,
                          "biu", VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, BIU_OFS, &m->biumem);

    /* EXP0 scratch registers */
    memory_region_init_io(&m->exp0mem, OBJECT(machine), &maciifx_exp0_ops, m,
                          "exp0", VIA_REGION_SIZE);
    memory_region_add_subregion(&m->macio, EXP0_OFS, &m->exp0mem);

    /* NuBus slots 9-E; slot interrupts feed OSS sources 0-5 */
    object_initialize_child(OBJECT(machine), "mac-nubus-bridge",
                            &m->mac_nubus_bridge, TYPE_MAC_NUBUS_BRIDGE);
    sysbus = SYS_BUS_DEVICE(&m->mac_nubus_bridge);
    dev = DEVICE(&m->mac_nubus_bridge);
    qdev_prop_set_uint32(dev, "slot-available-mask",
                         BIT(0x9) | BIT(0xa) | BIT(0xb) | BIT(0xc) |
                         BIT(0xd) | BIT(0xe));
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                MAC_NUBUS_FIRST_SLOT * NUBUS_SUPER_SLOT_SIZE,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_add_subregion(get_system_memory(),
                                NUBUS_SLOT_BASE +
                                MAC_NUBUS_FIRST_SLOT * NUBUS_SLOT_SIZE,
                                sysbus_mmio_get_region(sysbus, 1));
    {
        qemu_irq *slot_irq = qemu_allocate_irqs(maciifx_nubus_irq, m,
                                                MAC_NUBUS_LAST_SLOT + 1);

        for (i = MAC_NUBUS_FIRST_SLOT; i <= MAC_NUBUS_LAST_SLOT; i++) {
            qdev_connect_gpio_out(dev, i, slot_irq[i]);
        }
    }

    /*
     * No onboard video: the IIfx needs a NuBus video card WITH a
     * declaration ROM (the system ROM carries no video driver, unlike
     * the Quadras whose ROM drives pseudo-slot 9 itself).  QEMU's
     * macfb NuBus card has no DeclROM so the Slot Manager cannot see
     * it; use the Radius PrecisionColor 24Xp model instead:
     *   -device radius-24xp,slot=0xe,romfile=RadiusPrecisionColor24XPv2.0.bin
     */
    nubus = NUBUS_BUS(qdev_get_child_bus(dev, "nubus-bus.0"));
    (void)nubus;

    /* ROM */
    memory_region_init_rom(&m->rom, NULL, "maciifx.rom", MACIIFX_ROM_SIZE,
                           &error_abort);
    memory_region_add_subregion(get_system_memory(), MACIIFX_ROM_ADDR,
                                &m->rom);

    memory_region_init_alias(&m->rom_alias, NULL, "maciifx.rom-alias",
                             &m->rom, 0, MACIIFX_ROM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x40000000,
                                &m->rom_alias);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, MACIIFX_ROM_ADDR,
                                        MACIIFX_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }

    if (!qtest_enabled()) {
        if (bios_size <= 0 || bios_size > MACIIFX_ROM_SIZE) {
            error_report("could not load MacROM '%s'", bios_name);
            exit(1);
        }

        cs = CPU(&m->cpu);
        ptr = rom_ptr(MACIIFX_ROM_ADDR, bios_size);
        assert(ptr != NULL);
        stl_phys(cs->as, 0, ldl_be_p(ptr));    /* reset initial SP */
        /*
         * Reset initial PC: dword@4 of this ROM is the ABSOLUTE entry
         * address (0x4080002A) — unlike e.g. the IIsi ROM where it is
         * an offset from the ROM base.  Accept both forms.
         */
        {
            uint32_t entry = ldl_be_p(ptr + 4);

            if (entry < MACIIFX_ROM_SIZE) {
                entry += MACIIFX_ROM_ADDR;
            }
            stl_phys(cs->as, 4, entry);
        }
    }
}

static void maciifx_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68030"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Macintosh IIfx";
    mc->init = maciifx_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->block_default_type = IF_SCSI;
    mc->default_ram_size = 8 * MiB;
    mc->default_ram_id = "maciifx.ram";
    machine_add_audiodev_property(mc);
}

static const TypeInfo maciifx_machine_typeinfo[] = {
    {
        .name       = TYPE_MOS6522_MACIIFX,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522MacIIfxState),
        .instance_init = mos6522_maciifx_init,
        .class_init = mos6522_maciifx_class_init,
    },
    {
        .name       = TYPE_MACIIFX_MACHINE,
        .parent     = TYPE_MACHINE,
        .instance_size = sizeof(MacIIfxMachineState),
        .class_init = maciifx_machine_class_init,
    },
};

DEFINE_TYPES(maciifx_machine_typeinfo)
