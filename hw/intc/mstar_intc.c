/*
 * MStar/SigmaStar mst-intc interrupt controller
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

/* ------------------------------------------------------------- mst-intc */

/* mst-intc register block (matches drivers/irqchip/irq-mst-intc.c). */
#define INTC_MASK           0x00
#define INTC_REV_POLARITY   0x10
#define INTC_EOI            0x20

static void mst_intc_set_irq(void *opaque, int n, int level)
{
    MstIntcState *s = opaque;
    bool masked = s->mask[n / 16] & (1u << (n % 16));

    if (level) {
        s->level |= 1ULL << n;
    } else {
        s->level &= ~(1ULL << n);
    }
    qemu_set_irq(s->irq_out[n], level && !masked);
}

static void mst_intc_update(MstIntcState *s)
{
    unsigned int i;

    for (i = 0; i < s->num_irqs; i++) {
        bool masked = s->mask[i / 16] & (1u << (i % 16));
        bool active = (s->level >> i) & 1;

        qemu_set_irq(s->irq_out[i], active && !masked);
    }
}

static uint64_t mst_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    MstIntcState *s = opaque;
    unsigned int idx = (addr & 0xf) / 4;

    if (addr < INTC_REV_POLARITY) {
        return s->mask[idx];
    } else if (addr < INTC_EOI) {
        return s->polarity[idx];
    }
    return 0; /* EOI registers read as zero */
}

static void mst_intc_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    MstIntcState *s = opaque;
    unsigned int idx = (addr & 0xf) / 4;

    if (addr < INTC_REV_POLARITY) {
        s->mask[idx] = val;
        mst_intc_update(s);
    } else if (addr < INTC_EOI) {
        s->polarity[idx] = val;
    }
    /* INTC_EOI: the forwarding is level based, so EOI writes are ignored. */
}

static const MemoryRegionOps mst_intc_ops = {
    .read = mst_intc_read,
    .write = mst_intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void mst_intc_reset_hold(Object *obj, ResetType type)
{
    MstIntcState *s = MST_INTC(obj);

    memset(s->mask, 0, sizeof(s->mask));
    memset(s->polarity, 0, sizeof(s->polarity));
    s->level = 0;
}

static void mst_intc_realize(DeviceState *dev, Error **errp)
{
    MstIntcState *s = MST_INTC(dev);
    unsigned int i;

    if (s->num_irqs == 0 || s->num_irqs > MST_INTC_MAX_IRQS) {
        error_setg(errp, "num-irqs must be between 1 and %d",
                   MST_INTC_MAX_IRQS);
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &mst_intc_ops, s,
                          "mstar.mst-intc", 0x40);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    qdev_init_gpio_in(dev, mst_intc_set_irq, s->num_irqs);
    for (i = 0; i < s->num_irqs; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq_out[i]);
    }
}

static const Property mst_intc_properties[] = {
    DEFINE_PROP_UINT32("irq-start", MstIntcState, irq_start, 0),
    DEFINE_PROP_UINT32("num-irqs", MstIntcState, num_irqs, 0),
};

static const VMStateDescription vmstate_mstar_mst_intc = {
    .name = "mstar-mst-intc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(mask, MstIntcState, MST_INTC_MAX_IRQS / 16),
        VMSTATE_UINT16_ARRAY(polarity, MstIntcState, MST_INTC_MAX_IRQS / 16),
        VMSTATE_UINT64(level, MstIntcState),
        VMSTATE_END_OF_LIST()
    },
};

static void mst_intc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mst_intc_realize;
    rc->phases.hold = mst_intc_reset_hold;
    dc->vmsd = &vmstate_mstar_mst_intc;
    device_class_set_props(dc, mst_intc_properties);
}

static const TypeInfo mstar_intc_types[] = {
    {
        .name           = TYPE_MST_INTC,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MstIntcState),
        .class_init     = mst_intc_class_init,
    },
};

DEFINE_TYPES(mstar_intc_types)
