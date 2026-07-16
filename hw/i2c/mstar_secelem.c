/*
 * MStar/SigmaStar board "security element" - dummy i2c slave
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Miyoo Mini (SSD202D) carries a small auth/security chip on i2c1 at
 * address 0x3d. The vendor 4.9 kernel's client driver dereferences a NULL
 * pointer - killing PID 1 and panicking - when that chip does not respond, so
 * an otherwise-working boot never reaches userspace. We do not know (or need)
 * the real crypto protocol; this slave simply ACKs every transfer and returns
 * a canned, non-zero response so the driver's probe/handshake succeeds. The
 * bytes the host writes are captured (and traced) so the exchange can be
 * studied and the responses refined later if a real protocol is wanted.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/mstar.h"
#include "trace.h"

static int mstar_secelem_event(I2CSlave *i2c, enum i2c_event event)
{
    MstarSecElemState *s = MSTAR_SECELEM(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->reading = false;
        s->cmd_len = 0;
        trace_mstar_secelem_event("START_SEND");
        break;
    case I2C_START_RECV:
        s->reading = true;
        s->resp_pos = 0;
        trace_mstar_secelem_event("START_RECV");
        break;
    case I2C_FINISH:
        trace_mstar_secelem_event("FINISH");
        break;
    case I2C_NACK:
        trace_mstar_secelem_event("NACK");
        break;
    default:
        break;
    }
    return 0;                           /* never NAK */
}

static int mstar_secelem_send(I2CSlave *i2c, uint8_t data)
{
    MstarSecElemState *s = MSTAR_SECELEM(i2c);

    if (s->cmd_len < MSTAR_SECELEM_BUFSZ) {
        s->cmd[s->cmd_len] = data;
    }
    trace_mstar_secelem_send(s->cmd_len, data);
    s->cmd_len++;
    return 0;                           /* ACK */
}

static uint8_t mstar_secelem_recv(I2CSlave *i2c)
{
    MstarSecElemState *s = MSTAR_SECELEM(i2c);
    uint8_t data;

    /*
     * Canned reply. Returning a stream of a fixed non-zero byte is enough for
     * the vendor driver to consider the device present and get a valid pointer
     * back; the first byte often reads as a status/length, so keep it non-zero.
     */
    data = 0xa5;
    trace_mstar_secelem_recv(s->resp_pos, data);
    s->resp_pos++;
    return data;
}

static void mstar_secelem_reset(DeviceState *dev)
{
    MstarSecElemState *s = MSTAR_SECELEM(dev);

    s->reading = false;
    s->cmd_len = 0;
    s->resp_pos = 0;
}

static void mstar_secelem_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_secelem_reset);
    sc->event = mstar_secelem_event;
    sc->send = mstar_secelem_send;
    sc->recv = mstar_secelem_recv;
}

static const TypeInfo mstar_secelem_types[] = {
    {
        .name           = TYPE_MSTAR_SECELEM,
        .parent         = TYPE_I2C_SLAVE,
        .instance_size  = sizeof(MstarSecElemState),
        .class_init     = mstar_secelem_class_init,
    },
};

DEFINE_TYPES(mstar_secelem_types)
