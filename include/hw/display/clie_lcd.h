/*
 * Sony CLIE color HiRes companion LCD controller ("MediaQ 1100/1132"
 * class chip, as used on the color VZ328 CLIEs -- N600C/N700C/T600C/
 * T625C/T650C and, per this tree's CLIE-RESEARCH.md/CLIE-POC-NOTES.md,
 * the SJ-series "Modena"/"YellowStone" board family).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HW_DISPLAY_CLIE_LCD_H
#define HW_DISPLAY_CLIE_LCD_H

#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_CLIE_LCD "clie-lcd"
OBJECT_DECLARE_SIMPLE_TYPE(ClieLcdState, CLIE_LCD)

/* Register file: Cloudpilot-emu's HwrMediaQ11xxType is 0x2000 bytes. */
#define CLIE_LCD_REGS_SIZE    0x2000
/*
 * Video aperture ("T_BASE" window): 256KB by default, mapped right
 * before the register window (which sits at T_BASE +
 * CLIE_LCD_REGS_OFFSET, i.e. Cloudpilot's "MMIO_BASE" == T_BASE +
 * MMIO_OFFSET).  The SZ328 CLIEs (NR70/SJ33) carry the "T2" variant
 * of the chip with 320KB of VRAM and the register window at +0x50000
 * (Cloudpilot's EmRegsMQLCDControlT2) -- same GC register set; those
 * machines set the "vram-size" property and map the windows to match.
 */
#define CLIE_LCD_VMEM_SIZE     0x40000
#define CLIE_LCD_REGS_OFFSET   0x40000
#define CLIE_LCD_T2_VMEM_SIZE  0x50000
#define CLIE_LCD_T2_REGS_OFFSET 0x50000

typedef struct ClieLcdState {
    SysBusDevice parent_obj;

    MemoryRegion vmem_mr;
    MemoryRegion regs_mr;
    QemuConsole *con;
    uint32_t vmem_size;

    uint8_t regs[CLIE_LCD_REGS_SIZE];
} ClieLcdState;

#endif
