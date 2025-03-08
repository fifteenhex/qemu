/*
 * QEMU Motorola MVME147 System Emulator
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
#include "hw/boards.h"
#include "hw/char/escc.h"
#include "hw/loader.h"
#include "hw/misc/mvme147_pcc.h"
#include "hw/misc/mvme147_vmechip.h"
#include "hw/net/lance.h"
#include "hw/scsi/wd33c93.h"
#include "hw/rtc/m48t59.h"
#include "elf.h"
#include "exec/memory.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/qdev-properties.h"

#include "target/m68k/cpu.h"
#include "hw/intc/m68k_irqc.h"
#include "hw/block/flash.h"

#define MVME147_ROM_BANK1    0xff800000
#define MVME147_ROM_BANK1_SZ (2 * MiB)
#define MVME147_ROM_BANK2    0xffa00000
#define MVME147_ROM_BANK2_SZ (2 * MiB)
#define MVME147_BBRAM        0xfffe0000
#define MVME147_BBRAM_SZ     2024
#define MVME147_PCC          0xfffe1000
#define MVME147_LANCE        0xfffe1800
#define MVME147_VMECHIP      0xfffe2000
#define MVME147_SCC          0xfffe3000
#define MVME147_SCSI         0xfffe4000

void ledma_memory_read(void *opaque, hwaddr addr,
                       uint8_t *buf, int len, int do_bswap);
void ledma_memory_write(void *opaque, hwaddr addr,
                        uint8_t *buf, int len, int do_bswap);

void ledma_memory_write(void *opaque, hwaddr addr,
                        uint8_t *buf, int len, int do_bswap)
{
	//printf("ledma write - 0x%x (%d bytes) - %d\n", (unsigned) addr, len, do_bswap);

	/* data bytes are swapped */
	if (!do_bswap){
		for (int i = 0; i < len; i +=2) {
			uint8_t tmp = buf[i];
			buf[i] = buf[i + 1];
			buf[i + 1] = tmp;
		}
	}
	dma_memory_write(&address_space_memory, addr, buf, len, MEMTXATTRS_UNSPECIFIED);
}

void ledma_memory_read(void *opaque, hwaddr addr,
                       uint8_t *buf, int len, int do_bswap)
{
	//printf("ledma read - 0x%x (%d bytes) - %d\n", (unsigned) addr, len, do_bswap);

    dma_memory_read(&address_space_memory, addr, buf, len, MEMTXATTRS_UNSPECIFIED);
    /* data bytes are swapped */
    if (!do_bswap) {
		for (int i = 0; i < len; i +=2) {
			uint8_t tmp = buf[i];
			buf[i] = buf[i + 1];
			buf[i + 1] = tmp;
		}
    }
}

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    printf("reset!\n");

    cpu_reset(cs);

    cpu->env.pc = 0xff823952;

    printf("0x%08x\n", (unsigned) cpu->env.pc);
}

static void mvme147_init(MachineState *machine)
{
    M68kCPU *cpu = NULL;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *rombank1 = g_new(MemoryRegion, 1);
    MemoryRegion *rombank2 = g_new(MemoryRegion, 1);
    MemoryRegion *lance_alias = g_new(MemoryRegion, 1);

    DeviceState *bbram_dev;
    DeviceState *pcc_dev;
    DeviceState *lance_dev;
    DeviceState *vmechip_dev;
    DeviceState *serial_dev;
    DeviceState *scsi_dev;

    if(!machine)
        printf("machine is null\n");

    /* CPU init */
    cpu = M68K_CPU(cpu_create(machine->cpu_type));
    qemu_register_reset(main_cpu_reset, cpu);

    /* RAM */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    /* ROM bank 1 */
    memory_region_init_rom(rombank1, NULL, "mvme147.rombank1", MVME147_ROM_BANK1_SZ, &error_fatal);
    memory_region_add_subregion(sysmem, MVME147_ROM_BANK1, rombank1);
    if (machine->firmware)
        load_image_targphys(machine->firmware, MVME147_ROM_BANK1, MVME147_ROM_BANK1_SZ);

    /* ROM bank 2 */
    memory_region_init_rom(rombank2, NULL, "mvme147.rombank2", MVME147_ROM_BANK2_SZ, &error_fatal);
    memory_region_add_subregion(sysmem, MVME147_ROM_BANK2, rombank2);

    /* BBRAM */
    bbram_dev = qdev_new("sysbus-m48t02");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(bbram_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(bbram_dev), 0, MVME147_BBRAM);

    /* PCC */
    pcc_dev = qdev_new(TYPE_MVME147_PCC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(pcc_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(pcc_dev), 0, MVME147_PCC);

    /* LANCE */
    lance_dev = qdev_new(TYPE_LANCE);
    qemu_configure_nic_device(lance_dev, true, NULL);
    //object_property_set_link(OBJECT(lance_dev), "dma", OBJECT(sysmem), &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(lance_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(lance_dev), 0, MVME147_LANCE);
    /* 147bug uses a mirror */
    memory_region_init_alias(lance_alias, NULL, "lance.alias",
    		SYS_BUS_DEVICE(lance_dev)->mmio[0].memory, 0, 0x4);
    memory_region_add_subregion(sysmem, MVME147_LANCE + 0x4, lance_alias);

    /* VMEChip */
    vmechip_dev = qdev_new(TYPE_MVME147_VMECHIP);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(vmechip_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(vmechip_dev), 0, MVME147_VMECHIP);

    /* SCC */
    serial_dev = qdev_new(TYPE_ESCC);
    qdev_prop_set_chr(serial_dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(serial_dev, "chrB", serial_hd(1));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(serial_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(serial_dev), 0, MVME147_SCC);

    /* SCSI */
    scsi_dev = qdev_new(TYPE_WD33C93);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(scsi_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(scsi_dev), 0, MVME147_SCSI);
}

#define MVME147_DEFAULT_SDRAM_SIZE (16 * MiB)

static void mvme147_machine_init(MachineClass *mc)
{
    mc->desc = "mvme147";
    mc->init = mvme147_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->default_ram_size = MVME147_DEFAULT_SDRAM_SIZE;
    mc->default_ram_id = "dram";
}

DEFINE_MACHINE("mvme147", mvme147_machine_init)
