/*
 * MStar/SigmaStar GPIO (main and PM pad banks)
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_MSTAR_GPIO_H
#define HW_GPIO_MSTAR_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_GPIO "mstar-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(MStarGpioState, MSTAR_GPIO)

#define MSTAR_GPIO_BANK_SIZE 0x200
#define MSTAR_GPIO_NUM_PADS  (MSTAR_GPIO_BANK_SIZE / 4)

/* Input line names, one line per pad register (index = offset / 4) */
#define MSTAR_GPIO_MAIN_PAD  "main-pad"
#define MSTAR_GPIO_PM_PAD    "pm-pad"

struct MStarGpioState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion main;      /* main pad bank @0x1f207800 */
    MemoryRegion pm;        /* PM pad bank   @0x1f001e00 */

    uint8_t main_regs[MSTAR_GPIO_NUM_PADS];
    uint8_t pm_regs[MSTAR_GPIO_NUM_PADS];
    /* External level driven onto each pad (buttons, straps, ...) */
    uint32_t main_ext[MSTAR_GPIO_NUM_PADS / 32];
    uint32_t pm_ext[MSTAR_GPIO_NUM_PADS / 32];
};

#endif /* HW_GPIO_MSTAR_GPIO_H */
