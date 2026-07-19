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
#include "hw/display/dsi.h"
#include "hw/i2c/alpu.h"
#include "hw/sd/sd.h"

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
    DeviceState *panel;
    uint64_t offset;

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

    /*
     * The panel is board specific: it hangs off the SoC's DSI
     * controller on the point-to-point MIPI link. Create it and link
     * it to the controller before the SoC (and its DSI) is realized.
     */
    panel = qdev_new(TYPE_DSI_DCS_PANEL);
    object_property_add_child(OBJECT(machine), "panel", OBJECT(panel));
    qdev_realize_and_unref(panel, NULL, &error_fatal);
    object_property_set_link(OBJECT(&MSTARV7_SOC(&s->soc)->dsi), "panel",
                             OBJECT(panel), &error_fatal);

    /* The panel is mounted upside down on this board */
    object_property_set_bool(OBJECT(&MSTARV7_SOC(&s->soc)->disp), "flip",
                             true, &error_fatal);

    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    /*
     * The Miyoo Mini carries a Neowine ALPU-FA copy-protection chip on
     * i2c bus 1 at 0x3d; the vendor kernel and MainUI refuse to run
     * without it. It is board specific, so it is wired here rather
     * than in the SoC.
     */
    i2c_slave_create_simple(MSTARV7_SOC(&s->soc)->i2c[1].bus, TYPE_ALPU, 0x3d);

    /* An SD card in the slot, if the user supplied one with -drive if=sd */
    dinfo = drive_get(IF_SD, 0, 0);
    if (dinfo) {
        BusState *sdbus = qdev_get_child_bus(DEVICE(&MSTARV7_SOC(&s->soc)->fcie),
                                             "sd-bus");
        DeviceState *card = qdev_new(TYPE_SD_CARD);

        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
        qdev_realize_and_unref(card, sdbus, &error_fatal);
    }

    memory_region_add_subregion(get_system_memory(), MSTARV7_MIU0_BASE,
                                machine->ram);

    /*
     * The DRAM wraps in the MIU window; the IPL sizes memory by
     * writing markers and looking for where they alias back.
     */
    for (offset = machine->ram_size;
         offset + machine->ram_size <= MSTARV7_MIU0_WINDOW;
         offset += machine->ram_size) {
        MemoryRegion *mirror = g_new(MemoryRegion, 1);

        memory_region_init_alias(mirror, OBJECT(machine), "miyoomini.mirror",
                                 machine->ram, 0, machine->ram_size);
        memory_region_add_subregion(get_system_memory(),
                                    MSTARV7_MIU0_BASE + offset, mirror);
    }

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
