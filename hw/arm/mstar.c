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
};

struct MStarSoCClass {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/
    MStarSoCInfo info;
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
}

static void mstar_soc_realize(DeviceState *dev, Error **errp)
{
    MStarSoCState *s = MSTAR_SOC(dev);
    MStarSoCClass *sc = MSTAR_SOC_GET_CLASS(dev);
    unsigned int i;

    for (i = 0; i < sc->info.num_cpus; i++) {
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

    /* "pm" UART - the kernel console. */
    serial_mm_init(get_system_memory(), MSTAR_PM_UART_BASE,
                   MSTAR_PM_UART_REGSHIFT, NULL,
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
