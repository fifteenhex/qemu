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

/*
 * MegaDrive + Everdrive memory map:
 *
 *   0x000000 – 0x3FFFFF   Cartridge ROM
 *   0xFF0000 – 0xFFFFFF   Work RAM (64 KB, mirrored)
 *   0xC00000 – 0xC0001F   VDP ports
 *   0xA10000 – 0xA1001F   I/O ports
 *   0xA11100              Bus request
 *   0xA11200              Reset (Z80)
 *   0xA130D0 – 0xA130DF   Everdrive SSF2 mapper registers
 */
#define MD_VDP_BASE         0xC00000
#define MD_EVERDRIVE_BASE   0xA130D0

/*
 * ROM is always at zero, also we are going to emulate as
 * if the SSF2 mapper was configured to expose 4MB of PSRAM.
 */
#define MD_ROM_BASE         0x000000

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
    M68kCPU       *cpu = NULL;
    DeviceState   *irqc;
    DeviceState   *vdp_dev;
    SysBusDevice  *vdp_sbd;
    DeviceState   *everdrive_dev;
    SysBusDevice  *everdrive_sbd;
    MemoryRegion  *address_space_mem = get_system_memory();
    ssize_t        rom_size;

    if (!machine->firmware) {
        error_report("No firmware specified. Use -bios <rom.bin>");
        exit(1);
    }

    cpu = M68K_CPU(cpu_create(machine->cpu_type));
    qemu_register_reset(main_cpu_reset, cpu);

    irqc = qdev_new(TYPE_M68K_IRQC);
    object_property_set_link(OBJECT(irqc), "m68k-cpu", OBJECT(cpu),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(irqc), &error_fatal);

    memory_region_add_subregion(address_space_mem, 0, machine->ram);

    /* Load the ROM into memory */
    rom_size = load_image_size(machine->firmware,
                               memory_region_get_ram_ptr(machine->ram),
                               machine->ram_size);
    if (rom_size < 0) {
        error_report("Failed to load firmware '%s'", machine->firmware);
        exit(1);
    }

    printf("Loaded firmware: %s (%zd KB)\n", machine->firmware, rom_size / 1024);

    /* VDP */
    vdp_dev = qdev_new(TYPE_MD_VDP);
    vdp_sbd = SYS_BUS_DEVICE(vdp_dev);
    sysbus_realize_and_unref(vdp_sbd, &error_fatal);
    sysbus_mmio_map(vdp_sbd, 0, MD_VDP_BASE);

    sysbus_connect_irq(vdp_sbd, 0, qdev_get_gpio_in(irqc, M68K_IRQC_LEVEL_6));
    sysbus_connect_irq(vdp_sbd, 1, qdev_get_gpio_in(irqc, M68K_IRQC_LEVEL_4));

    everdrive_dev = qdev_new(TYPE_MD_EVERDRIVE);
    qdev_prop_set_chr(everdrive_dev, "chardev", serial_hd(0));
    everdrive_sbd = SYS_BUS_DEVICE(everdrive_dev);
    sysbus_realize_and_unref(everdrive_sbd, &error_fatal);
    sysbus_mmio_map(everdrive_sbd, 0, MD_EVERDRIVE_BASE);
}

#define MEGADRIVE_DEFAULT_PSRAM_SIZE (4 * MiB)

static void megadrive_machine_init(MachineClass *mc)
{
    mc->desc           = "MEGADRIVE";
    mc->init           = megadrive_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->default_ram_size = MEGADRIVE_DEFAULT_PSRAM_SIZE;
    mc->default_ram_id   = "psram";
}

DEFINE_MACHINE("megadrive", megadrive_machine_init)
