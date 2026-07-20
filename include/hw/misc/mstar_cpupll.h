/*
 * MStar/SigmaStar CPU PLL
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MSTAR_CPUPLL_H
#define HW_MISC_MSTAR_CPUPLL_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_CPUPLL "mstar-cpupll"
OBJECT_DECLARE_SIMPLE_TYPE(MStarCpupllState, MSTAR_CPUPLL)

#define MSTAR_CPUPLL_SIZE     0x200
#define MSTAR_CPUPLL_NUM_REGS (MSTAR_CPUPLL_SIZE / 4)

struct MStarCpupllState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    uint16_t regs[MSTAR_CPUPLL_NUM_REGS];
};

#endif /* HW_MISC_MSTAR_CPUPLL_H */
