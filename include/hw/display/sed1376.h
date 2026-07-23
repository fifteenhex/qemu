/*
 * Epson SED1376 embedded-memory LCD controller.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HW_DISPLAY_SED1376_H
#define HW_DISPLAY_SED1376_H

#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_SED1376 "sed1376"
OBJECT_DECLARE_SIMPLE_TYPE(SED1376State, SED1376)

#define SED1376_REGS_SIZE    0x100
/* the video SRAM decodes at this offset from the register block */
#define SED1376_VMEM_OFFSET  0x20000
#define SED1376_VMEM_SIZE    (80 * 1024)

typedef struct SED1376State {
    SysBusDevice parent_obj;

    MemoryRegion regs_mr;
    MemoryRegion vmem_mr;
    QemuConsole *con;

    uint8_t regs[SED1376_REGS_SIZE];
    uint32_t lut[256];          /* xrgb */
} SED1376State;

#endif
