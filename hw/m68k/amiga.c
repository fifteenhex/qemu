/*
 * Commodore Amiga machine family, common core.
 *
 * This models the hardware every big-box/classic Amiga shares: chip
 * RAM, the Kickstart ROM (with the reset-time overlay at address 0
 * controlled by CIA-A PA0), the two 8520 CIAs and the custom chip
 * register block.  Board variants subclass this to add their CPU,
 * memory controller and I/O specifics.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/datadir.h"
#include "system/reset.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "hw/m68k/amiga.h"
#include "hw/m68k/amiga_custom.h"
#include "hw/m68k/amiga_fdc.h"
#include "hw/m68k/mos8520.h"
#include "system/blockdev.h"
#include "target/m68k/cpu.h"

#define AMIGA_CIAB_BASE     0xbfd000
#define AMIGA_CIAA_BASE     0xbfe000
#define AMIGA_CUSTOM_BASE   0xdff000

/* Paula presents interrupts on the 68k autovectors */
static void amiga_set_ipl(void *opaque, int n, int level)
{
    M68kCPU *cpu = opaque;

    if (level) {
        m68k_set_irq_level(cpu, level, EXCP_INT_LEVEL_1 + level - 1);
    } else {
        m68k_set_irq_level(cpu, 0, 0);
    }
}

/* CIA-A PA0: ROM overlay at address 0 (pulled up, so on at reset) */
static void amiga_overlay_set(void *opaque, int n, int level)
{
    AmigaMachineState *ams = opaque;

    memory_region_set_enabled(&ams->rom_overlay, level != 0);
}

static void amiga_overlay_reset(void *opaque)
{
    AmigaMachineState *ams = opaque;

    memory_region_set_enabled(&ams->rom_overlay, true);
}

static void amiga_cpu_reset(void *opaque)
{
    AmigaMachineState *ams = opaque;
    M68kCPU *cpu = ams->cpu;
    uint8_t *rom = memory_region_get_ram_ptr(&ams->rom);

    cpu_reset(CPU(cpu));
    /* initial SP/PC come from the ROM via the reset-time overlay */
    cpu->env.aregs[7] = ldl_be_p(rom);
    cpu->env.pc = ldl_be_p(rom + 4);
}

/*
 * Chip-bus filler: the glue logic terminates every cycle in this range,
 * so probing unpopulated addresses reads open bus rather than faulting.
 */
static uint64_t amiga_open_bus_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "amiga: open-bus read 0x%08" HWADDR_PRIx "\n",
                  addr);
    return (uint64_t)-1;
}

static void amiga_open_bus_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
}

const MemoryRegionOps amiga_open_bus_ops = {
    .read = amiga_open_bus_read,
    .write = amiga_open_bus_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void amiga_machine_init(MachineState *machine)
{
    AmigaMachineState *ams = AMIGA_MACHINE(machine);
    AmigaMachineClass *amc = AMIGA_MACHINE_GET_CLASS(machine);
    MemoryRegion *sysmem = get_system_memory();
    ssize_t rom_loaded;
    DriveInfo *dinfo;
    char *filename;

    /*
     * Register the overlay reset before creating the CPU so the ROM is
     * mapped at 0 by the time the CPU reset fetches its initial SP/PC.
     */
    qemu_register_reset(amiga_overlay_reset, ams);

    ams->cpu = M68K_CPU(cpu_create(machine->cpu_type));
    qemu_register_reset(amiga_cpu_reset, ams);

    if (amc->open_bus_size) {
        memory_region_init_io(&ams->open_bus, OBJECT(machine),
                              &amiga_open_bus_ops, NULL, "amiga.open-bus",
                              amc->open_bus_size);
        memory_region_add_subregion_overlap(sysmem, 0, &ams->open_bus, -1);
    }

    memory_region_init_ram(&ams->chipram, NULL, "amiga.chipram",
                           amc->chipram_size, &error_fatal);
    memory_region_add_subregion(sysmem, 0, &ams->chipram);

    /* Kickstart ROM, and its reset-time overlay at address 0 */
    memory_region_init_rom(&ams->rom, NULL, "amiga.kickstart",
                           amc->rom_size, &error_fatal);
    memory_region_add_subregion(sysmem, amc->rom_base, &ams->rom);
    if (!machine->firmware) {
        error_report("A Kickstart ROM image is required, use -bios");
        exit(1);
    }
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    if (!filename) {
        error_report("Could not find Kickstart ROM '%s'", machine->firmware);
        exit(1);
    }
    /*
     * Load the image directly rather than through the rom loader: the
     * contents must be in place before the first CPU reset fetches the
     * initial SP/PC through the overlay.
     */
    rom_loaded = load_image_size(filename, memory_region_get_ram_ptr(&ams->rom),
                                 amc->rom_size);
    g_free(filename);
    if (rom_loaded < 0) {
        error_report("Could not load Kickstart ROM '%s'", machine->firmware);
        exit(1);
    }

    memory_region_init_alias(&ams->rom_overlay, OBJECT(machine),
                             "amiga.kickstart-overlay", &ams->rom, 0,
                             amc->rom_size);
    memory_region_add_subregion_overlap(sysmem, 0, &ams->rom_overlay, 1);

    /* DF0, the internal floppy drive */
    ams->fdc = qdev_new(TYPE_AMIGA_FDC);
    dinfo = drive_get(IF_FLOPPY, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive(ams->fdc, "drive", blk_by_legacy_dinfo(dinfo));
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ams->fdc), &error_fatal);

    /* custom chips */
    ams->custom = qdev_new(TYPE_AMIGA_CUSTOM);
    qdev_prop_set_chr(ams->custom, "chardev", serial_hd(0));
    qdev_prop_set_uint32(ams->custom, "agnus-id", amc->agnus_id);
    object_property_set_link(OBJECT(ams->custom), "fdc", OBJECT(ams->fdc),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ams->custom), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ams->custom), 0, AMIGA_CUSTOM_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(ams->custom), 0,
                       qemu_allocate_irq(amiga_set_ipl, ams->cpu, 0));

    /* CIA-A: TOD counts vertical sync */
    ams->ciaa = qdev_new(TYPE_MOS8520);
    qdev_prop_set_uint32(ams->ciaa, "clock-frequency", amc->cia_clock_hz);
    qdev_prop_set_uint32(ams->ciaa, "tod-frequency", 50);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ams->ciaa), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ams->ciaa), 0, AMIGA_CIAA_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(ams->ciaa), 0,
                       qdev_get_gpio_in_named(ams->custom, "cia-irq", 0));
    qdev_connect_gpio_out_named(ams->ciaa, "port-a-out", 0,
                                qemu_allocate_irq(amiga_overlay_set, ams, 0));

    /* CIA-B: TOD counts horizontal sync */
    ams->ciab = qdev_new(TYPE_MOS8520);
    qdev_prop_set_uint32(ams->ciab, "clock-frequency", amc->cia_clock_hz);
    qdev_prop_set_uint32(ams->ciab, "tod-frequency", 15625);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ams->ciab), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ams->ciab), 0, AMIGA_CIAB_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(ams->ciab), 0,
                       qdev_get_gpio_in_named(ams->custom, "cia-irq", 1));

    /*
     * DF0: control lines on CIA-B port B, status lines on CIA-A port
     * A, and the index pulse on CIA-B FLAG.
     */
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_SEL0,
                                qdev_get_gpio_in_named(ams->fdc, "sel", 0));
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_MTR,
                                qdev_get_gpio_in_named(ams->fdc, "mtr", 0));
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_STEP,
                                qdev_get_gpio_in_named(ams->fdc, "step", 0));
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_DIR,
                                qdev_get_gpio_in_named(ams->fdc, "dir", 0));
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_SIDE,
                                qdev_get_gpio_in_named(ams->fdc, "side", 0));
    qdev_connect_gpio_out_named(ams->fdc, "chng", 0,
                                qdev_get_gpio_in_named(ams->ciaa, "port-in",
                                                       AMIGA_CIAA_PA_CHNG));
    qdev_connect_gpio_out_named(ams->fdc, "wpro", 0,
                                qdev_get_gpio_in_named(ams->ciaa, "port-in",
                                                       AMIGA_CIAA_PA_WPRO));
    qdev_connect_gpio_out_named(ams->fdc, "tk0", 0,
                                qdev_get_gpio_in_named(ams->ciaa, "port-in",
                                                       AMIGA_CIAA_PA_TK0));
    qdev_connect_gpio_out_named(ams->fdc, "rdy", 0,
                                qdev_get_gpio_in_named(ams->ciaa, "port-in",
                                                       AMIGA_CIAA_PA_RDY));
    qdev_connect_gpio_out_named(ams->fdc, "index", 0,
                                qdev_get_gpio_in_named(ams->ciab, "flag", 0));

    if (amc->board_init) {
        amc->board_init(ams);
    }
}

static void amiga_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AmigaMachineClass *amc = AMIGA_MACHINE_CLASS(oc);

    mc->init = amiga_machine_init;
    mc->block_default_type = IF_NONE;
    mc->no_parallel = 1;
    mc->no_cdrom = 1;

    /* PAL defaults */
    amc->cia_clock_hz = 709379;     /* E clock: 7.09MHz / 10 */
    amc->agnus_id = 0x22;           /* ECS 2MB Agnus */
}

static const TypeInfo amiga_machine_types[] = {
    {
        .name          = TYPE_AMIGA_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(AmigaMachineState),
        .class_size    = sizeof(AmigaMachineClass),
        .class_init    = amiga_machine_class_init,
        .abstract      = true,
    },
};

DEFINE_TYPES(amiga_machine_types)
