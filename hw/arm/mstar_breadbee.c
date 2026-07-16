/*
 * thingy.jp BreadBee board (MStar infinity3/MSC313E)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A plain MSC313E dev board. It uses the infinity3 SoC (see mstar_infinity3.c)
 * as-is: the SoC's on-die camera capture pipeline is present, but the BreadBee
 * has no sensor wired to the SCCB bus, so it declares no board devices.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "mstar-soc.h"

static void breadbee_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    MStarMachineClass *mmc = MSTAR_MACHINE_CLASS(oc);

    mc->desc = "thingy.jp BreadBee (MStar infinity3/MSC313E)";
    mc->default_ram_size = 64 * MiB;
    mc->max_cpus = 1;
    mmc->soc_type = TYPE_MSTAR_INFINITY3_SOC;
}

static const TypeInfo mstar_breadbee_types[] = {
    {
        .name           = MACHINE_TYPE_NAME("breadbee"),
        .parent         = TYPE_MSTAR_MACHINE,
        .class_init     = breadbee_machine_class_init,
    },
};

DEFINE_TYPES(mstar_breadbee_types)
