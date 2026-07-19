/*
 * MStar/SigmaStar FSP flash sequencer
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_MSTAR_FSP_H
#define HW_SSI_MSTAR_FSP_H

#include "qemu/units.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_FSP "mstar-fsp"
OBJECT_DECLARE_SIMPLE_TYPE(MStarFspState, MSTAR_FSP)

#define MSTAR_FSP_REGION_SIZE 0x200
#define MSTAR_FSP_NUM_REGS    (MSTAR_FSP_REGION_SIZE / 4)

/* The size of the memory mapped XIP read window */
#define MSTAR_FSP_XIP_SIZE    (16 * MiB)

struct MStarFspState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    MemoryRegion xip;
    BlockBackend *blk;
    uint16_t regs[MSTAR_FSP_NUM_REGS];
};

#endif /* HW_SSI_MSTAR_FSP_H */
