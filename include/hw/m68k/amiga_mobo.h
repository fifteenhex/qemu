/*
 * Amiga big-box motherboard glue: Ramsey memory controller + Fat Gary.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_M68K_AMIGA_MOBO_H
#define HW_M68K_AMIGA_MOBO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_AMIGA_MOBO "amiga-mobo"
OBJECT_DECLARE_SIMPLE_TYPE(AmigaMoboState, AMIGA_MOBO)

struct AmigaMoboState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t ramsey_version;     /* value the Ramsey version register reports */
    uint8_t ramsey_ctrl;
    uint8_t gary[3];
};

#endif
