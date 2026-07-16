/*
 * MStar/SigmaStar infinity2m (SSD20xD) SoCs
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The infinity2m/SSD20xD is a dual-core Cortex-A7 SoC with an in-package DDR
 * and the GOP/MOP display pipeline. SSD203D is the same die with an HDMI
 * transmitter added, so it subclasses infinity2m. Both subclass the common SoC
 * base in mstar.c. The boards that use them live in their own machine files
 * (mstar_dongshanpione.c, mstar_miyoomini.c, mstar_n1pro.c).
 */

#include "qemu/osdep.h"
#include "mstar-soc.h"

static void mstar_infinity2m_soc_class_init(ObjectClass *oc, const void *data)
{
    MStarSoCClass *sc = MSTAR_SOC_CLASS(oc);

    /* SSD20xD: dual Cortex-A7, secondary released via the smpctrl mailbox. */
    sc->info.cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    sc->info.num_cpus = 2;
    sc->info.has_display = true;
    sc->info.timer_freq = 432000000;    /* clk_timer */
    sc->info.bond = 0x1e;               /* SSD202D (128MB in-package DRAM) */
    sc->info.chip_id = 0xf0;            /* SSD20xD */
    sc->info.clkgen_type = TYPE_SSD20XD_CLKGEN;
    sc->info.pinctrl_type = TYPE_SSD20XD_PINCTRL;
}

static void mstar_ssd203d_soc_class_init(ObjectClass *oc, const void *data)
{
    MStarSoCClass *sc = MSTAR_SOC_CLASS(oc);

    /*
     * SSD203D == SSD202D (infinity2m) with an HDMI transmitter (hdmitx)
     * bolted onto the display pipeline.  It inherits the infinity2m class
     * (dual Cortex-A7, same clkgen/pinctrl base, bond 0x1e, chip 0xf0 — the
     * n1pro boot ROM reports "D-1e" like the SSD202D) and only flags the
     * extra HDMI block so the display model can wire it up.
     */
    sc->info.has_hdmi = true;
}

static const TypeInfo mstar_infinity2m_types[] = {
    {
        .name           = TYPE_MSTAR_INFINITY2M_SOC,
        .parent         = TYPE_MSTAR_SOC,
        .class_init     = mstar_infinity2m_soc_class_init,
    },
    {
        /* SSD203D derives from infinity2m and only adds HDMI. */
        .name           = TYPE_MSTAR_SSD203D_SOC,
        .parent         = TYPE_MSTAR_INFINITY2M_SOC,
        .class_init     = mstar_ssd203d_soc_class_init,
    },
};

DEFINE_TYPES(mstar_infinity2m_types)
