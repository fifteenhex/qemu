/*
 * MStar/SigmaStar MIPI DSI controller
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_MSTAR_DSI_H
#define HW_DISPLAY_MSTAR_DSI_H

#include "hw/core/sysbus.h"
#include "hw/display/dsi.h"
#include "qom/object.h"

#define TYPE_MSTAR_DSI "mstar-dsi"
OBJECT_DECLARE_SIMPLE_TYPE(MStarDsiState, MSTAR_DSI)

#define MSTAR_DSI_REGION_SIZE 0x400
#define MSTAR_DSI_NUM_REGS    (MSTAR_DSI_REGION_SIZE / 4)

struct MStarDsiState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[MSTAR_DSI_NUM_REGS];
    DsiPanel *panel;        /* the panel on the far end of the link */
};

#endif /* HW_DISPLAY_MSTAR_DSI_H */
