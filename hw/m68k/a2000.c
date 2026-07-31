/*
 * Commodore Amiga 2000.
 *
 * The big-box sibling of the A500: a 68000 at 7MHz on the same chipset
 * generation, with room for Zorro II cards, an ISA bridgeboard and a
 * CPU slot (none of which are modelled).  This follows the late rev
 * 6.x boards: 1MB of chip RAM under the ECS 1MB Agnus (8372A) with the
 * original OCS Denise (8362), which has no ID register, so DENISEID
 * reads open bus.  The 0x00C00000 "ranger" slow RAM block decoded by
 * Gary is available through -m for the earlier 512KB+512KB layouts.
 *
 * Boots both the 512KB A500/A600/A2000 Kickstarts (2.x/3.1) and the
 * 256KB Kickstart 1.x images (which the shared base mirrors across the
 * 512KB ROM window, as the real socket decode does).
 *
 * Memory map (motherboard):
 *   0x00000000  chip RAM (1MB)
 *   0x00bfd000  CIA-B
 *   0x00bfe000  CIA-A
 *   0x00c00000  slow "ranger" RAM (optional, -m)
 *   0x00dff000  custom chips
 *   0x00f80000  Kickstart ROM (512KB)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/m68k/amiga.h"
#include "target/m68k/cpu.h"

#define A2000_SLOWRAM_BASE      0x00c00000
#define A2000_SLOWRAM_MAX       (0x00d80000 - A2000_SLOWRAM_BASE)  /* 1.5MB */
#define A2000_ROM_BASE          0x00f80000

#define TYPE_A2000_MACHINE MACHINE_TYPE_NAME("a2000")

static void a2000_board_init(AmigaMachineState *ams)
{
    MachineState *machine = MACHINE(ams);
    MemoryRegion *sysmem = get_system_memory();

    /* the 0x00C00000 slow RAM block, sized and free-listed by Kickstart */
    if (machine->ram_size > A2000_SLOWRAM_MAX) {
        error_report("a2000: ranger slow RAM is limited to 1.5MB");
        exit(1);
    }
    if (machine->ram_size) {
        memory_region_add_subregion(sysmem, A2000_SLOWRAM_BASE, machine->ram);
    }
}

static void a2000_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AmigaMachineClass *amc = AMIGA_MACHINE_CLASS(oc);

    mc->desc = "Commodore Amiga 2000 (68000)";
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    /* the rev 6.x boards carry a full megabyte of chip RAM, no ranger */
    mc->default_ram_size = 0;
    mc->default_ram_id = "amiga.slowram";

    amc->rom_base = A2000_ROM_BASE;
    amc->rom_size = 512 * KiB;
    amc->chipram_size = 1 * MiB;
    /* the 68000's whole 24-bit address space terminates in open bus */
    amc->open_bus_size = 0x01000000;
    amc->agnus_id = 0x20;       /* ECS 1MB Agnus 8372A, PAL */
    amc->denise_id = 0xff;      /* OCS Denise 8362: no ID, open bus */
    amc->board_init = a2000_board_init;
}

static const TypeInfo a2000_machine_types[] = {
    {
        .name          = TYPE_A2000_MACHINE,
        .parent        = TYPE_AMIGA_MACHINE,
        .instance_size = sizeof(AmigaMachineState),
        .class_init    = a2000_machine_class_init,
    },
};

DEFINE_TYPES(a2000_machine_types)
