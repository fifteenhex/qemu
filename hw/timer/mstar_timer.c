/*
 * MStar/SigmaStar msc313e timer
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/loader.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

/* --------------------------------------------------------- msc313e-timer */

#define TIMER_CTRL          0x00
#define TIMER_CTRL_EN       (1 << 0)
#define TIMER_CTRL_TRIG     (1 << 1)
#define TIMER_CTRL_INT_EN   (1 << 8)
#define TIMER_MAX_LOW       0x08
#define TIMER_MAX_HIGH      0x0c
#define TIMER_COUNTER_LOW   0x10
#define TIMER_COUNTER_HIGH  0x14
#define TIMER_DIVIDE        0x18

static uint64_t msc313e_timer_value(Msc313eTimerState *s)
{
    uint64_t v = s->base_count;

    if (s->ctrl & TIMER_CTRL_EN) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        /*
         * The counter is clocked at freq / (DIVIDE + 1). msc313e/infinity3
         * leaves DIVIDE at 0 (so the full 12MHz xtal_div2), while the SSD20xD
         * timer driver programs DIVIDE = 8 to run the 432MHz clk_timer at
         * 432/9 = 48MHz - the rate that variant's clocksource/sched_clock is
         * registered at, so getting the division wrong skews all timekeeping.
         */
        v += muldiv64(now - s->base_ns, s->freq,
                      (uint64_t)NANOSECONDS_PER_SECOND * (s->divide + 1));
    }
    return v;
}

/*
 * (Re)arm the host timer that models the counter reaching MAX. The counter
 * only raises an interrupt when running (EN for periodic, TRIG for one-shot)
 * with INT_EN set and a non-zero MAX; otherwise it is just a free-running
 * clocksource and we cancel any pending expiry.
 */
static void msc313e_timer_update(Msc313eTimerState *s)
{
    bool running = s->ctrl & (TIMER_CTRL_EN | TIMER_CTRL_TRIG);

    if (running && (s->ctrl & TIMER_CTRL_INT_EN) && s->max) {
        uint64_t cur = msc313e_timer_value(s);
        uint64_t remaining = s->max > cur ? s->max - cur : 0;
        int64_t fire = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
            muldiv64(remaining,
                     (uint64_t)NANOSECONDS_PER_SECOND * (s->divide + 1),
                     s->freq);

        timer_mod(s->hrtimer, fire);
    } else {
        timer_del(s->hrtimer);
    }
}

static void msc313e_timer_expire(void *opaque)
{
    Msc313eTimerState *s = opaque;

    /*
     * The interrupt is level based (LEVEL_HIGH in the DT) and the driver
     * acknowledges it by rewriting CTRL when it re-arms the one-shot, so we
     * just latch it high here. Periodic mode (EN without a one-shot TRIG)
     * reloads the counter and keeps running.
     */
    s->int_pending = true;
    if (s->ctrl & TIMER_CTRL_INT_EN) {
        qemu_set_irq(s->irq, 1);
    }

    if ((s->ctrl & TIMER_CTRL_EN) && !(s->ctrl & TIMER_CTRL_TRIG)) {
        s->base_count = 0;
        s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        msc313e_timer_update(s);
    }
}

static uint64_t msc313e_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313eTimerState *s = opaque;

    switch (addr) {
    case TIMER_CTRL:
        return s->ctrl;
    case TIMER_MAX_LOW:
        return s->max & 0xffff;
    case TIMER_MAX_HIGH:
        return s->max >> 16;
    case TIMER_COUNTER_LOW:
        /* Latch the whole value so a following HIGH read is consistent. */
        s->latch = msc313e_timer_value(s);
        return s->latch & 0xffff;
    case TIMER_COUNTER_HIGH:
        return s->latch >> 16;
    case TIMER_DIVIDE:
        return s->divide;
    }
    return 0;
}

static void msc313e_timer_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    Msc313eTimerState *s = opaque;

    switch (addr) {
    case TIMER_CTRL: {
        uint64_t cur = msc313e_timer_value(s);

        s->ctrl = val;
        s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->base_count = (val & TIMER_CTRL_TRIG) ? 0 : cur;
        /* Writing CTRL (stop, or re-arm) acknowledges any pending interrupt. */
        s->int_pending = false;
        qemu_set_irq(s->irq, 0);
        msc313e_timer_update(s);
        break;
    }
    case TIMER_MAX_LOW:
        s->max = (s->max & 0xffff0000) | (val & 0xffff);
        msc313e_timer_update(s);
        break;
    case TIMER_MAX_HIGH:
        s->max = (s->max & 0xffff) | ((val & 0xffff) << 16);
        msc313e_timer_update(s);
        break;
    case TIMER_DIVIDE:
        s->divide = val;
        msc313e_timer_update(s);
        break;
    }
}

static const MemoryRegionOps msc313e_timer_ops = {
    .read = msc313e_timer_read,
    .write = msc313e_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void msc313e_timer_reset_hold(Object *obj, ResetType type)
{
    Msc313eTimerState *s = MSC313E_TIMER(obj);

    s->ctrl = 0;
    s->divide = 0;
    s->max = 0;
    s->base_count = 0;
    s->latch = 0;
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->int_pending = false;
    timer_del(s->hrtimer);
    qemu_set_irq(s->irq, 0);
}

static void msc313e_timer_realize(DeviceState *dev, Error **errp)
{
    Msc313eTimerState *s = MSC313E_TIMER(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313e_timer_ops, s,
                          "mstar.msc313e-timer", MSTAR_TIMER_STRIDE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->hrtimer = timer_new_ns(QEMU_CLOCK_VIRTUAL, msc313e_timer_expire, s);
}

static const Property msc313e_timer_properties[] = {
    DEFINE_PROP_UINT32("freq", Msc313eTimerState, freq, MSTAR_TIMER_FREQ),
};

static const VMStateDescription vmstate_mstar_msc313e_timer = {
    .name = "mstar-msc313e-timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(ctrl, Msc313eTimerState),
        VMSTATE_UINT16(divide, Msc313eTimerState),
        VMSTATE_UINT32(max, Msc313eTimerState),
        VMSTATE_INT64(base_ns, Msc313eTimerState),
        VMSTATE_UINT64(base_count, Msc313eTimerState),
        VMSTATE_UINT32(latch, Msc313eTimerState),
        VMSTATE_BOOL(int_pending, Msc313eTimerState),
        VMSTATE_TIMER_PTR(hrtimer, Msc313eTimerState),
        VMSTATE_END_OF_LIST()
    },
};

static void msc313e_timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313e_timer_realize;
    rc->phases.hold = msc313e_timer_reset_hold;
    dc->vmsd = &vmstate_mstar_msc313e_timer;
    device_class_set_props(dc, msc313e_timer_properties);
}

static const TypeInfo mstar_timer_types[] = {
    {
        .name           = TYPE_MSC313E_TIMER,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313eTimerState),
        .class_init     = msc313e_timer_class_init,
    },
};

DEFINE_TYPES(mstar_timer_types)
