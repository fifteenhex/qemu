/*
 * SigmaStar SSD202D SoC
 *
 * An infinity2m family SoC with 128 MiB of DDR3 in the package.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "hw/arm/ssd202d.h"

/* The SSD202D's chiptop package bond strap value */
#define SSD202D_BOND    0x1e

static void ssd202d_soc_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    SSD202DSoCState *s = SSD202D_SOC(dev);
    SSD202DSoCClass *sc = SSD202D_SOC_GET_CLASS(dev);

    sc->parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    memory_region_init_rom(&s->bootrom, OBJECT(dev), "ssd202d.bootrom",
                           SSD202D_BOOTROM_SIZE, errp);
    if (*errp) {
        return;
    }
    memory_region_add_subregion(get_system_memory(), SSD202D_BOOTROM_BASE,
                                &s->bootrom);
}

static void ssd202d_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SSD202DSoCClass *sc = SSD202D_SOC_CLASS(oc);
    MStarV7SoCClass *msc = MSTARV7_SOC_CLASS(oc);

    msc->bond = SSD202D_BOND;
    device_class_set_parent_realize(dc, ssd202d_soc_realize,
                                    &sc->parent_realize);
}

static const TypeInfo ssd202d_soc_types[] = {
    {
        .name           = TYPE_SSD202D_SOC,
        .parent         = TYPE_INFINITY2M_SOC,
        .instance_size  = sizeof(SSD202DSoCState),
        .class_size     = sizeof(SSD202DSoCClass),
        .class_init     = ssd202d_soc_class_init,
    },
};

DEFINE_TYPES(ssd202d_soc_types)
