/*
 * Palm V PDA (Motorola MC68EZ328 "DragonBall EZ")
 *
 * A Palm V is essentially the DragonBall EZ reference design: the SoC
 * provides everything except the flash ROM, the pseudo-static RAM and
 * the touchscreen ADC.
 *
 * Memory map (from the PalmOS ROM boot code, see PALM-NOTES.md):
 *   0x00000000  RAM (2MB on a Palm V), DRAM controller chip select
 *   0x10c00000  flash ROM window, CSA (CSGBA is set to 0x8600 by the
 *               boot code, i.e. base = 0x8600 << 13 = 0x10c00000)
 *   0x10c08000  "big ROM" — the PalmOS image proper.  ROM files from
 *               the usual archives contain only the big ROM (the card
 *               header at file offset 0 carries bigROMOffset =
 *               0x10c08000 and the reset vectors), so the file is
 *               loaded at +0x8000 and the small-ROM area reads as
 *               erased flash (0xff).
 *   0xfffff000  on-chip peripherals
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
#include "qemu/error-report.h"
#include "qemu/units.h"

#include "target/m68k/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/split-irq.h"

#include "hw/misc/dragonball_pll.h"
#include "hw/intc/dragonball_intc.h"
#include "hw/gpio/dragonball_gpio.h"
#include "hw/timer/dragonball_timer.h"
#include "hw/ssi/dragonball_spi.h"
#include "hw/char/dragonball_uart.h"
#include "hw/display/dragonball_lcdc.h"
#include "hw/rtc/dragonball_rtc.h"
#include "hw/input/ads7843.h"

#define PALM_ROM_BASE        0x10c00000
#define PALM_ROM_SIZE        (4 * MiB)
#define PALM_BIGROM_OFFSET   0x8000

#define PALM_MMIO_PLL        0xfffff200
#define PALM_MMIO_INTC       0xfffff300
#define PALM_MMIO_GPIO       0xfffff400
#define PALM_MMIO_TIMER      0xfffff600
#define PALM_MMIO_SPI        0xfffff800
#define PALM_MMIO_UART       0xfffff900
#define PALM_MMIO_LCDC       0xfffffa00
#define PALM_MMIO_RTC        0xfffffb00

/* IPR/IMR bit numbers of the on-chip interrupt sources */
#define DRAGONBALL_IRQ_SPI   0
#define DRAGONBALL_IRQ_TMR   1
#define DRAGONBALL_IRQ_UART  2
#define DRAGONBALL_IRQ_WDT   3
#define DRAGONBALL_IRQ_RTC   4
#define DRAGONBALL_IRQ_PEN   20

/* /PENIRQ is also readable as a GPIO: port F bit 1 (low = pen down) */
#define PALM_PENIRQ_GPIO     (5 * 8 + 1)
/* /POWERFAIL from the supply supervisor: port G bit 2, low = battery dead */
#define PALM_POWERFAIL_GPIO  (6 * 8 + 2)

typedef struct PalmMachineState {
    MachineState parent_obj;
    MemoryRegion rom;
} PalmMachineState;

#define TYPE_PALM_MACHINE MACHINE_TYPE_NAME("palmv")
OBJECT_DECLARE_SIMPLE_TYPE(PalmMachineState, PALM_MACHINE)

static void palm_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    /*
     * On silicon, CSA0 answers the whole address space out of reset so
     * the reset vectors are fetched from the flash; the big-ROM card
     * header carries them.  Load them by hand instead of modelling
     * that aliasing.
     */
    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, PALM_ROM_BASE + PALM_BIGROM_OFFSET);
    cpu->env.pc = ldl_phys(cs->as, PALM_ROM_BASE + PALM_BIGROM_OFFSET + 4);
}

static void palm_init(MachineState *machine)
{
    PalmMachineState *pms = PALM_MACHINE(machine);
    M68kCPU *cpu;
    DeviceState *pll_dev, *intc_dev, *gpio_dev, *timer_dev,
                *spi_dev, *uart_dev, *lcdc_dev, *rtc_dev, *adc_dev,
                *pen_split;
    MemoryRegion *sysmem = get_system_memory();
    ssize_t size;

    if (!machine->firmware) {
        error_report("palmv: a PalmOS ROM must be given with -bios");
        exit(1);
    }

    cpu = M68K_CPU(cpu_create(machine->cpu_type));
    qemu_register_reset(palm_cpu_reset, cpu);

    /* RAM */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    /*
     * ROM.  The window reads as erased flash (0xff) where the image
     * does not cover it — PalmOS looks for saved parameters in the
     * small-ROM area and must find "erased", not zeroes.
     */
    memory_region_init_rom(&pms->rom, NULL, "palm.rom", PALM_ROM_SIZE,
                           &error_fatal);
    memset(memory_region_get_ram_ptr(&pms->rom), 0xff, PALM_ROM_SIZE);
    memory_region_add_subregion(sysmem, PALM_ROM_BASE, &pms->rom);

    /*
     * Load straight into the region instead of going through the ROM
     * loader: the loader copies the image in from a reset hook, which
     * would race with palm_cpu_reset() reading the vectors.
     */
    size = load_image_size(machine->firmware,
                           (uint8_t *)memory_region_get_ram_ptr(&pms->rom) +
                           PALM_BIGROM_OFFSET,
                           PALM_ROM_SIZE - PALM_BIGROM_OFFSET);
    if (size < 0) {
        error_report("palmv: could not load ROM '%s'", machine->firmware);
        exit(1);
    }

    /* PLL */
    pll_dev = qdev_new(TYPE_DRAGONBALL_PLL);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(pll_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(pll_dev), 0, PALM_MMIO_PLL);

    /* INTC */
    intc_dev = qdev_new(TYPE_DRAGONBALL_INTC);
    object_property_set_link(OBJECT(intc_dev), "m68k-cpu",
                             OBJECT(cpu), &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(intc_dev), 0, PALM_MMIO_INTC);

    /* GPIO */
    gpio_dev = qdev_new(TYPE_DRAGONBALL_GPIO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio_dev), 0, PALM_MMIO_GPIO);
    /* the battery is always healthy here */
    qemu_irq_raise(qdev_get_gpio_in(gpio_dev, PALM_POWERFAIL_GPIO));

    /* Timer */
    timer_dev = qdev_new(TYPE_DRAGONBALL_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer_dev), 0, PALM_MMIO_TIMER);
    qdev_connect_gpio_out_named(timer_dev, "sysbus-irq", 0,
                                qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_IRQ_TMR));

    /* SPI master: the touchscreen ADC lives here on real hardware */
    spi_dev = qdev_new(TYPE_DRAGONBALL_SPI);
    qdev_prop_set_bit(spi_dev, "bitwise", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(spi_dev), 0, PALM_MMIO_SPI);
    qdev_connect_gpio_out_named(spi_dev, "sysbus-irq", 0,
                                qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_IRQ_SPI));

    /*
     * Touchscreen ADC.  Pen-down asserts the PENIRQ interrupt source
     * and pulls the /PENIRQ pin (port F bit 1) low — PalmOS polls the
     * pin state through the GPIO block.
     */
    adc_dev = ssi_create_peripheral(
        (SSIBus *)qdev_get_child_bus(spi_dev, "ssi"), TYPE_ADS7843);
    pen_split = qdev_new(TYPE_SPLIT_IRQ);
    qdev_prop_set_uint16(pen_split, "num-lines", 2);
    qdev_realize_and_unref(pen_split, NULL, &error_fatal);
    qdev_connect_gpio_out(pen_split, 0,
                          qdev_get_gpio_in_named(intc_dev,
                                                 "peripheral_interrupts",
                                                 DRAGONBALL_IRQ_PEN));
    qdev_connect_gpio_out(pen_split, 1,
                          qemu_irq_invert(qdev_get_gpio_in(gpio_dev,
                                                           PALM_PENIRQ_GPIO)));
    qdev_connect_gpio_out_named(adc_dev, "penirq", 0,
                                qdev_get_gpio_in(pen_split, 0));

    /* UART: the cradle serial port */
    uart_dev = qdev_new(TYPE_DRAGONBALL_UART);
    qdev_prop_set_chr(uart_dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart_dev), 0, PALM_MMIO_UART);
    qdev_connect_gpio_out_named(uart_dev, "sysbus-irq", 0,
                                qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_IRQ_UART));

    /* LCDC: 160x160 panel */
    lcdc_dev = qdev_new(TYPE_DRAGONBALL_LCDC);
    object_property_set_link(OBJECT(lcdc_dev), "framebuffer-memory",
                             OBJECT(sysmem), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(lcdc_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(lcdc_dev), 0, PALM_MMIO_LCDC);

    /* RTC */
    rtc_dev = qdev_new(TYPE_DRAGONBALL_RTC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(rtc_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(rtc_dev), 0, PALM_MMIO_RTC);
    qdev_connect_gpio_out_named(rtc_dev, "sysbus-irq", 0,
                                qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_IRQ_WDT));
    qdev_connect_gpio_out_named(rtc_dev, "sysbus-irq", 1,
                                qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_IRQ_RTC));
}

static void palm_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Palm V (MC68EZ328)";
    mc->init = palm_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->default_ram_size = 2 * MiB;
    mc->default_ram_id = "palm.ram";
    /*
     * The whole 0xfffffxxx page is on-chip; nothing bus-errors on the
     * real device even where we have no model yet (SCR, chip selects,
     * DRAM controller).
     */
    mc->ignore_memory_transaction_failures = true;
}

static const TypeInfo palm_machine_types[] = {
    {
        .name          = TYPE_PALM_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(PalmMachineState),
        .class_init    = palm_machine_class_init,
    },
};

DEFINE_TYPES(palm_machine_types)
