/*
 * Commodore Amiga 1000.
 *
 * The original 1985 Amiga: a 68000 at 7MHz with the OCS chipset
 * (Agnus 8361/8367 and Denise 8362, neither of which has an ID
 * register) and 256KB of chip RAM, doubled to 512KB by the common
 * front-panel expansion cartridge.
 *
 * The real machine has no Kickstart ROM.  At reset a small bootstrap
 * ROM is mapped at the CPU's reset vectors; it loads the 256KB
 * Kickstart image from a boot floppy into the "writable control
 * store" (WCS), 256KB of RAM at 0xfc0000, then write-protects the WCS
 * and restarts, after which the machine behaves as if that Kickstart
 * were in ROM until power-off.  We take the common emulator shortcut:
 * -bios supplies the 256KB Kickstart image directly and it is mapped
 * write-protected at 0xfc0000 like a ROM.  The authentic flow could be
 * modelled later with _amiga_assets/a1000_bootstrap.rom and a
 * write-enabled WCS.
 *
 * Memory map (motherboard):
 *   0x00000000  chip RAM (256KB, front expansion continues at 0x40000)
 *   0x00bfd000  CIA-B
 *   0x00bfe000  CIA-A
 *   0x00dff000  custom chips
 *   0x00fc0000  Kickstart in the WCS (256KB, modelled as ROM)
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

#define A1000_CHIPRAM_SIZE      (256 * KiB)
#define A1000_EXPRAM_BASE       A1000_CHIPRAM_SIZE
#define A1000_EXPRAM_MAX        (256 * KiB)
#define A1000_WCS_BASE          0x00fc0000

#define TYPE_A1000_MACHINE MACHINE_TYPE_NAME("a1000")

static void a1000_board_init(AmigaMachineState *ams)
{
    MachineState *machine = MACHINE(ams);
    MemoryRegion *sysmem = get_system_memory();

    /*
     * The front-panel expansion cartridge: another 256KB of chip RAM,
     * contiguous with the onboard bank, sized by Kickstart's probe.
     */
    if (machine->ram_size > A1000_EXPRAM_MAX) {
        error_report("a1000: the front expansion adds at most 256KB");
        exit(1);
    }
    if (machine->ram_size) {
        memory_region_add_subregion(sysmem, A1000_EXPRAM_BASE, machine->ram);
    }
}

static void a1000_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AmigaMachineClass *amc = AMIGA_MACHINE_CLASS(oc);

    mc->desc = "Commodore Amiga 1000 (68000)";
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->default_ram_size = A1000_EXPRAM_MAX;
    mc->default_ram_id = "amiga.expram";

    amc->rom_base = A1000_WCS_BASE;
    amc->rom_size = 256 * KiB;
    amc->chipram_size = A1000_CHIPRAM_SIZE;
    /* the 68000's whole 24-bit address space terminates in open bus */
    amc->open_bus_size = 0x01000000;
    amc->agnus_id = 0x00;       /* OCS Agnus: VPOSR id bits read 0 */
    amc->denise_id = 0xff;      /* OCS Denise 8362: no ID, open bus */
    amc->board_init = a1000_board_init;
}

static const TypeInfo a1000_machine_types[] = {
    {
        .name          = TYPE_A1000_MACHINE,
        .parent        = TYPE_AMIGA_MACHINE,
        .instance_size = sizeof(AmigaMachineState),
        .class_init    = a1000_machine_class_init,
    },
};

DEFINE_TYPES(a1000_machine_types)
