/*
 * MStar/SigmaStar ARMv7 SoC base class
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_MSTARV7_H
#define HW_ARM_MSTARV7_H

#include "system/memory.h"
#include "target/arm/cpu.h"
#include "qom/object.h"

#define TYPE_MSTARV7_SOC "mstarv7-soc"
OBJECT_DECLARE_TYPE(MStarV7SoCState, MStarV7SoCClass, MSTARV7_SOC)

/* The largest core count of any MStar ARMv7 SoC (dual Cortex-A7) */
#define MSTARV7_SOC_MAX_CPUS 2

/*
 * Physical addresses shared by every MStar ARMv7 SoC.
 *
 * Source: mainline Linux arch/arm/boot/dts/mstar-v7.dtsi
 */
/* Start of DRAM, the MIU0 address space */
#define MSTARV7_MIU0_BASE       0x20000000
/* The RIU register bus, where almost all peripherals live */
#define MSTARV7_RIU_BASE        0x1f000000
#define MSTARV7_RIU_SIZE        0x00400000
/* Cortex-A7 PERIPHBASE (read back via CBAR); the GIC lives in here */
#define MSTARV7_PERIPHBASE      0x16000000
/* SCU/GIC/timer private region size, from the Cortex-A7 MPCore TRM */
#define MSTARV7_PERIPHBASE_SIZE 0x8000
/* IMI SRAM; the size varies between SoCs */
#define MSTARV7_IMI_BASE        0xa0000000
/*
 * The PM UART, a 16550 with the registers on an 8 byte stride. The
 * boot ROM prints its messages here.
 */
#define MSTARV7_PM_UART_BASE    (MSTARV7_RIU_BASE + 0x221000)
#define MSTARV7_PM_UART_REGSHIFT 3
/* The real UART clock is unknown; this only affects the reported baud */
#define MSTARV7_PM_UART_BAUDBASE 115200

struct MStarV7SoCState {
    /*< private >*/
    DeviceState parent_obj;
    /*< public >*/

    ARMCPU cpus[MSTARV7_SOC_MAX_CPUS];
    MemoryRegion imi;
};

struct MStarV7SoCClass {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/

    /* Number of Cortex-A7 cores in this SoC */
    unsigned num_cpus;
    /* Size of the IMI SRAM */
    uint64_t imi_size;
};

#endif /* HW_ARM_MSTARV7_H */
