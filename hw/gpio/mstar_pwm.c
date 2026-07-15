/*
 * MStar/SigmaStar PWM controller
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

/* ------------------------------------------------------------------- pwm */

/* Register map from the mainline driver drivers/pwm/pwm-msc313e.c. */
#define PWM_CHANNEL_STRIDE  0x80
#define PWM_DUTY            0x08    /* low @+0x08, high (bits 0-2) @+0x0c */
#define PWM_PERIOD          0x10    /* low @+0x10, high (bits 0-2) @+0x14 */
#define PWM_CTRL            0x1c    /* bit 4 = polarity (inverted) */
#define PWM_SWRST           0x1fc   /* per-channel bit; 1 = held in reset */

/* Backlight level of a PWM channel, as an 0..256 scale (256 = full on). */
unsigned int msc313_pwm_brightness(Msc313PwmState *s, unsigned int ch)
{
    unsigned int base = ch * PWM_CHANNEL_STRIDE;
    uint32_t duty, period;
    unsigned int frac;

    if (s->regs[PWM_SWRST / 4] & (1u << ch)) {
        return 0;                                   /* channel disabled */
    }
    duty = s->regs[(base + PWM_DUTY) / 4] |
           ((uint32_t)(s->regs[(base + PWM_DUTY + 4) / 4] & 0x7) << 16);
    period = s->regs[(base + PWM_PERIOD) / 4] |
             ((uint32_t)(s->regs[(base + PWM_PERIOD + 4) / 4] & 0x7) << 16);
    if (period == 0) {
        return 256;
    }
    frac = duty >= period ? 256 : (duty * 256 / period);
    if (s->regs[(base + PWM_CTRL) / 4] & (1 << 4)) {
        frac = 256 - frac;                          /* inverted polarity */
    }
    return frac;
}

static uint64_t msc313_pwm_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313PwmState *s = opaque;

    return s->regs[addr / 4];
}

static void msc313_pwm_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Msc313PwmState *s = opaque;

    s->regs[addr / 4] = val;
}

static const MemoryRegionOps msc313_pwm_ops = {
    .read = msc313_pwm_read,
    .write = msc313_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void msc313_pwm_reset_hold(Object *obj, ResetType type)
{
    Msc313PwmState *s = MSC313_PWM(obj);
    unsigned int i;

    memset(s->regs, 0, sizeof(s->regs));
    /* Channels come out of reset held (disabled), as the hardware does. */
    for (i = 0; i < MSTAR_PWM_CHANNELS; i++) {
        s->regs[PWM_SWRST / 4] |= 1u << i;
    }
}

static void msc313_pwm_realize(DeviceState *dev, Error **errp)
{
    Msc313PwmState *s = MSC313_PWM(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313_pwm_ops, s,
                          "mstar.pwm", MSTAR_PWM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msc313_pwm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313_pwm_realize;
    rc->phases.hold = msc313_pwm_reset_hold;
}

static const TypeInfo mstar_pwm_types[] = {
    {
        .name           = TYPE_MSC313_PWM,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313PwmState),
        .class_init     = msc313_pwm_class_init,
    },
};

DEFINE_TYPES(mstar_pwm_types)
