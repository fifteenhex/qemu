/*
 * MStar/SigmaStar GE (2D graphics engine)
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_MSTAR_GE_H
#define HW_DISPLAY_MSTAR_GE_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_GE "mstar-ge"
OBJECT_DECLARE_SIMPLE_TYPE(MStarGeState, MSTAR_GE)

#define MSTAR_GE_SIZE     0x200
#define MSTAR_GE_NUM_REGS (MSTAR_GE_SIZE / 4)

struct MStarGeState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    /* Physical base of DRAM; the GE's surface addresses are MIU offsets */
    uint32_t dram_base;

    uint16_t regs[MSTAR_GE_NUM_REGS];
};

#endif /* HW_DISPLAY_MSTAR_GE_H */
