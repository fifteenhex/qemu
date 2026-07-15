/*
 * MStar/SigmaStar msc313 GPIO
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/loader.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "hw/arm/mstar.h"

/* ---------------------------------------------------------- msc313-gpio */

#define GPIO_IN     (1 << 0)
#define GPIO_OUT    (1 << 4)
#define GPIO_OEN    (1 << 5)

static uint64_t msc313_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313GpioState *s = opaque;
    uint8_t v = s->regs[addr];

    /*
     * When a pad is driven (output enabled, OEN clear) its input level reads
     * back the output value; model that loopback so a gpioget reflects
     * whatever gpioset drove. An input pad has no external source here, so it
     * reads low.
     */
    if (!(v & GPIO_OEN) && (v & GPIO_OUT)) {
        v |= GPIO_IN;
    } else {
        v &= ~GPIO_IN;
    }
    return v;
}

static void msc313_gpio_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    Msc313GpioState *s = opaque;

    /* Only OUT/OEN are writable; IN reflects the pin and is computed on read. */
    s->regs[addr] = val & (GPIO_OUT | GPIO_OEN);
}

static const MemoryRegionOps msc313_gpio_ops = {
    .read = msc313_gpio_read,
    .write = msc313_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void msc313_gpio_reset_hold(Object *obj, ResetType type)
{
    Msc313GpioState *s = MSC313_GPIO(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void msc313_gpio_realize(DeviceState *dev, Error **errp)
{
    Msc313GpioState *s = MSC313_GPIO(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313_gpio_ops, s,
                          "mstar.msc313-gpio", MSTAR_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msc313_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313_gpio_realize;
    rc->phases.hold = msc313_gpio_reset_hold;
}

static const TypeInfo mstar_gpio_types[] = {
    {
        .name           = TYPE_MSC313_GPIO,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313GpioState),
        .class_init     = msc313_gpio_class_init,
    },
};

DEFINE_TYPES(mstar_gpio_types)
