/*
 * Configurable MStar/SigmaStar camera-module sensor (I2C / SCCB slave)
 *
 * A generic image-sensor model for reverse-engineering the sensor bring-up
 * sequences that different camera modules use on these SoCs. It ACKs SCCB
 * reads/writes at a configurable I2C address, auto-increments the register
 * pointer, stores writes and reads them back, and logs every register access
 * so the firmware's per-sensor register program becomes visible.
 *
 * Configure per module (qdev properties, or -global):
 *   address    I2C slave address        (default 0x36, e.g. Sony IMX323)
 *   reg-bytes  register-pointer width   (1 = 8-bit, 2 = 16-bit; default 2 -
 *              IMX323 / OV / most modern sensors use 16-bit register addresses)
 *   id-reg     chip-id register offset  (probe register)
 *   id-val     value returned at id-reg (so the firmware's probe matches)
 *
 * Set the environment variable MSTAR_SENSOR_LOG=1 to print every access, i.e.
 * the exact register program the firmware writes to configure the module.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"

#define TYPE_MSTAR_CAM_SENSOR "mstar-cam-sensor"
#define TYPE_IMX323 "imx323"        /* Sony IMX323 preset (also in mstar.h) */
OBJECT_DECLARE_SIMPLE_TYPE(MstarCamSensorState, MSTAR_CAM_SENSOR)

#define SENSOR_REGSPACE 0x10000

struct MstarCamSensorState {
    I2CSlave parent_obj;

    uint8_t reg_bytes;          /* 1 or 2 address bytes */
    uint32_t id_reg;
    uint32_t id_val;

    uint8_t *regs;              /* SENSOR_REGSPACE bytes */
    uint32_t ptr;               /* current register pointer */
    int addr_pos;               /* address bytes received so far this write */
    uint32_t addr_acc;
};

static void cam_log(MstarCamSensorState *s, char rw, uint32_t reg, uint8_t val)
{
    if (getenv("MSTAR_SENSOR_LOG")) {
        fprintf(stderr, "[sensor %02x] %c reg 0x%0*x = 0x%02x\n",
                I2C_SLAVE(s)->address, rw, s->reg_bytes * 2, reg, val);
        fflush(stderr);
    }
}

static int mstar_cam_sensor_event(I2CSlave *i2c, enum i2c_event event)
{
    MstarCamSensorState *s = MSTAR_CAM_SENSOR(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->addr_pos = 0;
        s->addr_acc = 0;
        break;
    case I2C_START_RECV:
    case I2C_FINISH:
    case I2C_NACK:
        break;
    default:
        break;
    }
    return 0;
}

static int mstar_cam_sensor_send(I2CSlave *i2c, uint8_t data)
{
    MstarCamSensorState *s = MSTAR_CAM_SENSOR(i2c);

    if (s->addr_pos < s->reg_bytes) {
        /* Building the register-address pointer (MSB first). */
        s->addr_acc = (s->addr_acc << 8) | data;
        if (++s->addr_pos == s->reg_bytes) {
            s->ptr = s->addr_acc & (SENSOR_REGSPACE - 1);
        }
        return 0;
    }
    /* Register data, auto-incrementing pointer. */
    cam_log(s, 'W', s->ptr, data);
    s->regs[s->ptr] = data;
    s->ptr = (s->ptr + 1) & (SENSOR_REGSPACE - 1);
    return 0;
}

static uint8_t mstar_cam_sensor_recv(I2CSlave *i2c)
{
    MstarCamSensorState *s = MSTAR_CAM_SENSOR(i2c);
    uint8_t val = s->regs[s->ptr];

    cam_log(s, 'R', s->ptr, val);
    s->ptr = (s->ptr + 1) & (SENSOR_REGSPACE - 1);
    return val;
}

static void mstar_cam_sensor_reset_hold(Object *obj, ResetType type)
{
    MstarCamSensorState *s = MSTAR_CAM_SENSOR(obj);

    memset(s->regs, 0, SENSOR_REGSPACE);
    s->ptr = 0;
    s->addr_pos = 0;
    /* Pre-load the chip id so the firmware's probe/read matches (big-endian). */
    if (s->id_val) {
        s->regs[s->id_reg & (SENSOR_REGSPACE - 1)] = (s->id_val >> 8) & 0xff;
        s->regs[(s->id_reg + 1) & (SENSOR_REGSPACE - 1)] = s->id_val & 0xff;
    }
}

static void mstar_cam_sensor_realize(DeviceState *dev, Error **errp)
{
    MstarCamSensorState *s = MSTAR_CAM_SENSOR(dev);

    if (s->reg_bytes != 1 && s->reg_bytes != 2) {
        s->reg_bytes = 2;
    }
    s->regs = g_malloc0(SENSOR_REGSPACE);
}

static const Property mstar_cam_sensor_props[] = {
    DEFINE_PROP_UINT8("reg-bytes", MstarCamSensorState, reg_bytes, 2),
    DEFINE_PROP_UINT32("id-reg", MstarCamSensorState, id_reg, 0x3000),
    DEFINE_PROP_UINT32("id-val", MstarCamSensorState, id_val, 0),
};

static void mstar_cam_sensor_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_cam_sensor_realize;
    device_class_set_props(dc, mstar_cam_sensor_props);
    rc->phases.hold = mstar_cam_sensor_reset_hold;
    sc->event = mstar_cam_sensor_event;
    sc->send = mstar_cam_sensor_send;
    sc->recv = mstar_cam_sensor_recv;
}

/*
 * Sony IMX323 (1/2.9" 1080p) - the sensor on the MSC313E camera boards this
 * emulates. It is just mstar-cam-sensor with the IMX323's defaults: 16-bit
 * register addresses, and the chip-id register 0x301c reading 0x50 (from the
 * vendor sensor driver's Sensor_id_table: reg 0x301c == 0x50). Reusable as
 * `-device imx323` or attached by any MStar camera board. Its full power-on
 * register program (Sensor_init_table, 52 regs) is store/read-back here; the
 * driver writes it and reads it back to verify, which just works.
 */
/*
 * The IMX323's id registers are single bytes, so preset them directly rather
 * than via the generic (16-bit) id-reg/id-val. Two drivers probe two different
 * registers, so answer both:
 *   - reg 0x301c == 0x50  (vendor MStar libdrv_ms_cus_imx323 Sensor_id_table)
 *   - reg 0x0112 == 0x0a  (mainline imx323.c IMX323_REG_CHIP_ID)
 */
static void imx323_reset_hold(Object *obj, ResetType type)
{
    MstarCamSensorState *s = MSTAR_CAM_SENSOR(obj);

    memset(s->regs, 0, SENSOR_REGSPACE);
    s->ptr = 0;
    s->addr_pos = 0;
    s->regs[0x301c] = 0x50;
    s->regs[0x0112] = 0x0a;
}

static void imx323_realize(DeviceState *dev, Error **errp)
{
    MstarCamSensorState *s = MSTAR_CAM_SENSOR(dev);

    s->reg_bytes = 2;               /* 16-bit register addresses */
    mstar_cam_sensor_realize(dev, errp);
}

static void imx323_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = imx323_realize;   /* IMX323 preset over the generic sensor */
    rc->phases.hold = imx323_reset_hold;
}

static const TypeInfo mstar_cam_sensor_types[] = {
    {
        .name           = TYPE_MSTAR_CAM_SENSOR,
        .parent         = TYPE_I2C_SLAVE,
        .instance_size  = sizeof(MstarCamSensorState),
        .class_init     = mstar_cam_sensor_class_init,
    },
    {
        .name           = TYPE_IMX323,
        .parent         = TYPE_MSTAR_CAM_SENSOR,
        .class_init     = imx323_class_init,
    },
};

DEFINE_TYPES(mstar_cam_sensor_types)
