/*
 * Commodore Amiga CD32.
 *
 * The 1993 console: electrically an A1200 — 68EC020 at 14MHz with the
 * AGA chipset and 2MB chip RAM — with the Akiko chip in place of
 * Gayle, a CD-ROM drive instead of the floppy, and the OS split
 * across two 512KB ROMs: the CD32 Kickstart 3.1 at 0xf80000 (-bios)
 * and the extended ROM at 0xe00000 (cd.device, the boot animation and
 * the CD32 user interface; machine property "extrom", default file
 * name "cd32_ext.rom").
 *
 * QEMU has no EC020 model, so as on the a1200 the full 68020 core
 * stands in and the whole 32-bit address space terminates in open
 * bus.  Akiko's C2P port and NVRAM work; the CD drive itself is not
 * modelled yet (hw/m68k/cd32_akiko.c), so the console boots to its
 * "insert disc" startup screen.
 *
 * Memory map (motherboard):
 *   0x00000000  chip RAM (2MB)
 *   0x00b80000  Akiko
 *   0x00bfd000  CIA-B
 *   0x00bfe000  CIA-A
 *   0x00dff000  custom chips
 *   0x00e00000  extended ROM (512KB)
 *   0x00f80000  Kickstart ROM (512KB)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qemu/datadir.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/m68k/amiga.h"
#include "hw/m68k/cd32_akiko.h"
#include "target/m68k/cpu.h"

#define CD32_EXTROM_BASE        0x00e00000
#define CD32_EXTROM_SIZE        (512 * KiB)
#define CD32_EXTROM_DEFAULT     "cd32_ext.rom"

#define TYPE_CD32_MACHINE MACHINE_TYPE_NAME("cd32")
OBJECT_DECLARE_SIMPLE_TYPE(CD32MachineState, CD32_MACHINE)

struct CD32MachineState {
    AmigaMachineState parent_obj;

    MemoryRegion extrom;
    char *extrom_name;
};

static void cd32_board_init(AmigaMachineState *ams)
{
    CD32MachineState *s = CD32_MACHINE(ams);
    MemoryRegion *sysmem = get_system_memory();
    const char *extrom_name;
    DeviceState *akiko;
    char *filename;

    /* the extended ROM with cd.device and the CD32 boot user interface */
    /* NULL owner: RAM-backed regions need a device owner or none */
    memory_region_init_rom(&s->extrom, NULL, "cd32.extended-rom",
                           CD32_EXTROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, CD32_EXTROM_BASE, &s->extrom);
    extrom_name = s->extrom_name ? s->extrom_name : CD32_EXTROM_DEFAULT;
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, extrom_name);
    if (!filename) {
        error_report("Could not find CD32 extended ROM '%s' "
                     "(set with -M cd32,extrom=...)", extrom_name);
        exit(1);
    }
    if (load_image_size(filename, memory_region_get_ram_ptr(&s->extrom),
                        CD32_EXTROM_SIZE) < 0) {
        error_report("Could not load CD32 extended ROM '%s'", filename);
        exit(1);
    }
    g_free(filename);

    akiko = qdev_new(TYPE_CD32_AKIKO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(akiko), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(akiko), 0, CD32_AKIKO_BASE);
    /* Akiko's CD interrupts present on INT2 */
    sysbus_connect_irq(SYS_BUS_DEVICE(akiko), 0,
                       qdev_get_gpio_in_named(ams->custom, "ports-irq", 0));
    ams->rsto_dev[0] = akiko;
}

static char *cd32_get_extrom(Object *obj, Error **errp)
{
    CD32MachineState *s = CD32_MACHINE(obj);

    return g_strdup(s->extrom_name);
}

static void cd32_set_extrom(Object *obj, const char *value, Error **errp)
{
    CD32MachineState *s = CD32_MACHINE(obj);

    g_free(s->extrom_name);
    s->extrom_name = g_strdup(value);
}

static void cd32_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AmigaMachineClass *amc = AMIGA_MACHINE_CLASS(oc);

    mc->desc = "Commodore Amiga CD32 (68EC020, AGA)";
    /* QEMU has no EC020 variant; the 020 core stands in for it */
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68020");

    amc->rom_base = 0x00f80000;
    amc->rom_size = 512 * KiB;
    amc->chipram_size = 2 * MiB;
    /* every cycle terminates: 24-bit decode below, EC020 wrap above */
    amc->open_bus_size = 0x100000000ULL;
    /* AGA: 2MB Alice reports VPOSR id 0x23, Lisa reports Denise id 0xf8 */
    amc->agnus_id = 0x23;
    amc->denise_id = 0xf8;
    amc->board_init = cd32_board_init;

    object_class_property_add_str(oc, "extrom", cd32_get_extrom,
                                  cd32_set_extrom);
    object_class_property_set_description(oc, "extrom",
        "CD32 extended ROM image file (default: cd32_ext.rom)");
}

static const TypeInfo cd32_machine_types[] = {
    {
        .name          = TYPE_CD32_MACHINE,
        .parent        = TYPE_AMIGA_MACHINE,
        .instance_size = sizeof(CD32MachineState),
        .class_init    = cd32_machine_class_init,
    },
};

DEFINE_TYPES(cd32_machine_types)
