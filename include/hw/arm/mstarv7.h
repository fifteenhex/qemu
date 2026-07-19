/*
 * MStar/SigmaStar ARMv7 SoC base class
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_MSTARV7_H
#define HW_ARM_MSTARV7_H

#include "qemu/units.h"
#include "system/memory.h"
#include "hw/adc/mstar_sar.h"
#include "hw/cpu/a15mpcore.h"
#include "hw/dma/mstar_bdma.h"
#include "hw/i2c/mstar_i2c.h"
#include "hw/misc/mstar_regbank.h"
#include "hw/ssi/mstar_fsp.h"
#include "hw/timer/mstar_timer.h"
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
/*
 * Start of DRAM, the MIU0 address space. The window decodes 512 MiB;
 * smaller DRAM wraps within it, which is how the IPL sizes memory.
 */
#define MSTARV7_MIU0_BASE       0x20000000
#define MSTARV7_MIU0_WINDOW     (512 * MiB)
/* The RIU register bus, where almost all peripherals live */
#define MSTARV7_RIU_BASE        0x1f000000
#define MSTARV7_RIU_SIZE        0x00400000
/* Cortex-A7 PERIPHBASE (read back via CBAR); the GIC lives in here */
#define MSTARV7_PERIPHBASE      0x16000000
/* SCU/GIC/timer private region size, from the Cortex-A7 MPCore TRM */
#define MSTARV7_PERIPHBASE_SIZE 0x8000
/* GIC-400 SPI count, from the previous branch */
#define MSTARV7_GIC_NUM_SPI     128
/* IMI SRAM; the size varies between SoCs */
#define MSTARV7_IMI_BASE        0xa0000000
/*
 * The "DID" block. The boot ROM reads the boot-media strap from
 * DID_KEY bits[5:2] to decide where to load the IPL from.
 */
#define MSTARV7_DID_BASE        (MSTARV7_RIU_BASE + 0x7000)
#define MSTARV7_DID_SIZE        0x200
#define MSTARV7_DID_KEY         0x1c0
#define MSTARV7_BOOT_MEDIA_SPI_NOR      0x20
#define MSTARV7_BOOT_MEDIA_NAND         0x10
#define MSTARV7_BOOT_MEDIA_SPI_NAND     0x08
/*
 * The clkgen bank. Software programs clock gates/muxes here and
 * derives clock rates from reading them back, so it must retain what
 * is written; modelled as plain storage.
 */
#define MSTARV7_CLKGEN_BASE     (MSTARV7_RIU_BASE + 0x207000)
/* The two HWI2C masters (i2c@223000 and i2c@223200) */
#define MSTARV7_NUM_I2C         2
#define MSTARV7_I2C_BASE        (MSTARV7_RIU_BASE + 0x223000)
#define MSTARV7_I2C_STRIDE      0x200
/* The SAR ADC; boards hang keypads off its input channels */
#define MSTARV7_SAR_BASE        (MSTARV7_RIU_BASE + 0x2800)
/*
 * The ISP SPI NOR controller: the FSP command sequencer bank and the
 * memory mapped XIP read window the flash contents appear in.
 */
#define MSTARV7_FSP_BASE        (MSTARV7_RIU_BASE + 0x2c00)
#define MSTARV7_ISP_XIP_BASE    0x14000000
/* The BDMA engine; the boot ROM DMAs the IPL from flash into IMI */
#define MSTARV7_BDMA_BASE       (MSTARV7_RIU_BASE + 0x200400)
/*
 * The MIU DDR controller: an analog block (PLL/PHY), an arbiter and
 * a digital block. Modelled just enough for the IPL's DDR bring-up:
 * training and the memory BIST complete instantly.
 */
#define MSTARV7_MIU_BASE        (MSTARV7_RIU_BASE + 0x202000)
#define MSTARV7_MIU_SIZE        0x1000
#define MSTARV7_MIU_ANA_DDFSET_L        0x060
#define MSTARV7_MIU_ANA_DDFSET_H        0x064
#define MSTARV7_MIU_DIG_CNTRL0          0x400
#define MSTARV7_MIU_DIG_CNTRL0_INITDONE (1 << 15)
#define MSTARV7_MIU_DIG_BIST_CTRL       0x5c0
#define MSTARV7_MIU_DIG_BIST_DONE       (1 << 15)
/*
 * What the vendor IPL programs into the DDR PLL frequency-set pair;
 * reported back so the kernel's MIU clock driver computes a sane
 * DDR rate instead of dividing by zero.
 */
#define MSTARV7_MIU_DDFSET_L_VALUE      0x8000
#define MSTARV7_MIU_DDFSET_H_VALUE      0x0029
/*
 * The smpctrl secondary-core boot mailbox. On real hardware CPU1
 * runs the mask ROM and parks polling UNLOCK for the magic, then
 * jumps to the address in BOOT_LOW/HIGH; mainline and the vendor
 * kernel both post the entry address this way. In the model CPU1 is
 * powered off instead and the unlock powers it on at that address.
 * Software also spins reading back what it wrote here, so the bank
 * is a plain register file.
 */
#define MSTARV7_SMPCTRL_BASE        (MSTARV7_RIU_BASE + 0x204000)
#define MSTARV7_SMPCTRL_SIZE        0x200
#define MSTARV7_SMPCTRL_NUM_REGS    (MSTARV7_SMPCTRL_SIZE / 4)
#define MSTARV7_SMPCTRL_BOOT_HIGH   0x4c
#define MSTARV7_SMPCTRL_BOOT_LOW    0x50
#define MSTARV7_SMPCTRL_UNLOCK      0x58
#define MSTARV7_SMPCTRL_UNLOCK_MAGIC 0xbabe
/*
 * The l3bridge, used as a write barrier: software pokes the trigger
 * and polls the flush done bit, per mainline's mstarv7 barrier code.
 */
#define MSTARV7_L3BRIDGE_BASE   (MSTARV7_RIU_BASE + 0x204400)
#define MSTARV7_L3BRIDGE_SIZE   0x200
#define MSTARV7_L3BRIDGE_STATUS 0x40
#define MSTARV7_L3BRIDGE_STATUS_DONE (1 << 12)
/*
 * The three timers (timer@6040/6080/60c0 in the device trees). The
 * boot ROM uses the first to time its flash operations.
 */
#define MSTARV7_NUM_TIMERS      3
#define MSTARV7_TIMER_BASE      (MSTARV7_RIU_BASE + 0x6040)
#define MSTARV7_TIMER_STRIDE    0x40
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
    A15MPPrivState a7mpcore;
    MemoryRegion imi;
    MemoryRegion did;
    MemoryRegion miu;
    MemoryRegion l3bridge;
    MemoryRegion smpctrl_mr;
    uint16_t smpctrl[MSTARV7_SMPCTRL_NUM_REGS];
    MStarBdmaState bdma;
    MStarFspState fsp;
    MStarSarState sar;
    MStarI2cState i2c[MSTARV7_NUM_I2C];
    MStarRegbankState clkgen;
    MStarTimerState timer[MSTARV7_NUM_TIMERS];

    /* DID_KEY value, i.e. the boot-media strap ("did-key" property) */
    uint16_t did_key;
};

struct MStarV7SoCClass {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/

    /* Number of Cortex-A7 cores in this SoC */
    unsigned num_cpus;
    /* Size of the IMI SRAM */
    uint64_t imi_size;
    /* Input clock of the timers in Hz */
    uint32_t timer_freq;
};

#endif /* HW_ARM_MSTARV7_H */
