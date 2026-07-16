/*
 * MStar/SigmaStar PM-domain GPIO (gpio_pm)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The power-management GPIO bank (gpio_pm@1e00). Its pads keep their state
 * across suspend and carry a handful of board signals - notably the SD
 * card-detect (SD_CDZ). This model stores/returns the bank registers and, for
 * the SD_CDZ input, reports whether an SD card is present so the sdmmc host
 * enumerates a "-drive if=sd" card.
 *
 * The Miyoo Mini wires the card-detect here (6.5 dts: cd-gpios =
 * <&gpio_pm SSD20XD_PM_SD_CDZ GPIO_ACTIVE_LOW>); the vendor sdmmc driver reads
 * it as bank register 0x47 (byte offset 0x11c) bit 2. Active low: the pad is
 * pulled low while a card is inserted, high (its pull-up) when the slot is
 * empty.
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
#include "system/blockdev.h"
#include "hw/arm/mstar.h"

#define PM_GPIO_SD_CDZ      0x11c   /* bank register 0x47 */
#define PM_GPIO_SD_CDZ_BIT  (1 << 2)

static uint64_t mstar_pm_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarPmGpioState *s = opaque;
    uint16_t v = s->regs[addr / 4];

    if (addr == PM_GPIO_SD_CDZ) {
        /* Active low: card present drives the pad low, empty slot reads high. */
        if (s->card_present) {
            v &= ~PM_GPIO_SD_CDZ_BIT;
        } else {
            v |= PM_GPIO_SD_CDZ_BIT;
        }
    }
    return v;
}

static void mstar_pm_gpio_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    MstarPmGpioState *s = opaque;

    s->regs[addr / 4] = val;
}

static const MemoryRegionOps mstar_pm_gpio_ops = {
    .read = mstar_pm_gpio_read,
    .write = mstar_pm_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_pm_gpio_reset_hold(Object *obj, ResetType type)
{
    MstarPmGpioState *s = MSTAR_PM_GPIO(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mstar_pm_gpio_realize(DeviceState *dev, Error **errp)
{
    MstarPmGpioState *s = MSTAR_PM_GPIO(dev);

    /* A card is present iff the machine was given one via -drive if=sd. */
    s->card_present = drive_get(IF_SD, 0, 0) != NULL;

    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_pm_gpio_ops, s,
                          "mstar.pm-gpio", MSTAR_PM_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void mstar_pm_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_pm_gpio_realize;
    rc->phases.hold = mstar_pm_gpio_reset_hold;
}

static const TypeInfo mstar_pm_gpio_types[] = {
    {
        .name           = TYPE_MSTAR_PM_GPIO,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MstarPmGpioState),
        .class_init     = mstar_pm_gpio_class_init,
    },
};

DEFINE_TYPES(mstar_pm_gpio_types)
