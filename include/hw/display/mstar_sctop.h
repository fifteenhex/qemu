/*
 * MStar/SigmaStar mercury5 SC_TOP - scaler/display-top interrupt bank
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_MSTAR_SCTOP_H
#define HW_DISPLAY_MSTAR_SCTOP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_MSTAR_SCTOP "mstar-sctop"
OBJECT_DECLARE_SIMPLE_TYPE(MstarScTopState, MSTAR_SCTOP)

#define MSTAR_SCTOP_REGSIZE 0x200
#define MSTAR_SCTOP_NWORDS  4

struct MstarScTopState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    QEMUTimer *vsync;

    uint16_t raw[MSTAR_SCTOP_NWORDS];       /* latched interrupt sources */
    uint16_t regs[MSTAR_SCTOP_REGSIZE / 4]; /* store/read-back for the rest */
};

#endif
