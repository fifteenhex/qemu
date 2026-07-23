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
#include "hw/core/split-irq.h"
#include "hw/m68k/amiga.h"
#include "hw/m68k/amiga_custom.h"
#include "hw/m68k/amiga_fdc.h"
#include "hw/m68k/amiga_kbd.h"
#include "hw/m68k/mos8520.h"
#include "hw/ide/gayle.h"
#include "system/blockdev.h"
#include "target/m68k/cpu.h"

#define AMIGA_CIAB_BASE     0xbfd000
#define AMIGA_CIAA_BASE     0xbfe000
#define AMIGA_CUSTOM_BASE   0xdff000

static const char *const floppy_line_name[FLOPPY_LINE_COUNT] = {
    [FLOPPY_LINE_CHNG] = "chng",
    [FLOPPY_LINE_WPRO] = "wpro",
    [FLOPPY_LINE_TK0] = "tk0",
    [FLOPPY_LINE_RDY] = "rdy",
    [FLOPPY_LINE_INDEX] = "index",
};

static void amiga_floppy_line_set(void *opaque, int n, int level)
{
    struct AmigaFloppyLine *l = opaque;

    l->released = deposit32(l->released, n, 1, level);
    qemu_set_irq(l->out, l->released == (1 << AMIGA_FLOPPY_DRIVES) - 1);
}

/* fan one CIA output out to every drive */
static qemu_irq amiga_floppy_split_in(AmigaMachineState *ams,
                                      const char *name)
{
    DeviceState *split = qdev_new(TYPE_SPLIT_IRQ);
    int d;

    qdev_prop_set_uint16(split, "num-lines", AMIGA_FLOPPY_DRIVES);
    qdev_realize_and_unref(split, NULL, &error_fatal);
    for (d = 0; d < AMIGA_FLOPPY_DRIVES; d++) {
        qdev_connect_gpio_out(split, d,
                              qdev_get_gpio_in_named(ams->fdc[d], name, 0));
    }
    return qdev_get_gpio_in(split, 0);
}

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

/*
 * The RESET instruction: /RSTO resets every chip on the board except
 * the CPU, which carries on with the next instruction.  Crucially the
 * CIA reset floats PA0 back high, so the Kickstart overlay returns:
 * the ROM reboot sequence is "reset" followed by a jump through the
 * initial-PC vector at address 4, which must read the ROM again.
 */
static void amiga_reset_out(void *opaque, int n, int level)
{
    AmigaMachineState *ams = opaque;
    int i;

    if (!level) {
        return;
    }
    device_cold_reset(ams->ciaa);
    device_cold_reset(ams->ciab);
    device_cold_reset(ams->custom);
    device_cold_reset(ams->kbd);
    for (i = 0; i < AMIGA_FLOPPY_DRIVES; i++) {
        device_cold_reset(ams->fdc[i]);
    }
    for (i = 0; i < AMIGA_RSTO_DEVS; i++) {
        if (ams->rsto_dev[i]) {
            device_cold_reset(ams->rsto_dev[i]);
        }
    }
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

void amiga_gayle_init(AmigaMachineState *ams)
{
    DeviceState *gayle = qdev_new(TYPE_GAYLE_IDE);
    SysBusDevice *sbd = SYS_BUS_DEVICE(gayle);
    int i;

    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, GAYLE_IDE_ATA_BASE);
    sysbus_mmio_map(sbd, 1, GAYLE_IDE_CTRL_BASE);
    sysbus_mmio_map(sbd, 2, GAYLE_IDE_ID_BASE);
    /* the IDE interrupt is on INT2 */
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in_named(ams->custom, "ports-irq", 0));
    gayle_ide_init_drives(gayle, drive_get(IF_IDE, 0, 0),
                          drive_get(IF_IDE, 0, 1));

    for (i = 0; i < AMIGA_RSTO_DEVS; i++) {
        if (!ams->rsto_dev[i]) {
            ams->rsto_dev[i] = gayle;
            break;
        }
    }
}

static void amiga_machine_init(MachineState *machine)
{
    AmigaMachineState *ams = AMIGA_MACHINE(machine);
    AmigaMachineClass *amc = AMIGA_MACHINE_GET_CLASS(machine);
    MemoryRegion *sysmem = get_system_memory();
    ssize_t rom_loaded;
    DriveInfo *dinfo;
    char *filename;
    int i;

    /*
     * Register the overlay reset before creating the CPU so the ROM is
     * mapped at 0 by the time the CPU reset fetches its initial SP/PC.
     */
    qemu_register_reset(amiga_overlay_reset, ams);

    ams->cpu = M68K_CPU(cpu_create(machine->cpu_type));
    qemu_register_reset(amiga_cpu_reset, ams);
    qdev_connect_gpio_out_named(DEVICE(ams->cpu), "reset-out", 0,
                                qemu_allocate_irq(amiga_reset_out, ams, 0));

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
    /*
     * A 256KB Kickstart (the 1.x images) in a 512KB ROM window sits in
     * a socket whose top address line the chip ignores, so the image
     * appears twice.  Replicate it so the 0xfc0000-based vectors of
     * those images land on code in the upper half of the window.
     */
    if (rom_loaded > 0 && rom_loaded * 2 == amc->rom_size) {
        uint8_t *rom = memory_region_get_ram_ptr(&ams->rom);

        memcpy(rom + rom_loaded, rom, rom_loaded);
    }

    memory_region_init_alias(&ams->rom_overlay, OBJECT(machine),
                             "amiga.kickstart-overlay", &ams->rom, 0,
                             amc->rom_size);
    memory_region_add_subregion_overlap(sysmem, 0, &ams->rom_overlay, 1);

    /* the floppy drives, DF0 and DF1 */
    for (i = 0; i < AMIGA_FLOPPY_DRIVES; i++) {
        ams->fdc[i] = qdev_new(TYPE_AMIGA_FDC);
        dinfo = drive_get(IF_FLOPPY, 0, i);
        if (dinfo) {
            qdev_prop_set_drive(ams->fdc[i], "drive",
                                blk_by_legacy_dinfo(dinfo));
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(ams->fdc[i]), &error_fatal);
    }

    /* custom chips */
    ams->custom = qdev_new(TYPE_AMIGA_CUSTOM);
    qdev_prop_set_chr(ams->custom, "chardev", serial_hd(0));
    if (machine->audiodev) {
        qdev_prop_set_string(ams->custom, "audiodev", machine->audiodev);
    }
    qdev_prop_set_uint32(ams->custom, "agnus-id", amc->agnus_id);
    qdev_prop_set_uint32(ams->custom, "denise-id", amc->denise_id);
    object_property_set_link(OBJECT(ams->custom), "fdc0",
                             OBJECT(ams->fdc[0]), &error_abort);
    object_property_set_link(OBJECT(ams->custom), "fdc1",
                             OBJECT(ams->fdc[1]), &error_abort);
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
    qdev_connect_gpio_out_named(ams->custom, "mouse-btn", 0,
                                qdev_get_gpio_in_named(ams->ciaa, "port-in",
                                                       AMIGA_CIAA_PA_FIR0));
    qdev_connect_gpio_out_named(ams->custom, "joy2-btn", 0,
                                qdev_get_gpio_in_named(ams->ciaa, "port-in",
                                                       AMIGA_CIAA_PA_FIR1));

    /* keyboard: clocks codes into CIA-A's SDR, acked by its SP output */
    ams->kbd = qdev_new(TYPE_AMIGA_KBD);
    object_property_set_link(OBJECT(ams->kbd), "cia", OBJECT(ams->ciaa),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ams->kbd), &error_fatal);
    qdev_connect_gpio_out_named(ams->ciaa, "sp-out", 0,
                                qdev_get_gpio_in_named(ams->kbd, "ack", 0));

    /* CIA-B: TOD counts horizontal sync */
    ams->ciab = qdev_new(TYPE_MOS8520);
    qdev_prop_set_uint32(ams->ciab, "clock-frequency", amc->cia_clock_hz);
    qdev_prop_set_uint32(ams->ciab, "tod-frequency", 15625);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ams->ciab), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ams->ciab), 0, AMIGA_CIAB_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(ams->ciab), 0,
                       qdev_get_gpio_in_named(ams->custom, "cia-irq", 1));

    /*
     * The floppy drives: control lines on CIA-B port B (the four
     * shared lines fanned out to every drive, one select line each),
     * the wired-AND status lines on CIA-A port A, and the index
     * pulse on CIA-B FLAG.
     */
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_MTR,
                                amiga_floppy_split_in(ams, "mtr"));
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_STEP,
                                amiga_floppy_split_in(ams, "step"));
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_DIR,
                                amiga_floppy_split_in(ams, "dir"));
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_SIDE,
                                amiga_floppy_split_in(ams, "side"));
    for (i = 0; i < AMIGA_FLOPPY_DRIVES; i++) {
        qdev_connect_gpio_out_named(ams->ciab, "port-b-out",
                                    AMIGA_CIAB_PB_SEL0 + i,
                                    qdev_get_gpio_in_named(ams->fdc[i],
                                                           "sel", 0));
    }

    ams->floppy_line[FLOPPY_LINE_CHNG].out =
        qdev_get_gpio_in_named(ams->ciaa, "port-in", AMIGA_CIAA_PA_CHNG);
    ams->floppy_line[FLOPPY_LINE_WPRO].out =
        qdev_get_gpio_in_named(ams->ciaa, "port-in", AMIGA_CIAA_PA_WPRO);
    ams->floppy_line[FLOPPY_LINE_TK0].out =
        qdev_get_gpio_in_named(ams->ciaa, "port-in", AMIGA_CIAA_PA_TK0);
    ams->floppy_line[FLOPPY_LINE_RDY].out =
        qdev_get_gpio_in_named(ams->ciaa, "port-in", AMIGA_CIAA_PA_RDY);
    ams->floppy_line[FLOPPY_LINE_INDEX].out =
        qdev_get_gpio_in_named(ams->ciab, "flag", 0);
    for (i = 0; i < FLOPPY_LINE_COUNT; i++) {
        struct AmigaFloppyLine *l = &ams->floppy_line[i];
        int d;

        l->released = (1 << AMIGA_FLOPPY_DRIVES) - 1;
        for (d = 0; d < AMIGA_FLOPPY_DRIVES; d++) {
            qdev_connect_gpio_out_named(ams->fdc[d], floppy_line_name[i], 0,
                                        qemu_allocate_irq(
                                            amiga_floppy_line_set, l, d));
        }
    }

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
    machine_add_audiodev_property(mc);

    /* PAL defaults */
    amc->cia_clock_hz = 709379;     /* E clock: 7.09MHz / 10 */
    amc->agnus_id = 0x22;           /* ECS 2MB Agnus */
    amc->denise_id = 0xfc;          /* ECS Denise */
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
