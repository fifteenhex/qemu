/*
 * TI ADS7843 touchscreen ADC.
 *
 * Byte-oriented model: a command byte (start bit set) selects a
 * channel and starts a conversion; the 12-bit result is shifted out
 * over the next two bytes, one leading null bit first, as on the real
 * part.  The /PENIRQ output is presented as an active-high "pen down"
 * gpio line, ready for an interrupt controller input.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/input/ads7843.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "ui/input.h"
#include "ui/console.h"

#define ADS7843_CMD_START   0x80
#define ADS7843_CMD_CHANNEL(_c) (((_c) >> 4) & 7)
#define ADS7843_CMD_8BIT    0x08

/* differential-mode channel assignments */
#define ADS7843_CHANNEL_XPOS  1
#define ADS7843_CHANNEL_Z1    3
#define ADS7843_CHANNEL_Z2    4
#define ADS7843_CHANNEL_YPOS  5

#define ADS7843_MAX 0xfff

static uint16_t ads7843_sample(ADS7843State *s, int channel)
{
    switch (channel) {
    case ADS7843_CHANNEL_XPOS:
        /* with the pen up the position inputs float to the rail */
        return s->pen_down ? s->x : 0;
    case ADS7843_CHANNEL_YPOS:
        return s->pen_down ? s->y : 0;
    case ADS7843_CHANNEL_Z1:
        return s->pen_down ? 0x600 : 0;
    case ADS7843_CHANNEL_Z2:
        return s->pen_down ? 0x600 : ADS7843_MAX;
    case 2:
        return 0xbd0; /* ch2 */
    case 6:
        return 0xbd0; /* ch6 */
    default:
        /*
         * IN3/IN4: the Palm V measures its battery here.  Report a
         * healthy Li-ion voltage or PalmOS goes straight to sleep.
         */
        return 0xbd0;
    }
}

/*
 * One bit in, one bit out per call: the DragonBall SPI master runs
 * its bus in bitwise mode.  The part idles until a start bit
 * arrives, eats the 7 remaining command bits, then answers with one
 * null bit followed by the 12-bit conversion MSB-first, as real
 * silicon does.  A new start bit can arrive while data is still
 * being shifted out.
 */
static uint32_t ads7843_transfer(SSIPeripheral *dev, uint32_t value)
{
    ADS7843State *s = ADS7843(dev);
    uint32_t out = 0;

    /* shift the response, if any is left */
    if (s->outbits > 0) {
        s->outbits--;
        out = (s->outsr >> s->outbits) & 1;
    }

    if (s->cmdbits == 0) {
        /* waiting for a start bit */
        if (value & 1) {
            s->cmd = 1;
            s->cmdbits = 1;
        }
    } else {
        s->cmd = (s->cmd << 1) | (value & 1);
        if (++s->cmdbits == 8) {
            uint16_t sample = ads7843_sample(s, ADS7843_CMD_CHANNEL(s->cmd));

            if (s->cmd & ADS7843_CMD_8BIT) {
                sample &= 0xff0;
            }
            /* null bit + 12 data bits */
            s->outsr = sample;
            s->outbits = 13;
            s->cmdbits = 0;
        }
    }

    return out;
}

static void ads7843_input_event(DeviceState *dev, QemuConsole *src,
                                QemuInputEvent *evt)
{
    ADS7843State *s = ADS7843(dev);

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS: {
        /*
         * Produce what a real Palm V panel produces, so that the
         * PalmOS *default* pen calibration (screenX = 0.72*raw8 - 2,
         * screenY = raw8 - 3, measured against the OS 3.1 ROM) maps
         * the pen exactly onto the pixel being pointed at.
         */
        int val = qemu_input_scale_axis(evt->abs.value,
                                        INPUT_EVENT_ABS_MIN,
                                        INPUT_EVENT_ABS_MAX,
                                        0, 159);
        if (evt->abs.axis == INPUT_AXIS_X) {
            s->x = ((val * 1387) / 1000 + 3) << 4;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            s->y = (val + 3) << 4;
        }
        break;
    }
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            s->pen_down = evt->btn.down;
        }
        break;
    default:
        break;
    }
}

static void ads7843_input_sync(DeviceState *dev)
{
    ADS7843State *s = ADS7843(dev);

    qemu_set_irq(s->penirq, s->pen_down);
}

static const QemuInputHandler ads7843_handler = {
    .name = TYPE_ADS7843,
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = ads7843_input_event,
    .sync = ads7843_input_sync,
};

static void ads7843_realize(SSIPeripheral *d, Error **errp)
{
    DeviceState *dev = DEVICE(d);
    ADS7843State *s = ADS7843(d);

    qdev_init_gpio_out_named(dev, &s->penirq, "penirq", 1);

    s->hs = qemu_input_handler_register(dev, &ads7843_handler);
    qemu_input_handler_activate(s->hs);
}

static const VMStateDescription vmstate_ads7843 = {
    .name = "ads7843",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, ADS7843State),
        VMSTATE_UINT16(x, ADS7843State),
        VMSTATE_UINT16(y, ADS7843State),
        VMSTATE_BOOL(pen_down, ADS7843State),
        VMSTATE_UINT8(cmd, ADS7843State),
        VMSTATE_INT32(cmdbits, ADS7843State),
        VMSTATE_UINT16(outsr, ADS7843State),
        VMSTATE_INT32(outbits, ADS7843State),
        VMSTATE_END_OF_LIST()
    }
};

static void ads7843_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->realize = ads7843_realize;
    k->transfer = ads7843_transfer;
    /* the Palm V keeps /CS grounded; the part is always listening */
    k->cs_polarity = SSI_CS_NONE;
    dc->vmsd = &vmstate_ads7843;
}

static const TypeInfo ads7843_info = {
    .name          = TYPE_ADS7843,
    .parent        = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(ADS7843State),
    .class_init    = ads7843_class_init,
};

static void ads7843_register_types(void)
{
    type_register_static(&ads7843_info);
}

type_init(ads7843_register_types)
