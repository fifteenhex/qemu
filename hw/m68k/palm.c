/*
 * Palm PDA machines on Motorola DragonBall SoCs:
 *
 *   palmv    — Palm V, MC68EZ328 "DragonBall EZ" @ 16.58MHz
 *   palmiiix — Palm IIIx, MC68EZ328 @ 16.58MHz, 4MB RAM
 *   palmvx   — Palm Vx, MC68EZ328 @ 20MHz, 8MB RAM
 *   palmm100 — Palm m100, MC68EZ328 @ 16.58MHz, 2MB RAM
 *   palmm500 — Palm m500, MC68VZ328 "DragonBall VZ" @ 33.16MHz
 *
 * All are close to the reference designs: the SoC provides
 * everything except the flash ROM, the RAM and the touchscreen ADC.
 *
 * Memory map (from the PalmOS ROM boot code, see PALM-NOTES.md):
 *   0x00000000  RAM, DRAM controller chip select
 *   ROM window  0x10c00000 on the Palm V (CSGBA=0x8600 << 13),
 *               0x10000000 on the m500; the PalmOS "big ROM" image
 *               sits at a device-specific offset inside it (the
 *               archive .rom files contain only the big ROM: the
 *               card header at file offset 0 carries bigROMOffset
 *               and the reset vectors), the rest reads as erased
 *               flash (0xff).
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
#include "hw/misc/dragonball_scr.h"
#include "hw/intc/dragonball_intc.h"
#include "hw/gpio/dragonball_gpio.h"
#include "hw/timer/dragonball_timer.h"
#include "hw/ssi/dragonball_spi.h"
#include "hw/char/dragonball_uart.h"
#include "hw/display/dragonball_lcdc.h"
#include "hw/display/sed1376.h"
#include "hw/display/clie_lcd.h"
#include "hw/rtc/dragonball_rtc.h"
#include "hw/input/ads7843.h"
#include "hw/input/palm_keypad.h"
#include "hw/audio/dragonball_pwm.h"
#include "hw/ssi/dragonball_spi1.h"
#include "hw/ssi/ssi.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "hw/misc/sony_mshc_stub.h"
#include "hw/misc/dragonball_sz.h"

#define PALM_MMIO_SCR        0xfffff000
#define PALM_MMIO_PLL        0xfffff200
#define PALM_MMIO_INTC       0xfffff300
#define PALM_MMIO_GPIO       0xfffff400
#define PALM_MMIO_PWM        0xfffff500
#define PALM_MMIO_TIMER1     0xfffff600
#define PALM_MMIO_TIMER2     0xfffff610
#define PALM_MMIO_SPI        0xfffff800
#define PALM_MMIO_UART       0xfffff900
#define PALM_MMIO_UART2      0xfffff910
#define PALM_MMIO_LCDC       0xfffffa00
#define PALM_MMIO_RTC        0xfffffb00

/*
 * GPIO block pin number of bit <bit> in port <port>, for ports 'A'
 * to 'G' (the VZ's extra ports J/K/M don't follow the letters: they
 * are blocks 7/8/9).
 */
#define PALM_GPIO(port, bit) (((port) - 'A') * 8 + (bit))

/* /PENIRQ is also readable as a GPIO: port F bit 1 (low = pen down) */
#define PALM_PENIRQ_GPIO     (5 * 8 + 1)
/* /POWERFAIL from the supply supervisor: port D bit 7, low = battery dead */
#define PALM_POWERFAIL_GPIO  (3 * 8 + 7)
/* m500: SD card detect on port D bit 5, high = no card */
#define PALM_M500_CARDDET_GPIO (3 * 8 + 5)
/* m500: AC power sense on port K bit 2, high = not on the charger */
#define PALM_M500_ACPWR_GPIO   (8 * 8 + 2)
/* m500: SD card chip select on port J bit 3 (active low) */
#define PALM_M500_SDCS_GPIO    (7 * 8 + 3)

#define PALM_MMIO_SPI1       0xfffff700

/*
 * Sony CLIE (EZ-based S-series) GPIO quirks, from Cloudpilot-emu's
 * EmRegsEzPegS300::GetPortInputValue/GetPortInternalValue and
 * EmSonyXzWithSlot<>::GetPortInternalValue (see CLIE-RESEARCH.md sec 3.3
 * and CLIE-POC-NOTES.md for the trace).  None of these lines are wired
 * to a real signal in this stub build; they are simply tied to the
 * level the Sony HAL requires so it does not spin forever waiting for
 * a transition:
 *   - port B bit 1: "LCD powered" sense -- HwrDisplayWake polls this
 *     and hangs if it never reads back set.
 *   - port D bit 4: dock/HotSync button, active low; the base EZ HAL
 *     already forces port D bit 7 (PALM_POWERFAIL_GPIO); Sony's board
 *     additionally wants bit 4 held so HotSync targets the cradle
 *     port instead of the modem.
 *   - port D bit 6: MB86189 (Memory Stick host controller) IRQ-line
 *     sense, active low; our MS stub never raises an interrupt, so
 *     this reads permanently idle (high).
 *   - port D bit 3 (MS card-inserted) is intentionally left low
 *     (its GPIO reset default): "no card in the slot".
 */
#define PALM_CLIE_LCDPWR_GPIO   PALM_GPIO('B', 1)
#define PALM_CLIE_DOCKBTN_GPIO  PALM_GPIO('D', 4)
#define PALM_CLIE_MSIDLE_GPIO   PALM_GPIO('D', 6)

/* EZ SYSCLK = 32768 * ((P + 1) * 14 + Q + 1); PalmOS HAL PLL tables */
#define EZ_SYSCLK 16580608              /* P=0x23 Q=0x01: the spec'd 16.58MHz */
#define EZ_SYSCLK_20MHZ (32768 * 612)   /* P=0x2a Q=0x09: Palm Vx "20MHz" */
#define VZ_SYSCLK (2 * EZ_SYSCLK)

typedef struct PalmMachineClass {
    MachineClass parent_class;

    hwaddr rom_base;
    uint32_t rom_size;
    /* where in the window the ROM file is loaded */
    uint32_t rom_load_offset;
    /* the big ROM (whose card header holds the reset vectors) */
    uint32_t bigrom_offset;
    uint32_t sysclk;
    uint8_t chip_id;
    uint8_t mask_id;
    uint8_t gpio_ports;
    uint16_t adc_dock_value;
    /* gpio lines of the keyboard rows */
    uint16_t kbd_row_gpio[PALM_KEYPAD_ROWS];
    /* SED1376 color LCD controller chip select base, 0 = none */
    hwaddr sed1376_base;
    /*
     * Sony CLIE color-HiRes "MediaQ"-class companion LCD controller
     * (hw/display/clie_lcd.c): video aperture base, 0 = none.  Mutually
     * exclusive with sed1376_base (only the m515 uses SED1376 here);
     * the register window sits at clie_lcd_base + CLIE_LCD_REGS_OFFSET.
     */
    hwaddr clie_lcd_base;
    bool has_timer2;
    /*
     * Sony CLIE quirks: tie the GPIO lines the Sony HAL polls (see
     * PALM_CLIE_*_GPIO above) and instantiate the Memory Stick host
     * controller "no card" stub at this chip-select base (0 = no
     * Sony peripherals on this machine).
     */
    hwaddr ms_stub_base;
    /* which of the PALM_CLIE_*_GPIO lines this board's HAL needs tied */
    bool clie_gpio_lcdpwr;
    bool clie_gpio_dockbtn;
    bool clie_gpio_msidle;
    /*
     * ROM address of the guest HwrSleep routine's entry `linkw`
     * opcode, neutralised to an early RTS at boot (see the comment in
     * palm_init() below); 0 = don't patch.  Address is specific to the
     * exact ROM image each machine loads.
     */
    hwaddr hwrsleep_patch_addr;
    /*
     * MC68SZ328 "Super VZ" SoC (Sony CLIE PEG-SJ33/NR70V): entirely
     * different register window (0xFFFE0000, not 0xfffffxxx) and an
     * enhanced on-chip color LCDC, so this machine takes a separate
     * bring-up path in palm_init() -- none of the EZ/VZ SCR/PLL/INTC/
     * GPIO/timer/UART/LCDC blocks are instantiated.
     */
    bool is_sz328;
} PalmMachineClass;

typedef struct PalmMachineState {
    MachineState parent_obj;
    MemoryRegion rom;
    hwaddr vector_base;
} PalmMachineState;

#define TYPE_PALM_MACHINE MACHINE_TYPE_NAME("palm-common")
OBJECT_DECLARE_TYPE(PalmMachineState, PalmMachineClass, PALM_MACHINE)

typedef struct PalmResetInfo {
    M68kCPU *cpu;
    hwaddr vector_base;
} PalmResetInfo;

static void palm_cpu_reset(void *opaque)
{
    PalmResetInfo *ri = opaque;
    CPUState *cs = CPU(ri->cpu);

    /*
     * On silicon, CSA0 answers the whole address space out of reset so
     * the reset vectors are fetched from the flash; the big-ROM card
     * header carries them.  Load them by hand instead of modelling
     * that aliasing.
     */
    cpu_reset(cs);
    ri->cpu->env.aregs[7] = ldl_phys(cs->as, ri->vector_base);
    ri->cpu->env.pc = ldl_phys(cs->as, ri->vector_base + 4);
}

static void palm_init(MachineState *machine)
{
    PalmMachineState *pms = PALM_MACHINE(machine);
    PalmMachineClass *pmc = PALM_MACHINE_GET_CLASS(machine);
    M68kCPU *cpu;
    PalmResetInfo *ri;
    DeviceState *scr_dev, *pll_dev, *intc_dev, *gpio_dev, *timer_dev,
                *spi_dev, *uart_dev, *lcdc_dev, *rtc_dev, *adc_dev,
                *pen_split, *kpd_dev, *pwm_dev;
    MemoryRegion *sysmem = get_system_memory();
    ssize_t size;
    int i;

    if (!machine->firmware) {
        error_report("%s: a PalmOS ROM must be given with -bios",
                     MACHINE_GET_CLASS(machine)->name);
        exit(1);
    }

    cpu = M68K_CPU(cpu_create(machine->cpu_type));
    /* the DragonBall's FLX68000 core drives 32 address lines */
    cpu->env.features &= ~BIT_ULL(M68K_FEATURE_ADDR24);
    ri = g_new0(PalmResetInfo, 1);
    ri->cpu = cpu;
    ri->vector_base = pmc->rom_base + pmc->bigrom_offset;
    qemu_register_reset(palm_cpu_reset, ri);

    /* RAM */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    /*
     * ROM.  The window reads as erased flash (0xff) where the image
     * does not cover it — PalmOS looks for saved parameters in the
     * small-ROM area and must find "erased", not zeroes.
     */
    memory_region_init_rom(&pms->rom, NULL, "palm.rom", pmc->rom_size,
                           &error_fatal);
    memset(memory_region_get_ram_ptr(&pms->rom), 0xff, pmc->rom_size);
    memory_region_add_subregion(sysmem, pmc->rom_base, &pms->rom);

    /*
     * Load straight into the region instead of going through the ROM
     * loader: the loader copies the image in from a reset hook, which
     * would race with palm_cpu_reset() reading the vectors.
     */
    size = load_image_size(machine->firmware,
                           (uint8_t *)memory_region_get_ram_ptr(&pms->rom) +
                           pmc->rom_load_offset,
                           pmc->rom_size - pmc->rom_load_offset);
    if (size < 0) {
        error_report("palm: could not load ROM '%s'", machine->firmware);
        exit(1);
    }

    /*
     * CLIE: keep the digitizer alive through the Setup wizard.
     *
     * A second or two after boot the Sony HAL calls HwrSleep to power the
     * unit down (an auto-off / battery-gauge decision the Sony firmware
     * makes that the plain Palm-V HAL does not).  HwrSleep runs PenSleep,
     * which masks the pen interrupt (INTC IRQ5) and reconfigures the
     * /PENIRQ pin (port F bit 1) to an output — the digitizer is dead
     * after that, the LCD keeps showing the last "Setup 1 of 4" frame, and
     * no screen tap can wake the device (HwrSleep's wake mask is buttons
     * only), so Setup never advances.  This is the exact "stuck at the
     * Welcome screen" symptom the S300 PoC sessions chased (see
     * CLIE-POC-NOTES.md Session 4).
     *
     * With HwrSleep neutralised (patched to an early RTS) the unit never
     * powers the digitizer down; Setup, the digitizer calibration and the
     * Basic-Skills tutorial can all be tapped through to the launcher.
     * HwrSleep's entry is a `linkw %fp,#-N` (opcode 0x4e56); guard on that
     * opcode so a mismatched address (wrong ROM revision) is left
     * untouched instead of corrupting something else.
     * pmc->hwrsleep_patch_addr is 0 for the Palm V/Vx/m500/m515 machines,
     * which never take this path and stay bit-exact.
     */
    if (pmc->hwrsleep_patch_addr) {
        uint8_t *rom = memory_region_get_ram_ptr(&pms->rom);
        uint32_t off = pmc->hwrsleep_patch_addr - pmc->rom_base;

        if (off + 1 < pmc->rom_size &&
            rom[off] == 0x4e && rom[off + 1] == 0x56) {   /* linkw */
            rom[off] = 0x4e;
            rom[off + 1] = 0x75;                          /* rts */
        }
    }

    if (pmc->is_sz328) {
        /*
         * MC68SZ328 "Super VZ": the whole on-chip register file moves to
         * 0xFFFE0000 with a completely different layout (Cloudpilot's
         * EmRegsSZ), so none of the EZ/VZ 0xfffffxxx peripheral models
         * apply.  Instantiate the single SZ SoC + enhanced-LCDC device
         * and stop -- the SZ INTC/timer/UART/GPIO dynamic behaviour is
         * future work (see CLIE-POC-NOTES.md).  ROM/RAM/CPU/reset above
         * are SoC-agnostic and shared.
         */
        DeviceState *sz_dev = qdev_new(TYPE_DRAGONBALL_SZ);

        sysbus_realize_and_unref(SYS_BUS_DEVICE(sz_dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(sz_dev), 0, DRAGONBALL_SZ_BASE);
        return;
    }

    /* System control / chip ID */
    scr_dev = qdev_new(TYPE_DRAGONBALL_SCR);
    qdev_prop_set_uint8(scr_dev, "chip-id", pmc->chip_id);
    qdev_prop_set_uint8(scr_dev, "mask-id", pmc->mask_id);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(scr_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(scr_dev), 0, PALM_MMIO_SCR);

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
    qdev_prop_set_uint8(gpio_dev, "num-ports", pmc->gpio_ports);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio_dev), 0, PALM_MMIO_GPIO);
    /* the battery is always healthy here */
    qemu_irq_raise(qdev_get_gpio_in(gpio_dev, PALM_POWERFAIL_GPIO));

    if (pmc->ms_stub_base) {
        /*
         * Sony CLIE GPIO quirks -- see PALM_CLIE_*_GPIO above.  Which
         * lines a given board actually needs tied differs by Sony HAL
         * class (traced per-machine; see each machine_class_init()'s
         * comment), hence the three individual flags rather than tying
         * all three unconditionally.
         */
        if (pmc->clie_gpio_lcdpwr) {
            qemu_irq_raise(qdev_get_gpio_in(gpio_dev, PALM_CLIE_LCDPWR_GPIO));
        }
        if (pmc->clie_gpio_dockbtn) {
            qemu_irq_raise(qdev_get_gpio_in(gpio_dev, PALM_CLIE_DOCKBTN_GPIO));
        }
        if (pmc->clie_gpio_msidle) {
            qemu_irq_raise(qdev_get_gpio_in(gpio_dev, PALM_CLIE_MSIDLE_GPIO));
        }

        /* Memory Stick host controller: "no card" stub (Phase 1) */
        DeviceState *ms_dev = qdev_new(TYPE_SONY_MSHC_STUB);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(ms_dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(ms_dev), 0, pmc->ms_stub_base);
    }

    /* PWM 1: the speaker */
    pwm_dev = qdev_new(TYPE_DRAGONBALL_PWM);
    qdev_prop_set_uint32(pwm_dev, "sysclk", pmc->sysclk);
    if (machine->audiodev) {
        qdev_prop_set_string(pwm_dev, "audiodev", machine->audiodev);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(pwm_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(pwm_dev), 0, PALM_MMIO_PWM);

    /* Timer(s) */
    timer_dev = qdev_new(TYPE_DRAGONBALL_TIMER);
    qdev_prop_set_uint32(timer_dev, "sysclk", pmc->sysclk);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer_dev), 0, PALM_MMIO_TIMER1);
    qdev_connect_gpio_out_named(timer_dev, "sysbus-irq", 0,
                                qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_INTC_TMR));
    if (pmc->has_timer2) {
        timer_dev = qdev_new(TYPE_DRAGONBALL_TIMER);
        qdev_prop_set_uint32(timer_dev, "sysclk", pmc->sysclk);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(timer_dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(timer_dev), 0, PALM_MMIO_TIMER2);
        qdev_connect_gpio_out_named(timer_dev, "sysbus-irq", 0,
                                    qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_INTC_TMR2));
    }

    /* SPI master: the touchscreen ADC lives here on real hardware */
    spi_dev = qdev_new(TYPE_DRAGONBALL_SPI);
    qdev_prop_set_bit(spi_dev, "bitwise", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(spi_dev), 0, PALM_MMIO_SPI);
    qdev_connect_gpio_out_named(spi_dev, "sysbus-irq", 0,
                                qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_INTC_SPI));

    /*
     * Touchscreen ADC.  Pen-down asserts the PENIRQ interrupt source
     * and pulls the /PENIRQ pin (port F bit 1) low — PalmOS polls the
     * pin state through the GPIO block.
     */
    adc_dev = qdev_new(TYPE_ADS7843);
    qdev_prop_set_uint16(adc_dev, "dock-value", pmc->adc_dock_value);
    ssi_realize_and_unref(adc_dev,
                          (SSIBus *)qdev_get_child_bus(spi_dev, "ssi"),
                          &error_fatal);
    pen_split = qdev_new(TYPE_SPLIT_IRQ);
    qdev_prop_set_uint16(pen_split, "num-lines", 2);
    qdev_realize_and_unref(pen_split, NULL, &error_fatal);
    qdev_connect_gpio_out(pen_split, 0,
                          qdev_get_gpio_in_named(intc_dev,
                                                 "peripheral_interrupts",
                                                 DRAGONBALL_INTC_IRQ5));
    qdev_connect_gpio_out(pen_split, 1,
                          qemu_irq_invert(qdev_get_gpio_in(gpio_dev,
                                                           PALM_PENIRQ_GPIO)));
    qdev_connect_gpio_out_named(adc_dev, "penirq", 0,
                                qdev_get_gpio_in(pen_split, 0));

    /*
     * UART 1: the cradle serial port on the EZ devices, the IR port
     * on the m500 (whose cradle hangs off UART 2 instead).  The
     * serial console goes wherever the cradle is.
     */
    uart_dev = qdev_new(TYPE_DRAGONBALL_UART);
    qdev_prop_set_chr(uart_dev, "chardev",
                      pmc->has_timer2 ? serial_hd(1) : serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart_dev), 0, PALM_MMIO_UART);
    qdev_connect_gpio_out_named(uart_dev, "sysbus-irq", 0,
                                qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_INTC_UART));
    if (pmc->has_timer2) {
        uart_dev = qdev_new(TYPE_DRAGONBALL_UART);
        qdev_prop_set_chr(uart_dev, "chardev", serial_hd(0));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(uart_dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(uart_dev), 0, PALM_MMIO_UART2);
        qdev_connect_gpio_out_named(uart_dev, "sysbus-irq", 0,
                                    qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_INTC_UART2));
    }

    if (pmc->sed1376_base) {
        /*
         * Color panel on a SED1376 companion controller; the on-chip
         * LCDC is unused on those devices (its register page still
         * doesn't fault, via ignore_memory_transaction_failures).
         */
        DeviceState *sed_dev = qdev_new(TYPE_SED1376);

        sysbus_realize_and_unref(SYS_BUS_DEVICE(sed_dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(sed_dev), 0, pmc->sed1376_base);
        sysbus_mmio_map(SYS_BUS_DEVICE(sed_dev), 1,
                        pmc->sed1376_base + SED1376_VMEM_OFFSET);
    } else if (pmc->clie_lcd_base) {
        /*
         * Color HiRes panel on a Sony "MediaQ"-class companion
         * controller (hw/display/clie_lcd.c); the on-chip LCDC is
         * unused on those devices, same rationale as SED1376 above.
         */
        DeviceState *clie_lcd_dev = qdev_new(TYPE_CLIE_LCD);

        sysbus_realize_and_unref(SYS_BUS_DEVICE(clie_lcd_dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(clie_lcd_dev), 0, pmc->clie_lcd_base);
        sysbus_mmio_map(SYS_BUS_DEVICE(clie_lcd_dev), 1,
                        pmc->clie_lcd_base + CLIE_LCD_REGS_OFFSET);
    } else {
        /* LCDC: 160x160 panel */
        lcdc_dev = qdev_new(TYPE_DRAGONBALL_LCDC);
        object_property_set_link(OBJECT(lcdc_dev), "framebuffer-memory",
                                 OBJECT(sysmem), &error_fatal);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(lcdc_dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(lcdc_dev), 0, PALM_MMIO_LCDC);
    }

    /* RTC */
    rtc_dev = qdev_new(TYPE_DRAGONBALL_RTC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(rtc_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(rtc_dev), 0, PALM_MMIO_RTC);
    qdev_connect_gpio_out_named(rtc_dev, "sysbus-irq", 0,
                                qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_INTC_WDT));
    qdev_connect_gpio_out_named(rtc_dev, "sysbus-irq", 1,
                                qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_INTC_RTC));

    /* board-specific idle pin levels and the m500 SD slot */
    if (pmc->has_timer2) {
        DeviceState *spi1_dev, *sd_dev;
        DriveInfo *dinfo;

        /* not sitting on the charger */
        qemu_irq_raise(qdev_get_gpio_in(gpio_dev, PALM_M500_ACPWR_GPIO));

        /* SPI unit 1: the FIFO SPI that carries the SD card */
        spi1_dev = qdev_new(TYPE_DRAGONBALL_SPI1);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(spi1_dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(spi1_dev), 0, PALM_MMIO_SPI1);
        sysbus_connect_irq(SYS_BUS_DEVICE(spi1_dev), 0,
                           qdev_get_gpio_in_named(intc_dev,
                                                  "peripheral_interrupts",
                                                  DRAGONBALL_INTC_SPI1));

        sd_dev = qdev_new("ssi-sd");
        ssi_realize_and_unref(sd_dev,
                              (SSIBus *)qdev_get_child_bus(spi1_dev, "ssi"),
                              &error_fatal);
        /* the card chip select is port J bit 3, active low */
        qdev_connect_gpio_out(gpio_dev, PALM_M500_SDCS_GPIO,
                              qdev_get_gpio_in_named(sd_dev, SSI_GPIO_CS, 0));

        dinfo = drive_get(IF_SD, 0, 0);
        if (dinfo) {
            DeviceState *card;

            card = qdev_new(TYPE_SD_CARD_SPI);
            qdev_prop_set_drive_err(card, "drive",
                                    blk_by_legacy_dinfo(dinfo), &error_fatal);
            qdev_realize_and_unref(card,
                                   qdev_get_child_bus(sd_dev, "sd-bus"),
                                   &error_fatal);
            /* card detect (port D bit 5) reads low when a card is in */
        } else {
            qemu_irq_raise(qdev_get_gpio_in(gpio_dev, PALM_M500_CARDDET_GPIO));
        }
    }

    /*
     * Hard buttons: rows scanned from GPIO outputs, columns read on
     * port D bits 0-3 which are also the INT0-3 interrupt pins; the
     * OR of the columns feeds the KB interrupt source.
     */
    kpd_dev = qdev_new(TYPE_PALM_KEYPAD);
    qdev_realize_and_unref(kpd_dev, NULL, &error_fatal);
    for (i = 0; i < PALM_KEYPAD_ROWS; i++) {
        qdev_connect_gpio_out(gpio_dev, pmc->kbd_row_gpio[i],
                              qdev_get_gpio_in_named(kpd_dev, "rows", i));
    }
    for (i = 0; i < PALM_KEYPAD_COLS; i++) {
        qdev_connect_gpio_out_named(kpd_dev, "cols", i,
                                    qdev_get_gpio_in(gpio_dev, 3 * 8 + i));
        qdev_connect_gpio_out_named(gpio_dev, "portd-int", i,
                                    qdev_get_gpio_in_named(intc_dev,
                                                     "peripheral_interrupts",
                                                     DRAGONBALL_INTC_INT0 + i));
    }
    qdev_connect_gpio_out_named(gpio_dev, "kb-int", 0,
                                qdev_get_gpio_in_named(intc_dev,
                                                       "peripheral_interrupts",
                                                       DRAGONBALL_INTC_KB));
    for (i = 0; i < PALM_KEYPAD_SILK; i++) {
        qdev_connect_gpio_out_named(kpd_dev, "silk", i,
                                    qdev_get_gpio_in_named(adc_dev,
                                                           "silk-tap", i));
    }
}

static void palm_machine_common_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = palm_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->default_ram_id = "palm.ram";
    machine_add_audiodev_property(mc);
    /*
     * The whole 0xfffffxxx page is on-chip; nothing bus-errors on the
     * real device even where we have no model yet (chip selects, DRAM
     * controller).
     */
    mc->ignore_memory_transaction_failures = true;
}

static void palm_set_kbd_rows(PalmMachineClass *pmc,
                              uint16_t row0, uint16_t row1, uint16_t row2)
{
    pmc->kbd_row_gpio[0] = row0;
    pmc->kbd_row_gpio[1] = row1;
    pmc->kbd_row_gpio[2] = row2;
}

/*
 * Shared platform values for the EZ-based Palms (PalmOS "Razor"
 * reference design): ROM window at CSA0's 0x10c00000 with the big ROM
 * at +0x8000 (all the ROM card headers carry bigROMOffset
 * 0x10c08000), serial cradle whose ADC dock-sense channel idles low,
 * keyboard rows on port F bits 4-6 ("Sumo"/"Brad" wiring).
 */
static void palm_ez_machine_class_init(ObjectClass *oc)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PalmMachineClass *pmc = PALM_MACHINE_CLASS(oc);

    mc->default_ram_size = 2 * MiB;
    pmc->rom_base = 0x10c00000;
    pmc->rom_size = 4 * MiB;
    pmc->rom_load_offset = 0x8000;
    pmc->bigrom_offset = 0x8000;
    pmc->sysclk = EZ_SYSCLK;
    pmc->chip_id = 0x43;        /* EZ */
    pmc->mask_id = 0x01;
    pmc->gpio_ports = 7;
    pmc->adc_dock_value = 0;    /* serial dock sense idles low */
    palm_set_kbd_rows(pmc, PALM_GPIO('F', 4), PALM_GPIO('F', 5),
                      PALM_GPIO('F', 6));
    pmc->has_timer2 = false;
}

static void palmv_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    palm_ez_machine_class_init(oc);
    mc->desc = "Palm V (MC68EZ328)";
}

static void palmiiix_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    palm_ez_machine_class_init(oc);
    mc->desc = "Palm IIIx (MC68EZ328)";
    mc->default_ram_size = 4 * MiB;
}

static void palmvx_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PalmMachineClass *pmc = PALM_MACHINE_CLASS(oc);

    palm_ez_machine_class_init(oc);
    mc->desc = "Palm Vx (MC68EZ328 at 20MHz)";
    mc->default_ram_size = 8 * MiB;
    pmc->sysclk = EZ_SYSCLK_20MHZ;
    /*
     * The HAL only skips the throttle-to-13.5MHz path for EZs of
     * mask revision 4 up ("0J83C" parts).
     */
    pmc->mask_id = 0x04;
    /* "Cobra 2" moves the keyboard rows up to port F bits 5-7 */
    palm_set_kbd_rows(pmc, PALM_GPIO('F', 5), PALM_GPIO('F', 6),
                      PALM_GPIO('F', 7));
}

static void palmm100_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PalmMachineClass *pmc = PALM_MACHINE_CLASS(oc);

    palm_ez_machine_class_init(oc);
    mc->desc = "Palm m100 (MC68EZ328)";
    /*
     * The m100 ("Calvin") scans its keyboard rows on port B bits
     * 0/3/6 (found by tracing the ROM's GPIO writes; the columns are
     * the usual port D bits 0-3, taken as INT0-3 edges rather than
     * the KB source).
     */
    palm_set_kbd_rows(pmc, PALM_GPIO('B', 0), PALM_GPIO('B', 3),
                      PALM_GPIO('B', 6));
}

static void palmm500_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PalmMachineClass *pmc = PALM_MACHINE_CLASS(oc);

    mc->desc = "Palm m500 (MC68VZ328)";
    mc->default_ram_size = 8 * MiB;
    pmc->rom_base = 0x10000000;
    pmc->rom_size = 4 * MiB;
    pmc->rom_load_offset = 0x10000;
    pmc->bigrom_offset = 0x10000;
    pmc->sysclk = VZ_SYSCLK;
    pmc->chip_id = 0x56;        /* VZ */
    pmc->mask_id = 0x01;
    pmc->gpio_ports = 10;
    pmc->adc_dock_value = 0xfff; /* "twister" dock sense idles high */
    /* rows on port K (block 8) bits 5-7 */
    palm_set_kbd_rows(pmc, 8 * 8 + 5, 8 * 8 + 6, 8 * 8 + 7);
    pmc->has_timer2 = true;
}

static void palmm515_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PalmMachineClass *pmc = PALM_MACHINE_CLASS(oc);

    palmm500_machine_class_init(oc, data);
    mc->desc = "Palm m515 (MC68VZ328)";
    mc->default_ram_size = 16 * MiB;
    /*
     * The m515 image is a whole-flash dump, small ROM at file offset
     * 0, big ROM at +0x10000.  Boot through the big ROM's vectors as
     * on the other machines: the small ROM's boot path polls the
     * (unmodelled) USB device controller at 0x10400000 forever.
     */
    pmc->rom_load_offset = 0;
    /* color panel; the HAL points CSGBB (0xffc0 << 13) at the chip */
    pmc->sed1376_base = 0x1ff80000;
}

/*
 * Sony CLIE PEG-S300 (Palm OS Phase-1 target, see CLIE-RESEARCH.md).
 * MC68EZ328 "Sumo" reference design plus Sony's glue: confirmed
 * kDevicePEGS300 in Cloudpilot-emu's EmDevice.cpp instantiates plain
 * EmRegsEzPegS300 (an EZ-family HAL) with an EmRegsMB86189 Memory
 * Stick host controller at 0x10200000 -- unlike PEG-S320, whose
 * EmDevice.cpp entry is actually EmRegsVzPegNasca (VZ328/"Nasca"),
 * confirmed independently by the "68VZ328 LCD Controller" and
 * "sonynasc" strings in the archive.org PEG-S320 ROM dump.  The
 * CLIE-RESEARCH.md taxonomy table had S320 down as EZ328/EzPegS300;
 * S300 is the model that actually matches, so it is the PoC target
 * (see CLIE-POC-NOTES.md).
 *
 * ROM layout (verified against S300en.rom, md5
 * 16673e1569569171e57c4def5ea99479 for the archive.org zip): whole-flash
 * dump, small-ROM reset vectors at file 0 (PC 0x100002a2, matching
 * CLIE-RESEARCH.md sec 7.2), big-ROM card header at +0x8000 -- same
 * shape as the Palm V/IIIx/Vx/m100 EZ machines, just with rom_base at
 * the m500's 0x10000000 instead of the Palm V's 0x10c00000 (the S300
 * HAL programs CSA0 to the low chip-select window).
 */
static void clies300_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PalmMachineClass *pmc = PALM_MACHINE_CLASS(oc);

    palm_ez_machine_class_init(oc);
    mc->desc = "Sony CLIE PEG-S300 (MC68EZ328)";
    mc->default_ram_size = 8 * MiB;
    pmc->rom_base = 0x10000000;
    pmc->rom_size = 2 * MiB;
    pmc->rom_load_offset = 0;
    pmc->bigrom_offset = 0x8000;
    /*
     * The S300 board wires the hard-button/jog-dial matrix to port F
     * bits 0/5/6 (EmRegsEzPegS300.cpp hwrEZPegPortFKbdRow0/1/2), not
     * the plain-EZ F4/F5/F6 wiring -- row 1 (PageUp/PageDown) is the
     * jog-dial rotation, read through the same matrix the tree
     * already emulates for every other Palm's hard buttons, so no new
     * device is needed for jog rotate (see CLIE-POC-NOTES.md sec on
     * Jog Dial).  Jog press / Back button pin numbers are not in this
     * source file (likely a separate GPIO edge, per
     * CLIE-RESEARCH.md sec 4.4) and are deferred to Phase 4.
     */
    palm_set_kbd_rows(pmc, PALM_GPIO('F', 0), PALM_GPIO('F', 5),
                      PALM_GPIO('F', 6));
    /* Memory Stick host controller (MB86189) chip select */
    pmc->ms_stub_base = 0x10200000;
    /* HwrSleep entry (a `linkw %fp,#-60`) in this ROM -- see palm_init(). */
    pmc->hwrsleep_patch_addr = 0x10076bda;
    /* GPIO ties from the shared EmSonyXzWithSlot<> base HAL + the EZ-
     * family "LCD powered" quirk EmRegsEzPegS300 adds on top (see
     * PALM_CLIE_*_GPIO above). */
    pmc->clie_gpio_lcdpwr = true;
    pmc->clie_gpio_dockbtn = true;
    pmc->clie_gpio_msidle = true;
}

/*
 * Sony CLIE PEG-T600C -- a VZ328 color-HiRes CLIE (see CLIE-RESEARCH.md
 * sec 2.1/4.2 and CLIE-POC-NOTES.md).  MC68VZ328 "DragonBall VZ" @
 * 33MHz, 320x320 color via a companion "MediaQ" MQ11xx-class LCD
 * controller (hw/display/clie_lcd.c).
 *
 * NB: this machine was first added (Session 5) as "clie-sj33" on the
 * mistaken belief -- from CLIE-RESEARCH.md's `≈`-flagged, unconfirmed
 * SJ33 taxonomy row -- that the PEG-SJ33 is a VZ328.  It is not: the
 * real PEG-SJ33 is a Motorola DragonBall *Super VZ* (MC68SZ328) @
 * 66MHz whose color comes from an *on-chip* enhanced LCD controller,
 * not an external MediaQ (see clie-sj33 below, which models that SoC).
 * The VZ328 + MediaQ + T600C-ROM machine built here is a faithful
 * PEG-T600C, so it keeps that name; only the SJ33 label was wrong.
 *
 * ROM: PEG-T600C English (archive.org item jp.sony.clie, zip md5
 * 2a78d3b44070d5acd067041c7c3d66d4).  The ROM self-identifies as board
 * "Modena" (string "sonymdna"); Cloudpilot's EmDevice.cpp
 * `case kDevicePEGT600:` instantiates `EmRegsVzPegModena` + an
 * `EmRegsMediaQ11xx` companion controller + `EmRegsFMSound`.
 *
 * ROM layout (verified by header inspection,
 * Sony-CLIE-PEG-T600C-en.rom): whole-flash dump, small ROM at file 0
 * (SP 0x316, PC 0x100002a2), big-ROM card header at +0x8000 (SP
 * 0x488a, PC 0x1000c796) -- the same whole-flash shape as clie-s300/
 * T400 (rom_load_offset=0, bigrom_offset=0x8000), not the m500/m515
 * small-ROM-at-file-offset-0x10000 shape palmm500_machine_class_init()
 * otherwise sets up.
 */
static void cliet600c_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PalmMachineClass *pmc = PALM_MACHINE_CLASS(oc);

    palmm500_machine_class_init(oc, data);
    mc->desc = "Sony CLIE PEG-T600C (MC68VZ328, 320x320 MediaQ color)";
    mc->default_ram_size = 16 * MiB;
    pmc->rom_base = 0x10000000;
    pmc->rom_size = 4 * MiB;
    pmc->rom_load_offset = 0;
    pmc->bigrom_offset = 0x8000;
    /*
     * Color HiRes MediaQ controller: video aperture at T_BASE,
     * registers at T_BASE + CLIE_LCD_REGS_OFFSET -- fixed constants
     * for every MediaQ-equipped Sony VZ328 CLIE per Cloudpilot's
     * EmRegsMediaQ11xx.h (T_BASE/MMIO_BASE); overrides the m500/m515
     * SD-card/SED1376 defaults inherited above.
     */
    pmc->sed1376_base = 0;
    pmc->clie_lcd_base = 0x1f000000;
    /*
     * Memory Stick host controller (MB86189): base per Cloudpilot's
     * EmDevice.cpp kDevicePEGT600 case.
     */
    pmc->ms_stub_base = 0x10800000;
    /*
     * GPIO ties: EmRegsVzPegModena::GetPortInternalValue only forces
     * dock-button + PowerFail (the latter is already the generic
     * PALM_POWERFAIL_GPIO tie every Palm machine gets); it does *not*
     * force the EZ-family "LCD powered" bit (that force is present but
     * explicitly commented out in the Modena source -- a VZ-family
     * quirk, not applicable here) or explicitly re-derive the MS-idle
     * bit itself (inherited from the shared EmSonyXzWithSlot<> base
     * class the S300 quirk above was also traced from, so still tied).
     */
    pmc->clie_gpio_lcdpwr = false;
    pmc->clie_gpio_dockbtn = true;
    pmc->clie_gpio_msidle = true;
    /*
     * HwrSleep patch: left at 0.  Unlike the S300, the T600C
     * demonstrably keeps *drawing* into MediaQ video memory well past
     * boot (traced this session: real 8bpp framebuffer content, not a
     * frozen last frame), so it is not auto-sleeping the way the S300
     * did -- no HwrSleep neutralise is needed to keep the UI live.
     */
    pmc->hwrsleep_patch_addr = 0;
}

/*
 * Sony CLIE PEG-SJ33 -- the real device.  MC68SZ328 "DragonBall Super
 * VZ" @ 66MHz, 16MB RAM, 320x320 16-bit color from the SoC's *on-chip*
 * enhanced TFT LCD controller (there is no external MediaQ here -- that
 * was the T600C).  This is the "Tier C" SoC CLIE-RESEARCH.md sec 4.1
 * scoped as not-yet-modelled; see hw/misc/dragonball_sz.c.
 *
 * ROM: no PEG-SJ33 firmware dump is obtainable (see clie-t600c above
 * and CLIE-POC-NOTES.md), so this uses the PEG-NR70V ROM -- the *same*
 * MC68SZ328 SoC (halID `sonyrdwd`, confirmed in CLIE-RESEARCH.md sec
 * 7.2), fetched from the PalmDB archive.org mirror.  The NR70V is a
 * 320x480 flip model vs the SJ33's 320x320, but it exercises the exact
 * same SZ328 SoC bring-up + enhanced on-chip LCDC, which is the point.
 *
 * ROM layout (CLIE-RESEARCH.md sec 7.2, re-verified this session): 8MB
 * whole-flash dump, small ROM reset vectors at file 0 (PC 0x100002a2),
 * big-ROM card header at +0x10000 (bigROMOffset 0x10010000) -- so
 * rom_load_offset=0, bigrom_offset=0x10000, base 0x10000000.
 */
static void cliesj33_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PalmMachineClass *pmc = PALM_MACHINE_CLASS(oc);

    mc->desc = "Sony CLIE PEG-SJ33 (MC68SZ328 Super VZ, on-chip color LCDC)";
    mc->default_ram_size = 16 * MiB;

    pmc->rom_base = 0x10000000;
    pmc->rom_size = 8 * MiB;
    pmc->rom_load_offset = 0;
    pmc->bigrom_offset = 0x10000;
    /* 66MHz CPU clock (informational; the SZ timer model is future work) */
    pmc->sysclk = 2 * VZ_SYSCLK;
    pmc->chip_id = 0x56;
    pmc->mask_id = 0x01;
    pmc->gpio_ports = 10;
    pmc->is_sz328 = true;
}

static const TypeInfo palm_machine_types[] = {
    {
        .name          = TYPE_PALM_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(PalmMachineState),
        .class_size    = sizeof(PalmMachineClass),
        .class_init    = palm_machine_common_class_init,
        .abstract      = true,
    },
    {
        .name          = MACHINE_TYPE_NAME("palmv"),
        .parent        = TYPE_PALM_MACHINE,
        .class_init    = palmv_machine_class_init,
    },
    {
        .name          = MACHINE_TYPE_NAME("palmiiix"),
        .parent        = TYPE_PALM_MACHINE,
        .class_init    = palmiiix_machine_class_init,
    },
    {
        .name          = MACHINE_TYPE_NAME("palmvx"),
        .parent        = TYPE_PALM_MACHINE,
        .class_init    = palmvx_machine_class_init,
    },
    {
        .name          = MACHINE_TYPE_NAME("palmm100"),
        .parent        = TYPE_PALM_MACHINE,
        .class_init    = palmm100_machine_class_init,
    },
    {
        .name          = MACHINE_TYPE_NAME("palmm500"),
        .parent        = TYPE_PALM_MACHINE,
        .class_init    = palmm500_machine_class_init,
    },
    {
        .name          = MACHINE_TYPE_NAME("palmm515"),
        .parent        = TYPE_PALM_MACHINE,
        .class_init    = palmm515_machine_class_init,
    },
    {
        .name          = MACHINE_TYPE_NAME("clie-s300"),
        .parent        = TYPE_PALM_MACHINE,
        .class_init    = clies300_machine_class_init,
    },
    {
        .name          = MACHINE_TYPE_NAME("clie-t600c"),
        .parent        = TYPE_PALM_MACHINE,
        .class_init    = cliet600c_machine_class_init,
    },
    {
        .name          = MACHINE_TYPE_NAME("clie-sj33"),
        .parent        = TYPE_PALM_MACHINE,
        .class_init    = cliesj33_machine_class_init,
    },
};

DEFINE_TYPES(palm_machine_types)
