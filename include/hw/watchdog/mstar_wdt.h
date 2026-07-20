/*
 * MStar/SigmaStar watchdog timer
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_WATCHDOG_MSTAR_WDT_H
#define HW_WATCHDOG_MSTAR_WDT_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_MSTAR_WDT "mstar-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(MStarWdtState, MSTAR_WDT)

#define MSTAR_WDT_SIZE     0x40
#define MSTAR_WDT_NUM_REGS (MSTAR_WDT_SIZE / 4)

struct MStarWdtState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    QEMUTimer timer;
    uint32_t freq;      /* watchdog counter clock, Hz */

    uint16_t regs[MSTAR_WDT_NUM_REGS];
};

#endif /* HW_WATCHDOG_MSTAR_WDT_H */
