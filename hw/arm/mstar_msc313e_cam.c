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
#include "hw/nvram/eeprom_at24c.h"
#include "mstar-soc.h"

/*
 * Board devices: the Sony IMX323 sensor (i2c address 0x36). The vendor firmware
 * actually reaches the sensor through the ISP's own i2c master (registers at
 * 0x1f244d00, inside the ISP block - see mstar_infinity3.c), which is not yet
 * modelled; until it is, attach the sensor on the buses a firmware might use
 * instead - HWI2C "adapter 1" (soc->i2c[1], the bus media_server opens as
 * /dev/i2c-1) and the gpio bit-banged SCCB - so it is ready to respond. The
 * imx323 model presets the chip-id (reg 0x301c = 0x50) its driver probes for.
 */
static void msc313e_cam_board_init(MStarSoCState *soc)
{
    /*
     * The module-ID EEPROM (24C02 at 0x50) hangs off the one GPIO8/9 bit-banged
     * SCCB bus (Linux "mstar,infinity-gpioi2c", pads +0x58/+0x5c); the firmware
     * reads all 256 bytes of it during bring-up.
     */
    if (soc->gpio.i2c_bus[0]) {
        at24c_eeprom_init(soc->gpio.i2c_bus[0], 0x50, 256);
    }
    /*
     * The Sony IMX323 sensor is on HWI2C "adapter 1" - the hardware MIIC master
     * at 0x1f223200 (soc->i2c[1]), which media_server opens as /dev/i2c-1
     * (sensor_db.cfg BUS_TYPE = ISP_I2C_EXTERNAL_1). The vendor kernel driver
     * runs the chip-id read via the MIIC's DMA descriptor engine (see
     * hw/i2c/mstar_i2c.c); the imx323 model presets reg 0x301c = 0x50 so the
     * probe matches and media_server leaves ISP_PATTERN. The sensor answers at
     * i2c address 0x1a (bus address byte 0x34/0x35).
     */
    i2c_slave_create_simple(soc->i2c[1].bus, TYPE_IMX323, 0x1a);
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
