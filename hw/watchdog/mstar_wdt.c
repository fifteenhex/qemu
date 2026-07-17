/*
 * MStar/SigmaStar watchdog timer (watchdog@1f006000)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A small free-running-counter watchdog common to the MStar Armv7 SoCs (it is
 * declared in mstar-v7.dtsi, compatible "mstar,msc313e-wdt", clocked by
 * xtal_div2 = 12MHz, its pre-timeout interrupt on the "fiq" mst-intc line 2).
 * The register interface is from the mainline driver
 * drivers/watchdog/msc313e_wdt.c - 16-bit registers at the 4-byte RIU stride:
 *
 *   0x00  CLR         write 1 to reset the counter (ping); the driver also
 *                     writes 1 to start and 0 (with a zero period) to stop
 *   0x0c  INT         pre-timeout: the interrupt fires when the top 16 bits of
 *                     the counter reach this value (0 bottom 16 bits)
 *   0x10  MAX_PRD_L   reset period, low 16 bits (in input-clock ticks)
 *   0x14  MAX_PRD_H   reset period, high 16 bits; a zero period means stopped
 *
 * The hardware counts up at the input clock and resets the SoC when the counter
 * reaches MAX_PRD without a ping. We model that with a QEMU timer that performs
 * the configured watchdog action on expiry, plus the pre-timeout interrupt.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/watchdog.h"
#include "hw/arm/mstar.h"

#define WDT_CLR         0x00
#define WDT_INT         0x0c
#define WDT_MAX_PRD_L   0x10
#define WDT_MAX_PRD_H   0x14

static uint32_t mstar_wdt_period(MstarWdtState *s)
{
    return s->prd_l | ((uint32_t)s->prd_h << 16);
}

/* (Re)arm or disarm the reset + pre-timeout timers from the current registers. */
static void mstar_wdt_rearm(MstarWdtState *s)
{
    uint32_t period = mstar_wdt_period(s);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t pretimeout;

    if (period == 0 || s->freq == 0) {          /* stopped */
        timer_del(s->reset_timer);
        timer_del(s->int_timer);
        qemu_set_irq(s->irq, 0);
        return;
    }

    timer_mod(s->reset_timer,
              now + (int64_t)period * NANOSECONDS_PER_SECOND / s->freq);

    /* Pre-timeout interrupt when the top 16 bits of the counter reach INT. */
    pretimeout = (uint32_t)s->intr << 16;
    if (pretimeout != 0 && pretimeout < period) {
        timer_mod(s->int_timer,
                  now + (int64_t)pretimeout * NANOSECONDS_PER_SECOND / s->freq);
    } else {
        timer_del(s->int_timer);
    }
}

static void mstar_wdt_reset_expired(void *opaque)
{
    /* The counter reached MAX_PRD without a ping: reset the SoC. */
    watchdog_perform_action();
}

static void mstar_wdt_int_expired(void *opaque)
{
    MstarWdtState *s = opaque;

    qemu_set_irq(s->irq, 1);                    /* pre-timeout warning */
}

static uint64_t mstar_wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarWdtState *s = opaque;

    switch (addr) {
    case WDT_INT:
        return s->intr;
    case WDT_MAX_PRD_L:
        return s->prd_l;
    case WDT_MAX_PRD_H:
        return s->prd_h;
    default:
        return 0;
    }
}

static void mstar_wdt_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MstarWdtState *s = opaque;

    if (getenv("MSTAR_WDT_DBG")) {
        fprintf(stderr, "[wdt] write %02x = %04x (period=%u ticks)\n",
                (unsigned)addr, (unsigned)val, mstar_wdt_period(s));
    }

    switch (addr) {
    case WDT_CLR:
        /* Ping: restart the counter (and drop a pending pre-timeout). */
        qemu_set_irq(s->irq, 0);
        mstar_wdt_rearm(s);
        break;
    case WDT_INT:
        s->intr = val;
        break;
    case WDT_MAX_PRD_L:
        s->prd_l = val;
        mstar_wdt_rearm(s);
        break;
    case WDT_MAX_PRD_H:
        s->prd_h = val;
        mstar_wdt_rearm(s);
        break;
    }
}

static const MemoryRegionOps mstar_wdt_ops = {
    .read = mstar_wdt_read,
    .write = mstar_wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void mstar_wdt_reset_hold(Object *obj, ResetType type)
{
    MstarWdtState *s = MSTAR_WDT(obj);

    s->intr = 0;
    s->prd_l = 0;
    s->prd_h = 0;
    timer_del(s->reset_timer);
    timer_del(s->int_timer);
    qemu_set_irq(s->irq, 0);
}

static void mstar_wdt_realize(DeviceState *dev, Error **errp)
{
    MstarWdtState *s = MSTAR_WDT(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_wdt_ops, s,
                          "mstar.wdt", MSTAR_WDT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->reset_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mstar_wdt_reset_expired, s);
    s->int_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mstar_wdt_int_expired, s);
}

static const VMStateDescription vmstate_mstar_wdt = {
    .name = "mstar-wdt",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(intr, MstarWdtState),
        VMSTATE_UINT16(prd_l, MstarWdtState),
        VMSTATE_UINT16(prd_h, MstarWdtState),
        VMSTATE_TIMER_PTR(reset_timer, MstarWdtState),
        VMSTATE_TIMER_PTR(int_timer, MstarWdtState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mstar_wdt_props[] = {
    /* Input clock: xtal_div2 = 12MHz on the MStar Armv7 SoCs. */
    DEFINE_PROP_UINT32("clock-frequency", MstarWdtState, freq, 12000000),
};

static void mstar_wdt_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_wdt_realize;
    dc->vmsd = &vmstate_mstar_wdt;
    rc->phases.hold = mstar_wdt_reset_hold;
    device_class_set_props(dc, mstar_wdt_props);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo mstar_wdt_types[] = {
    {
        .name           = TYPE_MSTAR_WDT,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MstarWdtState),
        .class_init     = mstar_wdt_class_init,
    },
};

DEFINE_TYPES(mstar_wdt_types)
