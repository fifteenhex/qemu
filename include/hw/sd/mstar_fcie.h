/*
 * MStar/SigmaStar FCIE SD/MMC host controller
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SD_MSTAR_FCIE_H
#define HW_SD_MSTAR_FCIE_H

#include "hw/core/sysbus.h"
#include "hw/sd/sd.h"
#include "qom/object.h"

#define TYPE_MSTAR_FCIE     "mstar-fcie"
OBJECT_DECLARE_SIMPLE_TYPE(MStarFcieState, MSTAR_FCIE)

#define TYPE_MSTAR_FCIE_BUS "mstar-fcie-bus"

#define MSTAR_FCIE_REGION_SIZE 0x100
#define MSTAR_FCIE_NUM_REGS    (MSTAR_FCIE_REGION_SIZE / 4)
#define MSTAR_FCIE_FIFO_BYTES  64

struct MStarFcieState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    SDBus sdbus;
    qemu_irq irq;

    uint16_t regs[MSTAR_FCIE_NUM_REGS];
    uint8_t fifo[MSTAR_FCIE_FIFO_BYTES];
    uint8_t last_cmd;       /* opcode of the in-flight command (auto-stop) */
};

#endif /* HW_SD_MSTAR_FCIE_H */
