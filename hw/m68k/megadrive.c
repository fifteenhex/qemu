/*
 * QEMU Sega MegaDrive with Everdrive SSF2 mapper emulation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "system/reset.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "elf.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/memory.h"

#include "target/m68k/cpu.h"
#include "hw/intc/m68k_irqc.h"

#include "hw/display/md_vdp.h"
#include "hw/misc/md_everdrive.h"
#include "hw/misc/md_sys.h"
#include "hw/input/md_io.h"

/*
 * MegaDrive memory map:
 *
 *   0x000000 – 0x3FFFFF   Cartridge ROM (or Everdrive PSRAM)
 *   0xA00000 – 0xA0FFFF   Z80 area (sound RAM, YM2612)
 *   0xA10000 – 0xA1001F   I/O ports
 *   0xA11100              Z80 bus request
 *   0xA11200              Z80 reset
 *   0xA130D0 – 0xA130DF   Everdrive SSF2 mapper registers (mapper=everdrive)
 *   0xC00000 – 0xC0001F   VDP ports
 *   0xE00000 – 0xFFFFFF   Work RAM (64 KB, mirrored every 64 KB)
 */
#define MD_VDP_BASE         0xC00000
#define MD_EVERDRIVE_BASE   0xA130D0
#define MD_IO_BASE          0xA10000
#define MD_Z80_BASE         0xA00000
#define MD_CTRL_BASE        0xA11000

/*
 * ROM is always at zero; with mapper=everdrive we emulate as if the SSF2
 * mapper was configured to expose 4MB of PSRAM.
 */
#define MD_ROM_BASE         0x000000

/*
 * Built-in work RAM (the console's 64 KB of SRAM), mirrored across the
 * whole 0xE00000-0xFFFFFF window as on real hardware.
 */
#define MD_WORK_RAM_MIRROR_BASE 0xE00000
#define MD_WORK_RAM_SIZE    (64 * KiB)
#define MD_WORK_RAM_MIRRORS 32

struct MegadriveMachineState {
    MachineState parent_obj;

    char *mapper;
};

/*
 * The 68000 in the MegaDrive has no bus-error generator: reads of
 * unpopulated addresses return open-bus garbage and writes disappear.
 * Model that with a background region behind everything else so stray
 * guest accesses don't raise access faults.
 */
static uint64_t md_open_bus_read(void *opaque, hwaddr offset, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
        "megadrive: open-bus read at 0x%08" HWADDR_PRIx "\n", offset);
    return 0;
}

static void md_open_bus_write(void *opaque, hwaddr offset, uint64_t val,
                              unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
        "megadrive: open-bus write 0x%" PRIx64 " at 0x%08" HWADDR_PRIx "\n",
        val, offset);
}

static const MemoryRegionOps md_open_bus_ops = {
    .read       = md_open_bus_read,
    .write      = md_open_bus_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

#define TYPE_MEGADRIVE_MACHINE MACHINE_TYPE_NAME("megadrive")
OBJECT_DECLARE_SIMPLE_TYPE(MegadriveMachineState, MEGADRIVE_MACHINE)

static void main_cpu_reset(void *opaque)
{
    M68kCPU  *cpu = opaque;
    CPUState *cs  = CPU(cpu);

    cpu_reset(cs);

    cpu->env.aregs[7] = ldl_phys(cs->as, MD_ROM_BASE + 0x00);
    cpu->env.pc       = ldl_phys(cs->as, MD_ROM_BASE + 0x04);

    printf("reset: SP=0x%08x PC=0x%08x\n",
           (unsigned)cpu->env.aregs[7],
           (unsigned)cpu->env.pc);
}

static void megadrive_init(MachineState *machine)
{
    MegadriveMachineState *mms = MEGADRIVE_MACHINE(machine);
    M68kCPU       *cpu = NULL;
    DeviceState   *irqc;
    DeviceState   *vdp_dev;
    SysBusDevice  *vdp_sbd;
    DeviceState   *sys_dev;
    SysBusDevice  *sys_sbd;
    DeviceState   *io_dev;
    SysBusDevice  *io_sbd;
    MemoryRegion  *address_space_mem = get_system_memory();
    MemoryRegion  *rom_region;
    MemoryRegion  *work_ram;
    ssize_t        rom_size;
    bool           everdrive;
    unsigned       i;

    if (!mms->mapper || !strcmp(mms->mapper, "everdrive")) {
        everdrive = true;
    } else if (!strcmp(mms->mapper, "cart")) {
        everdrive = false;
    } else {
        error_report("Unknown mapper '%s' (use 'everdrive' or 'cart')",
                     mms->mapper);
        exit(1);
    }

    if (!machine->firmware) {
        error_report("No firmware specified. Use -bios <rom.bin>");
        exit(1);
    }

    cpu = M68K_CPU(cpu_create(machine->cpu_type));
    qemu_register_reset(main_cpu_reset, cpu);

    {
        MemoryRegion *open_bus = g_new(MemoryRegion, 1);

        memory_region_init_io(open_bus, NULL, &md_open_bus_ops, NULL,
                              "md.open-bus", 0x1000000);
        memory_region_add_subregion_overlap(address_space_mem, 0, open_bus,
                                            -1);
    }

    irqc = qdev_new(TYPE_M68K_IRQC);
    object_property_set_link(OBJECT(irqc), "m68k-cpu", OBJECT(cpu),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(irqc), &error_fatal);

    if (everdrive) {
        /* Everdrive PSRAM: the whole cartridge window is writable RAM */
        rom_region = machine->ram;
    } else {
        /* Real cartridge: the ROM image is read-only, sized to the image */
        rom_size = get_image_size(machine->firmware, NULL);
        if (rom_size <= 0) {
            error_report("Failed to load firmware '%s'", machine->firmware);
            exit(1);
        }
        rom_region = g_new(MemoryRegion, 1);
        memory_region_init_rom(rom_region, NULL, "md.cart",
                               ROUND_UP(rom_size, 2), &error_fatal);
    }
    memory_region_add_subregion(address_space_mem, MD_ROM_BASE, rom_region);

    /* Load the ROM into memory */
    rom_size = load_image_size(machine->firmware,
                               memory_region_get_ram_ptr(rom_region),
                               memory_region_size(rom_region));
    if (rom_size < 0) {
        error_report("Failed to load firmware '%s'", machine->firmware);
        exit(1);
    }

    printf("Loaded firmware: %s (%zd KB)\n", machine->firmware, rom_size / 1024);

    /* Built-in 64 KB work RAM, mirrored across 0xE00000-0xFFFFFF */
    work_ram = g_new(MemoryRegion, 1);
    memory_region_init_ram(work_ram, NULL, "md.work-ram",
                           MD_WORK_RAM_SIZE, &error_fatal);
    for (i = 0; i < MD_WORK_RAM_MIRRORS; i++) {
        MemoryRegion *mirror = g_new(MemoryRegion, 1);

        memory_region_init_alias(mirror, NULL, "md.work-ram.mirror", work_ram,
                                 0, MD_WORK_RAM_SIZE);
        memory_region_add_subregion(address_space_mem,
                                    MD_WORK_RAM_MIRROR_BASE +
                                    i * MD_WORK_RAM_SIZE, mirror);
    }

    /* VDP */
    vdp_dev = qdev_new(TYPE_MD_VDP);
    vdp_sbd = SYS_BUS_DEVICE(vdp_dev);
    sysbus_realize_and_unref(vdp_sbd, &error_fatal);
    sysbus_mmio_map(vdp_sbd, 0, MD_VDP_BASE);

    sysbus_connect_irq(vdp_sbd, 0, qdev_get_gpio_in(irqc, M68K_IRQC_LEVEL_6));
    sysbus_connect_irq(vdp_sbd, 1, qdev_get_gpio_in(irqc, M68K_IRQC_LEVEL_4));

    if (everdrive) {
        DeviceState  *everdrive_dev;
        SysBusDevice *everdrive_sbd;

        everdrive_dev = qdev_new(TYPE_MD_EVERDRIVE);
        qdev_prop_set_chr(everdrive_dev, "chardev", serial_hd(0));
        everdrive_sbd = SYS_BUS_DEVICE(everdrive_dev);
        sysbus_realize_and_unref(everdrive_sbd, &error_fatal);
        sysbus_mmio_map(everdrive_sbd, 0, MD_EVERDRIVE_BASE);
    }

    /* Z80 area + bus request / reset */
    sys_dev = qdev_new(TYPE_MD_SYS);
    sys_sbd = SYS_BUS_DEVICE(sys_dev);
    sysbus_realize_and_unref(sys_sbd, &error_fatal);
    sysbus_mmio_map(sys_sbd, 0, MD_Z80_BASE);
    sysbus_mmio_map(sys_sbd, 1, MD_CTRL_BASE);

    /* I/O area: version register + controller ports */
    io_dev = qdev_new(TYPE_MD_IO);
    io_sbd = SYS_BUS_DEVICE(io_dev);
    sysbus_realize_and_unref(io_sbd, &error_fatal);
    sysbus_mmio_map(io_sbd, 0, MD_IO_BASE);
}

#define MEGADRIVE_DEFAULT_PSRAM_SIZE (4 * MiB)

static char *megadrive_get_mapper(Object *obj, Error **errp)
{
    MegadriveMachineState *mms = MEGADRIVE_MACHINE(obj);

    return g_strdup(mms->mapper);
}

static void megadrive_set_mapper(Object *obj, const char *value, Error **errp)
{
    MegadriveMachineState *mms = MEGADRIVE_MACHINE(obj);

    g_free(mms->mapper);
    mms->mapper = g_strdup(value);
}

static void megadrive_instance_init(Object *obj)
{
    MegadriveMachineState *mms = MEGADRIVE_MACHINE(obj);

    mms->mapper = g_strdup("everdrive");
}

static void megadrive_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc           = "MEGADRIVE";
    mc->init           = megadrive_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->default_ram_size = MEGADRIVE_DEFAULT_PSRAM_SIZE;
    mc->default_ram_id   = "psram";

    object_class_property_add_str(oc, "mapper",
                                  megadrive_get_mapper, megadrive_set_mapper);
    object_class_property_set_description(oc, "mapper",
        "Cartridge mapper: 'everdrive' (default, writable PSRAM) or "
        "'cart' (plain read-only cartridge ROM)");
}

static const TypeInfo megadrive_machine_typeinfo = {
    .name          = TYPE_MEGADRIVE_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(MegadriveMachineState),
    .instance_init = megadrive_instance_init,
    .class_init    = megadrive_machine_class_init,
};

static void megadrive_machine_register_types(void)
{
    type_register_static(&megadrive_machine_typeinfo);
}

type_init(megadrive_machine_register_types)
