/*
 * MStar/SigmaStar mercury5 SoCs (SSC8336, ...)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The mercury5 family is SigmaStar's dual Cortex-A7 camera/dashcam line (e.g.
 * the SSC8336, used in 70mai dashcams). It subclasses the common SoC base in
 * mstar.c like the other families; the boards live in their own machine files
 * (mstar_70mai.c).
 *
 * mercury5 is close to infinity3/MSC313E (the same ISP/SCL/VIF capture pipeline,
 * see the Mercury5 SDK headers we used for the camera work) but dual-core and
 * with its own clkgen/pinctrl register maps. The exact chip-id / DRAM strap
 * (bond) / timer clock come from the SSC8336 boot ROM + firmware; the values
 * below are provisional and marked TODO until those are in hand.
 */

#include "qemu/osdep.h"
#include "mstar-soc.h"

static void mstar_mercury5_soc_class_init(ObjectClass *oc, const void *data)
{
    MStarSoCClass *sc = MSTAR_SOC_CLASS(oc);

    /* Dual Cortex-A7 (secondary released via the smpctrl mailbox, as SSD20xD). */
    sc->info.cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    sc->info.num_cpus = 2;
    sc->info.has_display = false;       /* TODO: confirm from firmware/board */
    sc->info.timer_freq = 12000000;     /* TODO: confirm mercury5 PIT clock */
    sc->info.bond = 0;                  /* TODO: DRAM strap from the boot ROM */
    sc->info.chip_id = 0;               /* TODO: CHIPID @0x1f003c00 from boot ROM */
    /*
     * TODO: mercury5 has its own clkgen/pinctrl register maps; reuse the
     * infinity3 (msc313) reg-probe tables for now (mercury5 is closest to it),
     * and add mercury5-specific tables once the firmware's register use is
     * captured.
     */
    sc->info.clkgen_type = TYPE_MSC313_CLKGEN;
    sc->info.pinctrl_type = TYPE_MSC313_PINCTRL;
}

static void mstar_ssc8336_soc_class_init(ObjectClass *oc, const void *data)
{
    /*
     * SSC8336 (SSC8336N): a concrete mercury5 SoC. Inherits the family defaults
     * above; any SSC8336-specific info (chip-id, bond, extra blocks) is filled
     * in here once the boot ROM + firmware are available.
     */
}

static const TypeInfo mstar_mercury5_types[] = {
    {
        .name           = TYPE_MSTAR_MERCURY5_SOC,
        .parent         = TYPE_MSTAR_SOC,
        .class_init     = mstar_mercury5_soc_class_init,
        .abstract       = true,         /* a family base; use a concrete SoC */
    },
    {
        .name           = TYPE_MSTAR_SSC8336_SOC,
        .parent         = TYPE_MSTAR_MERCURY5_SOC,
        .class_init     = mstar_ssc8336_soc_class_init,
    },
};

DEFINE_TYPES(mstar_mercury5_types)
