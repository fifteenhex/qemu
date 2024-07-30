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
#include "sysemu/reset.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "elf.h"
#include "exec/memory.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/qdev-properties.h"

#include "target/m68k/cpu.h"
#include "hw/intc/m68k_irqc.h"
#include "hw/block/flash.h"
#include "hw/ssi/ssi.h"
#include "hw/sd/sd.h"

#define MVME147_ROM_BANK1 0xff800000
#define MVME147_ROM_BANK2 0xffa00000

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    printf("reset!\n");

    cpu_reset(cs);


    printf("0x%08x\n", (unsigned) cpu->env.pc);
}

//static MemoryRegion flash = { 0 };

static void mvme147_init(MachineState *machine)
{
    M68kCPU *cpu = NULL;

//    DriveInfo *dinfo;
//    BlockBackend *blk;
//    DeviceState *card_dev;

    if(!machine)
        printf("machine is null\n");

    printf("%s\n", machine->firmware);

    /* CPU init */
    cpu = M68K_CPU(cpu_create(machine->cpu_type));
    qemu_register_reset(main_cpu_reset, cpu);

    /* RAM */
    memory_region_add_subregion(get_system_memory(), 0, machine->ram);

    /* ROM */
//    memory_region_init_rom(&flash, NULL, "mc68ez328.flash",
  //                         MC68EZ328_FLASHSZ, &error_fatal);
    //memory_region_add_subregion(get_system_memory(), MC68EZ328_FLASHBASE,
      //                          &flash);
    //load_image_targphys(machine->firmware, MC68EZ328_FLASHBASE, MC68EZ328_FLASHSZ);
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
