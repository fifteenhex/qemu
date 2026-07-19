/*
 * Miyoo Mini handheld game console
 *
 * A handheld game console built around the SigmaStar SSD202D.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/ssd202d.h"

#define TYPE_MIYOOMINI_MACHINE MACHINE_TYPE_NAME("miyoomini")
OBJECT_DECLARE_SIMPLE_TYPE(MiyooMiniMachineState, MIYOOMINI_MACHINE)

struct MiyooMiniMachineState {
    /*< private >*/
    MachineState parent_obj;
    /*< public >*/

    SSD202DSoCState soc;
    struct arm_boot_info binfo;
};

static void miyoomini_init(MachineState *machine)
{
    MiyooMiniMachineState *s = MIYOOMINI_MACHINE(machine);
    MStarV7SoCState *soc;

    /* The DRAM is inside the SoC package so its size is fixed */
    if (machine->ram_size != SSD202D_DRAM_SIZE) {
        char *size_str = size_to_str(SSD202D_DRAM_SIZE);
        error_report("Invalid RAM size, should be %s", size_str);
        g_free(size_str);
        exit(1);
    }

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_SSD202D_SOC);
    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    memory_region_add_subregion(get_system_memory(), MSTARV7_MIU0_BASE,
                                machine->ram);

    soc = MSTARV7_SOC(&s->soc);
    s->binfo.loader_start = MSTARV7_MIU0_BASE;
    s->binfo.ram_size = machine->ram_size;
    arm_load_kernel(&soc->cpus[0], machine, &s->binfo);
}

static void miyoomini_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a7"),
        NULL
    };

    mc->desc = "Miyoo Mini (SigmaStar SSD202D)";
    mc->init = miyoomini_init;
    mc->min_cpus = INFINITY2M_NUM_CPUS;
    mc->max_cpus = INFINITY2M_NUM_CPUS;
    mc->default_cpus = INFINITY2M_NUM_CPUS;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = SSD202D_DRAM_SIZE;
    mc->default_ram_id = "miyoomini.ram";
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
}

static const TypeInfo miyoomini_machine_types[] = {
    {
        .name           = TYPE_MIYOOMINI_MACHINE,
        .parent         = TYPE_MACHINE,
        .instance_size  = sizeof(MiyooMiniMachineState),
        .class_init     = miyoomini_machine_class_init,
        .interfaces     = arm_machine_interfaces,
    },
};

DEFINE_TYPES(miyoomini_machine_types)
