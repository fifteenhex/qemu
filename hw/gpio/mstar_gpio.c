/*
 * MStar/SigmaStar GPIO (main and PM pad banks)
 *
 * Each pad has one 16-bit register on the usual 4-byte RIU stride, in
 * one of two banks: the main bank (gpio@207800 in the mainline device
 * trees) and the PM bank inside the always-on power-management domain.
 * The register bits, per the mainline gpio-msc313 driver:
 *
 *   bit0 IN    the level on the pad
 *   bit4 OUT   the level driven when the output is enabled
 *   bit5 OEN   output disable (0 = pad driven with OUT)
 *
 * External devices (board buttons, straps) drive a pad through one
 * GPIO input line per pad register, named "main-pad"/"pm-pad" and
 * indexed by the pad register offset / 4. A pad the guest drives as
 * an output reads back its own OUT level.
 *
 * Interrupt-capable pads exist but are not modelled: the Miyoo Mini's
 * buttons hang off these banks and its kernel polls them
 * (gpio-keys-polled, 100 ms), so input works without them.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "system/blockdev.h"
#include "hw/block/block.h"
#include "hw/gpio/mstar_gpio.h"

/*
 * Register bit layouts, from the vendor kernel's GPIO HAL pad table
 * (one 24-byte entry per pad: oen/out/in register and mask each).
 * The main bank and the PM bank use different bit positions.
 */
#define MSTAR_GPIO_MAIN_IN   (1 << 0)
#define MSTAR_GPIO_MAIN_OUT  (1 << 4)
#define MSTAR_GPIO_MAIN_OEN  (1 << 5)
#define MSTAR_GPIO_PM_OEN    (1 << 0)
#define MSTAR_GPIO_PM_OUT    (1 << 1)
#define MSTAR_GPIO_PM_IN     (1 << 2)

/*
 * The SD card-detect (SD_CDZ) hangs off the PM bank, at register 0x47
 * (byte offset 0x11c) bit 2, active low: a card in the slot pulls the
 * pad low, an empty slot reads high behind its pull-up. The vendor
 * sdmmc driver reads it here to decide whether to enumerate a card.
 */
#define MSTAR_GPIO_PM_SD_CDZ     0x11c
#define MSTAR_GPIO_PM_SD_CDZ_BIT (1 << 2)

static uint64_t mstar_gpio_bank_read(const uint8_t *regs, const uint32_t *ext,
                                     hwaddr addr, uint8_t in_bit,
                                     uint8_t out_bit, uint8_t oen_bit)
{
    unsigned int pad = addr / 4;
    uint8_t v = regs[pad];
    bool level;

    if (!(v & oen_bit)) {
        /* Driven as an output: the pad reads back what it drives */
        level = v & out_bit;
    } else {
        level = (ext[pad / 32] >> (pad % 32)) & 1;
    }
    return level ? (v | in_bit) : v;
}

static void mstar_gpio_bank_write(uint8_t *regs, hwaddr addr, uint64_t val,
                                  uint8_t out_bit, uint8_t oen_bit)
{
    regs[addr / 4] = val & (out_bit | oen_bit);
}

static uint64_t mstar_gpio_main_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarGpioState *s = MSTAR_GPIO(opaque);

    return mstar_gpio_bank_read(s->main_regs, s->main_ext, addr,
                                MSTAR_GPIO_MAIN_IN, MSTAR_GPIO_MAIN_OUT,
                                MSTAR_GPIO_MAIN_OEN);
}

static void mstar_gpio_main_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    MStarGpioState *s = MSTAR_GPIO(opaque);

    mstar_gpio_bank_write(s->main_regs, addr, val,
                          MSTAR_GPIO_MAIN_OUT, MSTAR_GPIO_MAIN_OEN);
}

static uint64_t mstar_gpio_pm_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarGpioState *s = MSTAR_GPIO(opaque);
    uint64_t v = mstar_gpio_bank_read(s->pm_regs, s->pm_ext, addr,
                                      MSTAR_GPIO_PM_IN, MSTAR_GPIO_PM_OUT,
                                      MSTAR_GPIO_PM_OEN);

    if (addr == MSTAR_GPIO_PM_SD_CDZ) {
        /* Active low: a card present holds the pad low, empty reads high */
        if (s->card_present) {
            v &= ~(uint64_t)MSTAR_GPIO_PM_SD_CDZ_BIT;
        } else {
            v |= MSTAR_GPIO_PM_SD_CDZ_BIT;
        }
    }
    return v;
}

static void mstar_gpio_pm_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    MStarGpioState *s = MSTAR_GPIO(opaque);

    mstar_gpio_bank_write(s->pm_regs, addr, val,
                          MSTAR_GPIO_PM_OUT, MSTAR_GPIO_PM_OEN);
}

static const MemoryRegionOps mstar_gpio_main_ops = {
    .read = mstar_gpio_main_read,
    .write = mstar_gpio_main_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps mstar_gpio_pm_ops = {
    .read = mstar_gpio_pm_read,
    .write = mstar_gpio_pm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_gpio_main_set(void *opaque, int line, int level)
{
    MStarGpioState *s = MSTAR_GPIO(opaque);

    s->main_ext[line / 32] = deposit32(s->main_ext[line / 32],
                                       line % 32, 1, !!level);
}

static void mstar_gpio_pm_set(void *opaque, int line, int level)
{
    MStarGpioState *s = MSTAR_GPIO(opaque);

    s->pm_ext[line / 32] = deposit32(s->pm_ext[line / 32],
                                     line % 32, 1, !!level);
}

static void mstar_gpio_reset(DeviceState *dev)
{
    MStarGpioState *s = MSTAR_GPIO(dev);

    /*
     * Only the guest-programmed pad config resets; the external levels
     * reflect what the board wiring drives, which a reset doesn't change.
     */
    memset(s->main_regs, MSTAR_GPIO_MAIN_OEN, sizeof(s->main_regs));
    memset(s->pm_regs, MSTAR_GPIO_PM_OEN, sizeof(s->pm_regs));
}

static void mstar_gpio_init(Object *obj)
{
    MStarGpioState *s = MSTAR_GPIO(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->main, obj, &mstar_gpio_main_ops, s,
                          "mstar-gpio.main", MSTAR_GPIO_BANK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->main);
    memory_region_init_io(&s->pm, obj, &mstar_gpio_pm_ops, s,
                          "mstar-gpio.pm", MSTAR_GPIO_BANK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->pm);

    qdev_init_gpio_in_named(dev, mstar_gpio_main_set, MSTAR_GPIO_MAIN_PAD,
                            MSTAR_GPIO_NUM_PADS);
    qdev_init_gpio_in_named(dev, mstar_gpio_pm_set, MSTAR_GPIO_PM_PAD,
                            MSTAR_GPIO_NUM_PADS);
}

static void mstar_gpio_realize(DeviceState *dev, Error **errp)
{
    MStarGpioState *s = MSTAR_GPIO(dev);

    /* A card is present iff the machine was given one via -drive if=sd */
    s->card_present = drive_get(IF_SD, 0, 0) != NULL;
}

static void mstar_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mstar_gpio_realize;
    device_class_set_legacy_reset(dc, mstar_gpio_reset);
}

static const TypeInfo mstar_gpio_types[] = {
    {
        .name           = TYPE_MSTAR_GPIO,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarGpioState),
        .instance_init  = mstar_gpio_init,
        .class_init     = mstar_gpio_class_init,
    },
};

DEFINE_TYPES(mstar_gpio_types)
