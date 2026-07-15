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
} MStarSoCInfo;

struct MStarSoCState {
    /*< private >*/
    DeviceState parent_obj;
    /*< public >*/
    ARMCPU cpu[MSTAR_SOC_MAX_CPUS];
    GICState gic;
    MstIntcState intc_irq;
    MstIntcState intc_fiq;
    MemoryRegion l3bridge;
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


/* ------------------------------------------------------------------ SoC */

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
         * No EL2/EL3: the mainline kernel starts at EL1 (SVC). With EL2
         * present QEMU's cortex-a7 would enter HYP and hang on the unwired
         * HYP timer.
         */
        object_property_set_bool(OBJECT(&s->cpu[i]), "has_el2", false,
                                 &error_abort);
        object_property_set_bool(OBJECT(&s->cpu[i]), "has_el3", false,
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

    /* The two "mst-intc" instances between the peripherals and the GIC. */
    if (!mstar_realize_intc(&s->intc_irq, gicdev, MSTAR_INTC_IRQ_BASE,
                            MSTAR_INTC_IRQ_START, MSTAR_INTC_IRQ_NUM, errp) ||
        !mstar_realize_intc(&s->intc_fiq, gicdev, MSTAR_INTC_FIQ_BASE,
                            MSTAR_INTC_FIQ_START, MSTAR_INTC_FIQ_NUM, errp)) {
        return;
    }

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
    qdev_realize(DEVICE(soc), NULL, &error_fatal);

    memory_region_add_subregion(get_system_memory(), MSTAR_DRAM_BASE,
                                machine->ram);

    mstar_binfo.loader_start = MSTAR_DRAM_BASE;
    mstar_binfo.ram_size = machine->ram_size;
    arm_load_kernel(&soc->cpu[0], machine, &mstar_binfo);
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
