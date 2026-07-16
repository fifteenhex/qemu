/*
 * N1PRO board (MStar infinity2m/SSD203D, HDMI)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * An SSD203D board with HDMI output. It uses the SSD203D SoC (see
 * mstar_infinity2m.c) with no board-specific extra devices (no auth chip, and
 * the HDMI output is not a panel).
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "mstar-soc.h"

static void n1pro_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    MStarMachineClass *mmc = MSTAR_MACHINE_CLASS(oc);

    mc->desc = "N1PRO (MStar infinity2m/SSD203D, HDMI)";
    mc->default_ram_size = 128 * MiB;
    mc->min_cpus = 2;
    mc->default_cpus = 2;
    mc->max_cpus = 2;
    mmc->soc_type = TYPE_MSTAR_SSD203D_SOC;
}

static const TypeInfo mstar_n1pro_types[] = {
    {
        .name           = MACHINE_TYPE_NAME("n1pro"),
        .parent         = TYPE_MSTAR_MACHINE,
        .class_init     = n1pro_machine_class_init,
    },
};

DEFINE_TYPES(mstar_n1pro_types)
