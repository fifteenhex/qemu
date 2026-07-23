/*
 * Commodore Amiga 1200.
 *
 * The 1992 AGA wedge: a 68EC020 at 14MHz with the AGA chipset
 * (Alice/Lisa) and 2MB of chip RAM, plus the Gayle gate array for IDE
 * and PCMCIA.  QEMU has no separate EC020 model, so the machine uses
 * the full 68020 core.  The EC020's address bus is only 24 bits wide,
 * so on real hardware accesses above 16MB wrap into the low memory
 * map; with QEMU's 020 driving full 32-bit addresses, the machine
 * instead terminates the whole 32-bit space in open bus, which
 * satisfies both Kickstart's expansion-RAM probe (it walks upwards in
 * 1MB steps and must not bus-error) and the Zorro III config scan
 * (0xff reads mean "no board").
 *
 * Gayle, the IDE port and PCMCIA are not modelled yet, so their
 * register space reads open bus and Kickstart's probes see nothing.
 * There is no fast RAM on the motherboard; trapdoor expansions carry
 * their own autoconfig logic and are not modelled either.
 *
 * As on the A4000, the AGA display renders through the ECS-compatible
 * path plus the 8-bitplane/256-colour support in amiga_custom.
 *
 * Memory map (motherboard):
 *   0x00000000  chip RAM (2MB)
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
#include "qemu/units.h"
#include "hw/core/boards.h"
#include "hw/m68k/amiga.h"
#include "target/m68k/cpu.h"

#define A1200_ROM_BASE          0x00f80000

#define TYPE_A1200_MACHINE MACHINE_TYPE_NAME("a1200")

static void a1200_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AmigaMachineClass *amc = AMIGA_MACHINE_CLASS(oc);

    mc->desc = "Commodore Amiga 1200 (68EC020, AGA)";
    /* QEMU has no EC020 variant; the 020 core stands in for it */
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68020");

    amc->rom_base = A1200_ROM_BASE;
    amc->rom_size = 512 * KiB;
    amc->chipram_size = 2 * MiB;
    /* every cycle terminates: 24-bit decode below, EC020 wrap above */
    amc->open_bus_size = 0x100000000ULL;
    /* AGA: 2MB Alice reports VPOSR id 0x23, Lisa reports Denise id 0xf8 */
    amc->agnus_id = 0x23;
    amc->denise_id = 0xf8;
}

static const TypeInfo a1200_machine_types[] = {
    {
        .name          = TYPE_A1200_MACHINE,
        .parent        = TYPE_AMIGA_MACHINE,
        .instance_size = sizeof(AmigaMachineState),
        .class_init    = a1200_machine_class_init,
    },
};

DEFINE_TYPES(a1200_machine_types)
