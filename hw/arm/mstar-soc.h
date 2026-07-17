/*
 * MStar/SigmaStar Armv7 SoC container - private board-side header
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Shared internals for the ARM-specific SoC container and the boards. Unlike
 * the public device inventory in include/hw/arm/mstar.h (target-independent),
 * this pulls in target/arm and the GIC, so it is only for the hw/arm board
 * files: mstar.c (the common base) and the per-family files mstar_<family>.c.
 *
 * The SoC and machine types are split so support for more chips and boards is
 * added by defining new subclasses:
 *   - mstar.c            common SoC base (TYPE_MSTAR_SOC) + abstract machine
 *   - mstar_infinity3.c  infinity3/MSC313E SoC + its camera boards
 *   - mstar_infinity2m.c infinity2m/SSD20xD SoC + its display boards
 */
#ifndef HW_ARM_MSTAR_SOC_H
#define HW_ARM_MSTAR_SOC_H

#include "qemu/osdep.h"
#include "hw/arm/mstar.h"
#include "hw/core/boards.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/boot.h"
#include "target/arm/cpu.h"
#include "hw/intc/arm_gic.h"
#include "qom/object.h"

/*
 * Abstract base type for the MStar/SigmaStar Armv7 SoCs. Concrete SoCs
 * (infinity3, infinity2m, ...) derive from it and only have to describe
 * themselves via MStarSoCInfo in their class_init; the shared realize code in
 * mstar.c builds the common peripherals and then calls the variant's
 * realize_extra() hook for any family-specific blocks.
 */
#define TYPE_MSTAR_SOC "mstar-soc"
OBJECT_DECLARE_TYPE(MStarSoCState, MStarSoCClass, MSTAR_SOC)

/* Concrete SoC variants (defined in the per-family files). */
#define TYPE_MSTAR_INFINITY3_SOC "mstar-infinity3-soc"
#define TYPE_MSTAR_INFINITY2M_SOC "mstar-infinity2m-soc"
/* SSD203D: an SSD202D (infinity2m) with an HDMI transmitter added. */
#define TYPE_MSTAR_SSD203D_SOC "mstar-ssd203d-soc"

#define MSTAR_SOC_MAX_CPUS 2

struct MStarSoCState; /* forward, for the realize_extra hook below */

/* Per-variant description, filled in by each SoC's class_init. */
typedef struct MStarSoCInfo {
    const char *cpu_type;
    unsigned int num_cpus;
    bool has_display;           /* SSD20xD-style GOP/display pipeline */
    uint32_t timer_freq;        /* SoC PIT (timer@6040) input clock, Hz */
    uint16_t bond;              /* package/variant strap at chiptop+0x120; the
                                 * IPL reads it to pick the DRAM profile/size
                                 * (0x1d = SSD201/64MB, 0x1e = SSD202D/128MB) */
    uint16_t chip_id;           /* CHIPID @0x1f003c00: 0xc2 = MSC313E/infinity3,
                                 * 0xf0 = SSD20xD/infinity2m */
    const char *clkgen_type;    /* SoC-specific clkgen/pinctrl reg-probe types */
    const char *pinctrl_type;
    bool has_hdmi;              /* SSD203D adds an HDMI transmitter (hdmitx) */
} MStarSoCInfo;

struct MStarSoCState {
    /*< private >*/
    DeviceState parent_obj;
    /*< public >*/
    bool secure_boot;       /* start the CPU in Secure EL3 (mask ROM boot) */
    ARMCPU cpu[MSTAR_SOC_MAX_CPUS];
    GICState gic;
    MstIntcState intc_irq;
    MstIntcState intc_fiq;
    MemoryRegion l3bridge;
    Msc313eTimerState timer[MSTAR_NUM_TIMERS];
    Msc313RtcState rtc;
    Msc313SarState sar;
    Msc313GpioState gpio;
    MstarPmGpioState pm_gpio;
    Msc313IspState isp;
    Msc313BdmaState bdma;
    MstarRegProbeState clkgen;
    MstarRegProbeState pinctrl;
    Msc313SdioState sdio;
    Msc313PwmState pwm;
    Msc313DispState disp;
    MstarDphyState dphy;
    Msc313BachState bach;
    MstarEmacState emac;
    MstarWdtState wdt;
    MstarRegbankState efuse;
    MstarRegbankState syscon;
    Msc313I2cState i2c[MSTAR_NUM_I2C];
    MemoryRegion imi;
    MemoryRegion smpctrl;   /* secondary-CPU boot mailbox (multi-core SoCs) */
    MemoryRegion cpupll;    /* CPU PLL registers (nonzero LPF/post-div) */
    uint32_t smp_bootaddr;  /* latched CPU1 entry address from smpctrl */
    uint16_t smpctrl_regs[0x200 / 4];   /* MSTAR_SMPCTRL_SIZE/4, read-back */
    /*
     * infinity3 (MSC313E) on-die camera capture pipeline. These blocks are set
     * up by the infinity3 SoC realize (see mstar_infinity3.c), so they are
     * present on every MSC313E board; whether a sensor is actually wired to the
     * SCCB bus is a per-board choice (only the camera board attaches one).
     */
    MstarVifState vif;      /* camera sensor video-input front-end (csi@1f240800) */
    MemoryRegion scldma;    /* camera scaler-DMA capture */
    MemoryRegion isppoll;   /* camera ISP frame-counter poll */
    MemoryRegion hvsp;      /* camera HVSP/SCL scaler */
    QEMUTimer *scldma_timer;
    qemu_irq scldma_irq;    /* SCLINTR / scaler-DMA frame-done (line 20) */
    qemu_irq isp_img_irq;   /* image-ISP frame-done (line 25, GIC 89) */
    uint32_t frame_count;   /* advanced once per fake captured frame */
    int frame_phase;        /* 0 = raise IRQs, 1 = lower IRQs */
};

struct MStarSoCClass {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/
    MStarSoCInfo info;
    /*
     * The base TYPE_MSTAR_SOC realize builds the common peripherals. A concrete
     * SoC that has extra on-die blocks (e.g. the infinity3 camera pipeline)
     * overrides realize with device_class_set_parent_realize() and chains to
     * this, mirroring the chip hierarchy in the QOM class hierarchy.
     */
    DeviceRealize parent_realize;
};

/*
 * Abstract base machine. Concrete boards (defined in the per-family files)
 * derive from it and set soc_type + the board options below; the shared
 * mstar_machine_init in mstar.c builds the SoC and loads the firmware/kernel.
 */
#define TYPE_MSTAR_MACHINE MACHINE_TYPE_NAME("mstar")
OBJECT_DECLARE_TYPE(MStarMachineState, MStarMachineClass, MSTAR_MACHINE)

struct MStarMachineState {
    MachineState parent_obj;
};

struct MStarMachineClass {
    MachineClass parent_class;
    const char *soc_type;
    bool has_gpioi2c;       /* enable GPIO8/9 bit-banged sensor SCCB (camera boards) */
    /*
     * Board-specific device wiring, run once by mstar_machine_init after the
     * SoC is realized. Boards that carry extra hardware not on every board with
     * the same SoC - e.g. the Miyoo Mini's alpu-fa auth chip, or a camera
     * board's capture pipeline + sensor - do it here, in their own machine
     * file, so it never leaks onto the other boards sharing the SoC.
     */
    void (*board_init)(MStarSoCState *soc);
};

/*
 * RIU I/O tracer helper shared with the per-family register stubs: returns true
 * the first time an (address, direction) is seen when MSTAR_IOLOG_UNIQUE is
 * active, so a boot logs the *set* of touched registers instead of every poll.
 * Defined in mstar.c; used by the camera capture stubs in mstar_infinity3.c.
 */
bool mstar_iolog_first(hwaddr addr, bool write);

#endif /* HW_ARM_MSTAR_SOC_H */
