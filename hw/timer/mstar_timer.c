/*
 * MStar/SigmaStar timer
 *
 * A free-running 32-bit up-counter behind a pair of 16-bit RIU
 * registers. The register layout comes from the mainline Linux
 * driver (drivers/clocksource/timer-msc313e.c); the boot ROM
 * programs MAX to 0xffffffff and busy-polls COUNTER to time its
 * flash operations.
 *
 * The counter raises its interrupt on reaching MAX when INT_EN is
 * set: level high, held until software acknowledges by rewriting
 * CTRL (which is also how the mainline driver re-arms its one-shot).
 * A running periodic timer reloads and keeps counting.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/timer/mstar_timer.h"

/* Register byte offsets (16-bit registers on a 4 byte stride) */
#define TIMER_CTRL          0x00
#define TIMER_CTRL_EN       (1 << 0)
#define TIMER_CTRL_TRIG     (1 << 1)
#define TIMER_CTRL_INT_EN   (1 << 8)
#define TIMER_MAX_LOW       0x08
#define TIMER_MAX_HIGH      0x0c
#define TIMER_COUNTER_LOW   0x10
#define TIMER_COUNTER_HIGH  0x14
#define TIMER_DIVIDE        0x18

#define TIMER_REGION_SIZE   0x20

static uint32_t mstar_timer_count(MStarTimerState *s)
{
    int64_t elapsed_ns;
    uint64_t ticks = s->base_count;

    if (s->ctrl & TIMER_CTRL_EN) {
        /* The counter is clocked at freq / (DIVIDE + 1) */
        elapsed_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->base_ns;
        ticks += muldiv64(elapsed_ns, s->freq,
                          (uint64_t)NANOSECONDS_PER_SECOND * (s->divide + 1));
    }

    if (s->max != 0 && s->max != UINT32_MAX) {
        ticks %= (uint64_t)s->max + 1;
    }
    return ticks;
}

/*
 * (Re)arm the host timer modelling the counter reaching MAX. Without
 * INT_EN or a MAX to reach the counter is just a free-running
 * clocksource and no expiry is pending.
 */
static void mstar_timer_update(MStarTimerState *s)
{
    if ((s->ctrl & TIMER_CTRL_EN) && (s->ctrl & TIMER_CTRL_INT_EN) &&
        s->max) {
        uint64_t cur = mstar_timer_count(s);
        uint64_t remaining = s->max > cur ? s->max - cur : 0;

        timer_mod(&s->expiry, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  muldiv64(remaining,
                           (uint64_t)NANOSECONDS_PER_SECOND * (s->divide + 1),
                           s->freq));
    } else {
        timer_del(&s->expiry);
    }
}

static void mstar_timer_expire(void *opaque)
{
    MStarTimerState *s = MSTAR_TIMER(opaque);

    /*
     * Level interrupt: held high until software acknowledges by
     * rewriting CTRL. A running periodic timer reloads the counter
     * and keeps counting.
     */
    qemu_set_irq(s->irq, 1);

    if (s->ctrl & TIMER_CTRL_EN) {
        s->base_count = 0;
        s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        mstar_timer_update(s);
    }
}

static uint64_t mstar_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarTimerState *s = MSTAR_TIMER(opaque);

    switch (addr) {
    case TIMER_CTRL:
        return s->ctrl;
    case TIMER_MAX_LOW:
        return s->max & 0xffff;
    case TIMER_MAX_HIGH:
        return s->max >> 16;
    case TIMER_COUNTER_LOW: {
        /*
         * Latch the high half on a low read so a 16-bit guest sees a
         * coherent 32-bit value. Whether real hardware latches this
         * way is unconfirmed.
         */
        uint32_t count = mstar_timer_count(s);

        s->latch = count >> 16;
        return count & 0xffff;
    }
    case TIMER_COUNTER_HIGH:
        return s->latch;
    case TIMER_DIVIDE:
        return s->divide;
    default:
        qemu_log_mask(LOG_UNIMP, "mstar-timer: unimplemented read 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void mstar_timer_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MStarTimerState *s = MSTAR_TIMER(opaque);

    switch (addr) {
    case TIMER_CTRL: {
        uint32_t cur = mstar_timer_count(s);

        /*
         * Starting or retriggering restarts the count from zero. TRIG
         * is self-clearing: the boot ROM pulses it and polls it clear.
         */
        if ((val & TIMER_CTRL_TRIG) ||
            (!(s->ctrl & TIMER_CTRL_EN) && (val & TIMER_CTRL_EN))) {
            cur = 0;
        }
        s->ctrl = val & ~TIMER_CTRL_TRIG;
        s->base_count = cur;
        s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        /* Rewriting CTRL acknowledges a pending interrupt */
        qemu_set_irq(s->irq, 0);
        mstar_timer_update(s);
        break;
    }
    case TIMER_MAX_LOW:
        s->max = (s->max & 0xffff0000) | (val & 0xffff);
        mstar_timer_update(s);
        break;
    case TIMER_MAX_HIGH:
        s->max = (s->max & 0x0000ffff) | ((val & 0xffff) << 16);
        mstar_timer_update(s);
        break;
    case TIMER_DIVIDE:
        s->divide = val & 0xffff;
        mstar_timer_update(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "mstar-timer: unimplemented write 0x%"
                      HWADDR_PRIx " = 0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps mstar_timer_ops = {
    .read = mstar_timer_read,
    .write = mstar_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_timer_reset(DeviceState *dev)
{
    MStarTimerState *s = MSTAR_TIMER(dev);

    s->ctrl = 0;
    s->divide = 0;
    s->max = 0;
    s->latch = 0;
    s->base_ns = 0;
    s->base_count = 0;
    timer_del(&s->expiry);
    qemu_set_irq(s->irq, 0);
}

static void mstar_timer_init(Object *obj)
{
    MStarTimerState *s = MSTAR_TIMER(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_timer_ops, s,
                          TYPE_MSTAR_TIMER, TIMER_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    timer_init_ns(&s->expiry, QEMU_CLOCK_VIRTUAL, mstar_timer_expire, s);
}

static const Property mstar_timer_properties[] = {
    DEFINE_PROP_UINT32("freq", MStarTimerState, freq, 0),
};

static void mstar_timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_timer_reset);
    device_class_set_props(dc, mstar_timer_properties);
}

static const TypeInfo mstar_timer_types[] = {
    {
        .name           = TYPE_MSTAR_TIMER,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarTimerState),
        .instance_init  = mstar_timer_init,
        .class_init     = mstar_timer_class_init,
    },
};

DEFINE_TYPES(mstar_timer_types)
