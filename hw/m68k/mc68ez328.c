/*
 * QEMU Motorola MC68EZ328 System Emulator
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

/* MMIO hardware headers */
#include "hw/misc/dragonball_pll.h"
#include "hw/intc/dragonball_intc.h"
#include "hw/gpio/dragonball_gpio.h"
#include "hw/timer/dragonball_timer.h"
#include "hw/ssi/dragonball_spi.h"
#include "hw/char/dragonball_uart.h"
#include "hw/display/dragonball_lcdc.h"
#include "hw/rtc/dragonball_rtc.h"

#define MC68EZ328_FLASHBASE  0x10000000
#define MC68EZ328_FLASHSZ    0x800000
#define MC68EZ328_MMIO_SCR   0xfffff000
#define MC68EZ328_MMIO_CS    0xfffff100
#define MC68EZ328_MMIO_PLL   0xfffff200
#define MC68EZ328_MMIO_INTC  0xfffff300
#define MC68EZ328_MMIO_GPIO  0xfffff400
#define MC68EZ328_MMIO_TIMER 0xfffff600
#define MC68EZ328_MMIO_SPI   0xfffff800
#define MC68EZ328_MMIO_UART  0xfffff900
#define MC68EZ328_MMIO_LCDC  0xfffffa00
#define MC68EZ328_MMIO_RTC   0xfffffb00
#define MC68EZ328_MMIO_DRAMC 0xfffffc00

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    printf("reset!\n");

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, MC68EZ328_FLASHBASE + 0);
    //cpu->env.pc = ldl_phys(cs->as, MC68EZ328_FLASHBASE + 4);
    cpu->env.pc = MC68EZ328_FLASHBASE + 0x400;


    printf("0x%08x\n", (unsigned) cpu->env.pc);
}

static MemoryRegion flash = { 0 };

static void mc68ez328_init(MachineState *machine)
{
    M68kCPU *cpu = NULL;
    DeviceState *pll_dev, *intc_dev, *gpio_dev, *timer_dev,
                *spi_dev, *uart_dev, *lcdc_dev, *rtc_dev;
    DeviceState *ds1305_dev, *sd_dev;
    SSIBus *ssi_bus;
    MemoryRegion *address_space_mem = get_system_memory();

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
    memory_region_init_rom(&flash, NULL, "mc68ez328.flash",
                           MC68EZ328_FLASHSZ, &error_fatal);
    memory_region_add_subregion(get_system_memory(), MC68EZ328_FLASHBASE,
                                &flash);
    load_image_targphys(machine->firmware, MC68EZ328_FLASHBASE, MC68EZ328_FLASHSZ);

    /* PLL */
    pll_dev = qdev_new(TYPE_DRAGONBALL_PLL);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(pll_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(pll_dev), 0, MC68EZ328_MMIO_PLL);

    /* DragonBall INTC */
    intc_dev = qdev_new(TYPE_DRAGONBALL_INTC);
    object_property_set_link(OBJECT(intc_dev), "m68k-cpu",
                             OBJECT(cpu), &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(intc_dev), 0, MC68EZ328_MMIO_INTC);

    /* GPIO */
    gpio_dev = qdev_new(TYPE_DRAGONBALL_GPIO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio_dev), 0, MC68EZ328_MMIO_GPIO);

    /* Timer */
    timer_dev = qdev_new(TYPE_DRAGONBALL_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer_dev), 0, MC68EZ328_MMIO_TIMER);
    qdev_connect_gpio_out_named(timer_dev, "sysbus-irq", 0,
                          qdev_get_gpio_in_named(intc_dev, "peripheral_interrupts", 1));

    /* SPI */
    spi_dev = qdev_new(TYPE_DRAGONBALL_SPI);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(spi_dev), 0, MC68EZ328_MMIO_SPI);
    qdev_connect_gpio_out_named(spi_dev, "sysbus-irq", 0,
                          qdev_get_gpio_in_named(intc_dev, "peripheral_interrupts", 0));

    ssi_bus = (SSIBus *) qdev_get_child_bus(spi_dev, "ssi");

    /* DS1305 connected to SPI */
    ds1305_dev = ssi_create_peripheral(ssi_bus, "ds1305");
    qdev_connect_gpio_out(gpio_dev, 9,
                          qdev_get_gpio_in_named(ds1305_dev, SSI_GPIO_CS, 0));

    /* SD card connected to SPI */
    sd_dev = qdev_new("ssi-sd");
    qdev_prop_set_uint8(sd_dev, "cs", 1);
    ssi_realize_and_unref(sd_dev, ssi_bus, &error_fatal);
    qdev_connect_gpio_out(gpio_dev, 24,
                          qdev_get_gpio_in_named(sd_dev, SSI_GPIO_CS, 0));

    /* UART */
    uart_dev = qdev_new(TYPE_DRAGONBALL_UART);
    qdev_prop_set_chr(uart_dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart_dev), 0, MC68EZ328_MMIO_UART);
    qdev_connect_gpio_out_named(uart_dev, "sysbus-irq", 0,
                          qdev_get_gpio_in_named(intc_dev, "peripheral_interrupts", 2));

    /* LCDC */
    lcdc_dev = qdev_new(TYPE_DRAGONBALL_LCDC);
    object_property_set_link(OBJECT(lcdc_dev), "framebuffer-memory",
                             OBJECT(address_space_mem), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(lcdc_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(lcdc_dev), 0, MC68EZ328_MMIO_LCDC);

    /* RTC */
    rtc_dev = qdev_new(TYPE_DRAGONBALL_RTC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(rtc_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(rtc_dev), 0, MC68EZ328_MMIO_RTC);
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
