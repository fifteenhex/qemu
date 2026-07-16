/*
 * 100ask DongShanPi One board (MStar infinity2m/SSD202D)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A plain SSD202D board. It uses the infinity2m SoC (see mstar_infinity2m.c)
 * with no board-specific extra devices.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "mstar-soc.h"

static void dongshanpione_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    MStarMachineClass *mmc = MSTAR_MACHINE_CLASS(oc);

    mc->desc = "100ask DongShanPi One (MStar infinity2m/SSD202D)";
    mc->default_ram_size = 128 * MiB;
    mc->min_cpus = 2;
    mc->default_cpus = 2;
    mc->max_cpus = 2;
    mmc->soc_type = TYPE_MSTAR_INFINITY2M_SOC;
}

static const TypeInfo mstar_dongshanpione_types[] = {
    {
        .name           = MACHINE_TYPE_NAME("dongshanpione"),
        .parent         = TYPE_MSTAR_MACHINE,
        .class_init     = dongshanpione_machine_class_init,
    },
};

DEFINE_TYPES(mstar_dongshanpione_types)
