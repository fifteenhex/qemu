/*
 * Miyoo Mini board (MStar infinity2m/SSD202D + alpu-fa auth chip)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Miyoo Mini reuses the infinity2m SoC (see mstar_infinity2m.c) but is the
 * only board with the "alpu-fa" i2c copy-protection / auth chip on i2c1 at
 * 0x3d (both the vendor kernel and MainUI refuse to run without it). That
 * board-specific device is wired here, in the board's own file, so it never
 * leaks onto the other SSD202D boards. The panel is also mounted upside down.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "mstar-soc.h"

/*
 * Board devices: attach the alpu-fa auth chip on i2c1 @ 0x3d and flag the
 * upside-down panel. Run once, after the SoC is realized (so the i2c bus and
 * the display model exist). TYPE_MSTAR_SECELEM models the alpu-fa crypto (see
 * hw/i2c/mstar_secelem.c - to be renamed alpu-fa).
 */
static void miyoomini_board_init(MStarSoCState *soc)
{
    i2c_slave_create_simple(soc->i2c[1].bus, TYPE_MSTAR_SECELEM, 0x3d);
    /* The panel is mounted 180deg: flip both the GOP (RGB) and mopg (video). */
    soc->gop.flip = true;
    soc->disp.flip = true;
}

static void miyoomini_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    MStarMachineClass *mmc = MSTAR_MACHINE_CLASS(oc);

    mc->desc = "Miyoo Mini (MStar infinity2m/SSD202D)";
    mc->default_ram_size = 128 * MiB;
    mc->min_cpus = 2;
    mc->default_cpus = 2;
    mc->max_cpus = 2;
    mmc->soc_type = TYPE_MSTAR_INFINITY2M_SOC;
    mmc->board_init = miyoomini_board_init;
}

static const TypeInfo mstar_miyoomini_types[] = {
    {
        .name           = MACHINE_TYPE_NAME("miyoomini"),
        .parent         = TYPE_MSTAR_MACHINE,
        .class_init     = miyoomini_machine_class_init,
    },
};

DEFINE_TYPES(mstar_miyoomini_types)
