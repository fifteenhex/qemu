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
#include "hw/arm/ssd202d.h"

static const TypeInfo ssd202d_soc_types[] = {
    {
        .name           = TYPE_SSD202D_SOC,
        .parent         = TYPE_INFINITY2M_SOC,
        .instance_size  = sizeof(SSD202DSoCState),
        .class_size     = sizeof(SSD202DSoCClass),
    },
};

DEFINE_TYPES(ssd202d_soc_types)
