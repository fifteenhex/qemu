/*
 * Injoinic IP6303 PMIC (i2c slave)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The PMIC used by the 70mai dashcam (mercury5/SSC8336), on i2c0 at address
 * 0x30. Register map from the mainline ip6xxx MFD driver
 * (include/linux/mfd/ip6xxx.h) plus the 70mai firmware's APK_IP6303_* HAL
 * (reverse engineered - see the register notes below).
 *
 * Modelled as a plain byte register file with an auto-incrementing pointer
 * (write: reg pointer + data bytes; read: data from the pointer on). Only a
 * few registers carry behaviour the firmware actually tests:
 *
 *   0x54 CHG_DIG_CTL1  bits[7:5] charge state (5 = charge end)
 *   0x55 CHG_DIG_CTL2  bit3 = battery/external power OK
 *   0x64 ADC_DATA_VBAT battery voltage, mV = val * 15.625 + 500
 *   0x71               interrupt flags; the firmware writes 0xff to clear
 *                      (write-1-to-clear, so reads stay 0 = nothing pending)
 *   0x72               input level status: bit5 = power key line (high = not
 *                      pressed). Apk_PmuIsPwrKeyDown returns !bit5; with the
 *                      chip absent the firmware's cached fallback made the key
 *                      look permanently held down.
 *
 * Set MSTAR_PMIC_LOG=1 to log every register access.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

#define IP6303_CHG_DIG_CTL1     0x54
#define IP6303_CHG_DIG_CTL2     0x55
#define IP6303_ADC_DATA_VBAT    0x64
#define IP6303_INT_FLAG         0x71
#define IP6303_LEVEL_STATUS     0x72

#define IP6303_PWRKEY_UP_BIT    0x20    /* 0x72 bit5: high = key not pressed */

static void ip6303_log(Ip6303State *s, char rw, uint8_t reg, uint8_t val)
{
    if (getenv("MSTAR_PMIC_LOG")) {
        fprintf(stderr, "[ip6303] %c reg 0x%02x = 0x%02x\n", rw, reg, val);
        fflush(stderr);
    }
}

static int ip6303_event(I2CSlave *i2c, enum i2c_event event)
{
    Ip6303State *s = IP6303(i2c);

    if (event == I2C_START_SEND) {
        s->have_ptr = false;
    }
    return 0;
}

static int ip6303_send(I2CSlave *i2c, uint8_t data)
{
    Ip6303State *s = IP6303(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
        return 0;
    }
    ip6303_log(s, 'W', s->ptr, data);
    if (s->ptr == IP6303_INT_FLAG) {
        s->regs[s->ptr] &= ~data;               /* write-1-to-clear */
    } else {
        s->regs[s->ptr] = data;
    }
    s->ptr++;
    return 0;
}

static uint8_t ip6303_recv(I2CSlave *i2c)
{
    Ip6303State *s = IP6303(i2c);
    uint8_t data = s->regs[s->ptr];

    ip6303_log(s, 'R', s->ptr, data);
    s->ptr++;
    return data;
}

static void ip6303_reset(DeviceState *dev)
{
    Ip6303State *s = IP6303(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->ptr = 0;
    s->have_ptr = false;

    s->regs[IP6303_CHG_DIG_CTL1] = 5 << 5;      /* charge state: charge end */
    s->regs[IP6303_CHG_DIG_CTL2] = 1 << 3;      /* battery/ext power OK */
    s->regs[IP6303_ADC_DATA_VBAT] = 211;        /* ~3.8 V */
    s->regs[IP6303_LEVEL_STATUS] = IP6303_PWRKEY_UP_BIT;
}

static const VMStateDescription vmstate_ip6303 = {
    .name = "ip6303",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Ip6303State),
        VMSTATE_UINT8_ARRAY(regs, Ip6303State, 256),
        VMSTATE_UINT8(ptr, Ip6303State),
        VMSTATE_BOOL(have_ptr, Ip6303State),
        VMSTATE_END_OF_LIST()
    },
};

static void ip6303_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    device_class_set_legacy_reset(dc, ip6303_reset);
    dc->vmsd = &vmstate_ip6303;
    sc->event = ip6303_event;
    sc->send = ip6303_send;
    sc->recv = ip6303_recv;
}

static const TypeInfo ip6303_types[] = {
    {
        .name           = TYPE_IP6303,
        .parent         = TYPE_I2C_SLAVE,
        .instance_size  = sizeof(Ip6303State),
        .class_init     = ip6303_class_init,
    },
};

DEFINE_TYPES(ip6303_types)
