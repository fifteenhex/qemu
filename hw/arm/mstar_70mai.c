/*
 * 70mai dashcam board (SigmaStar mercury5 / SSC8336)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A 70mai dashcam built on the SSC8336 (mercury5 family; see mstar_mercury5.c).
 * Board-specific device wiring (image sensor on its SCCB bus, any auth/eeprom,
 * etc.) goes in board_init once the boot ROM + firmware are in hand. RAM size,
 * CPU count and defaults below are provisional (TODO).
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "mstar-soc.h"

static void mai70_board_init(MStarSoCState *soc)
{
    /* Injoinic IP6303 PMIC on i2c0 @ 0x30 (custom address). Without it every
     * APK_IP6303_* read NAKs and the app believes the power key is held down. */
    i2c_slave_create_simple(soc->i2c[0].bus, TYPE_IP6303, 0x30);

    /*
     * SC7A30E G-sensor (accelerometer) on i2c0, 7-bit address 0x1d (the firmware
     * probes bus byte 0x3a for WHO_AM_I == 0x11). This is the dashcam's impact/
     * motion sensor; a motion-triggered power-on ("move boot") is what starts
     * event recording (ApkIsMoveBoot wants its boot int-status == 0x0a).
     */
    i2c_slave_create_simple(soc->i2c[0].bus, TYPE_SC7A30E, 0x1d);

    /* TODO: image sensor (IMX307) once the record pipeline reaches capture. */
}

static void mai70_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    MStarMachineClass *mmc = MSTAR_MACHINE_CLASS(oc);

    mc->desc = "70mai dashcam (SigmaStar mercury5/SSC8336)";
    mc->default_ram_size = 128 * MiB;   /* TODO: confirm from the boot ROM/DTB */
    mc->min_cpus = 2;
    mc->default_cpus = 2;
    mc->max_cpus = 2;
    mmc->soc_type = TYPE_MSTAR_SSC8336_SOC;
    mmc->board_init = mai70_board_init;
}

static const TypeInfo mstar_70mai_types[] = {
    {
        .name           = MACHINE_TYPE_NAME("70mai"),
        .parent         = TYPE_MSTAR_MACHINE,
        .class_init     = mai70_machine_class_init,
    },
};

DEFINE_TYPES(mstar_70mai_types)
