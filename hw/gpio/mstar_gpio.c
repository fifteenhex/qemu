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
#include "qapi/visitor.h"
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

/*
 * The board buttons wired to this pad bank (Miyoo Mini, from the 6.5 device
 * tree gpio-keys node; confirmed against the running firmware, which polls
 * exactly these pad offsets). Each button pulls its pad to the active level
 * when pressed; the bit index into the "buttons" mask is this array index.
 * Pressing a button from the monitor:
 *   qom-set /machine/soc/gpio buttons=<mask>   (bit N = mstar_buttons[N])
 *
 * key_down and key_left live on the separate PM gpio bank and are not modelled
 * here yet.
 */
typedef struct {
    uint8_t offset;             /* pad register offset in this bank */
    bool active_low;            /* pressed pulls the pad low (has a pull-up) */
    const char *name;
} MstarButton;

static const MstarButton mstar_buttons[] = {
    { 0x04, true,  "up" },      /* GPIO1  */
    { 0x14, true,  "right" },   /* GPIO5  */
    { 0x18, true,  "b" },       /* GPIO6  */
    { 0x1c, true,  "a" },       /* GPIO7  */
    { 0x20, true,  "y" },       /* GPIO8  */
    { 0x24, true,  "x" },       /* GPIO9  */
    { 0x28, true,  "start" },   /* GPIO10 */
    { 0x2c, true,  "select" },  /* GPIO11 */
    { 0x30, true,  "menu" },    /* GPIO12 */
    { 0x34, true,  "l2" },      /* GPIO13 */
    { 0x38, true,  "l" },       /* GPIO14 */
    { 0x60, true,  "r" },       /* UART0_RX */
    { 0x114, true, "r2" },      /* GPIO90 */
    { 0x104, false, "power" },  /* GPIO86, active high */
};

#define MSTAR_NUM_BUTTONS ARRAY_SIZE(mstar_buttons)

/* If addr is a button pad, return its input level (bit0), else -1. */
static int msc313_gpio_button_level(Msc313GpioState *s, hwaddr addr)
{
    unsigned int i;

    for (i = 0; i < MSTAR_NUM_BUTTONS; i++) {
        if (mstar_buttons[i].offset == addr) {
            bool pressed = s->buttons & (1u << i);
            /* active-low: idle high, pressed low; active-high: the reverse. */
            return mstar_buttons[i].active_low ? !pressed : pressed;
        }
    }
    return -1;
}

static uint64_t msc313_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313GpioState *s = opaque;
    uint8_t v = s->regs[addr];
    int level = msc313_gpio_button_level(s, addr);

    if (level >= 0) {
        /* A button pad: its input bit reflects the (physical) button level. */
        return level ? (v | GPIO_IN) : (v & ~GPIO_IN);
    }
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
    s->buttons = 0;
}

static void msc313_gpio_get_buttons(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    Msc313GpioState *s = MSC313_GPIO(obj);
    uint32_t value = s->buttons;

    visit_type_uint32(v, name, &value, errp);
}

static void msc313_gpio_set_buttons(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    Msc313GpioState *s = MSC313_GPIO(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    s->buttons = value;
}

static void msc313_gpio_realize(DeviceState *dev, Error **errp)
{
    Msc313GpioState *s = MSC313_GPIO(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313_gpio_ops, s,
                          "mstar.msc313-gpio", MSTAR_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    /*
     * A live-settable bitmask of the pressed board buttons (see mstar_buttons);
     * e.g. `qom-set /machine/soc/gpio buttons=<mask>` from the QMP monitor.
     */
    object_property_add(OBJECT(dev), "buttons", "uint32",
                        msc313_gpio_get_buttons, msc313_gpio_set_buttons,
                        NULL, NULL);
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
