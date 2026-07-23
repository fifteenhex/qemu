/*
 * Amiga keyboard: feeds raw keycodes into CIA-A's serial port.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_M68K_AMIGA_KBD_H
#define HW_M68K_AMIGA_KBD_H

#include "hw/core/sysbus.h"
#include "hw/m68k/mos8520.h"
#include "qemu/timer.h"
#include "ui/input.h"
#include "qom/object.h"

#define TYPE_AMIGA_KBD "amiga-kbd"
OBJECT_DECLARE_SIMPLE_TYPE(AmigaKbdState, AMIGA_KBD)

#define AMIGA_KBD_FIFO 256

struct AmigaKbdState {
    SysBusDevice parent_obj;

    MOS8520State *cia;          /* CIA-A, whose SDR the keyboard clocks */
    QemuInputHandlerState *hs;

    uint8_t fifo[AMIGA_KBD_FIFO];
    unsigned head, tail;
    bool waiting;               /* a code is out, awaiting the ack pulse */
    QEMUTimer timer;
};

#endif
