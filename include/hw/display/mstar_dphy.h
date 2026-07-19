/*
 * MStar/SigmaStar MIPI D-PHY
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_MSTAR_DPHY_H
#define HW_DISPLAY_MSTAR_DPHY_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_DPHY "mstar-dphy"
OBJECT_DECLARE_SIMPLE_TYPE(MStarDphyState, MSTAR_DPHY)

#define MSTAR_DPHY_REGION_SIZE 0x200
#define MSTAR_DPHY_NUM_REGS    (MSTAR_DPHY_REGION_SIZE / 4)

struct MStarDphyState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    uint16_t regs[MSTAR_DPHY_NUM_REGS];
};

#endif /* HW_DISPLAY_MSTAR_DPHY_H */
