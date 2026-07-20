/*
 * MStar/SigmaStar "infinity" PWM
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MSTAR_PWM_H
#define HW_MISC_MSTAR_PWM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_PWM "mstar-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(MStarPwmState, MSTAR_PWM)

/* The register window the device tree gives the block (reg = <base 0x600>) */
#define MSTAR_PWM_SIZE     0x600
#define MSTAR_PWM_NUM_REGS (MSTAR_PWM_SIZE / 4)

struct MStarPwmState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    uint16_t regs[MSTAR_PWM_NUM_REGS];
};

#endif /* HW_MISC_MSTAR_PWM_H */
