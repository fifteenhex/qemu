/*
 * Commodore Amiga 500 Plus.
 *
 * The 1991 ECS refresh of the A500: a 68000 at 7MHz with the 2MB ECS
 * Agnus (8375) and ECS Denise (8373), 1MB of chip RAM on board and a
 * trapdoor slot whose RAM extends chip RAM contiguously up to 2MB
 * (unlike the plain A500, where the trapdoor is slow ranger RAM).
 * Shipped with Kickstart 2.04.
 *
 * Memory map (motherboard):
 *   0x00000000  chip RAM (1MB, trapdoor continues at 0x100000)
 *   0x00bfd000  CIA-B
 *   0x00bfe000  CIA-A
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

#define A500P_CHIPRAM_SIZE      (1 * MiB)
#define A500P_TRAPDOOR_BASE     A500P_CHIPRAM_SIZE
#define A500P_TRAPDOOR_MAX      (1 * MiB)
#define A500P_ROM_BASE          0x00f80000

#define TYPE_A500PLUS_MACHINE MACHINE_TYPE_NAME("a500plus")

static void a500plus_board_init(AmigaMachineState *ams)
{
    MachineState *machine = MACHINE(ams);
    MemoryRegion *sysmem = get_system_memory();

    /*
     * The trapdoor expansion: chip RAM contiguous with the onboard
     * megabyte, all of it under the 8375's 2MB DMA reach.
     */
    if (machine->ram_size > A500P_TRAPDOOR_MAX) {
        error_report("a500plus: the trapdoor extends chip RAM by at most 1MB");
        exit(1);
    }
    if (machine->ram_size) {
        memory_region_add_subregion(sysmem, A500P_TRAPDOOR_BASE, machine->ram);
    }
}

static void a500plus_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AmigaMachineClass *amc = AMIGA_MACHINE_CLASS(oc);

    mc->desc = "Commodore Amiga 500 Plus (68000, ECS)";
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->default_ram_size = A500P_TRAPDOOR_MAX;
    mc->default_ram_id = "amiga.chipram-exp";

    amc->rom_base = A500P_ROM_BASE;
    amc->rom_size = 512 * KiB;
    amc->chipram_size = A500P_CHIPRAM_SIZE;
    /* the 68000's whole 24-bit address space terminates in open bus */
    amc->open_bus_size = 0x01000000;
    /* ECS 2MB Agnus 8375 and ECS Denise 8373: the shared base defaults */
    amc->board_init = a500plus_board_init;
}

static const TypeInfo a500plus_machine_types[] = {
    {
        .name          = TYPE_A500PLUS_MACHINE,
        .parent        = TYPE_AMIGA_MACHINE,
        .instance_size = sizeof(AmigaMachineState),
        .class_init    = a500plus_machine_class_init,
    },
};

DEFINE_TYPES(a500plus_machine_types)
