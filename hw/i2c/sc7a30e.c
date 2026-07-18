/*
 * Silan/QST SC7A30E 3-axis accelerometer (G-sensor) - i2c slave
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The impact/motion G-sensor on the 70mai dashcam (i2c0, 7-bit address 0x1d =
 * bus byte 0x3a). The firmware probes it by WHO_AM_I (reg 0x0f == 0x11); a
 * "move boot" (motion/impact-triggered power-on) is detected when the boot
 * interrupt-status read returns 0x0a (ApkIsMoveBoot, RTOS 0x2000bfdc) - which is
 * what starts event recording. This models an ST LIS2DH-compatible register
 * file (auto-incrementing 8-bit register pointer) with the WHO_AM_I and a
 * configurable "motion pending" interrupt source so the trigger can be driven.
 *
 * Set SC7A30E_LOG=1 to log every register access (to map the firmware's exact
 * register program). The "motion" property injects a pending motion interrupt.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

/*
 * WHO_AM_I: the 70mai firmware's Gsensor_Module_Init reads register 0x0f and
 * accepts the chip only if it reads back 0x11 (RTOS: read fn 0x2000b06c forces
 * reg=0x0f, checked by "cmp #0x11" at 0x2000c054, else "Wrong G-Sensor ID for
 * SC7A30E"). Was mistakenly 0x39, so the probe always failed.
 */
#define SC7A30E_WHOAMI      0x0f
#define SC7A30E_WHOAMI_VAL  0x11
#define SC7A30E_INT1_SRC    0x31        /* LIS2DH INT1 source (motion) */
#define SC7A30E_MOVE_INT    0x0a        /* value ApkIsMoveBoot wants (?= 10) */

static void sc7a30e_log(Sc7a30eState *s, char rw, uint8_t reg, uint8_t val)
{
    if (getenv("SC7A30E_LOG")) {
        fprintf(stderr, "[sc7a30e] %c reg 0x%02x = 0x%02x\n", rw, reg, val);
        fflush(stderr);
    }
}

static uint8_t sc7a30e_reg_read(Sc7a30eState *s, uint8_t reg)
{
    switch (reg) {
    case SC7A30E_WHOAMI:
        return SC7A30E_WHOAMI_VAL;
    case SC7A30E_INT1_SRC:
        /* Report a pending motion interrupt (impact) when armed, so the
         * firmware's move-boot check fires. Reading it clears the latch. */
        if (s->motion) {
            s->motion = false;
            return SC7A30E_MOVE_INT;
        }
        return 0;
    default:
        return s->regs[reg];
    }
}

static int sc7a30e_event(I2CSlave *i2c, enum i2c_event event)
{
    Sc7a30eState *s = SC7A30E(i2c);

    if (event == I2C_START_SEND) {
        s->have_ptr = false;
    }
    return 0;
}

static int sc7a30e_send(I2CSlave *i2c, uint8_t data)
{
    Sc7a30eState *s = SC7A30E(i2c);

    if (!s->have_ptr) {
        s->ptr = data & 0x7f;           /* bit7 = auto-increment flag, ignore */
        s->have_ptr = true;
        return 0;
    }
    sc7a30e_log(s, 'W', s->ptr, data);
    s->regs[s->ptr] = data;
    s->ptr = (s->ptr + 1) & 0x7f;
    return 0;
}

static uint8_t sc7a30e_recv(I2CSlave *i2c)
{
    Sc7a30eState *s = SC7A30E(i2c);
    uint8_t val = sc7a30e_reg_read(s, s->ptr);

    sc7a30e_log(s, 'R', s->ptr, val);
    s->ptr = (s->ptr + 1) & 0x7f;
    return val;
}

static void sc7a30e_reset(DeviceState *dev)
{
    Sc7a30eState *s = SC7A30E(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->ptr = 0;
    s->have_ptr = false;
    /* Arm a pending motion interrupt so the power-on is seen as a move-boot. */
    s->motion = s->move_boot;
}

static const VMStateDescription vmstate_sc7a30e = {
    .name = "sc7a30e",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Sc7a30eState),
        VMSTATE_UINT8_ARRAY(regs, Sc7a30eState, 128),
        VMSTATE_UINT8(ptr, Sc7a30eState),
        VMSTATE_BOOL(have_ptr, Sc7a30eState),
        VMSTATE_BOOL(motion, Sc7a30eState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sc7a30e_props[] = {
    /* Present the power-on as a motion/impact-triggered "move boot". */
    DEFINE_PROP_BOOL("move-boot", Sc7a30eState, move_boot, true),
};

static void sc7a30e_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    device_class_set_legacy_reset(dc, sc7a30e_reset);
    dc->vmsd = &vmstate_sc7a30e;
    device_class_set_props(dc, sc7a30e_props);
    sc->event = sc7a30e_event;
    sc->send = sc7a30e_send;
    sc->recv = sc7a30e_recv;
}

static const TypeInfo sc7a30e_types[] = {
    {
        .name           = TYPE_SC7A30E,
        .parent         = TYPE_I2C_SLAVE,
        .instance_size  = sizeof(Sc7a30eState),
        .class_init     = sc7a30e_class_init,
    },
};

DEFINE_TYPES(sc7a30e_types)
