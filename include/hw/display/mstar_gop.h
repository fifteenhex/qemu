/*
 * MStar/SigmaStar GOP (Graphics Output Plane)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_MSTAR_GOP_H
#define HW_DISPLAY_MSTAR_GOP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "qemu/timer.h"

typedef struct Msc313PwmState Msc313PwmState;

/*
 * Standalone GOP (Graphics Output Plane) - the MStar RGB overlay/primary plane
 * that scans a linear framebuffer out to a QEMU console (hw/display/mstar_gop.c).
 * The same IP appears on the SSD20xD (as the "disp" gop plane) and on mercury5
 * (the 70mai composites its UI through it), at the same base 0x1f246800 with the
 * same register layout, so it is modelled once here and instantiated per-SoC. It
 * is parametrised by properties: "regsize" (mmio window), "winoff" (byte offset
 * of the scanned-out window's registers within it, for SoCs whose GOP is one of
 * several instances), plus an optional PWM backlight link and a 180deg "flip".
 */
#define TYPE_MSTAR_GOP "mstar-gop"
OBJECT_DECLARE_SIMPLE_TYPE(MStarGopState, MSTAR_GOP)

struct MStarGopState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq gop_irq;           /* GOP/fbdev vsync interrupt */
    QemuConsole *con;
    QEMUTimer *vblank;
    QEMUTimer *gop_off;
    MemoryRegionSection fbsection;
    Msc313PwmState *backlight;  /* optional: PWM channel 0 dims the panel */
    uint32_t regsize;           /* property: mmio region size (bytes) */
    uint32_t winoff;            /* property: byte offset of the window regs */
    bool flip;                  /* property: rotate output 180deg */
    uint16_t *regs;             /* regsize/4 entries (16-bit lanes, 4-byte stride) */
    unsigned int brightness;
    uint32_t width, height;
    bool invalidate;
};

#endif /* HW_DISPLAY_MSTAR_GOP_H */
