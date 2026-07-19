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
#include "qapi/visitor.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/loader.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

#define PM_GPIO_SD_CDZ      0x11c   /* bank register 0x47 */
#define PM_GPIO_SD_CDZ_BIT  (1 << 2)

/*
 * Two board buttons live on this PM bank (Miyoo Mini 6.5 dts gpio-keys):
 *   key_down  -> SSD20XD_PM_LED0, PM pad register 0x128
 *   key_left  -> SSD20XD_PM_LED1, PM pad register 0x12c
 * Both active-low (idle high via pull-up, pressed low); their input level is
 * the pad IN bit. NB the PM bank's bit layout differs from the main gpio bank:
 * OEN=bit0, OUT=bit1, IN=bit2 (mainline gpio-msc313-pm.c) - which is also why
 * SD_CDZ above reads on bit2. The firmware polls these pads directly. Injected
 * through the "buttons" property: bit0 down, bit1 left.
 */
#define PM_GPIO_KEY_DOWN    0x128
#define PM_GPIO_KEY_LEFT    0x12c
#define PM_GPIO_IN          (1 << 2)

/*
 * Per-pad interrupt control bits (mainline gpio-msc313-pm.c). The pad's edge is
 * latched here and reported through a single aggregate output: the hardware
 * routes each pad to a "pm-intc" line ((off>>2)+2) that funnels to the "irq"
 * mst-intc, so at the delivery level the whole bank is one interrupt.
 */
#define PM_GPIO_IRQ_MASK    (1 << 4)
#define PM_GPIO_IRQ_CLEAR   (1 << 6)
#define PM_GPIO_IRQ_TYPE    (1 << 7)

/* The current input level (0/1) of a pad register, from its board signal. */
static int mstar_pm_gpio_in_level(MstarPmGpioState *s, unsigned int reg)
{
    hwaddr addr = reg * 4;

    if (addr == PM_GPIO_SD_CDZ) {
        return s->card_present ? 0 : 1;         /* active low */
    } else if (addr == PM_GPIO_KEY_DOWN) {
        return (s->buttons & 1u) ? 0 : 1;       /* active low */
    } else if (addr == PM_GPIO_KEY_LEFT) {
        return (s->buttons & 2u) ? 0 : 1;
    }
    return (s->regs[reg] & PM_GPIO_IN) ? 1 : 0;
}

/*
 * Re-sample every pad, latch a pending interrupt on the falling (assert) edge of
 * an unmasked pad (the board signals here are all active-low), and drive the
 * aggregate output = OR of the unmasked pending pads.
 */
static void mstar_pm_gpio_update(MstarPmGpioState *s)
{
    unsigned int reg;
    int active = 0;

    for (reg = 0; reg < MSTAR_PM_GPIO_NUM_REGS; reg++) {
        int level = mstar_pm_gpio_in_level(s, reg);
        bool masked = s->regs[reg] & PM_GPIO_IRQ_MASK;

        if (level == 0 && s->in_last[reg] == 1 && !masked) {
            s->pending[reg] = 1;
        }
        s->in_last[reg] = level;
        if (s->pending[reg] && !masked) {
            active = 1;
        }
    }
    qemu_set_irq(s->irq, active);
}

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
    } else if (addr == PM_GPIO_KEY_DOWN || addr == PM_GPIO_KEY_LEFT) {
        bool pressed = s->buttons & (addr == PM_GPIO_KEY_DOWN ? 1u : 2u);
        if (pressed) {
            v &= ~PM_GPIO_IN;   /* pressed: pull IN low */
        } else {
            v |= PM_GPIO_IN;    /* released: pull-up reads high */
        }
    }
    return v;
}

static void mstar_pm_gpio_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    MstarPmGpioState *s = opaque;

    s->regs[addr / 4] = val;

    /* A write with IRQ_CLEAR set acknowledges/clears that pad's pending edge. */
    if (val & PM_GPIO_IRQ_CLEAR) {
        s->pending[addr / 4] = 0;
    }
    /* Re-evaluate: an (un)mask or clear changes the aggregate output. */
    mstar_pm_gpio_update(s);
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
    unsigned int reg;

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->pending, 0, sizeof(s->pending));
    /* Sample the initial input levels so a present card / held key isn't
     * mistaken for an edge at the first poll. */
    for (reg = 0; reg < MSTAR_PM_GPIO_NUM_REGS; reg++) {
        s->in_last[reg] = mstar_pm_gpio_in_level(s, reg);
    }
    qemu_set_irq(s->irq, 0);
}

static void mstar_pm_gpio_get_buttons(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    MstarPmGpioState *s = MSTAR_PM_GPIO(obj);
    uint32_t value = s->buttons;

    visit_type_uint32(v, name, &value, errp);
}

static void mstar_pm_gpio_set_buttons(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    MstarPmGpioState *s = MSTAR_PM_GPIO(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    s->buttons = value;
    /* A press/release can be an interrupt edge on a key pad. */
    mstar_pm_gpio_update(s);
}

static void mstar_pm_gpio_realize(DeviceState *dev, Error **errp)
{
    MstarPmGpioState *s = MSTAR_PM_GPIO(dev);

    /* A card is present iff the machine was given one via -drive if=sd. */
    s->card_present = drive_get(IF_SD, 0, 0) != NULL;

    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_pm_gpio_ops, s,
                          "mstar.pm-gpio", MSTAR_PM_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    /* Aggregate PM-bank interrupt (wired to the "irq" mst-intc pm-intc line). */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    /*
     * Live-settable bitmask of the pressed PM-bank buttons (bit0 = down,
     * bit1 = left); e.g. `qom-set /machine/soc/pm-gpio buttons=<mask>`.
     */
    object_property_add(OBJECT(dev), "buttons", "uint32",
                        mstar_pm_gpio_get_buttons, mstar_pm_gpio_set_buttons,
                        NULL, NULL);
}

static const VMStateDescription vmstate_mstar_pm_gpio = {
    .name = "mstar-pm-gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, MstarPmGpioState, MSTAR_PM_GPIO_NUM_REGS),
        VMSTATE_UINT8_ARRAY(pending, MstarPmGpioState, MSTAR_PM_GPIO_NUM_REGS),
        VMSTATE_UINT8_ARRAY(in_last, MstarPmGpioState, MSTAR_PM_GPIO_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void mstar_pm_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_pm_gpio_realize;
    rc->phases.hold = mstar_pm_gpio_reset_hold;
    dc->vmsd = &vmstate_mstar_pm_gpio;
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
