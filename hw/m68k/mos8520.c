/*
 * MOS 8520 CIA (Complex Interface Adapter), as used in the Amiga line.
 *
 * The 8520 is the 6526 CIA variant Commodore used in the Amiga: same
 * register set, but the TOD clock is a plain 24-bit binary counter
 * rather than a BCD hours/minutes/seconds clock.
 *
 * On the Amiga the CIA registers are spaced 0x100 apart (A11-A8 select
 * the register), so the device occupies a 4KiB window.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/m68k/mos8520.h"
#include "migration/vmstate.h"

#define CIA_PRA     0
#define CIA_PRB     1
#define CIA_DDRA    2
#define CIA_DDRB    3
#define CIA_TALO    4
#define CIA_TAHI    5
#define CIA_TBLO    6
#define CIA_TBHI    7
#define CIA_TODLO   8
#define CIA_TODMID  9
#define CIA_TODHI   10
#define CIA_SDR     12
#define CIA_ICR     13
#define CIA_CRA     14
#define CIA_CRB     15

#define ICR_TA      0x01
#define ICR_TB      0x02
#define ICR_ALRM    0x04
#define ICR_SP      0x08
#define ICR_FLG     0x10

#define CR_START    0x01
#define CR_OUTMODE  0x04
#define CR_RUNMODE  0x08    /* 1 = one-shot */
#define CR_LOAD     0x10
#define CRA_INMODE  0x20    /* 1 = count CNT pulses */
#define CRA_SPMODE  0x40    /* 1 = SP is output */
#define CRB_INMODE_MASK 0x60
#define CRB_INMODE_TA   0x40    /* count timer A underflows */
#define CRB_ALARM   0x80    /* TOD writes set the alarm */

#define TOD_MASK    0xffffff

static void mos8520_update_irq(MOS8520State *s)
{
    qemu_set_irq(s->irq, (s->icr_status & s->icr_mask & 0x1f) != 0);
}

static void mos8520_post_irq(MOS8520State *s, uint8_t bits)
{
    s->icr_status |= bits;
    mos8520_update_irq(s);
}

/* effective pin level: driven value on outputs, pulled-up input otherwise */
static uint8_t mos8520_port_pins(uint8_t pr, uint8_t ddr, uint8_t input)
{
    return (pr & ddr) | (input & ~ddr);
}

static void mos8520_port_update(MOS8520State *s, int port)
{
    uint8_t pins;
    qemu_irq *out;
    int i;

    if (port == 0) {
        pins = mos8520_port_pins(s->pra, s->ddra, s->input_a);
        out = s->port_a_out;
    } else {
        pins = mos8520_port_pins(s->prb, s->ddrb, s->input_b);
        out = s->port_b_out;
    }
    for (i = 0; i < 8; i++) {
        qemu_set_irq(out[i], (pins >> i) & 1);
    }
}

/* --- interval timers --- */

static void mos8520_ta_hit(void *opaque)
{
    MOS8520State *s = opaque;

    mos8520_post_irq(s, ICR_TA);
    if (s->cra & CR_RUNMODE) {
        s->cra &= ~CR_START;
        ptimer_transaction_begin(s->timer_a);
        ptimer_stop(s->timer_a);
        ptimer_transaction_commit(s->timer_a);
    }
    /* timer B chained off timer A underflows */
    if ((s->crb & CRB_INMODE_MASK) == CRB_INMODE_TA && (s->crb & CR_START)) {
        if (s->tb_count-- == 0) {
            s->tb_count = s->tb_latch;
            mos8520_post_irq(s, ICR_TB);
            if (s->crb & CR_RUNMODE) {
                s->crb &= ~CR_START;
            }
        }
    }
}

static void mos8520_tb_hit(void *opaque)
{
    MOS8520State *s = opaque;

    mos8520_post_irq(s, ICR_TB);
    if (s->crb & CR_RUNMODE) {
        s->crb &= ~CR_START;
        ptimer_transaction_begin(s->timer_b);
        ptimer_stop(s->timer_b);
        ptimer_transaction_commit(s->timer_b);
    }
}

static uint64_t timer_limit(uint16_t latch)
{
    /* period is latch+1 input clocks; avoid a zero limit */
    return latch ? latch : 0x10000;
}

static void mos8520_timer_write_cr(MOS8520State *s, ptimer_state *t,
                                   uint16_t latch, uint8_t val, uint8_t old)
{
    ptimer_transaction_begin(t);
    if (val & CR_LOAD) {
        ptimer_set_limit(t, timer_limit(latch), 1);
    }
    if ((val & CR_START) && !(old & CR_START)) {
        ptimer_run(t, 0);
    } else if (!(val & CR_START)) {
        ptimer_stop(t);
    }
    ptimer_transaction_commit(t);
}

/* --- TOD --- */

static uint32_t mos8520_tod_now(MOS8520State *s)
{
    int64_t now;

    if (!s->tod_running) {
        return s->tod_base;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return (s->tod_base +
            muldiv64(now - s->tod_base_ns, s->tod_hz, NANOSECONDS_PER_SECOND))
           & TOD_MASK;
}

static void mos8520_alarm_rearm(MOS8520State *s)
{
    uint32_t delta;
    int64_t ns;

    timer_del(&s->alarm_timer);
    if (!s->tod_running || !s->tod_hz) {
        return;
    }
    delta = (s->tod_alarm - mos8520_tod_now(s)) & TOD_MASK;
    if (delta == 0) {
        delta = TOD_MASK + 1;
    }
    ns = muldiv64(delta, NANOSECONDS_PER_SECOND, s->tod_hz);
    timer_mod(&s->alarm_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static void mos8520_alarm_hit(void *opaque)
{
    MOS8520State *s = opaque;

    mos8520_post_irq(s, ICR_ALRM);
    mos8520_alarm_rearm(s);
}

static void mos8520_tod_set(MOS8520State *s, int byte, uint8_t val)
{
    uint32_t tod = s->tod_running ? mos8520_tod_now(s) : s->tod_base;

    tod = deposit32(tod, byte * 8, 8, val);
    s->tod_base = tod & TOD_MASK;
    if (byte == 2) {
        /* writing TODHI stops the counter ... */
        s->tod_running = false;
    } else if (byte == 0) {
        /* ... and writing TODLO restarts it */
        s->tod_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->tod_running = true;
    }
    mos8520_alarm_rearm(s);
}

/* --- register access --- */

static uint64_t mos8520_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS8520State *s = opaque;
    uint8_t ret = 0;

    switch ((addr >> 8) & 0xf) {
    case CIA_PRA:
        ret = mos8520_port_pins(s->pra, s->ddra, s->input_a);
        break;
    case CIA_PRB:
        ret = mos8520_port_pins(s->prb, s->ddrb, s->input_b);
        break;
    case CIA_DDRA:
        ret = s->ddra;
        break;
    case CIA_DDRB:
        ret = s->ddrb;
        break;
    case CIA_TALO:
        ret = ptimer_get_count(s->timer_a);
        break;
    case CIA_TAHI:
        ret = ptimer_get_count(s->timer_a) >> 8;
        break;
    case CIA_TBLO:
        if ((s->crb & CRB_INMODE_MASK) == CRB_INMODE_TA) {
            ret = s->tb_count;
        } else {
            ret = ptimer_get_count(s->timer_b);
        }
        break;
    case CIA_TBHI:
        if ((s->crb & CRB_INMODE_MASK) == CRB_INMODE_TA) {
            ret = s->tb_count >> 8;
        } else {
            ret = ptimer_get_count(s->timer_b) >> 8;
        }
        break;
    case CIA_TODLO:
        ret = s->tod_latched ? s->tod_latch : mos8520_tod_now(s);
        s->tod_latched = false;
        break;
    case CIA_TODMID:
        ret = (s->tod_latched ? s->tod_latch : mos8520_tod_now(s)) >> 8;
        break;
    case CIA_TODHI:
        /* reading TODHI latches the whole counter until TODLO is read */
        s->tod_latch = mos8520_tod_now(s);
        s->tod_latched = true;
        ret = s->tod_latch >> 16;
        break;
    case CIA_SDR:
        ret = s->sdr;
        break;
    case CIA_ICR:
        ret = s->icr_status;
        if (s->icr_status & s->icr_mask & 0x1f) {
            ret |= 0x80;
        }
        s->icr_status = 0;
        mos8520_update_irq(s);
        break;
    case CIA_CRA:
        ret = s->cra & ~CR_LOAD;
        break;
    case CIA_CRB:
        ret = s->crb & ~CR_LOAD;
        break;
    default:
        break;
    }
    return ret;
}

static void mos8520_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    MOS8520State *s = opaque;
    uint8_t v = val;
    uint8_t old;

    switch ((addr >> 8) & 0xf) {
    case CIA_PRA:
        s->pra = v;
        mos8520_port_update(s, 0);
        break;
    case CIA_PRB:
        s->prb = v;
        mos8520_port_update(s, 1);
        break;
    case CIA_DDRA:
        s->ddra = v;
        mos8520_port_update(s, 0);
        break;
    case CIA_DDRB:
        s->ddrb = v;
        mos8520_port_update(s, 1);
        break;
    case CIA_TALO:
        s->ta_latch = (s->ta_latch & 0xff00) | v;
        break;
    case CIA_TAHI:
        s->ta_latch = (s->ta_latch & 0x00ff) | (v << 8);
        /* writing TAHI with the timer stopped loads the counter; in
         * one-shot mode it also starts the timer */
        if (!(s->cra & CR_START)) {
            ptimer_transaction_begin(s->timer_a);
            ptimer_set_limit(s->timer_a, timer_limit(s->ta_latch), 1);
            if (s->cra & CR_RUNMODE) {
                s->cra |= CR_START;
                ptimer_run(s->timer_a, 0);
            }
            ptimer_transaction_commit(s->timer_a);
        }
        break;
    case CIA_TBLO:
        s->tb_latch = (s->tb_latch & 0xff00) | v;
        break;
    case CIA_TBHI:
        s->tb_latch = (s->tb_latch & 0x00ff) | (v << 8);
        if (!(s->crb & CR_START)) {
            if ((s->crb & CRB_INMODE_MASK) == CRB_INMODE_TA) {
                s->tb_count = s->tb_latch;
                if (s->crb & CR_RUNMODE) {
                    s->crb |= CR_START;
                }
            } else {
                ptimer_transaction_begin(s->timer_b);
                ptimer_set_limit(s->timer_b, timer_limit(s->tb_latch), 1);
                if (s->crb & CR_RUNMODE) {
                    s->crb |= CR_START;
                    ptimer_run(s->timer_b, 0);
                }
                ptimer_transaction_commit(s->timer_b);
            }
        }
        break;
    case CIA_TODLO:
        if (s->crb & CRB_ALARM) {
            s->tod_alarm = deposit32(s->tod_alarm, 0, 8, v) & TOD_MASK;
            mos8520_alarm_rearm(s);
        } else {
            mos8520_tod_set(s, 0, v);
        }
        break;
    case CIA_TODMID:
        if (s->crb & CRB_ALARM) {
            s->tod_alarm = deposit32(s->tod_alarm, 8, 8, v) & TOD_MASK;
            mos8520_alarm_rearm(s);
        } else {
            mos8520_tod_set(s, 1, v);
        }
        break;
    case CIA_TODHI:
        if (s->crb & CRB_ALARM) {
            s->tod_alarm = deposit32(s->tod_alarm, 16, 8, v) & TOD_MASK;
            mos8520_alarm_rearm(s);
        } else {
            mos8520_tod_set(s, 2, v);
        }
        break;
    case CIA_SDR:
        s->sdr = v;
        if (s->cra & CRA_SPMODE) {
            /* output mode: pretend the byte shifts out instantly */
            mos8520_post_irq(s, ICR_SP);
        }
        break;
    case CIA_ICR:
        if (v & 0x80) {
            s->icr_mask |= v & 0x1f;
        } else {
            s->icr_mask &= ~(v & 0x1f);
        }
        mos8520_update_irq(s);
        break;
    case CIA_CRA:
        old = s->cra;
        s->cra = v & ~CR_LOAD;
        if (v & CRA_INMODE) {
            qemu_log_mask(LOG_UNIMP, "mos8520: timer A CNT mode\n");
        }
        mos8520_timer_write_cr(s, s->timer_a, s->ta_latch, v, old);
        break;
    case CIA_CRB:
        old = s->crb;
        s->crb = v & ~CR_LOAD;
        if ((v & CRB_INMODE_MASK) == CRB_INMODE_TA) {
            if (v & CR_LOAD) {
                s->tb_count = s->tb_latch;
            }
        } else if (v & CRB_INMODE_MASK) {
            qemu_log_mask(LOG_UNIMP, "mos8520: timer B CNT mode\n");
        } else {
            mos8520_timer_write_cr(s, s->timer_b, s->tb_latch, v, old);
        }
        mos8520_alarm_rearm(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps mos8520_ops = {
    .read = mos8520_read,
    .write = mos8520_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

void mos8520_sdr_input(MOS8520State *s, uint8_t data)
{
    s->sdr = data;
    mos8520_post_irq(s, ICR_SP);
}

static void mos8520_flag_in(void *opaque, int n, int level)
{
    MOS8520State *s = opaque;

    if (s->flag_prev && !level) {
        mos8520_post_irq(s, ICR_FLG);
    }
    s->flag_prev = level;
}

static void mos8520_port_in(void *opaque, int n, int level)
{
    MOS8520State *s = opaque;

    if (n < 8) {
        s->input_a = deposit32(s->input_a, n, 1, level);
    } else {
        s->input_b = deposit32(s->input_b, n - 8, 1, level);
    }
}

static void mos8520_reset(DeviceState *dev)
{
    MOS8520State *s = MOS8520(dev);

    s->pra = s->prb = s->ddra = s->ddrb = 0;
    s->input_a = s->input_b = 0xff;
    s->ta_latch = s->tb_latch = 0xffff;
    s->tb_count = 0xffff;
    s->cra = s->crb = 0;
    s->icr_status = s->icr_mask = 0;
    s->sdr = 0;
    s->tod_base = 0;
    s->tod_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->tod_running = true;
    s->tod_latched = false;
    s->tod_alarm = 0;
    s->flag_prev = true;
    timer_del(&s->alarm_timer);
    ptimer_transaction_begin(s->timer_a);
    ptimer_stop(s->timer_a);
    ptimer_set_limit(s->timer_a, 0x10000, 1);
    ptimer_transaction_commit(s->timer_a);
    ptimer_transaction_begin(s->timer_b);
    ptimer_stop(s->timer_b);
    ptimer_set_limit(s->timer_b, 0x10000, 1);
    ptimer_transaction_commit(s->timer_b);
    mos8520_update_irq(s);
    /* DDR reset to input: pulled-up pins go high (OVL on CIA-A PA0) */
    mos8520_port_update(s, 0);
    mos8520_port_update(s, 1);
}

static void mos8520_realize(DeviceState *dev, Error **errp)
{
    MOS8520State *s = MOS8520(dev);

    s->timer_a = ptimer_init(mos8520_ta_hit, s, PTIMER_POLICY_LEGACY);
    s->timer_b = ptimer_init(mos8520_tb_hit, s, PTIMER_POLICY_LEGACY);
    ptimer_transaction_begin(s->timer_a);
    ptimer_set_freq(s->timer_a, s->clock_hz);
    ptimer_transaction_commit(s->timer_a);
    ptimer_transaction_begin(s->timer_b);
    ptimer_set_freq(s->timer_b, s->clock_hz);
    ptimer_transaction_commit(s->timer_b);
    timer_init_ns(&s->alarm_timer, QEMU_CLOCK_VIRTUAL, mos8520_alarm_hit, s);
}

static void mos8520_init(Object *obj)
{
    MOS8520State *s = MOS8520(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &mos8520_ops, s, "mos8520", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(dev, s->port_a_out, "port-a-out", 8);
    qdev_init_gpio_out_named(dev, s->port_b_out, "port-b-out", 8);
    qdev_init_gpio_in_named(dev, mos8520_port_in, "port-in", 16);
    qdev_init_gpio_in_named(dev, mos8520_flag_in, "flag", 1);
}

static const VMStateDescription vmstate_mos8520 = {
    .name = "mos8520",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(pra, MOS8520State),
        VMSTATE_UINT8(prb, MOS8520State),
        VMSTATE_UINT8(ddra, MOS8520State),
        VMSTATE_UINT8(ddrb, MOS8520State),
        VMSTATE_UINT8(input_a, MOS8520State),
        VMSTATE_UINT8(input_b, MOS8520State),
        VMSTATE_PTIMER(timer_a, MOS8520State),
        VMSTATE_PTIMER(timer_b, MOS8520State),
        VMSTATE_UINT16(ta_latch, MOS8520State),
        VMSTATE_UINT16(tb_latch, MOS8520State),
        VMSTATE_UINT16(tb_count, MOS8520State),
        VMSTATE_UINT8(cra, MOS8520State),
        VMSTATE_UINT8(crb, MOS8520State),
        VMSTATE_UINT32(tod_base, MOS8520State),
        VMSTATE_INT64(tod_base_ns, MOS8520State),
        VMSTATE_BOOL(tod_running, MOS8520State),
        VMSTATE_BOOL(tod_latched, MOS8520State),
        VMSTATE_UINT32(tod_latch, MOS8520State),
        VMSTATE_UINT32(tod_alarm, MOS8520State),
        VMSTATE_TIMER(alarm_timer, MOS8520State),
        VMSTATE_UINT8(sdr, MOS8520State),
        VMSTATE_UINT8(icr_status, MOS8520State),
        VMSTATE_UINT8(icr_mask, MOS8520State),
        VMSTATE_BOOL(flag_prev, MOS8520State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property mos8520_properties[] = {
    DEFINE_PROP_UINT32("clock-frequency", MOS8520State, clock_hz, 709379),
    DEFINE_PROP_UINT32("tod-frequency", MOS8520State, tod_hz, 50),
};

static void mos8520_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mos8520_realize;
    device_class_set_legacy_reset(dc, mos8520_reset);
    dc->vmsd = &vmstate_mos8520;
    device_class_set_props(dc, mos8520_properties);
}

static const TypeInfo mos8520_info = {
    .name          = TYPE_MOS8520,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MOS8520State),
    .instance_init = mos8520_init,
    .class_init    = mos8520_class_init,
};

static void mos8520_register_types(void)
{
    type_register_static(&mos8520_info);
}

type_init(mos8520_register_types)
