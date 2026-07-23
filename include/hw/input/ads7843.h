/*
 * TI ADS7843 touchscreen ADC on SSI
 */

#ifndef HW_INPUT_ADS7843_H
#define HW_INPUT_ADS7843_H

#include "hw/ssi/ssi.h"
#include "ui/input.h"
#include "qom/object.h"

#define TYPE_ADS7843 "ads7843"

typedef struct ADS7843State ADS7843State;
OBJECT_DECLARE_SIMPLE_TYPE(ADS7843State, ADS7843)

struct ADS7843State {
    /*< private >*/
    SSIPeripheral parent_obj;

    /*< public >*/
    QemuInputHandlerState *hs;

    /* pointer state, ADC scale (12 bit) */
    uint16_t x;
    uint16_t y;
    bool pen_down;

    /* board-specific idle levels */
    uint16_t battery;
    uint16_t dock;

    /* virtual taps on the silkscreen hotspots below the LCD */
    uint8_t silk_down;

    /* command byte being shifted in */
    uint8_t cmd;
    int cmdbits;
    /* response being shifted out, bits remaining */
    uint16_t outsr;
    int outbits;

    qemu_irq penirq;
};

#endif
