/*
 * MStar/SigmaStar timer
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_MSTAR_TIMER_H
#define HW_TIMER_MSTAR_TIMER_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_TIMER "mstar-timer"
OBJECT_DECLARE_SIMPLE_TYPE(MStarTimerState, MSTAR_TIMER)

struct MStarTimerState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;

    uint32_t freq;      /* input clock in Hz, from the "freq" property */

    uint16_t ctrl;
    uint16_t divide;
    uint32_t max;
    uint32_t latch;     /* counter high half, latched on a low read */
    int64_t base_ns;    /* virtual time the counter last (re)started */
};

#endif /* HW_TIMER_MSTAR_TIMER_H */
