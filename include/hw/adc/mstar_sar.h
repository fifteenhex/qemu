/*
 * MStar/SigmaStar SAR ADC
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ADC_MSTAR_SAR_H
#define HW_ADC_MSTAR_SAR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_SAR "mstar-sar"
OBJECT_DECLARE_SIMPLE_TYPE(MStarSarState, MSTAR_SAR)

#define MSTAR_SAR_REGION_SIZE 0x200
#define MSTAR_SAR_NUM_REGS    (MSTAR_SAR_REGION_SIZE / 4)
#define MSTAR_SAR_CHANNELS    8

struct MStarSarState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    qemu_irq irq;

    uint16_t regs[MSTAR_SAR_NUM_REGS];
    /* Synthesised sample per channel ("channelN" properties) */
    uint16_t chan_input[MSTAR_SAR_CHANNELS];
};

#endif /* HW_ADC_MSTAR_SAR_H */
