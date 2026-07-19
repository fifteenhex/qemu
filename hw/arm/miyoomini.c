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
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/block-backend-global-state.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
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

static void miyoomini_load_bootrom(MachineState *machine, SSD202DSoCState *soc)
{
    const char *bios_name = machine->firmware ?: SSD202D_BOOTROM_FILENAME;
    g_autofree char *filename = NULL;

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (!filename) {
        error_report("Could not find boot ROM image '%s'", bios_name);
        if (!machine->kernel_filename) {
            /* Nothing to run without a boot ROM or a kernel */
            exit(1);
        }
        return;
    }
    if (load_image_mr(filename, &soc->bootrom) < 0) {
        error_report("Failed to load boot ROM image '%s'", filename);
        exit(1);
    }
}

static void miyoomini_init(MachineState *machine)
{
    MiyooMiniMachineState *s = MIYOOMINI_MACHINE(machine);
    MStarV7SoCState *soc;
    DriveInfo *dinfo;

    /* The DRAM is inside the SoC package so its size is fixed */
    if (machine->ram_size != SSD202D_DRAM_SIZE) {
        char *size_str = size_to_str(SSD202D_DRAM_SIZE);
        error_report("Invalid RAM size, should be %s", size_str);
        g_free(size_str);
        exit(1);
    }

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_SSD202D_SOC);

    /* The SPI NOR image (-drive if=mtd) appears in the XIP window */
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(DEVICE(&MSTARV7_SOC(&s->soc)->fsp), "drive",
                                blk_by_legacy_dinfo(dinfo), &error_fatal);
    }

    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    memory_region_add_subregion(get_system_memory(), MSTARV7_MIU0_BASE,
                                machine->ram);

    miyoomini_load_bootrom(machine, &s->soc);

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
