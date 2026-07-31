/*
 * Gayle, the Amiga 600/1200 gate array: the IDE interface.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_IDE_GAYLE_H
#define HW_IDE_GAYLE_H

#include "hw/core/qdev.h"
#include "system/blockdev.h"

#define TYPE_GAYLE_IDE "gayle-ide"

/*
 * Board bases for the three register blocks (sysbus MMIO 0/1/2) on the
 * A600/A1200; see hw/ide/gayle.c for the register layout.
 */
#define GAYLE_IDE_ATA_BASE      0xda0000
#define GAYLE_IDE_ATA_SIZE      0x4000
#define GAYLE_IDE_CTRL_BASE     0xda8000
#define GAYLE_IDE_CTRL_SIZE     0x4000
#define GAYLE_IDE_ID_BASE       0xde1000
#define GAYLE_IDE_ID_SIZE       0x1000

void gayle_ide_init_drives(DeviceState *dev, DriveInfo *hd0, DriveInfo *hd1);

#endif
