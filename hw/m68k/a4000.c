/*
 * Commodore Amiga 4000.
 *
 * 68040 at 25MHz with the AGA chipset (Alice/Lisa) and 2MB chip RAM.
 * This is an early bring-up: it boots the A4000 Kickstart on the AGA
 * chipset IDs; the onboard IDE and the AGA-only display features are
 * not modelled yet, so the OS renders through the ECS-compatible path.
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
#include "hw/m68k/amiga_mobo.h"
#include "target/m68k/cpu.h"

#define A4000_FASTRAM_BASE      0x07000000
#define A4000_RAMSEY_BASE       0xde0000
#define A4000_ROM_BASE          0xf80000

#define TYPE_A4000_MACHINE MACHINE_TYPE_NAME("a4000")
OBJECT_DECLARE_SIMPLE_TYPE(A4000MachineState, A4000_MACHINE)

struct A4000MachineState {
    AmigaMachineState parent_obj;

    MemoryRegion z3_open_bus;
};

static void a4000_board_init(AmigaMachineState *ams)
{
    A4000MachineState *s = A4000_MACHINE(ams);
    MachineState *machine = MACHINE(ams);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *mobo_dev;

    /* 68040 local-bus fast RAM at 0x07000000 (the SIMM sockets) */
    if (machine->ram_size > 16 * MiB) {
        error_report("a4000: motherboard fast RAM is limited to 16MB");
        exit(1);
    }
    if (machine->ram_size) {
        memory_region_add_subregion(sysmem, A4000_FASTRAM_BASE, machine->ram);
    }

    /* Ramsey memory controller and Fat Gary bus glue */
    mobo_dev = qdev_new(TYPE_AMIGA_MOBO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(mobo_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(mobo_dev), 0, A4000_RAMSEY_BASE);

    /*
     * Zorro III configuration space reads open bus so the OS's config
     * chain scan sees no board present instead of bus-erroring.
     */
    memory_region_init_io(&s->z3_open_bus, OBJECT(s), &amiga_open_bus_ops,
                          NULL, "a4000.z3-config-open-bus", 0x01000000);
    memory_region_add_subregion(sysmem, 0xff000000, &s->z3_open_bus);
}

static void a4000_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AmigaMachineClass *amc = AMIGA_MACHINE_CLASS(oc);

    mc->desc = "Commodore Amiga 4000 (68040, AGA)";
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->default_ram_size = 8 * MiB;
    mc->default_ram_id = "amiga.fastram";

    amc->rom_base = A4000_ROM_BASE;
    amc->rom_size = 512 * KiB;
    amc->chipram_size = 2 * MiB;
    amc->open_bus_size = 0x10000000;
    /* AGA: 2MB Alice reports VPOSR id 0x23, Lisa reports Denise id 0xf8 */
    amc->agnus_id = 0x23;
    amc->denise_id = 0xf8;
    amc->board_init = a4000_board_init;
}

static const TypeInfo a4000_machine_types[] = {
    {
        .name          = TYPE_A4000_MACHINE,
        .parent        = TYPE_AMIGA_MACHINE,
        .instance_size = sizeof(A4000MachineState),
        .class_init    = a4000_machine_class_init,
    },
};

DEFINE_TYPES(a4000_machine_types)
