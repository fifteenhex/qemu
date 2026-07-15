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
#include "hw/ssi/ssi.h"
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
 * The "msc313e-timer": a free-running up-counter used as the clock source
 * and, on the other instances, as a clock event.
 */
#define TYPE_MSC313E_TIMER "mstar-msc313e-timer"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313eTimerState, MSC313E_TIMER)

struct Msc313eTimerState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;           /* counter-reached-MAX interrupt (INT_EN gated) */
    QEMUTimer *hrtimer;     /* fires in host time when the counter hits MAX */
    uint32_t freq;
    uint16_t ctrl;
    uint16_t divide;
    uint32_t max;
    int64_t base_ns;        /* virtual time the counter was (re)based */
    uint64_t base_count;    /* counter value at base_ns */
    uint32_t latch;         /* latched on a COUNTER_LOW read */
    bool int_pending;       /* the counter has reached MAX since the last ack */
};

#define MSTAR_NUM_TIMERS 3

/*
 * The "msc313-rtc": a free-running 1 Hz seconds counter with a match-based
 * alarm interrupt.
 */
#define TYPE_MSC313_RTC "mstar-msc313-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313RtcState, MSC313_RTC)

struct Msc313RtcState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint16_t ctrl;
    uint16_t status;
    uint32_t freq_cw;       /* clock divider written by the guest (unused) */
    uint32_t load_val;      /* seconds value to load into the counter */
    uint32_t match_val;     /* alarm match value */
    uint32_t cnt_latch;     /* counter latched on a READ_EN trigger */
    int64_t base_ns;        /* virtual time the counter was (re)based */
    uint32_t base_count;    /* counter value at base_ns */
};

/*
 * The "msc313-gpio": a bank of single-byte pad control registers. Each pad
 * offset holds IN (bit0, the pin level, read-only), OUT (bit4) and OEN
 * (bit5, output disable) bits. reg = <0x207800 0x200> on the riu bus.
 */
#define TYPE_MSC313_GPIO "mstar-msc313-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313GpioState, MSC313_GPIO)

#define MSTAR_GPIO_NUM_REGS 0x200

struct Msc313GpioState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint8_t regs[MSTAR_GPIO_NUM_REGS];
};

/*
 * The "isp" SPI-NOR controller (mstar,msc313-isp): a byte-at-a-time SPI
 * master plus a memory-mapped XIP read window. It drives an m25p80 SPI-NOR
 * flash over an SSI bus.
 */
#define TYPE_MSC313_ISP "mstar-msc313-isp"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313IspState, MSC313_ISP)

#define MSTAR_ISP_QSPI_NUM_REGS (0x200 / 4)
#define MSTAR_ISP_FSP_NUM_REGS  (0x200 / 4)

struct Msc313IspState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;     /* isp core registers @0x1f001000 */
    MemoryRegion fsp;       /* fsp flash controller  @0x1f002c00 */
    MemoryRegion qspi;      /* qspi config registers @0x1f002e00 */
    MemoryRegion xip;       /* memory-mapped read window @0x14000000 */
    SSIBus *spi;
    qemu_irq cs;            /* chip select to the flash (active low) */
    bool cs_asserted;
    uint16_t rdata;         /* last byte clocked in from the flash */
    uint8_t *flash_cache;   /* in-memory copy of the flash for XIP reads */
    uint16_t password;
    uint16_t clkdiv;
    uint16_t trigger;
    uint16_t rst;
    uint16_t qspi_regs[MSTAR_ISP_QSPI_NUM_REGS];
    uint16_t fsp_regs[MSTAR_ISP_FSP_NUM_REGS];
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

/*
 * The "l3bridge" MIU write-flush block. mstarv7_mb() triggers a flush and
 * spins on the STATUS DONE bit; since QEMU's memory is coherent the flush is
 * a no-op and STATUS always reports done.
 */
#define MSTAR_L3BRIDGE_BASE         (MSTAR_RIU_BASE + 0x204400)
#define MSTAR_L3BRIDGE_SIZE         0x200
#define MSTAR_L3BRIDGE_STATUS       0x40
#define MSTAR_L3BRIDGE_STATUS_DONE  (1 << 12)

/*
 * The msc313e-timer instances at 0x1f206040/80/c0, clocked from xtal_div2
 * (12MHz). Their interrupts are lines 0, 1 and 12 of the "fiq" mst-intc.
 */
#define MSTAR_TIMER_BASE            (MSTAR_RIU_BASE + 0x6040)
#define MSTAR_TIMER_STRIDE          0x40
#define MSTAR_TIMER_FREQ            12000000

/*
 * The "msc313-rtc" at 0x1f002400 (reg = <0x2400 0x40> on the riu bus). Its
 * alarm interrupt is line 44 of the "irq" mst-intc.
 */
#define MSTAR_RTC_BASE              (MSTAR_RIU_BASE + 0x2400)
#define MSTAR_RTC_SIZE              0x40
#define MSTAR_RTC_HWIRQ             44

/* The "msc313-gpio" pad register bank (reg = <0x207800 0x200>). */
#define MSTAR_GPIO_BASE             (MSTAR_RIU_BASE + 0x207800)
#define MSTAR_GPIO_SIZE             MSTAR_GPIO_NUM_REGS

/* The "fsp" flash controller (the ISP block's second register window). */
#define MSTAR_FSP_BASE          (MSTAR_RIU_BASE + 0x2c00)
#define MSTAR_FSP_SIZE          0x200

/*
 * The "isp" SPI-NOR controller: core regs @0x1f001000, qspi config regs
 * @0x1f002e00, and a 16 MiB memory-mapped XIP read window @0x14000000.
 */
#define MSTAR_ISP_BASE              (MSTAR_RIU_BASE + 0x1000)
#define MSTAR_ISP_SIZE              0x400
#define MSTAR_ISP_QSPI_BASE         (MSTAR_RIU_BASE + 0x2e00)
#define MSTAR_ISP_QSPI_SIZE         0x200
#define MSTAR_ISP_XIP_BASE          0x14000000
#define MSTAR_ISP_XIP_SIZE          0x1000000

#endif /* HW_ARM_MSTAR_H */
