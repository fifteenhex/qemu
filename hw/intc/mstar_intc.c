/*
 * MStar/SigmaStar mst-intc interrupt controller
 *
 * A thin layer between the peripherals and the GIC (also found on
 * MediaTek chips, hence the mainline name): each input line has a
 * mask bit and a polarity bit and forwards to one GIC SPI. Register
 * block per the mainline irq-mst-intc driver: 64 mask bits from
 * 0x00, 64 polarity bits from 0x10, EOI from 0x20. Forwarding here
 * is level based, so EOI writes have nothing to do and polarity is
 * only stored: QEMU peripherals signal "active" as level 1 and the
 * inverted-input distinction is not observable.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/mstar_intc.h"

#define INTC_MASK           0x00
#define INTC_REV_POLARITY   0x10
#define INTC_EOI            0x20

static void mstar_intc_update_one(MStarIntcState *s, unsigned int n)
{
    bool masked = s->mask[n / 16] & (1u << (n % 16));
    bool active = (s->level >> n) & 1;

    qemu_set_irq(s->irq_out[n], active && !masked);
}

static void mstar_intc_set_irq(void *opaque, int n, int level)
{
    MStarIntcState *s = MSTAR_INTC(opaque);

    s->level = deposit64(s->level, n, 1, !!level);
    mstar_intc_update_one(s, n);
}

static uint64_t mstar_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarIntcState *s = MSTAR_INTC(opaque);
    unsigned int idx = (addr & 0xf) / 4;

    if (addr < INTC_REV_POLARITY) {
        return s->mask[idx];
    } else if (addr < INTC_EOI) {
        return s->polarity[idx];
    }
    return 0;
}

static void mstar_intc_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MStarIntcState *s = MSTAR_INTC(opaque);
    unsigned int idx = (addr & 0xf) / 4;
    unsigned int i;

    if (addr < INTC_REV_POLARITY) {
        s->mask[idx] = val;
        for (i = idx * 16; i < (idx + 1) * 16 && i < s->num_irqs; i++) {
            mstar_intc_update_one(s, i);
        }
    } else if (addr < INTC_EOI) {
        s->polarity[idx] = val;
    }
    /* EOI: forwarding is level based, nothing to acknowledge */
}

static const MemoryRegionOps mstar_intc_ops = {
    .read = mstar_intc_read,
    .write = mstar_intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_intc_reset(DeviceState *dev)
{
    MStarIntcState *s = MSTAR_INTC(dev);

    /*
     * All lines pass through out of reset. The vendor kernel routes its
     * peripheral interrupts through the pass-through "main" interrupt nexus
     * straight to the GIC, and never programs this register-block intc's mask,
     * so it must not gate them off; the GIC's own enables do the masking.
     */
    memset(s->mask, 0, sizeof(s->mask));
    memset(s->polarity, 0, sizeof(s->polarity));
    s->level = 0;
}

static void mstar_intc_realize(DeviceState *dev, Error **errp)
{
    MStarIntcState *s = MSTAR_INTC(dev);
    unsigned int i;

    if (s->num_irqs > MSTAR_INTC_MAX_IRQS) {
        error_setg(errp, "num-irqs must be at most %d", MSTAR_INTC_MAX_IRQS);
        return;
    }

    qdev_init_gpio_in(dev, mstar_intc_set_irq, s->num_irqs);
    for (i = 0; i < s->num_irqs; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq_out[i]);
    }
}

static void mstar_intc_init(Object *obj)
{
    MStarIntcState *s = MSTAR_INTC(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_intc_ops, s,
                          TYPE_MSTAR_INTC, MSTAR_INTC_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property mstar_intc_properties[] = {
    DEFINE_PROP_UINT32("num-irqs", MStarIntcState, num_irqs,
                       MSTAR_INTC_MAX_IRQS),
};

static void mstar_intc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mstar_intc_realize;
    device_class_set_props(dc, mstar_intc_properties);
    device_class_set_legacy_reset(dc, mstar_intc_reset);
}

static const TypeInfo mstar_intc_types[] = {
    {
        .name           = TYPE_MSTAR_INTC,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarIntcState),
        .instance_init  = mstar_intc_init,
        .class_init     = mstar_intc_class_init,
    },
};

DEFINE_TYPES(mstar_intc_types)
