/*
 * MStar/SigmaStar display controller (GOP + display top)
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_MSTAR_DISP_H
#define HW_DISPLAY_MSTAR_DISP_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_MSTAR_DISP "mstar-disp"
OBJECT_DECLARE_SIMPLE_TYPE(MStarDispState, MSTAR_DISP)

#define MSTAR_DISP_GOP_SIZE 0x400
#define MSTAR_DISP_TOP_SIZE 0x200
#define MSTAR_DISP_MOP_SIZE 0x600
#define MSTAR_DISP_GOP_NUM_REGS (MSTAR_DISP_GOP_SIZE / 4)
#define MSTAR_DISP_TOP_NUM_REGS (MSTAR_DISP_TOP_SIZE / 4)
#define MSTAR_DISP_MOP_NUM_REGS (MSTAR_DISP_MOP_SIZE / 4)

struct MStarDispState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion gop;
    MemoryRegion top;
    MemoryRegion mop;
    QemuConsole *con;
    QEMUTimer *vblank;
    QEMUTimer *gop_pulse;   /* one-shot: end of the GOP vsync pulse */
    qemu_irq top_irq;       /* display-top vsync */
    qemu_irq gop_irq;       /* GOP/fbdev vsync */

    uint32_t width;
    uint32_t height;
    bool invalidate;
    bool flip;              /* panel mounted 180 degrees ("flip" property) */

    uint16_t gopregs[MSTAR_DISP_GOP_NUM_REGS];
    uint16_t topregs[MSTAR_DISP_TOP_NUM_REGS];
    uint16_t mopregs[MSTAR_DISP_MOP_NUM_REGS];
};

#endif /* HW_DISPLAY_MSTAR_DISP_H */
