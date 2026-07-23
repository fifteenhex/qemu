/*
 * Commodore Amiga 500.
 *
 * 68000 at 7MHz with the OCS/ECS chipset and 512KB chip RAM, plus the
 * classic A501 "trapdoor" expansion: up to 1.5MB of slow (ranger) RAM
 * starting at 0x00C00000.  There is no fast RAM (the 68000 has only a
 * 24-bit address bus), no Ramsey/Fat Gary, and no Zorro III space, so
 * the board is little more than the shared Amiga base plus the trapdoor
 * RAM.
 *
 * Boots the A500/A600/A2000 Kickstart (developed against 3.1 r40.63).
 * The chipset IDs are inherited from the shared base (ECS Agnus/Denise),
 * modelling an A500 fitted with the ECS upgrade and a 3.1 ROM.
 *
 * Memory map (motherboard):
 *   0x00000000  chip RAM (512KB)
 *   0x00bfd000  CIA-B
 *   0x00bfe000  CIA-A
 *   0x00c00000  slow "ranger" RAM (A501 trapdoor expansion)
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

#define A500_SLOWRAM_BASE       0x00c00000
#define A500_SLOWRAM_MAX        (0x00d80000 - A500_SLOWRAM_BASE)  /* 1.5MB */
#define A500_ROM_BASE           0x00f80000

#define TYPE_A500_MACHINE MACHINE_TYPE_NAME("a500")

static void a500_board_init(AmigaMachineState *ams)
{
    MachineState *machine = MACHINE(ams);
    MemoryRegion *sysmem = get_system_memory();

    /*
     * The A501 trapdoor expansion: "slow" RAM on the chip bus at
     * 0x00C00000, sized and added to the free list by Kickstart.
     */
    if (machine->ram_size > A500_SLOWRAM_MAX) {
        error_report("a500: trapdoor slow RAM is limited to 1.5MB");
        exit(1);
    }
    if (machine->ram_size) {
        memory_region_add_subregion(sysmem, A500_SLOWRAM_BASE, machine->ram);
    }
}

static void a500_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AmigaMachineClass *amc = AMIGA_MACHINE_CLASS(oc);

    mc->desc = "Commodore Amiga 500 (68000)";
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->default_ram_size = 512 * KiB;
    mc->default_ram_id = "amiga.slowram";

    amc->rom_base = A500_ROM_BASE;
    amc->rom_size = 512 * KiB;
    amc->chipram_size = 512 * KiB;
    /* the 68000's whole 24-bit address space terminates in open bus */
    amc->open_bus_size = 0x01000000;
    amc->board_init = a500_board_init;
}

static const TypeInfo a500_machine_types[] = {
    {
        .name          = TYPE_A500_MACHINE,
        .parent        = TYPE_AMIGA_MACHINE,
        .instance_size = sizeof(AmigaMachineState),
        .class_init    = a500_machine_class_init,
    },
};

DEFINE_TYPES(a500_machine_types)
