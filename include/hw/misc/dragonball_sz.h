/*
 * Motorola/Freescale MC68SZ328 "DragonBall Super VZ" on-chip register
 * block + enhanced TFT color LCD controller.
 *
 * The SZ328 is the SoC of the Sony CLIE PEG-NR70/NR70V and PEG-SJ33.
 * Unlike the EZ/VZ DragonBalls (whose on-chip peripherals live at
 * 0xfffffxxx), the SZ moves its entire register window to 0xFFFE0000
 * ("different than in previous chips!!!" -- Cloudpilot-emu's
 * EmRegsSZPrv.h, kMemoryStart = 0xFFFE0000).  This single device models
 * that whole window as one MMIO region and additionally scans out the
 * on-chip LCD controller's framebuffer (which lives in main DRAM at the
 * address the guest programs into lcdStartAddr).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HW_MISC_DRAGONBALL_SZ_H
#define HW_MISC_DRAGONBALL_SZ_H

#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_SZ "dragonball-sz"
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallSZState, DRAGONBALL_SZ)

/* Register window base and size (0xFFFE0000 .. 0xFFFF0E00-ish). */
#define DRAGONBALL_SZ_BASE      0xFFFE0000
#define DRAGONBALL_SZ_REGS_SIZE 0x11000

struct DragonBallSZState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    MemoryRegion *fbmem;        /* main system memory, for LCD scanout */
    QemuConsole *con;

    uint8_t regs[DRAGONBALL_SZ_REGS_SIZE];
};

#endif
