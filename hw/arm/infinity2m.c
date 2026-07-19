/*
 * MStar/SigmaStar infinity2m SoC family
 *
 * The infinity2m family are dual Cortex-A7 SoCs with in-package DRAM.
 * Known members are the SSD201 and the SSD202D, which differ only in
 * the amount and type of DRAM bonded into the package.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/arm/infinity2m.h"

static void infinity2m_soc_class_init(ObjectClass *oc, const void *data)
{
    MStarV7SoCClass *msc = MSTARV7_SOC_CLASS(oc);

    msc->num_cpus = INFINITY2M_NUM_CPUS;
    msc->imi_size = INFINITY2M_IMI_SIZE;
    msc->timer_freq = INFINITY2M_TIMER_FREQ;
}

static const TypeInfo infinity2m_soc_types[] = {
    {
        .name           = TYPE_INFINITY2M_SOC,
        .parent         = TYPE_MSTARV7_SOC,
        .instance_size  = sizeof(Infinity2MSoCState),
        .class_size     = sizeof(Infinity2MSoCClass),
        .class_init     = infinity2m_soc_class_init,
        .abstract       = true,
    },
};

DEFINE_TYPES(infinity2m_soc_types)
