/*
 * MStar/SigmaStar infinity2m SoC family
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_INFINITY2M_H
#define HW_ARM_INFINITY2M_H

#include "qemu/units.h"
#include "hw/arm/mstarv7.h"

#define TYPE_INFINITY2M_SOC "infinity2m-soc"
OBJECT_DECLARE_TYPE(Infinity2MSoCState, Infinity2MSoCClass, INFINITY2M_SOC)

/* All infinity2m SoCs are dual core */
#define INFINITY2M_NUM_CPUS 2

/*
 * The boot ROM uses IMI addresses up to 0xa000f8a5 so the SRAM is at
 * least 64 KiB; the true size has not been confirmed.
 */
#define INFINITY2M_IMI_SIZE (64 * KiB)

struct Infinity2MSoCState {
    /*< private >*/
    MStarV7SoCState parent_obj;
    /*< public >*/
};

struct Infinity2MSoCClass {
    /*< private >*/
    MStarV7SoCClass parent_class;
    /*< public >*/
};

#endif /* HW_ARM_INFINITY2M_H */
