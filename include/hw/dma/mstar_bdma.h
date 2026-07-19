/*
 * MStar/SigmaStar BDMA engine
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DMA_MSTAR_BDMA_H
#define HW_DMA_MSTAR_BDMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_BDMA "mstar-bdma"
OBJECT_DECLARE_SIMPLE_TYPE(MStarBdmaState, MSTAR_BDMA)

#define MSTAR_BDMA_NUM_CHANNELS 2
#define MSTAR_BDMA_CHAN_SIZE    0x40
#define MSTAR_BDMA_CHAN_NREGS   (MSTAR_BDMA_CHAN_SIZE / 4)
#define MSTAR_BDMA_REGION_SIZE  (MSTAR_BDMA_NUM_CHANNELS * \
                                 MSTAR_BDMA_CHAN_SIZE)

typedef struct MStarBdmaChan {
    uint16_t regs[MSTAR_BDMA_CHAN_NREGS];
    qemu_irq irq;
} MStarBdmaChan;

struct MStarBdmaState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    MStarBdmaChan chans[MSTAR_BDMA_NUM_CHANNELS];
};

#endif /* HW_DMA_MSTAR_BDMA_H */
