/*
 * SigmaStar SSD202D SoC
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_SSD202D_H
#define HW_ARM_SSD202D_H

#include "qemu/units.h"
#include "system/memory.h"
#include "hw/arm/infinity2m.h"

#define TYPE_SSD202D_SOC "ssd202d-soc"
OBJECT_DECLARE_TYPE(SSD202DSoCState, SSD202DSoCClass, SSD202D_SOC)

/* The SSD202D has 128 MiB of DDR3 in the package, wired to MIU0 */
#define SSD202D_DRAM_SIZE (128 * MiB)

/*
 * The mask ROM the boot core starts running at reset. The dump is
 * 16 KiB; it is modelled at the reset vector but whether the ROM
 * really sits at 0x0 or is aliased there has not been confirmed.
 */
#define SSD202D_BOOTROM_BASE     0x00000000
#define SSD202D_BOOTROM_SIZE     (16 * KiB)
#define SSD202D_BOOTROM_FILENAME "ssd202d_bootrom.bin"

struct SSD202DSoCState {
    /*< private >*/
    Infinity2MSoCState parent_obj;
    /*< public >*/

    MemoryRegion bootrom;
};

struct SSD202DSoCClass {
    /*< private >*/
    Infinity2MSoCClass parent_class;
    /*< public >*/

    DeviceRealize parent_realize;
};

#endif /* HW_ARM_SSD202D_H */
