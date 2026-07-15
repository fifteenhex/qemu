/*
 * MStar/SigmaStar Armv7 SoC family
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_MSTAR_H
#define HW_ARM_MSTAR_H

#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

/*
 * This header is the SoC's device inventory: the per-block QOM types and state
 * structs (implemented in hw/<subsystem>/mstar_*.c) plus the shared physical
 * memory map. It is deliberately free of target/arm includes so the device
 * models can live in the target-independent build; the ARM-specific SoC
 * container (MStarSoCState, which embeds the CPUs and GIC) lives in
 * hw/arm/mstar.c instead.
 */

/*
 * The "mst-intc" (also on MediaTek chips): a hierarchical interrupt
 * controller sitting between the peripherals and the GIC. It adds a per-line
 * mask and forwards each input to a GIC SPI (irq_start + line).
 */
#define TYPE_MST_INTC "mstar-mst-intc"
OBJECT_DECLARE_SIMPLE_TYPE(MstIntcState, MST_INTC)

#define MST_INTC_MAX_IRQS 64

struct MstIntcState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint32_t irq_start;                         /* GIC SPI of input 0 */
    uint32_t num_irqs;
    uint16_t mask[MST_INTC_MAX_IRQS / 16];
    uint16_t polarity[MST_INTC_MAX_IRQS / 16];
    uint64_t level;                             /* input line levels */
    qemu_irq irq_out[MST_INTC_MAX_IRQS];        /* to the GIC */
};

/*
 * Physical memory map shared by the MStar/SigmaStar Armv7 SoCs. The on-chip
 * peripherals live inside the "riu" register bus at 0x1f000000; DRAM is
 * mapped at 0x20000000.
 */
#define MSTAR_RIU_BASE          0x1f000000
#define MSTAR_PM_UART_BASE      (MSTAR_RIU_BASE + 0x221000)
#define MSTAR_PM_UART_REGSHIFT  3
#define MSTAR_PM_UART_CLK       172000000
#define MSTAR_PM_UART_HWIRQ     34      /* line on the "irq" mst-intc */

#define MSTAR_DRAM_BASE         0x20000000

/* GIC (arm,cortex-a7-gic), with 128 SPIs. */
#define MSTAR_GIC_NUM_SPI       128
#define MSTAR_GIC_DIST_BASE     0x16001000
#define MSTAR_GIC_CPU_BASE      0x16002000
#define MSTAR_GIC_HYP_BASE      0x16004000
#define MSTAR_GIC_VCPU_BASE     0x16006000

/* Architected timer frequency, from the arm,armv7-timer clock-frequency. */
#define MSTAR_ARCH_TIMER_FREQ   6000000

/* Architected timer PPIs, from the arm,armv7-timer node. */
#define MSTAR_GIC_PPI_HYPTIMER  10
#define MSTAR_GIC_PPI_VIRTTIMER 11
#define MSTAR_GIC_PPI_SECTIMER  13
#define MSTAR_GIC_PPI_PHYSTIMER 14

/*
 * The two mst-intc instances, from the mstar,irqs-map-range in the DT:
 * "irq" forwards to GIC SPI 32..95, "fiq" to GIC SPI 96..127.
 */
#define MSTAR_INTC_IRQ_BASE     (MSTAR_RIU_BASE + 0x201350)
#define MSTAR_INTC_IRQ_START    32
#define MSTAR_INTC_IRQ_NUM      64
#define MSTAR_INTC_FIQ_BASE     (MSTAR_RIU_BASE + 0x201310)
#define MSTAR_INTC_FIQ_START    96
#define MSTAR_INTC_FIQ_NUM      32

#endif /* HW_ARM_MSTAR_H */
