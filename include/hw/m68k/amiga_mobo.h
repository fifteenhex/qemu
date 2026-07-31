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

/*
 * Size of the vacant-slot region (sysbus MMIO 1): accesses read open
 * bus and latch Gary's bus timeout.  Boards map it over address space
 * where firmware distinguishes "chip absent" from "chip present" by
 * the timeout, e.g. the A4000T's onboard-SCSI page.
 */
#define AMIGA_MOBO_TIMEOUT_SLOT_SIZE    0x10000

struct AmigaMoboState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion timeout_slot;
    uint8_t ramsey_version;     /* value the Ramsey version register reports */
    uint8_t ramsey_ctrl;
    uint8_t gary[3];
    uint8_t gary_timeout_ctrl;
    bool gary_timeout;          /* a watched access timed out */
};

#endif
