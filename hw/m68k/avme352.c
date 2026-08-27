/*
 * AVAL DATA Corporation AVME-352 -- 6 channel intelligent serial I/O card
 * for the VMEbus.
 *
 * This is a reverse engineered model of the card, derived from the
 * "AVME-352 Ver 1.3" boot ROM (ROM Type = 0001, (c) AVAL-DATA Corp. 1994).
 * It models enough of the card for the on-board firmware to run so that the
 * VMEbus-side programming interface can be studied.
 *
 * Local (on-card 68020) memory map as used by the ROM:
 *
 *   0x00000000  512 KiB  local DRAM (vectors, shadowed ROM, buffers)
 *   0x00C00000  128 KiB  boot ROM
 *   0x00F00000     4     SCC #0   (ch B ctrl/data, ch A ctrl/data)
 *   0x00F00004     4     SCC #1
 *   0x00F00008     4     SCC #2
 *   0x00F0000C     4     board glue (straps, LEDs, Z8536 CIO tick)
 *   0x00F20000    12     doorbell enable      (3 banks x 6 channels)
 *   0x00F30000    12     doorbell acknowledge (3 banks x 6 channels)
 *   0x00FF0000    4 KiB  dual ported RAM, also visible from the VMEbus
 *
 * See contrib/avme352/AVME-352-programming.md for what the ROM does with
 * all of this and how the card looks from the VMEbus side.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/char/escc.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/runstate.h"
#include "system/reset.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/datadir.h"
#include "target/m68k/cpu.h"
#include "hw/vme/vme.h"

#define AVME352_RAM_BASE    0x00000000
#define AVME352_RAM_SIZE    (512 * KiB)
#define AVME352_ROM_BASE    0x00C00000
#define AVME352_ROM_SIZE    (128 * KiB)
#define AVME352_SCC_BASE    0x00F00000
#define AVME352_GLUE_BASE   0x00F0000C
#define AVME352_DBSET_BASE  0x00F20000
#define AVME352_DBCLR_BASE  0x00F30000
#define AVME352_DPRAM_BASE  0x00FF0000
#define AVME352_DPRAM_SIZE  0x1000

/*
 * The card presents the same 4 KiB of dual ported RAM to the VMEbus at a
 * jumper selected A24 base address.  There is no VMEbus in QEMU, so put the
 * slave window somewhere out of the way in the 68020's address space and
 * drive it from a debugger or a test harness.
 */
#define AVME352_VME_BASE    0x10000000

/* The last four bytes of the dual ported RAM are the host mailbox. */
#define AVME352_MBOX_OFF    0x0FFC

#define AVME352_NCHAN       6
#define AVME352_NSCC        3
#define AVME352_NBANK       3

/* Interrupt vectors handed to the CPU. */
#define VEC_DOORBELL0       0xC0    /* + channel */
#define VEC_DOORBELL1       0xC8    /* + channel */
#define VEC_DOORBELL2       0xD0    /* + channel */
#define VEC_TIMER           0xA0

#define TYPE_AVME352_GLUE "avme352-glue"
OBJECT_DECLARE_SIMPLE_TYPE(AVME352GlueState, AVME352_GLUE)

struct AVME352GlueState {
    SysBusDevice parent_obj;

    MemoryRegion glue_mr;
    MemoryRegion dbset_mr;
    MemoryRegion dbclr_mr;
    MemoryRegion mbox_mr;
    MemoryRegion vme_mr;

    M68kCPU *cpu;
    ESCCState *scc[AVME352_NSCC];

    /* configuration */
    uint8_t config;             /* 0xF0000D, read only switches/straps */
    /*
     * Interrupt levels.  The firmware's own critical sections give these
     * away: it masks with "move.w #$2300,sr" around the code that primes
     * the transmitter, which only protects anything if the SCCs sit at
     * IPL 3, and the command doorbell epilogue drops to "#$2100".
     */
    uint32_t scc_level;
    uint32_t timer_level;
    uint32_t db_level[AVME352_NBANK];
    uint32_t tick_ns;

    /* state */
    uint8_t leds;               /* 0xF0000E */
    uint8_t doorbell[AVME352_NBANK];    /* pending, one bit per channel */
    uint8_t db_enable[AVME352_NBANK];   /* enabled, one bit per channel */
    bool scc_irq[AVME352_NSCC];
    bool timer_ip;

    /* Z8536 style counter/timer behind 0xF0000F */
    uint8_t ctc_ptr;
    bool ctc_ptr_valid;
    bool ctc_running;
    bool ctc_ie;
    QEMUTimer *timer;

    /* host mailbox, 0xFF0FFC..0xFF0FFF */
    uint8_t mbox[4];
    qemu_irq vme_irq;
    uint8_t *dpram;
};

/* ------------------------------------------------------------------ */
/* interrupt delivery                                                  */

/*
 * Work out the vector an SCC would put on the bus.  The firmware programs
 * WR2 (the interrupt vector) through channel A and sets WR9 bit 0 (VIS,
 * "vector includes status") with status low, so the delivered vector is
 * the WR2 value with bits 3:1 replaced by a code identifying the source:
 *
 *   0 ch B Tx empty      4 ch A Tx empty
 *   1 ch B ext/status    5 ch A ext/status
 *   2 ch B Rx available  6 ch A Rx available
 *   3 ch B special Rx    7 ch A special Rx
 *
 * Derive the code from what is actually pending rather than from the RR2
 * shadow the ESCC model keeps: that shadow is overwritten with the "no
 * interrupt pending" code as soon as one of the two channels is served,
 * even while the other still has a request up.
 */
#define SCC_WR1_TXIE        0x02
#define SCC_WR1_RXMODE      0x18
#define SCC_WR15_BRKIE      0x80
#define SCC_RR0_BRK         0x80

static int scc_chn_status(ESCCChannelState *c, bool is_a)
{
    if (c->rxint && (c->wregs[1] & SCC_WR1_RXMODE)) {
        return is_a ? 6 : 2;
    }
    if (c->txint && (c->wregs[1] & SCC_WR1_TXIE)) {
        return is_a ? 4 : 0;
    }
    if ((c->wregs[15] & SCC_WR15_BRKIE) && (c->rregs[0] & SCC_RR0_BRK)) {
        return is_a ? 5 : 1;
    }
    return -1;
}

static int avme352_scc_vector(ESCCState *scc)
{
    /* chn[1] is channel A, chn[0] is channel B; A wins on the SCC. */
    uint8_t base = scc->chn[1].wregs[2] ?: scc->chn[0].wregs[2];
    int status = scc_chn_status(&scc->chn[1], true);

    if (status < 0) {
        status = scc_chn_status(&scc->chn[0], false);
    }
    if (status < 0) {
        status = 3;     /* nothing we model: "special receive", ch B */
    }
    return (base & 0xf1) | (status << 1);
}

static void avme352_update_irq(AVME352GlueState *s)
{
    int best_level = 0;
    int best_vector = 0;
    int i, ch;

    /* Highest numbered level wins; within a level, lowest channel wins. */
    for (i = 0; i < AVME352_NSCC; i++) {
        if (s->scc_irq[i] && (int)s->scc_level > best_level) {
            best_level = s->scc_level;
            best_vector = avme352_scc_vector(s->scc[i]);
        }
    }

    if (s->timer_ip && s->ctc_ie && (int)s->timer_level > best_level) {
        best_level = s->timer_level;
        best_vector = VEC_TIMER;
    }

    for (i = 0; i < AVME352_NBANK; i++) {
        static const int base[AVME352_NBANK] = {
            VEC_DOORBELL0, VEC_DOORBELL1, VEC_DOORBELL2
        };
        if (!s->doorbell[i] || (int)s->db_level[i] <= best_level) {
            continue;
        }
        for (ch = 0; ch < AVME352_NCHAN; ch++) {
            if (s->doorbell[i] & (1u << ch)) {
                best_level = s->db_level[i];
                best_vector = base[i] + ch;
                break;
            }
        }
    }

    m68k_set_irq_level(s->cpu, best_level, best_vector);
}

static void avme352_scc_irq(void *opaque, int n, int level)
{
    AVME352GlueState *s = opaque;

    s->scc_irq[n] = level;
    avme352_update_irq(s);
}

/* ------------------------------------------------------------------ */
/* Z8536 CIO style counter/timer at 0xF0000F                           */

/*
 * The ROM only ever touches two registers: 0x01 (master configuration) and
 * 0x0A (counter/timer 1 command and status).  Access is the usual Zilog
 * "write pointer, then write data" dance on a single byte port.  Command
 * 001 in bits 7:5 of the CS register is "clear IP and IUS", i.e. the EOI.
 */
#define CIO_MCC     0x01
#define CIO_CT1CS   0x0A

static void avme352_timer_tick(void *opaque)
{
    AVME352GlueState *s = opaque;

    s->timer_ip = true;
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->tick_ns);
    avme352_update_irq(s);
}

static void avme352_cio_write(AVME352GlueState *s, uint8_t val)
{
    if (!s->ctc_ptr_valid) {
        s->ctc_ptr = val;
        s->ctc_ptr_valid = true;
        return;
    }
    s->ctc_ptr_valid = false;

    switch (s->ctc_ptr) {
    case CIO_MCC:
        /* Bit 6 enables counter/timer 1. */
        s->ctc_ie = !!(val & 0x40);
        break;
    case CIO_CT1CS:
        switch (val >> 5) {
        case 1:             /* clear IP and IUS -- end of interrupt */
            s->timer_ip = false;
            break;
        case 5:             /* clear IP */
            s->timer_ip = false;
            break;
        case 6:             /* set IE */
            s->ctc_ie = true;
            break;
        case 7:             /* clear IE */
            s->ctc_ie = false;
            break;
        default:
            break;
        }
        if (val & 0x06) {   /* gate/trigger -- start counting */
            if (!s->ctc_running) {
                s->ctc_running = true;
                timer_mod(s->timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->tick_ns);
            }
        }
        break;
    default:
        break;
    }
    avme352_update_irq(s);
}

/* ------------------------------------------------------------------ */
/* mailbox / VMEbus interrupt                                          */

/*
 * 0xFF0FFC..0xFF0FFF are not RAM: they are the interrupt mailbox.  The card
 * writes the event code, the requesting channel and a non zero value into
 * the "irq" byte; that asserts the VMEbus interrupt request.  The host
 * clears the irq byte to acknowledge.
 */
static void avme352_mbox_set(AVME352GlueState *s, int idx, uint8_t val)
{
    static const char *name[4] = { "IRQ", "EVENT", "FATAL", "CHAN" };

    s->mbox[idx] = val;
    qemu_log_mask(LOG_TRACE, "avme352: mbox %s <- 0x%02x\n", name[idx], val);
    if (idx == 0) {
        qemu_set_irq(s->vme_irq, !!val);
    }
}

/* ------------------------------------------------------------------ */
/* board glue registers, 0xF0000C..0xF0000F                            */

static uint64_t avme352_glue_read(void *opaque, hwaddr addr, unsigned size)
{
    AVME352GlueState *s = opaque;

    switch (addr) {
    case 0x1:               /* 0xF0000D -- configuration switches */
        return s->config;
    case 0x2:               /* 0xF0000E -- front panel LEDs */
        return s->leds;
    case 0x3:               /* 0xF0000F -- counter/timer */
        s->ctc_ptr_valid = false;
        return 0;
    default:
        return 0xff;
    }
}

static void avme352_glue_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    AVME352GlueState *s = opaque;

    switch (addr) {
    case 0x2:
        s->leds = val;
        break;
    case 0x3:
        avme352_cio_write(s, val);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps avme352_glue_ops = {
    .read = avme352_glue_read,
    .write = avme352_glue_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/* ------------------------------------------------------------------ */
/* doorbell (software interrupt) registers                             */

/*
 * Three banks of six "doorbell" interrupts.  The requests themselves are
 * raised by the hardware when the VMEbus side touches one of the three
 * per channel handshake locations in the dual ported RAM:
 *
 *   bank 0, vector 0xC0 + ch: VMEbus *read*  of DPRAM 0x0000 + ch
 *   bank 1, vector 0xC8 + ch: VMEbus *write* of DPRAM 0x0008 + ch
 *   bank 2, vector 0xD0 + ch: VMEbus *write* of DPRAM 0x0010 + ch
 *
 * 0xF2000n is the enable register: the firmware writes (channel | 0x80) to
 * arm a channel and (channel) to disarm it.  0xF3000n is the acknowledge
 * register, written with the bare channel number at the top of each
 * service routine.
 */
static void avme352_doorbell_raise(AVME352GlueState *s, int bank, int ch)
{
    if (s->db_enable[bank] & (1u << ch)) {
        s->doorbell[bank] |= 1u << ch;
        avme352_update_irq(s);
    }
}

static void avme352_dbset_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    AVME352GlueState *s = opaque;
    int bank = addr / 4;
    int ch = val & 0x07;

    if (bank >= AVME352_NBANK || ch >= AVME352_NCHAN) {
        return;
    }
    if (val & 0x80) {
        s->db_enable[bank] |= 1u << ch;
    } else {
        s->db_enable[bank] &= ~(1u << ch);
        s->doorbell[bank] &= ~(1u << ch);
    }
    avme352_update_irq(s);
}

static void avme352_dbclr_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    AVME352GlueState *s = opaque;
    int bank = addr / 4;
    int ch = val & 0x07;

    if (bank >= AVME352_NBANK || ch >= AVME352_NCHAN) {
        return;
    }
    s->doorbell[bank] &= ~(1u << ch);
    avme352_update_irq(s);
}

static uint64_t avme352_dbset_read(void *opaque, hwaddr addr, unsigned size)
{
    AVME352GlueState *s = opaque;
    int bank = addr / 4;

    return bank < AVME352_NBANK ? s->db_enable[bank] : 0xff;
}

static uint64_t avme352_dbclr_read(void *opaque, hwaddr addr, unsigned size)
{
    AVME352GlueState *s = opaque;
    int bank = addr / 4;

    return bank < AVME352_NBANK ? s->doorbell[bank] : 0xff;
}

static const MemoryRegionOps avme352_dbset_ops = {
    .read = avme352_dbset_read,
    .write = avme352_dbset_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static const MemoryRegionOps avme352_dbclr_ops = {
    .read = avme352_dbclr_read,
    .write = avme352_dbclr_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/* ------------------------------------------------------------------ */
/* the VMEbus side of the dual ported RAM                              */

/*
 * On the card the dual ported RAM really has two ports, so a write made by
 * the on board 68020 does not look the same to the address decoder as a
 * write made by a VMEbus master.  Model the VMEbus port as a separate
 * window that shares the storage but raises the doorbells.
 */
#define VME_MBOX_IRQ    0x0FFC

static uint64_t avme352_vme_read(void *opaque, hwaddr addr, unsigned size)
{
    AVME352GlueState *s = opaque;
    uint8_t ret;

    if (addr >= VME_MBOX_IRQ) {
        ret = s->mbox[addr - VME_MBOX_IRQ];
    } else {
        ret = s->dpram[addr];
    }

    if (addr < AVME352_NCHAN) {
        /* reading the single byte receive port asks for the next byte */
        avme352_doorbell_raise(s, 0, addr);
    }
    return ret;
}

static void avme352_vme_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    AVME352GlueState *s = opaque;

    if (addr >= VME_MBOX_IRQ) {
        avme352_mbox_set(s, addr - VME_MBOX_IRQ, val);
        return;
    }

    s->dpram[addr] = val;

    if (addr >= 0x08 && addr < 0x08 + AVME352_NCHAN) {
        /* single byte transmit port */
        avme352_doorbell_raise(s, 1, addr - 0x08);
    } else if (addr >= 0x10 && addr < 0x10 + AVME352_NCHAN) {
        /* command byte */
        avme352_doorbell_raise(s, 2, addr - 0x10);
    }
}

static const MemoryRegionOps avme352_vme_ops = {
    .read = avme352_vme_read,
    .write = avme352_vme_write,
    .endianness = DEVICE_BIG_ENDIAN,
    /*
     * The host driver reaches the command/parameter block with 16- and
     * 32-bit iowrite/ioread (avme352_cmd_words()); model the port a byte
     * at a time and let the core split wider big-endian accesses into the
     * byte transfers the doorbell logic expects.  The doorbell trigger
     * offsets (rx port 0x00-05, tx port 0x08-0d, command 0x10-15) are only
     * ever byte-accessed by the driver, so splitting is safe.
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

/* ------------------------------------------------------------------ */
/* host mailbox, top four bytes of the dual ported RAM                 */

static uint64_t avme352_mbox_read(void *opaque, hwaddr addr, unsigned size)
{
    AVME352GlueState *s = opaque;

    return s->mbox[addr & 3];
}

static void avme352_mbox_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    avme352_mbox_set(opaque, addr & 3, val);
}

static const MemoryRegionOps avme352_mbox_ops = {
    .read = avme352_mbox_read,
    .write = avme352_mbox_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/* ------------------------------------------------------------------ */

static void avme352_glue_reset(DeviceState *dev)
{
    AVME352GlueState *s = AVME352_GLUE(dev);

    s->leds = 0;
    s->ctc_ptr_valid = false;
    s->ctc_running = false;
    s->ctc_ie = false;
    s->timer_ip = false;
    memset(s->doorbell, 0, sizeof(s->doorbell));
    memset(s->db_enable, 0, sizeof(s->db_enable));
    memset(s->scc_irq, 0, sizeof(s->scc_irq));
    memset(s->mbox, 0, sizeof(s->mbox));
    timer_del(s->timer);
}

static void avme352_glue_realize(DeviceState *dev, Error **errp)
{
    AVME352GlueState *s = AVME352_GLUE(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, avme352_timer_tick, s);
}

static void avme352_glue_init(Object *obj)
{
    AVME352GlueState *s = AVME352_GLUE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->glue_mr, obj, &avme352_glue_ops, s,
                          "avme352.glue", 4);
    memory_region_init_io(&s->dbset_mr, obj, &avme352_dbset_ops, s,
                          "avme352.doorbell-set", 4 * AVME352_NBANK);
    memory_region_init_io(&s->dbclr_mr, obj, &avme352_dbclr_ops, s,
                          "avme352.doorbell-clear", 4 * AVME352_NBANK);
    memory_region_init_io(&s->mbox_mr, obj, &avme352_mbox_ops, s,
                          "avme352.mailbox", 4);
    memory_region_init_io(&s->vme_mr, obj, &avme352_vme_ops, s,
                          "avme352.vme-window", AVME352_DPRAM_SIZE);

    sysbus_init_mmio(sbd, &s->glue_mr);
    sysbus_init_mmio(sbd, &s->dbset_mr);
    sysbus_init_mmio(sbd, &s->dbclr_mr);
    sysbus_init_mmio(sbd, &s->mbox_mr);
    sysbus_init_mmio(sbd, &s->vme_mr);

    qdev_init_gpio_in(DEVICE(obj), avme352_scc_irq, AVME352_NSCC);
    qdev_init_gpio_out_named(DEVICE(obj), &s->vme_irq, "vme-irq", 1);
}

static const Property avme352_glue_props[] = {
    DEFINE_PROP_UINT8("config", AVME352GlueState, config, 0x00),
    DEFINE_PROP_UINT32("scc-level", AVME352GlueState, scc_level, 3),
    DEFINE_PROP_UINT32("timer-level", AVME352GlueState, timer_level, 6),
    DEFINE_PROP_UINT32("db0-level", AVME352GlueState, db_level[0], 2),
    DEFINE_PROP_UINT32("db1-level", AVME352GlueState, db_level[1], 2),
    DEFINE_PROP_UINT32("db2-level", AVME352GlueState, db_level[2], 1),
    DEFINE_PROP_UINT32("tick-ns", AVME352GlueState, tick_ns, 10000000),
};

static void avme352_glue_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = avme352_glue_realize;
    device_class_set_legacy_reset(dc, avme352_glue_reset);
    device_class_set_props(dc, avme352_glue_props);
}

static const TypeInfo avme352_glue_info = {
    .name = TYPE_AVME352_GLUE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AVME352GlueState),
    .instance_init = avme352_glue_init,
    .class_init = avme352_glue_class_init,
};

static void avme352_register_types(void)
{
    type_register_static(&avme352_glue_info);
}

type_init(avme352_register_types)

/* ------------------------------------------------------------------ */
/* machine                                                             */

/*
 * The ROM dumps that are floating around have the two bytes of every 16 bit
 * word swapped.  Detect that from the copyright banner at offset 0x10 and
 * undo it so that either form of the image can be used.
 */
static void avme352_fixup_rom(uint8_t *rom, size_t len)
{
    size_t i;

    if (!memcmp(rom + 0x10, "Copyright", 9)) {
        return;
    }
    if (memcmp(rom + 0x10, "oCypirhg", 8)) {
        warn_report("avme352: unrecognised ROM image, loading verbatim");
        return;
    }
    for (i = 0; i + 1 < len; i += 2) {
        uint8_t t = rom[i];
        rom[i] = rom[i + 1];
        rom[i + 1] = t;
    }
}

/* ------------------------------------------------------------------ */
/* the card as a VMEbus device                                         */

/*
 * The whole card, packaged as one VME device.  It carries its own 68020
 * running the boot ROM in a private address space (the on-card local bus:
 * RAM, ROM, the SCCs and the glue), and presents to the backplane only the
 * dual ported RAM window and one interrupt request - exactly what a
 * processor board on the same backplane sees.
 */
#define TYPE_AVME352 "avme352"
OBJECT_DECLARE_SIMPLE_TYPE(AVME352State, AVME352)

struct AVME352State {
    VMEDevice parent_obj;

    MemoryRegion container;     /* the on-card 68020's whole address space */
    MemoryRegion ram;
    MemoryRegion dpram;
    MemoryRegion rom;

    M68kCPU *cpu;
    DeviceState *glue;

    char *romfile;
    uint32_t serial_base;       /* first -serial index for the 6 channels */
    uint32_t boot_sp, boot_pc;
};

static void avme352_vme_irq(void *opaque, int n, int level)
{
    /* The glue asserts this when it writes the mailbox interrupt byte. */
    vme_device_set_irq(VME_DEVICE(opaque), level);
}

static void avme352_cpu_reset(void *opaque)
{
    AVME352State *s = opaque;
    CPUM68KState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    /*
     * The 68020 fetches its initial stack pointer and program counter from
     * the boot ROM, which the card aliases over address zero for the first
     * few bus cycles.  Rather than model that overlay, prime the CPU
     * straight out of the ROM image.
     */
    env->aregs[7] = s->boot_sp;
    env->pc = s->boot_pc;
    env->sr = 0x2700;
    m68k_switch_sp(env);
}

static bool avme352_load_rom(AVME352State *s, Error **errp)
{
    const char *fw = s->romfile ?: "avme352.bin";
    uint8_t *rombuf = NULL;
    gsize romlen;

    if (!g_file_get_contents(fw, (gchar **)&rombuf, &romlen, NULL)) {
        char *path = qemu_find_file(QEMU_FILE_TYPE_BIOS, fw);
        bool ok = path &&
                  g_file_get_contents(path, (gchar **)&rombuf, &romlen, NULL);
        g_free(path);
        if (!ok) {
            error_setg(errp, "avme352: could not read ROM image '%s'", fw);
            return false;
        }
    }
    if (romlen > AVME352_ROM_SIZE) {
        romlen = AVME352_ROM_SIZE;
    }
    avme352_fixup_rom(rombuf, romlen);
    s->boot_sp = ldl_be_p(rombuf);
    s->boot_pc = ldl_be_p(rombuf + 4);
    memcpy(memory_region_get_ram_ptr(&s->rom), rombuf, romlen);
    g_free(rombuf);
    return true;
}

static void avme352_realize(DeviceState *dev, Error **errp)
{
    AVME352State *s = AVME352(dev);
    Object *o = OBJECT(dev);
    SysBusDevice *gsbd;
    Object *cpuobj;
    int i;

    /* the on-card 68020's private local bus */
    memory_region_init(&s->container, o, "avme352.local", 0x100000000ULL);

    memory_region_init_ram(&s->ram, o, "avme352.ram", AVME352_RAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(&s->container, AVME352_RAM_BASE, &s->ram);

    memory_region_init_ram(&s->dpram, o, "avme352.dpram", AVME352_DPRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(&s->container, AVME352_DPRAM_BASE, &s->dpram);

    memory_region_init_rom(&s->rom, o, "avme352.rom", AVME352_ROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(&s->container, AVME352_ROM_BASE, &s->rom);

    if (!avme352_load_rom(s, errp)) {
        return;
    }

    /* the card's CPU runs in that private space, not the system bus */
    cpuobj = object_new(M68K_CPU_TYPE_NAME("m68020"));
    object_property_add_child(o, "cpu", cpuobj);
    object_property_set_link(cpuobj, "memory", OBJECT(&s->container),
                             &error_abort);
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    s->cpu = M68K_CPU(cpuobj);

    /* board glue: doorbells, timer, mailbox and the two DPRAM ports */
    s->glue = qdev_new(TYPE_AVME352_GLUE);
    object_property_add_child(o, "glue", OBJECT(s->glue));
    AVME352_GLUE(s->glue)->cpu = s->cpu;
    AVME352_GLUE(s->glue)->dpram = memory_region_get_ram_ptr(&s->dpram);

    for (i = 0; i < AVME352_NSCC; i++) {
        DeviceState *scc = qdev_new(TYPE_ESCC);
        char name[16];

        qdev_prop_set_uint32(scc, "disabled", 0);
        qdev_prop_set_uint32(scc, "frequency", 4915200);
        qdev_prop_set_uint32(scc, "it_shift", 0);
        qdev_prop_set_bit(scc, "bit_swap", false);
        qdev_prop_set_chr(scc, "chrA", serial_hd(s->serial_base + i * 2));
        qdev_prop_set_chr(scc, "chrB", serial_hd(s->serial_base + i * 2 + 1));
        qdev_prop_set_uint32(scc, "chnAtype", escc_serial);
        qdev_prop_set_uint32(scc, "chnBtype", escc_serial);
        snprintf(name, sizeof(name), "scc%d", i);
        object_property_add_child(o, name, OBJECT(scc));
        /* Both channels of a chip share one request line. */
        sysbus_connect_irq(SYS_BUS_DEVICE(scc), 0, qdev_get_gpio_in(s->glue, i));
        sysbus_connect_irq(SYS_BUS_DEVICE(scc), 1, qdev_get_gpio_in(s->glue, i));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(scc), &error_fatal);
        memory_region_add_subregion(&s->container, AVME352_SCC_BASE + i * 4,
                                    sysbus_mmio_get_region(SYS_BUS_DEVICE(scc),
                                                           0));
        AVME352_GLUE(s->glue)->scc[i] = ESCC(scc);
    }

    /* the card raises the VMEbus interrupt through the bus */
    qdev_connect_gpio_out_named(s->glue, "vme-irq", 0,
                                qemu_allocate_irq(avme352_vme_irq, s, 0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->glue), &error_fatal);

    gsbd = SYS_BUS_DEVICE(s->glue);
    /* glue, doorbell and mailbox live on the card's local bus */
    memory_region_add_subregion(&s->container, AVME352_GLUE_BASE,
                                sysbus_mmio_get_region(gsbd, 0));
    memory_region_add_subregion(&s->container, AVME352_DBSET_BASE,
                                sysbus_mmio_get_region(gsbd, 1));
    memory_region_add_subregion(&s->container, AVME352_DBCLR_BASE,
                                sysbus_mmio_get_region(gsbd, 2));
    memory_region_add_subregion_overlap(&s->container,
                                        AVME352_DPRAM_BASE + AVME352_MBOX_OFF,
                                        sysbus_mmio_get_region(gsbd, 3), 1);
    /* the fifth region is the VMEbus port of the DPRAM: hand it to the bus */
    vme_device_map(VME_DEVICE(dev), sysbus_mmio_get_region(gsbd, 4));

    qemu_register_reset(avme352_cpu_reset, s);
}

static const Property avme352_props[] = {
    DEFINE_PROP_STRING("rom", AVME352State, romfile),
    DEFINE_PROP_UINT32("serial-base", AVME352State, serial_base, 0),
};

static void avme352_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "AVAL DATA AVME-352 6-channel VMEbus serial I/O card";
    dc->realize = avme352_realize;
    device_class_set_props(dc, avme352_props);
}

static const TypeInfo avme352_info = {
    .name = TYPE_AVME352,
    .parent = TYPE_VME_DEVICE,
    .instance_size = sizeof(AVME352State),
    .class_init = avme352_class_init,
};

static void avme352_card_register_types(void)
{
    type_register_static(&avme352_info);
}

type_init(avme352_card_register_types)

/* ------------------------------------------------------------------ */
/* stand-alone machine, for studying the card firmware on its own       */

/*
 * The card is normally a device on some processor board's VMEbus (see the
 * mvme147).  This little machine exists only to run the card's firmware by
 * itself: a bare VME host bridge with the slave window parked out of the
 * way, and the card's own 68020 as the only CPU.  Drive the DPRAM window
 * from a debugger or contrib/avme352/vmehost.py.
 */
static void avme352_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *bridge;
    DeviceState *card;
    VMEBus *bus;

    bridge = qdev_new(TYPE_VME_BRIDGE);
    object_property_add_child(OBJECT(machine), "vme", OBJECT(bridge));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(bridge), &error_fatal);
    /* park the slave window somewhere the harness can reach it */
    sysbus_mmio_map(SYS_BUS_DEVICE(bridge), 0, AVME352_VME_BASE);
    bus = vme_bridge_get_bus(VME_BRIDGE(bridge));

    card = qdev_new(TYPE_AVME352);
    if (machine->firmware) {
        qdev_prop_set_string(card, "rom", machine->firmware);
    }
    /* the harness pokes the window at AVME352_VME_BASE, i.e. VME base 0 */
    qdev_prop_set_uint64(card, "vme-base", 0);
    qdev_realize_and_unref(card, &bus->parent_obj, &error_fatal);

    /* nothing else lives in the host address space */
    (void)sysmem;
}

static void avme352_machine_init(MachineClass *mc)
{
    mc->desc = "AVAL DATA AVME-352 VMEbus serial I/O card";
    mc->init = avme352_init;
    mc->max_cpus = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

DEFINE_MACHINE("avme352", avme352_machine_init)
