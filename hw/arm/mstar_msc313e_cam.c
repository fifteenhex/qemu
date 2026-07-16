/*
 * MStar MSC313E IP-camera board (infinity3 SoC + a sensor wired up)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A generic MSC313E IP-camera reference board. The MSC313E's camera capture
 * pipeline (ISP/HVSP/SCLDMA) is on-die and modelled by the SoC itself (see
 * mstar_infinity3.c), so it is present on every MSC313E board. What makes this
 * a camera is the sensor actually wired to it: this board enables the GPIO8/9
 * bit-banged SCCB bus and attaches a configurable sensor module to it. The
 * BreadBee, by contrast, has no sensor wired (see mstar_breadbee.c).
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "mstar-soc.h"

/*
 * Board devices: attach a configurable sensor module to the gpio bit-banged
 * SCCB bus (enabled before realize via MStarMachineClass.has_gpioi2c). Default:
 * Sony IMX323 at SCCB address 0x36; override with -global
 * mstar-cam-sensor.id-val=... or attach another module to reverse-engineer a
 * different camera's bring-up.
 */
static void msc313e_cam_board_init(MStarSoCState *soc)
{
    if (soc->gpio.i2c_bus) {
        i2c_slave_create_simple(soc->gpio.i2c_bus, TYPE_MSTAR_CAM_SENSOR, 0x36);
    }
}

static void msc313e_cam_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    MStarMachineClass *mmc = MSTAR_MACHINE_CLASS(oc);

    mc->desc = "MStar MSC313E IP camera (infinity3, reference)";
    mc->default_ram_size = 64 * MiB;
    mc->max_cpus = 1;
    mmc->soc_type = TYPE_MSTAR_INFINITY3_SOC;
    mmc->has_gpioi2c = true;            /* wire a sensor on GPIO8/9 SCCB */
    mmc->board_init = msc313e_cam_board_init;
}

static const TypeInfo mstar_msc313e_cam_types[] = {
    {
        .name           = MACHINE_TYPE_NAME("msc313e_cam"),
        .parent         = TYPE_MSTAR_MACHINE,
        .class_init     = msc313e_cam_machine_class_init,
    },
};

DEFINE_TYPES(mstar_msc313e_cam_types)
