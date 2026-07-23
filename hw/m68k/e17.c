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
#include "elf.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/char/cd2401.h"
#include "hw/display/e17_vid.h"
#include "hw/misc/e17_sysc.h"
#include "hw/net/lance.h"
#include "hw/scsi/ncr53c720.h"
#include "system/dma.h"
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
#define E17_LANCE_RDP       0xfec68002
#define E17_LANCE_RAP       0xfec68006
#define E17_SCSI_BASE       0xfec6c000

#define E17_DEFAULT_RAM_SIZE (16 * MiB)

struct E17MachineState {
    MachineState parent_obj;
    bool video;
};

#define TYPE_E17_MACHINE MACHINE_TYPE_NAME("e17")
OBJECT_DECLARE_SIMPLE_TYPE(E17MachineState, E17_MACHINE)

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

/*
 * The ethernet chip is an Am79C900 ILACC (the 32-bit LANCE) wired
 * onto the big endian bus with full byte lane reversal, and the chip
 * compensates on the frame data path so bytes end up on the wire in
 * memory order.  Model: reverse every 32-bit group for descriptor
 * and initialization block DMA, pass frame data through.  The pcnet
 * core gives no direct descriptor-vs-data indication, but its
 * descriptor accesses are always exactly sizeof(initblk16/32) or
 * sizeof(xmd) — 12, 16 or 28 bytes — and frames are at least 60.
 */
static void e17_lance_swap(uint8_t *buf, int len)
{
    int i;

    if (len != 12 && len != 16 && len != 28) {
        return;
    }
    for (i = 0; i + 3 < len; i += 4) {
        uint8_t t = buf[i];

        buf[i] = buf[i + 3];
        buf[i + 3] = t;
        t = buf[i + 1];
        buf[i + 1] = buf[i + 2];
        buf[i + 2] = t;
    }
}

static void e17_lance_dma_read(void *opaque, hwaddr addr,
                               uint8_t *buf, int len, int do_bswap)
{
    dma_memory_read(&address_space_memory, addr, buf, len,
                    MEMTXATTRS_UNSPECIFIED);
    e17_lance_swap(buf, len);
}

static void e17_lance_dma_write(void *opaque, hwaddr addr,
                                uint8_t *buf, int len, int do_bswap)
{
    e17_lance_swap(buf, len);
    dma_memory_write(&address_space_memory, addr, buf, len,
                     MEMTXATTRS_UNSPECIFIED);
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
    E17MachineState *e17 = E17_MACHINE(machine);
    M68kCPU *cpu;
    M68kCPU *slave_cpu = NULL;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *rom = g_new(MemoryRegion, 1);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    DeviceState *sysc_dev;
    DeviceState *serial_dev;
    DeviceState *vid_dev;
    DeviceState *lance_dev;
    DeviceState *scsi_dev;
    MemoryRegion *lance_rdp = g_new(MemoryRegion, 1);
    MemoryRegion *lance_rap = g_new(MemoryRegion, 1);
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
    if (!machine->kernel_filename &&
        bios_size != E17_ROM_IMAGE_SIZE && bios_size != E17_ROM_SIZE) {
        bios_size_err = bios_size < 0 ?
            g_strdup("not found") :
            g_strdup_printf("has %" PRId64 " bytes", bios_size);
        error_report("e17 needs a 256KB or 1MB RMON EPROM image "
                     "(-bios rmon.bin): %s %s",
                     machine->firmware ? machine->firmware : "(no -bios)",
                     bios_size_err);
        exit(1);
    }
    for (i = 0; machine->firmware &&
                i < E17_ROM_SIZE / E17_ROM_IMAGE_SIZE; i++) {
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
     * vectors from the file.  -kernel loads an ELF (a u-boot build,
     * say) into RAM instead and starts at its entry point, standing
     * in for "load it over the ROM monitor and jump to it".
     */
    {
        E17ResetInfo *ri = g_new0(E17ResetInfo, 1);
        uint8_t vecs[8];
        uint64_t elf_entry;

        if (machine->kernel_filename) {
            if (load_elf(machine->kernel_filename, NULL, NULL, NULL,
                         &elf_entry, NULL, NULL, NULL,
                         ELFDATA2MSB, EM_68K, 0, 0) <= 0) {
                error_report("e17: cannot load %s",
                             machine->kernel_filename);
                exit(1);
            }
            ri->initial_sp = 0x8000;
            ri->initial_pc = elf_entry;
        } else {
            if (load_image_size(machine->firmware, vecs,
                                sizeof(vecs)) != sizeof(vecs)) {
                error_report("e17: cannot read reset vectors from %s",
                             machine->firmware);
                exit(1);
            }
            ri->initial_sp = ldl_be_p(&vecs[0]);
            ri->initial_pc = ldl_be_p(&vecs[4]);
        }
        ri->cpu = cpu;
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

    /*
     * Onboard video: VRAM plus the DAC/CRTC blocks inside the sysc
     * window.  video=off depopulates it, which makes RMON fall back
     * to the serial console (keyboard input is not modelled yet, so
     * with video fitted the monitor is display-only).
     */
    if (e17->video) {
        vid_dev = qdev_new(TYPE_E17_VID);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(vid_dev), &error_fatal);
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(vid_dev), 0,
                                E17_VID_DAC_BASE, 1);
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(vid_dev), 1,
                                E17_VID_CRTC_BASE, 1);
        sysbus_mmio_map(SYS_BUS_DEVICE(vid_dev), 2, E17_VRAM_BASE);
    }

    /*
     * LANCE ethernet.  The 16-bit RDP and RAP registers live in the
     * low halves of the first two 32-bit words at 0xfec68000, so
     * alias them onto the generic model's RDP+0/RAP+2 layout.
     */
    lance_dev = qdev_new(TYPE_LANCE);
    qdev_prop_set_bit(lance_dev, "ssize32", true);
    qemu_configure_nic_device(lance_dev, true, NULL);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(lance_dev), &error_fatal);
    {
        SysBusPCNetState *d = SYSBUS_PCNET(lance_dev);

        d->state.phys_mem_read = e17_lance_dma_read;
        d->state.phys_mem_write = e17_lance_dma_write;
    }
    memory_region_init_alias(lance_rdp, NULL, "e17.lance-rdp",
                             SYS_BUS_DEVICE(lance_dev)->mmio[0].memory,
                             0, 2);
    memory_region_add_subregion_overlap(sysmem, E17_LANCE_RDP,
                                        lance_rdp, 1);
    memory_region_init_alias(lance_rap, NULL, "e17.lance-rap",
                             SYS_BUS_DEVICE(lance_dev)->mmio[0].memory,
                             2, 2);
    memory_region_add_subregion_overlap(sysmem, E17_LANCE_RAP,
                                        lance_rap, 1);

    /*
     * NCR 53C720 SCSI (the register map RMON drives is the 720/8xx
     * one, not the 710's — see E17-NOTES.md).  Wired with the byte
     * lanes reversed within 32-bit words like the descriptor path of
     * the LANCE.  RMON boots from SCSI ID 6 by default:
     *   -device scsi-hd,scsi-id=6,drive=...
     */
    scsi_dev = qdev_new(TYPE_NCR53C720);
    qdev_prop_set_bit(scsi_dev, "lane-swap", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(scsi_dev), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(scsi_dev), 0, E17_SCSI_BASE, 1);
    scsi_bus_legacy_handle_cmdline(&NCR53C720(scsi_dev)->bus);

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
    /* the CD2401 interrupt line is monitored through VIC068A LICR6 */
    sysbus_connect_irq(SYS_BUS_DEVICE(serial_dev), 0,
                       qdev_get_gpio_in_named(sysc_dev, "cd2401-irq", 0));
}

static bool e17_get_video(Object *obj, Error **errp)
{
    return E17_MACHINE(obj)->video;
}

static void e17_set_video(Object *obj, bool value, Error **errp)
{
    E17_MACHINE(obj)->video = value;
}

static void e17_machine_instance_init(Object *obj)
{
    E17_MACHINE(obj)->video = true;
}

static void e17_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "ELTEC Eurocom E17";
    mc->init = e17_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->default_ram_size = E17_DEFAULT_RAM_SIZE;
    mc->default_ram_id = "dram";
    /* the board carries a second 68040; -smp 1 removes it */
    mc->max_cpus = 2;
    mc->default_cpus = 2;
    mc->block_default_type = IF_SCSI;

    object_class_property_add_bool(oc, "video", e17_get_video,
                                   e17_set_video);
    object_class_property_set_description(oc, "video",
        "Fit the onboard video (default on; off = serial console)");
}

static const TypeInfo e17_machine_typeinfo = {
    .name = TYPE_E17_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(E17MachineState),
    .instance_init = e17_machine_instance_init,
    .class_init = e17_machine_class_init,
};

static void e17_machine_register_types(void)
{
    type_register_static(&e17_machine_typeinfo);
}

type_init(e17_machine_register_types)
