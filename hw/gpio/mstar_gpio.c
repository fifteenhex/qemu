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
#include "hw/i2c/i2c.h"
#include "hw/i2c/bitbang_i2c.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

/*
 * Bit-banged SCCB bus (Linux i2c-gpio "mstar,infinity-gpioi2c", cam.dtb: sda-gpio
 * 8 / scl-gpio 9) on this pad bank at SDA/SCL = +0x58/+0x5c. The MSC313E camera
 * firmware carries the module-ID EEPROM (0x50) here. Open-drain: OEN (bit5) clear
 * drives low, set releases high; SDA input reads back as bit0. (The sensor is NOT
 * on a gpio bus - it hangs off the hardware MIIC master at 0x1f223200; see
 * hw/i2c/mstar_i2c.c and hw/arm/mstar_msc313e_cam.c.)
 */
static const struct { uint8_t sda, scl; } gpio_i2c_pads[MSTAR_GPIO_NUM_I2C] = {
    { 0x58, 0x5c },
};

/* ---------------------------------------------------------- msc313-gpio */

#define GPIO_IN     (1 << 0)
#define GPIO_OUT    (1 << 4)
#define GPIO_OEN    (1 << 5)

/*
 * The four spi0 pads are the only interrupt-capable pins (kernel
 * msc313e_gpio_child_to_parent_hwirq: pads at offsets 0x1c0..0x1cc -> parent
 * "fiq" mst-intc line ((off-0x1c0)>>2)+28). The GPIO block has no interrupt
 * enable/type/status of its own; the pad level feeds the intc line directly and
 * the intc does the masking. So each output IRQ just follows its pad's input.
 * The pad input is injected via the "spi0" property (bit i = pad i level).
 */
static const uint16_t msc313_gpio_irq_off[MSC313_GPIO_NUM_IRQS] = {
    0x1c0, 0x1c4, 0x1c8, 0x1cc,     /* spi0_cz, spi0_ck, spi0_di, spi0_do */
};

/* If addr is one of the spi0 IRQ pads, return its index, else -1. */
static int msc313_gpio_irq_index(hwaddr addr)
{
    unsigned int i;

    for (i = 0; i < MSC313_GPIO_NUM_IRQS; i++) {
        if (msc313_gpio_irq_off[i] == addr) {
            return i;
        }
    }
    return -1;
}

static void msc313_gpio_update_irq(Msc313GpioState *s)
{
    unsigned int i;

    for (i = 0; i < MSC313_GPIO_NUM_IRQS; i++) {
        qemu_set_irq(s->irq[i], (s->spi0 >> i) & 1);
    }
}

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
    int level;

    /* Bit-banged SCCB: an SDA pad reads back the (open-drain) bus level. */
    if (s->gpioi2c) {
        for (unsigned int i = 0; i < MSTAR_GPIO_NUM_I2C; i++) {
            if (addr == gpio_i2c_pads[i].sda) {
                return s->sda_level[i] ? (v | GPIO_IN) : (v & ~GPIO_IN);
            }
        }
    }

    /* An spi0 IRQ pad: its input bit reflects the injected level. */
    level = msc313_gpio_irq_index(addr);
    if (level >= 0) {
        return ((s->spi0 >> level) & 1) ? (v | GPIO_IN) : (v & ~GPIO_IN);
    }

    level = msc313_gpio_button_level(s, addr);
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

    /*
     * Bit-banged sensor SCCB. The pad is open-drain: with OEN set (input) the
     * line is released and pulled high; with OEN clear (output) it drives the
     * OUT bit (drivers only ever drive it low). Feed SCL/SDA to the bit-bang
     * engine, which returns the resulting bus SDA level for reads.
     */
    if (s->gpioi2c) {
        int drive = (val & GPIO_OEN) ? 1 : ((val & GPIO_OUT) ? 1 : 0);
        for (unsigned int i = 0; i < MSTAR_GPIO_NUM_I2C; i++) {
            if (addr == gpio_i2c_pads[i].sda || addr == gpio_i2c_pads[i].scl) {
                int line = (addr == gpio_i2c_pads[i].sda) ? BITBANG_I2C_SDA
                                                          : BITBANG_I2C_SCL;
                s->sda_level[i] = bitbang_i2c_set(
                    (bitbang_i2c_interface *)s->bbi2c[i], line, drive);
                break;
            }
        }
    }
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
    s->spi0 = 0;
    msc313_gpio_update_irq(s);
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

static void msc313_gpio_get_spi0(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    Msc313GpioState *s = MSC313_GPIO(obj);
    uint32_t value = s->spi0;

    visit_type_uint32(v, name, &value, errp);
}

static void msc313_gpio_set_spi0(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    Msc313GpioState *s = MSC313_GPIO(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    s->spi0 = value;
    msc313_gpio_update_irq(s);
}

static void msc313_gpio_realize(DeviceState *dev, Error **errp)
{
    Msc313GpioState *s = MSC313_GPIO(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    unsigned int i;

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313_gpio_ops, s,
                          "mstar.msc313-gpio", MSTAR_GPIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    /* One output per interrupt-capable spi0 pad (wired to "fiq" intc 28..31). */
    for (i = 0; i < MSC313_GPIO_NUM_IRQS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }

    /*
     * A live-settable bitmask of the pressed board buttons (see mstar_buttons);
     * e.g. `qom-set /machine/soc/gpio buttons=<mask>` from the QMP monitor.
     */
    object_property_add(OBJECT(dev), "buttons", "uint32",
                        msc313_gpio_get_buttons, msc313_gpio_set_buttons,
                        NULL, NULL);

    /*
     * Injected input level of the four interrupt-capable spi0 pads (bit i = pad
     * i); setting it drives the corresponding "fiq" mst-intc line 28+i, e.g.
     * `qom-set /machine/soc/gpio spi0=1` raises spi0_cz's interrupt.
     */
    object_property_add(OBJECT(dev), "spi0", "uint32",
                        msc313_gpio_get_spi0, msc313_gpio_set_spi0,
                        NULL, NULL);

    /*
     * Bit-banged SCCB (camera SoCs): create both i2c-gpio buses + their bit-bang
     * engines so the board can attach the module EEPROM (bus 0) and the sensor
     * (bus 1). See gpio_i2c_pads[] for the pads.
     */
    if (s->gpioi2c) {
        for (i = 0; i < MSTAR_GPIO_NUM_I2C; i++) {
            g_autofree char *name = g_strdup_printf("gpioi2c%u", i);
            s->i2c_bus[i] = i2c_init_bus(dev, name);
            s->bbi2c[i] = g_new0(bitbang_i2c_interface, 1);
            bitbang_i2c_init(s->bbi2c[i], s->i2c_bus[i]);
            s->sda_level[i] = 1;
        }
    }
}

/* NB: the bit-bang SCCB helper (bbi2c) keeps no migrated state; a
 * snapshot taken mid-bitbanged-transfer would resume mid-byte (camera
 * boards only - take snapshots at idle). */
static const VMStateDescription vmstate_mstar_msc313_gpio = {
    .name = "mstar-msc313-gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, Msc313GpioState, MSTAR_GPIO_NUM_REGS),
        VMSTATE_INT32_ARRAY(sda_level, Msc313GpioState, MSTAR_GPIO_NUM_I2C),
        VMSTATE_UINT32(spi0, Msc313GpioState),
        VMSTATE_END_OF_LIST()
    },
};

static void msc313_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313_gpio_realize;
    rc->phases.hold = msc313_gpio_reset_hold;
    dc->vmsd = &vmstate_mstar_msc313_gpio;
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
