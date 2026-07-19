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
#include "hw/arm/infinity2m.h"

#define TYPE_SSD202D_SOC "ssd202d-soc"
OBJECT_DECLARE_TYPE(SSD202DSoCState, SSD202DSoCClass, SSD202D_SOC)

/* The SSD202D has 128 MiB of DDR3 in the package, wired to MIU0 */
#define SSD202D_DRAM_SIZE (128 * MiB)

struct SSD202DSoCState {
    /*< private >*/
    Infinity2MSoCState parent_obj;
    /*< public >*/
};

struct SSD202DSoCClass {
    /*< private >*/
    Infinity2MSoCClass parent_class;
    /*< public >*/
};

#endif /* HW_ARM_SSD202D_H */
