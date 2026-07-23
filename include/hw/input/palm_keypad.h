/*
 * Palm hard-button matrix
 */

#ifndef HW_INPUT_PALM_KEYPAD_H
#define HW_INPUT_PALM_KEYPAD_H

#include "hw/core/qdev.h"
#include "ui/input.h"
#include "qom/object.h"

#define TYPE_PALM_KEYPAD "palm-keypad"

typedef struct PalmKeypadState PalmKeypadState;
OBJECT_DECLARE_SIMPLE_TYPE(PalmKeypadState, PALM_KEYPAD)

#define PALM_KEYPAD_ROWS 3
#define PALM_KEYPAD_COLS 4

struct PalmKeypadState {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    QemuInputHandlerState *hs;

    /* row select lines as driven by the SoC (high = deselected) */
    uint8_t row_high;
    /* pressed keys per row, as column bitmaps */
    uint8_t pressed[PALM_KEYPAD_ROWS];

    qemu_irq col_out[PALM_KEYPAD_COLS];
    qemu_irq any_out;
};

#endif
