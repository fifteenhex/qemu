/*
 * Palm hard-button matrix.
 *
 * 3 rows x 4 columns.  The rows are GPIO outputs driven low to
 * select (all low while idle, so any key fires a column); the
 * columns read back active-high on port D bits 0-3, which double as
 * the INT0-3 interrupt pins, with their OR feeding the KB source.
 * The machine wires "rows" in from the SoC GPIO block and the
 * "cols"/"any" outputs back to it and to the interrupt controller.
 *
 * Host keys: F1-F4 = the four application buttons, Up/PageUp and
 * Down/PageDown = the scroll rocker, F5 = power, F6 = contrast.
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
#include "hw/core/qdev.h"
#include "hw/input/palm_keypad.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "ui/input.h"
#include "ui/console.h"

typedef struct PalmKeypadKey {
    QKeyCode qcode;
    uint8_t row;
    uint8_t col;
} PalmKeypadKey;

static const PalmKeypadKey palm_keypad_keys[] = {
    { Q_KEY_CODE_F1, 0, 0 },        /* Date Book  */
    { Q_KEY_CODE_F2, 0, 1 },        /* Address    */
    { Q_KEY_CODE_F3, 0, 2 },        /* To Do      */
    { Q_KEY_CODE_F4, 0, 3 },        /* Memo Pad   */
    { Q_KEY_CODE_UP, 1, 0 },        /* Page Up    */
    { Q_KEY_CODE_PGUP, 1, 0 },
    { Q_KEY_CODE_DOWN, 1, 1 },      /* Page Down  */
    { Q_KEY_CODE_PGDN, 1, 1 },
    { Q_KEY_CODE_F5, 2, 0 },        /* Power      */
    { Q_KEY_CODE_F6, 2, 1 },        /* Contrast   */
};

static void palm_keypad_update(PalmKeypadState *s)
{
    uint8_t cols = 0;
    int row, col;

    for (row = 0; row < PALM_KEYPAD_ROWS; row++) {
        /* rows select when driven low */
        if (!(s->row_high & (1 << row)))
            cols |= s->pressed[row];
    }

    for (col = 0; col < PALM_KEYPAD_COLS; col++)
        qemu_set_irq(s->col_out[col], (cols >> col) & 1);
    qemu_set_irq(s->any_out, cols != 0);
}

static void palm_keypad_row_set(void *opaque, int line, int value)
{
    PalmKeypadState *s = opaque;

    if (value)
        s->row_high |= 1 << line;
    else
        s->row_high &= ~(1 << line);

    palm_keypad_update(s);
}

static void palm_keypad_event(DeviceState *dev, QemuConsole *src,
                              QemuInputEvent *evt)
{
    PalmKeypadState *s = PALM_KEYPAD(dev);
    int qcode;
    int i;

    if (evt->type != INPUT_EVENT_KIND_KEY)
        return;

    qcode = qemu_input_linux_to_qcode(evt->key.key);

    for (i = 0; i < ARRAY_SIZE(palm_keypad_keys); i++) {
        if (palm_keypad_keys[i].qcode == qcode) {
            uint8_t mask = 1 << palm_keypad_keys[i].col;

            if (evt->key.down)
                s->pressed[palm_keypad_keys[i].row] |= mask;
            else
                s->pressed[palm_keypad_keys[i].row] &= ~mask;
        }
    }
}

static void palm_keypad_sync(DeviceState *dev)
{
    palm_keypad_update(PALM_KEYPAD(dev));
}

static const QemuInputHandler palm_keypad_handler = {
    .name = TYPE_PALM_KEYPAD,
    .mask = INPUT_EVENT_MASK_KEY,
    .event = palm_keypad_event,
    .sync = palm_keypad_sync,
};

static void palm_keypad_realize(DeviceState *dev, Error **errp)
{
    PalmKeypadState *s = PALM_KEYPAD(dev);

    qdev_init_gpio_in_named(dev, palm_keypad_row_set, "rows",
                            PALM_KEYPAD_ROWS);
    qdev_init_gpio_out_named(dev, s->col_out, "cols", PALM_KEYPAD_COLS);
    qdev_init_gpio_out_named(dev, &s->any_out, "any", 1);

    s->hs = qemu_input_handler_register(dev, &palm_keypad_handler);
    qemu_input_handler_activate(s->hs);
}

static const VMStateDescription vmstate_palm_keypad = {
    .name = "palm_keypad",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(row_high, PalmKeypadState),
        VMSTATE_UINT8_ARRAY(pressed, PalmKeypadState, PALM_KEYPAD_ROWS),
        VMSTATE_END_OF_LIST()
    }
};

static void palm_keypad_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = palm_keypad_realize;
    dc->vmsd = &vmstate_palm_keypad;
}

static const TypeInfo palm_keypad_info = {
    .name          = TYPE_PALM_KEYPAD,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(PalmKeypadState),
    .class_init    = palm_keypad_class_init,
};

static void palm_keypad_register_types(void)
{
    type_register_static(&palm_keypad_info);
}

type_init(palm_keypad_register_types)
