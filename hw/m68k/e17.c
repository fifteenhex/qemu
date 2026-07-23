/*
 * ELTEC Eurocom E17 — VMEbus 68040 single board computer
 *
 * A reverse engineering aid as much as an emulator: the board is
 * modelled from what the RMON 3.1.3 monitor ROM does with the
 * hardware, documented in E17-NOTES.md in the tree root.
 *
 * Memory map (from the RMON boot path):
 *   0x00000000  DRAM (-m, sized by the firmware by wrap detection)
 *   0x0fc00000  video RAM, 4MB window (e17-vid)
 *   0xfe800000  1MB EPROM window: a 256KB image mirrored 4 times
 *   0xfea00000  battery backed SRAM, 1MB
 *   0xfec00000  onboard I/O (e17-sysc + CD2401 serial + e17-vid regs)
 *
 * Fit the RMON image with -bios; both the raw 256KB EPROM content
 * and a full 1MB dump of the window work.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/reset.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/char/cd2401.h"
#include "hw/display/e17_vid.h"
#include "hw/misc/e17_sysc.h"
#include "target/m68k/cpu.h"

#define E17_VRAM_BASE       0x0fc00000
#define E17_VID_DAC_BASE    0xfec40000
#define E17_VID_CRTC_BASE   0xfec48000
#define E17_ROM_BASE        0xfe800000
#define E17_ROM_SIZE        (1 * MiB)
#define E17_ROM_IMAGE_SIZE  (256 * KiB)
#define E17_SRAM_BASE       0xfea00000
#define E17_SRAM_SIZE       (1 * MiB)
#define E17_IO_BASE         0xfec00000
#define E17_CD2401_BASE     0xfec64000
#define E17_CD2401_IACK     0xfec66000

#define E17_DEFAULT_RAM_SIZE (16 * MiB)

typedef struct {
    M68kCPU *cpu;
    uint32_t initial_sp;
    uint32_t initial_pc;
} E17ResetInfo;

static void e17_cpu_reset(void *opaque)
{
    E17ResetInfo *ri = opaque;

    cpu_reset(CPU(ri->cpu));

    /*
     * The 68040 fetches the initial SSP and PC from 0 where the boot
     * logic maps the EPROM until the chip selects are set up; use the
     * values from the ROM image instead of modelling the overlay.
     */
    ri->cpu->env.aregs[7] = ri->initial_sp;
    ri->cpu->env.pc = ri->initial_pc;
}

/* The secondary CPU is held in halt until released via the sysc */
static void e17_slave_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cs->halted = 1;
}

/*
 * Releasing the secondary CPU (0x20 to the control register at
 * 0xfec58000) makes it fetch SP and PC from DRAM 0/4, where the
 * primary has planted a trampoline.
 */
static void e17_slave_run(void *opaque, int n, int level)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    if (!level || !cs->halted) {
        return;
    }
    cpu_reset(cs);
    cpu->env.aregs[7] = address_space_ldl(&address_space_memory, 0,
                                          MEMTXATTRS_UNSPECIFIED, NULL);
    cpu->env.pc = address_space_ldl(&address_space_memory, 4,
                                    MEMTXATTRS_UNSPECIFIED, NULL);
    cs->halted = 0;
    qemu_cpu_kick(cs);
}

static void e17_init(MachineState *machine)
{
    M68kCPU *cpu;
    M68kCPU *slave_cpu = NULL;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *rom = g_new(MemoryRegion, 1);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    DeviceState *sysc_dev;
    DeviceState *serial_dev;
    DeviceState *vid_dev;
    gchar *bios_size_err;
    int64_t bios_size = -1;
    int i;

    cpu = M68K_CPU(cpu_create(machine->cpu_type));

    /* the optional secondary CPU, released through the sysc */
    if (machine->smp.cpus > 1) {
        slave_cpu = M68K_CPU(cpu_create(machine->cpu_type));
        qemu_register_reset(e17_slave_cpu_reset, slave_cpu);
    }

    /* DRAM */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    /* battery backed SRAM */
    memory_region_init_ram(sram, NULL, "e17.sram", E17_SRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, E17_SRAM_BASE, sram);

    /* EPROM window: 256KB device appearing 4 times */
    memory_region_init_rom(rom, NULL, "e17.rom", E17_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, E17_ROM_BASE, rom);
    if (machine->firmware) {
        bios_size = get_image_size(machine->firmware, NULL);
    }
    if (bios_size != E17_ROM_IMAGE_SIZE && bios_size != E17_ROM_SIZE) {
        bios_size_err = bios_size < 0 ?
            g_strdup("not found") :
            g_strdup_printf("has %" PRId64 " bytes", bios_size);
        error_report("e17 needs a 256KB or 1MB RMON EPROM image "
                     "(-bios rmon.bin): %s %s",
                     machine->firmware ? machine->firmware : "(no -bios)",
                     bios_size_err);
        exit(1);
    }
    for (i = 0; i < E17_ROM_SIZE / E17_ROM_IMAGE_SIZE; i++) {
        load_image_targphys(machine->firmware,
                            E17_ROM_BASE + i * E17_ROM_IMAGE_SIZE,
                            E17_ROM_SIZE - i * E17_ROM_IMAGE_SIZE, NULL);
        if (bios_size == E17_ROM_SIZE) {
            break;
        }
    }

    /*
     * ROM blob contents only appear in the region at machine reset,
     * after init-registered reset handlers have run: take the reset
     * vectors from the file.
     */
    {
        E17ResetInfo *ri = g_new0(E17ResetInfo, 1);
        uint8_t vecs[8];

        if (load_image_size(machine->firmware, vecs,
                            sizeof(vecs)) != sizeof(vecs)) {
            error_report("e17: cannot read reset vectors from %s",
                         machine->firmware);
            exit(1);
        }
        ri->cpu = cpu;
        ri->initial_sp = ldl_be_p(&vecs[0]);
        ri->initial_pc = ldl_be_p(&vecs[4]);
        qemu_register_reset(e17_cpu_reset, ri);
    }

    /* onboard I/O */
    sysc_dev = qdev_new(TYPE_E17_SYSC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sysc_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(sysc_dev), 0, E17_IO_BASE);
    if (slave_cpu) {
        qdev_connect_gpio_out_named(sysc_dev, "slave-run", 0,
                                    qemu_allocate_irq(e17_slave_run,
                                                      slave_cpu, 0));
    }

    /* onboard video: VRAM plus the DAC/CRTC blocks inside the sysc */
    vid_dev = qdev_new(TYPE_E17_VID);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(vid_dev), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(vid_dev), 0,
                            E17_VID_DAC_BASE, 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(vid_dev), 1,
                            E17_VID_CRTC_BASE, 1);
    sysbus_mmio_map(SYS_BUS_DEVICE(vid_dev), 2, E17_VRAM_BASE);

    /* CD2401, serial ports 1-4; sits inside the e17-sysc window */
    serial_dev = qdev_new(TYPE_CD2401);
    qdev_prop_set_chr(serial_dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(serial_dev, "chrB", serial_hd(1));
    qdev_prop_set_chr(serial_dev, "chrC", serial_hd(2));
    qdev_prop_set_chr(serial_dev, "chrD", serial_hd(3));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(serial_dev), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(serial_dev), 0,
                            E17_CD2401_BASE, 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(serial_dev), 1,
                            E17_CD2401_IACK, 1);
}

static void e17_machine_init(MachineClass *mc)
{
    mc->desc = "ELTEC Eurocom E17";
    mc->init = e17_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->default_ram_size = E17_DEFAULT_RAM_SIZE;
    mc->default_ram_id = "dram";
    /* the board carries a second 68040; -smp 1 removes it */
    mc->max_cpus = 2;
    mc->default_cpus = 2;
}

DEFINE_MACHINE("e17", e17_machine_init)
