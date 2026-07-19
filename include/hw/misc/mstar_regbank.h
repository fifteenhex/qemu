/*
 * MStar/SigmaStar generic readback register bank
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MSTAR_REGBANK_H
#define HW_MISC_MSTAR_REGBANK_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_REGBANK "mstar-regbank"
OBJECT_DECLARE_SIMPLE_TYPE(MStarRegbankState, MSTAR_REGBANK)

#define MSTAR_REGBANK_SIZE     0x200
#define MSTAR_REGBANK_NUM_REGS (MSTAR_REGBANK_SIZE / 4)

struct MStarRegbankState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    uint16_t regs[MSTAR_REGBANK_NUM_REGS];
};

#endif /* HW_MISC_MSTAR_REGBANK_H */
