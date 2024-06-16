/*
 * QEMU Motorla MC68EZ328 System Emulator
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

#include "target/m68k/cpu.h"
#include "hw/intc/m68k_irqc.h"
#include "hw/block/flash.h"

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static void mc68ez328_init(MachineState *machine)
{
    M68kCPU *cpu = NULL;
    DeviceState *irqc_dev;

    if(!machine)
    printf("machine is null\n");

    /* CPU init */
    cpu = M68K_CPU(cpu_create(machine->cpu_type));
    qemu_register_reset(main_cpu_reset, cpu);

    /* RAM */
    memory_region_add_subregion(get_system_memory(), 0, machine->ram);

    /* ROM */

    /* IRQ Controller */
    irqc_dev = qdev_new(TYPE_M68K_IRQC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(irqc_dev), &error_fatal);
}

#define MC68EZ328_DEFAULT_SDRAM_SIZE (8 * MiB)

static void mc68ez328_machine_init(MachineClass *mc)
{
    mc->desc = "MC68EZ328";
    mc->init = mc68ez328_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->default_ram_size = MC68EZ328_DEFAULT_SDRAM_SIZE;
    mc->default_ram_id = "sdram";
}

DEFINE_MACHINE("mc68ez328", mc68ez328_machine_init)
