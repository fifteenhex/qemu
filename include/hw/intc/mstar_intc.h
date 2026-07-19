/*
 * MStar/SigmaStar mst-intc interrupt controller
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INTC_MSTAR_INTC_H
#define HW_INTC_MSTAR_INTC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_INTC "mstar-intc"
OBJECT_DECLARE_SIMPLE_TYPE(MStarIntcState, MSTAR_INTC)

#define MSTAR_INTC_MAX_IRQS    64
#define MSTAR_INTC_REGION_SIZE 0x40

struct MStarIntcState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;

    uint32_t num_irqs;          /* "num-irqs" property */
    uint16_t mask[MSTAR_INTC_MAX_IRQS / 16];
    uint16_t polarity[MSTAR_INTC_MAX_IRQS / 16];
    uint64_t level;             /* current input line levels */
    qemu_irq irq_out[MSTAR_INTC_MAX_IRQS];
};

#endif /* HW_INTC_MSTAR_INTC_H */
