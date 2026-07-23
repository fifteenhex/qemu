/*
 * Commodore Amiga 600.
 *
 * The 1992 compact wedge: electrically an A500 Plus - 68000 at 7MHz,
 * 2MB ECS Agnus (8375) and ECS Denise (8373), 1MB chip RAM on board
 * with a trapdoor extending chip RAM to 2MB - in a smaller case with
 * no numeric keypad, plus the Gayle gate array providing IDE and
 * PCMCIA.  Gayle, the IDE port and PCMCIA are not modelled yet, so
 * accesses to their register space read open bus and Kickstart's
 * probes see nothing.  Shipped with Kickstart 2.05.
 *
 * Memory map (motherboard):
 *   0x00000000  chip RAM (1MB, trapdoor continues at 0x100000)
 *   0x00bfd000  CIA-B
 *   0x00bfe000  CIA-A
 *   0x00da0000  Gayle IDE (unmodelled, open bus)
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

#define A600_CHIPRAM_SIZE       (1 * MiB)
#define A600_TRAPDOOR_BASE      A600_CHIPRAM_SIZE
#define A600_TRAPDOOR_MAX       (1 * MiB)
#define A600_ROM_BASE           0x00f80000

#define TYPE_A600_MACHINE MACHINE_TYPE_NAME("a600")

static void a600_board_init(AmigaMachineState *ams)
{
    MachineState *machine = MACHINE(ams);
    MemoryRegion *sysmem = get_system_memory();

    /* the trapdoor expansion: chip RAM contiguous with the onboard bank */
    if (machine->ram_size > A600_TRAPDOOR_MAX) {
        error_report("a600: the trapdoor extends chip RAM by at most 1MB");
        exit(1);
    }
    if (machine->ram_size) {
        memory_region_add_subregion(sysmem, A600_TRAPDOOR_BASE, machine->ram);
    }
}

static void a600_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AmigaMachineClass *amc = AMIGA_MACHINE_CLASS(oc);

    mc->desc = "Commodore Amiga 600 (68000, ECS)";
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->default_ram_size = A600_TRAPDOOR_MAX;
    mc->default_ram_id = "amiga.chipram-exp";

    amc->rom_base = A600_ROM_BASE;
    amc->rom_size = 512 * KiB;
    amc->chipram_size = A600_CHIPRAM_SIZE;
    /* the 68000's whole 24-bit address space terminates in open bus */
    amc->open_bus_size = 0x01000000;
    /* ECS 2MB Agnus 8375 and ECS Denise 8373: the shared base defaults */
    amc->board_init = a600_board_init;
}

static const TypeInfo a600_machine_types[] = {
    {
        .name          = TYPE_A600_MACHINE,
        .parent        = TYPE_AMIGA_MACHINE,
        .instance_size = sizeof(AmigaMachineState),
        .class_init    = a600_machine_class_init,
    },
};

DEFINE_TYPES(a600_machine_types)
