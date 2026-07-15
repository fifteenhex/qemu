/*
 * MStar/SigmaStar Armv7 SoCs and boards
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This models the common MStar/SigmaStar Armv7 SoC peripherals - Cortex-A
 * CPU(s), DRAM and the "pm" UART - enough to start a mainline kernel. The
 * per-block device models live in hw/<subsystem>/mstar_*.c; this file builds
 * the SoC container and the boards. The SoC and board types are split so that
 * support for more chips and boards can be added by defining new subclasses.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/mstar.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/char/serial-mm.h"
#include "hw/core/loader.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "hw/intc/arm_gic.h"
#include "qom/object.h"

/*
 * Abstract base type for the MStar/SigmaStar Armv7 SoCs. Concrete SoCs
 * (infinity3, infinity2m, ...) derive from it and only have to describe
 * themselves via MStarSoCInfo in their class_init; the shared realize code
 * builds the common peripherals. This container is ARM-specific (it embeds the
 * CPUs and GIC), so unlike the per-block device models (hw/<subsystem>/
 * mstar_*.c) it lives here rather than in the shared header.
 */
#define TYPE_MSTAR_SOC "mstar-soc"
OBJECT_DECLARE_TYPE(MStarSoCState, MStarSoCClass, MSTAR_SOC)

/* Concrete SoC variants */
#define TYPE_MSTAR_INFINITY3_SOC "mstar-infinity3-soc"

#define MSTAR_SOC_MAX_CPUS 2

/* Per-variant description, filled in by each SoC's class_init. */
typedef struct MStarSoCInfo {
    const char *cpu_type;
    unsigned int num_cpus;
    uint32_t timer_freq;        /* SoC PIT (timer@6040) input clock, Hz */
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
    Msc313GpioState gpio;
    Msc313IspState isp;
    Msc313BdmaState bdma;
    MemoryRegion imi;
};

struct MStarSoCClass {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/
    MStarSoCInfo info;
};

/* ----------------------------------------------------------- l3bridge */

static uint64_t mstar_l3bridge_read(void *opaque, hwaddr addr, unsigned size)
{
    /* Report every MIU flush as already complete. */
    if (addr == MSTAR_L3BRIDGE_STATUS) {
        return MSTAR_L3BRIDGE_STATUS_DONE;
    }
    return 0;
}

static void mstar_l3bridge_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
}

static const MemoryRegionOps mstar_l3bridge_ops = {
    .read = mstar_l3bridge_read,
    .write = mstar_l3bridge_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


/* --------------------------------------------------------------- did (chip) */

/*
 * The "did" block. The boot ROM reads DID_KEY (vendor register 0x70, i.e.
 * byte offset 0x1c0 at the 4-byte register stride) and takes bits[5:2] as
 * the boot-media strap:
 *   0x20 - SPI-NOR (read via the ISP/QSPI XIP window)
 *   0x10 - NAND
 *   0x08 - SPINAND / eMMC (a separate controller, not modelled)
 * Anything else makes the ROM print "... undefined! [HALT]". We report NOR,
 * which is what the breadbee-class boards boot from; that is all the ROM
 * needs from this block.
 */
#define DID_BOOTMEDIA_REG   0x1c0
#define DID_BOOTMEDIA_NOR   0x20

static uint64_t mstar_did_read(void *opaque, hwaddr addr, unsigned size)
{
    if (addr == DID_BOOTMEDIA_REG) {
        return DID_BOOTMEDIA_NOR;
    }
    return 0;
}

static void mstar_did_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
}

static const MemoryRegionOps mstar_did_ops = {
    .read = mstar_did_read,
    .write = mstar_did_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


/* ------------------------------------------------------------------- miu */

/*
 * The MIU (Memory Interface Unit - the DDR controller). QEMU's DRAM is a plain
 * always-there RAM region, so there is no analog PHY to train or memory to
 * bring up; this model exists only so the mask-ROM IPL's DDR bring-up runs to
 * completion instead of spinning on a status poll.
 *
 * The 0x1000 window at 0x1f202000 is split into four 16-bit register banks
 * (registers at their native 2-byte stride; the IPL uses byte and halfword
 * accesses interchangeably):
 *
 *   0x1f202000  MIU_ANA        analog PHY: DDR PLL, DQ/DQS phase/drive training
 *   0x1f202200  MIU_ARB        arbiter: request groups 4-6, protection windows
 *   0x1f202400  MIU_DIG        digital: memory config, DRAM timings, MRx, BIST
 *   0x1f202c00  MIU_M5_GROUPS  extra arbiter request groups (pioneer3 "m5")
 *
 * How the DDR init happens (register names/offsets from the u-boot mstar ddr
 * driver drivers/ddr/mstar/{ana,arb,dig}.h + miu.c, itself reverse engineered
 * from the vendor IPL). The full bring-up (miu_configure_dram()) is:
 *
 *   1. hold the digital block in reset      (DIG SW_RST 0x3c)
 *   2. block every arbiter request mask     (DIG/ARB group REQ_MASK regs)
 *   3. reset the analog block               (ANA F0/48 "drive cal" sequence)
 *   4. start the DDR PLL / set the clock     (ANA 6c=0x400, 68=0x2004, 114=1,
 *                                            DDFSET_L 0x60=0x8000, _H 0x64=0x29)
 *   5. program the memory config + timings  (DIG CONFIG1..7 0x04-0x1c: memtype/
 *                                            buswidth/banks/cols, tRCD/tRP/tRAS,
 *                                            tWL/tWR..., mode regs MR0..3 0x20)
 *   6. set the arbiter request priorities   (DIG groups 0-3 0x80/c0/100/140,
 *                                            ARB groups 4-6)
 *   7. analog config + power-up             (ANA E0/D8/DC/EC..., then the ANA00
 *                                            power-up + 3C=5/f/5 pulse)
 *   8. run the "initial cycle"              (DIG CNTRL0 0x00: step RSTZ->CS->CKE
 *                                            ->INIT_MIU, poll INITDONE bit15)
 *   9. latch init-done                      (DIG SW_RST 0x3c |= SW_INIT_DONE
 *                                            bit3 - "if this is not set any DDR
 *                                            access locks up the CPU")
 *  10. run the memory BIST self test        (see below)
 *
 * The vendor IPL in the emulated Miyoo/SSD202 flash takes a shortened path
 * (captured live with MSTAR_IOLOG): it can't identify the PLL ("unknown
 * miupll") so it skips the analog PLL/training in steps 3-4/7-8 and jumps
 * straight to the arbiter setup (step 6: DIG groups 0-3 CTRL=0x8015,
 * CONFIG0=0x2008, PRIORITY0..3=0x3210/7654/ba98/fedc; ARB group6 0x1c0..0x1f0),
 * unmasks the requests (step 2 in reverse), latches init-done (step 9: DIG
 * SW_RST 0x3c=0x8c08), then runs the BIST.
 *
 * The BIST lives at MIU_DIG + 0x1c0.. (0x1f2025c0). Programming, from the
 * trace: PROTECT2_START 0x1a4=0x9000, pulse 0x1bc 1->0, then length 0x1c8=
 * 0xffff, 0x1cc=0x01fe, test pattern 0x1d0=0x5aa5, MIUSEL0 0x1e0=0, and finally
 * write bit0 of the control/status reg 0x1c0 to start. The IPL then polls 0x1c0
 * for bit15 = done, bits[14:13] = error (so 0x8000 = done, no error; 0xe000 =
 * done, failed). This is the one register the IPL reads-and-waits-on.
 *
 * What we model: DRAM training is instantaneous here, so report both completion
 * gates as finished with no error - BIST 0x1c0 -> 0x8000, and (for the full
 * u-boot/kernel path) the initial-cycle CNTRL0 -> INITDONE. Every other MIU
 * register is write-only to the IPL (fire and forget) or only read to
 * read-modify-write, so returning zero for them is fine.
 *
 * Note the IPL does NOT read the DRAM size from any MIU register: it sizes
 * memory by probing address aliasing - writing distinct markers at DRAM +0,
 * +32M, +64M, +128M and +256M and reading them back to find where the address
 * wraps. Real DRAM wraps above its size; QEMU's flat RAM does not, so the
 * machine mirrors RAM above its real size (see mstar_machine_init) to make the
 * probe detect the true amount instead of the 512MB "no wrap" maximum.
 */
#define MIU_ANA_BASE            0x000
#define MIU_ARB_BASE            0x200
#define MIU_DIG_BASE            0x400
#define MIU_M5_GROUPS_BASE      0xc00

#define MIU_DIG_CNTRL0          (MIU_DIG_BASE + 0x000)  /* init-cycle control */
#define MIU_DIG_CNTRL0_INITDONE (1 << 15)
#define MIU_DIG_BIST_CTRL       (MIU_DIG_BASE + 0x1c0)  /* start (W) / status (R) */
#define MIU_DIG_BIST_DONE       (1 << 15)               /* bits 14:13 = error */

static uint64_t mstar_miu_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case MIU_DIG_BIST_CTRL:
        /* Memory self-test finished, no error (emulated DRAM never fails). */
        return MIU_DIG_BIST_DONE;
    case MIU_DIG_CNTRL0:
        /* Initial DRAM training cycle completed (full-config path). */
        return MIU_DIG_CNTRL0_INITDONE;
    }
    return 0;
}

static void mstar_miu_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    /* No analog PHY / DRAM to program; the training writes are no-ops here. */
}

static const MemoryRegionOps mstar_miu_ops = {
    .read = mstar_miu_read,
    .write = mstar_miu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


/* ------------------------------------------------------------------ SoC */

/* "fiq" mst-intc input line for each timer@6040/80/c0 (from the DT). */
static const unsigned int mstar_timer_hwirq[MSTAR_NUM_TIMERS] = { 0, 1, 12 };

static void mstar_soc_init(Object *obj)
{
    MStarSoCState *s = MSTAR_SOC(obj);
    MStarSoCClass *sc = MSTAR_SOC_GET_CLASS(obj);
    unsigned int i;

    for (i = 0; i < sc->info.num_cpus; i++) {
        object_initialize_child(obj, "cpu[*]", &s->cpu[i],
                                sc->info.cpu_type);
    }

    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GIC);
    object_initialize_child(obj, "intc-irq", &s->intc_irq, TYPE_MST_INTC);
    object_initialize_child(obj, "intc-fiq", &s->intc_fiq, TYPE_MST_INTC);

    for (i = 0; i < MSTAR_NUM_TIMERS; i++) {
        object_initialize_child(obj, "timer[*]", &s->timer[i],
                                TYPE_MSC313E_TIMER);
    }

    object_initialize_child(obj, "rtc", &s->rtc, TYPE_MSC313_RTC);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_MSC313_GPIO);
    object_initialize_child(obj, "isp", &s->isp, TYPE_MSC313_ISP);
    object_initialize_child(obj, "bdma", &s->bdma, TYPE_MSC313_BDMA);
}

static bool mstar_realize_intc(MstIntcState *intc, DeviceState *gicdev,
                               hwaddr base, uint32_t irq_start, uint32_t num,
                               Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(intc);
    unsigned int i;

    qdev_prop_set_uint32(DEVICE(intc), "irq-start", irq_start);
    qdev_prop_set_uint32(DEVICE(intc), "num-irqs", num);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, base);

    /* Each input forwards to GIC SPI (irq_start + line). */
    for (i = 0; i < num; i++) {
        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(gicdev, irq_start + i));
    }
    return true;
}

static void mstar_soc_realize(DeviceState *dev, Error **errp)
{
    MStarSoCState *s = MSTAR_SOC(dev);
    MStarSoCClass *sc = MSTAR_SOC_GET_CLASS(dev);
    DeviceState *gicdev = DEVICE(&s->gic);
    unsigned int i;

    for (i = 0; i < sc->info.num_cpus; i++) {
        /* The architected timer runs at the rate given in the device tree. */
        object_property_set_int(OBJECT(&s->cpu[i]), "cntfrq",
                                MSTAR_ARCH_TIMER_FREQ, &error_abort);
        /*
         * No EL2 for a kernel boot: the kernel starts at EL1 (SVC); with EL2
         * present QEMU's cortex-a7 would enter HYP and hang on the unwired HYP
         * timer. EL3 is only enabled for a mask-ROM boot: the ROM/IPL run in
         * Secure state and touch Secure-only registers (e.g. NSACR), which are
         * UNDEFINED without the Security Extensions.
         */
        object_property_set_bool(OBJECT(&s->cpu[i]), "has_el2", false,
                                 &error_abort);
        object_property_set_bool(OBJECT(&s->cpu[i]), "has_el3", s->secure_boot,
                                 &error_abort);
        if (!qdev_realize(DEVICE(&s->cpu[i]), NULL, errp)) {
            return;
        }
    }

    /* GIC-400 (GICv2). Model only the distributor + CPU interface. */
    qdev_prop_set_uint32(gicdev, "num-irq", MSTAR_GIC_NUM_SPI + GIC_INTERNAL);
    qdev_prop_set_uint32(gicdev, "revision", 2);
    qdev_prop_set_uint32(gicdev, "num-cpu", sc->info.num_cpus);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gic), errp)) {
        return;
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gic), 0, MSTAR_GIC_DIST_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gic), 1, MSTAR_GIC_CPU_BASE);

    for (i = 0; i < sc->info.num_cpus; i++) {
        DeviceState *cpudev = DEVICE(&s->cpu[i]);
        int ppibase = MSTAR_GIC_NUM_SPI + i * GIC_INTERNAL + GIC_NR_SGIS;

        /* CPU generic timer outputs -> GIC PPI inputs */
        qdev_connect_gpio_out(cpudev, GTIMER_PHYS,
                              qdev_get_gpio_in(gicdev,
                                       ppibase + MSTAR_GIC_PPI_PHYSTIMER));
        qdev_connect_gpio_out(cpudev, GTIMER_VIRT,
                              qdev_get_gpio_in(gicdev,
                                       ppibase + MSTAR_GIC_PPI_VIRTTIMER));

        /* GIC outputs -> CPU interrupt inputs */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i + sc->info.num_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
    }

    /* l3bridge (MIU write-flush) used by the SoC memory barrier. */
    memory_region_init_io(&s->l3bridge, OBJECT(s), &mstar_l3bridge_ops, s,
                          "mstar.l3bridge", MSTAR_L3BRIDGE_SIZE);
    memory_region_add_subregion(get_system_memory(), MSTAR_L3BRIDGE_BASE,
                                &s->l3bridge);

    /* On-chip SRAM ("IMI"), used by the boot ROM as scratch/IPL load area. */
    memory_region_init_ram(&s->imi, OBJECT(s), "mstar.imi", MSTAR_IMI_SIZE,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), MSTAR_IMI_BASE, &s->imi);

    /* "did" chip block - the boot ROM reads the boot-media strap from here. */
    {
        MemoryRegion *did = g_new(MemoryRegion, 1);
        memory_region_init_io(did, OBJECT(s), &mstar_did_ops, s,
                              "mstar.did", 0x200);
        memory_region_add_subregion(get_system_memory(), MSTAR_DID_BASE, did);
    }

    /* MIU (DDR controller) - only enough for the mask-ROM IPL's memory BIST. */
    {
        MemoryRegion *miu = g_new(MemoryRegion, 1);
        memory_region_init_io(miu, OBJECT(s), &mstar_miu_ops, s,
                              "mstar.miu", MSTAR_MIU_SIZE);
        memory_region_add_subregion(get_system_memory(), MSTAR_MIU_BASE, miu);
    }

    /* The two "mst-intc" instances between the peripherals and the GIC. */
    if (!mstar_realize_intc(&s->intc_irq, gicdev, MSTAR_INTC_IRQ_BASE,
                            MSTAR_INTC_IRQ_START, MSTAR_INTC_IRQ_NUM, errp) ||
        !mstar_realize_intc(&s->intc_fiq, gicdev, MSTAR_INTC_FIQ_BASE,
                            MSTAR_INTC_FIQ_START, MSTAR_INTC_FIQ_NUM, errp)) {
        return;
    }

    /*
     * SoC PIT timers (timer@6040, free-running counters). Their input clock is
     * per-variant: the SSD20xD family runs them from the 432MHz clk_timer, the
     * msc313/infinity3 from the 12MHz xtal_div2. The vendor kernel programs its
     * clockevent using this rate, so getting it wrong makes every deadline land
     * in the past and storms the timer interrupt.
     */
    for (i = 0; i < MSTAR_NUM_TIMERS; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->timer[i]);

        object_property_set_int(OBJECT(&s->timer[i]), "freq",
                                sc->info.timer_freq, &error_abort);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, MSTAR_TIMER_BASE + i * MSTAR_TIMER_STRIDE);
        /* timer@6040/80/c0 interrupt on "fiq" mst-intc lines 0, 1 and 12. */
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(DEVICE(&s->intc_fiq),
                                            mstar_timer_hwirq[i]));
    }

    /* RTC - its alarm interrupt routed through the "irq" mst-intc. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rtc), 0, MSTAR_RTC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_RTC_HWIRQ));

    /* GPIO pad register bank. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio), 0, MSTAR_GPIO_BASE);

    /* ISP SPI-NOR controller (core regs, qspi config, XIP window). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->isp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->isp), 0, MSTAR_ISP_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->isp), 1, MSTAR_FSP_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->isp), 2, MSTAR_ISP_QSPI_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->isp), 3, MSTAR_ISP_XIP_BASE);

    /* BDMA engine, its two channels interrupting via the "irq" mst-intc. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->bdma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->bdma), 0, MSTAR_BDMA_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->bdma), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_BDMA_CH0_HWIRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->bdma), 1,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_BDMA_CH1_HWIRQ));

    /* "pm" UART - the kernel console, its IRQ routed through the "irq" intc. */
    serial_mm_init(get_system_memory(), MSTAR_PM_UART_BASE,
                   MSTAR_PM_UART_REGSHIFT,
                   qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_PM_UART_HWIRQ),
                   MSTAR_PM_UART_CLK / 16, serial_hd(0), DEVICE_LITTLE_ENDIAN);
}

static void mstar_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mstar_soc_realize;
    /* SoCs are instantiated by their board, not directly by the user. */
    dc->user_creatable = false;
}

static void mstar_infinity3_soc_class_init(ObjectClass *oc, const void *data)
{
    MStarSoCClass *sc = MSTAR_SOC_CLASS(oc);

    sc->info.cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    sc->info.num_cpus = 1;
    sc->info.timer_freq = 12000000;     /* xtal_div2 */
}

/* ---------------------------------------------------------------- Boards */

#define TYPE_MSTAR_MACHINE MACHINE_TYPE_NAME("mstar")
OBJECT_DECLARE_TYPE(MStarMachineState, MStarMachineClass, MSTAR_MACHINE)

struct MStarMachineState {
    MachineState parent_obj;
};

struct MStarMachineClass {
    MachineClass parent_class;
    const char *soc_type;
};

static struct arm_boot_info mstar_binfo;

static void mstar_machine_init(MachineState *machine)
{
    MStarMachineClass *mmc = MSTAR_MACHINE_GET_CLASS(machine);
    MStarSoCState *soc;

    soc = MSTAR_SOC(object_new(mmc->soc_type));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc));
    object_unref(OBJECT(soc));
    /* A mask-ROM boot (-bios) runs in Secure state, so give the CPU EL3. */
    soc->secure_boot = machine->firmware != NULL;
    qdev_realize(DEVICE(soc), NULL, &error_fatal);

    memory_region_add_subregion(get_system_memory(), MSTAR_DRAM_BASE,
                                machine->ram);

    if (machine->firmware) {
        /*
         * Boot ROM mode: map the mask ROM at address 0 and let the CPU reset
         * straight into it (no kernel boot wrapper). The ROM brings up the
         * SoC and chain-loads the next stage itself. As observed on the
         * msc313e mask ROM, the flow is:
         *
         *   1. Read the boot-media strap from DID_KEY (0x1f0071c0) bits[5:2]:
         *      0x20 selects SPI-NOR (the ISP/QSPI).
         *   2. Read the "IPL" from the SPI-NOR flash (the partition table
         *      points it at flash offset 0x4000) through the memory-mapped
         *      XIP window at 0x14000000, and copy it into IMI SRAM at
         *      0xa0000000.
         *   3. Check the "IPL_" magic at IMI+4 and jump to 0xa0000000.
         *
         * So to reach the IPL, pass the ROM with -bios and the NOR flash
         * image with -drive if=mtd.
         *
         * The IPL then runs its own DDR/PLL bring-up, prints its banner
         * ("IPL <ver>", "512MB", ...) on the pm UART, passes the MIU memory
         * BIST (see the "miu" block), decompresses the next stage and hands
         * off to u-boot. u-boot comes up on the console, runs its MDIO PHY
         * scan against the modelled "emac" (finds no PHY, fakes a link) and
         * reaches its command prompt.
         */
        MemoryRegion *rom = g_new(MemoryRegion, 1);
        char *fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);

        memory_region_init_rom(rom, NULL, "mstar.bootrom", MSTAR_BOOTROM_SIZE,
                               &error_fatal);
        memory_region_add_subregion(get_system_memory(), MSTAR_BOOTROM_BASE,
                                    rom);
        if (load_image_mr(fn ? fn : machine->firmware, rom) < 0) {
            error_report("could not load boot ROM '%s'", machine->firmware);
            exit(1);
        }
        g_free(fn);
    } else {
        mstar_binfo.loader_start = MSTAR_DRAM_BASE;
        mstar_binfo.ram_size = machine->ram_size;
        /* The vendor kernel expects to enter in Secure state (see above). */
        mstar_binfo.secure_boot = soc->secure_boot;
        arm_load_kernel(&soc->cpu[0], machine, &mstar_binfo);
    }
}

static void mstar_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = mstar_machine_init;
    mc->default_ram_id = "mstar.dram";
    mc->min_cpus = 1;
    mc->default_cpus = 1;
    /*
     * Peripherals other than the console (GIC, timer, ...) are not modelled
     * yet, so let the kernel poke at their addresses without aborting.
     */
    mc->ignore_memory_transaction_failures = true;
}

static void breadbee_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    MStarMachineClass *mmc = MSTAR_MACHINE_CLASS(oc);

    mc->desc = "thingy.jp BreadBee (MStar infinity3/MSC313E)";
    mc->default_ram_size = 64 * MiB;
    mc->max_cpus = 1;
    mmc->soc_type = TYPE_MSTAR_INFINITY3_SOC;
}

/* ----------------------------------------------------------------- Types */

static const TypeInfo mstar_types[] = {
    {
        .name           = TYPE_MSTAR_SOC,
        .parent         = TYPE_DEVICE,
        .instance_size  = sizeof(MStarSoCState),
        .instance_init  = mstar_soc_init,
        .class_size     = sizeof(MStarSoCClass),
        .class_init     = mstar_soc_class_init,
        .abstract       = true,
    },
    {
        .name           = TYPE_MSTAR_INFINITY3_SOC,
        .parent         = TYPE_MSTAR_SOC,
        .class_init     = mstar_infinity3_soc_class_init,
    },
    {
        .name           = TYPE_MSTAR_MACHINE,
        .parent         = TYPE_MACHINE,
        .instance_size  = sizeof(MStarMachineState),
        .class_size     = sizeof(MStarMachineClass),
        .class_init     = mstar_machine_class_init,
        .abstract       = true,
        /* Make derived boards show up in qemu-system-arm/-aarch64. */
        .interfaces     = arm_machine_interfaces,
    },
    {
        .name           = MACHINE_TYPE_NAME("breadbee"),
        .parent         = TYPE_MSTAR_MACHINE,
        .class_init     = breadbee_machine_class_init,
    },
};

DEFINE_TYPES(mstar_types)
