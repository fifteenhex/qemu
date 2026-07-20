/*
 * MStar/SigmaStar "infinity" PWM
 *
 * The PWM controller (mainline device-tree compatible
 * "sstar,infinity-pwm"). On the Miyoo Mini its single channel drives
 * the LCD backlight: MainUI's Settings brightness slider (levels 0..10)
 * programs the duty cycle here as level * 1500 out of a fixed period of
 * 15000 (0x3a97). The per-channel registers, 16-bit on the usual 4-byte
 * RIU stride, from the offsets the vendor kernel's pwm driver touches:
 *
 *   0x08 DUTY_L / 0x0c DUTY_H      duty count (0..period)
 *   0x10 PERIOD_L / 0x14 PERIOD_H  period count
 *   0x18 DIV                       source-clock divider
 *   0x1c CTRL                      run/enable
 *
 * Software programs these and reads them back (the pwm driver's
 * .get_state, which the /sys/class/pwm interface exposes), so the block
 * is modelled as plain readback storage. Nothing consumes the duty yet
 * - the emulated panel is drawn at full brightness regardless - so this
 * captures the register state without dimming the display.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/mstar_pwm.h"

static uint64_t mstar_pwm_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarPwmState *s = MSTAR_PWM(opaque);

    return s->regs[addr / 4];
}

static void mstar_pwm_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MStarPwmState *s = MSTAR_PWM(opaque);

    s->regs[addr / 4] = val;
}

static const MemoryRegionOps mstar_pwm_ops = {
    .read = mstar_pwm_read,
    .write = mstar_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_pwm_reset(DeviceState *dev)
{
    MStarPwmState *s = MSTAR_PWM(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mstar_pwm_init(Object *obj)
{
    MStarPwmState *s = MSTAR_PWM(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_pwm_ops, s,
                          TYPE_MSTAR_PWM, MSTAR_PWM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void mstar_pwm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_pwm_reset);
}

static const TypeInfo mstar_pwm_types[] = {
    {
        .name           = TYPE_MSTAR_PWM,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarPwmState),
        .instance_init  = mstar_pwm_init,
        .class_init     = mstar_pwm_class_init,
    },
};

DEFINE_TYPES(mstar_pwm_types)
