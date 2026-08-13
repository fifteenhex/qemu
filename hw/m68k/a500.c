/*
 * Commodore Amiga 500.
 *
 * 68000 at 7MHz with the OCS/ECS chipset and 512KB chip RAM, plus the
 * classic A501 "trapdoor" expansion: up to 1.5MB of slow (ranger) RAM
 * starting at 0x00C00000.  There is no fast RAM (the 68000 has only a
 * 24-bit address bus), no Ramsey/Fat Gary, and no Zorro III space, so
 * the board is little more than the shared Amiga base plus the trapdoor
 * RAM.
 *
 * Boots the A500/A600/A2000 Kickstart (developed against 3.1 r40.63).
 * The chipset IDs are inherited from the shared base (ECS Agnus/Denise),
 * modelling an A500 fitted with the ECS upgrade and a 3.1 ROM.
 *
 * Memory map (motherboard):
 *   0x00000000  chip RAM (512KB)
 *   0x00bfd000  CIA-B
 *   0x00bfe000  CIA-A
 *   0x00c00000  slow "ranger" RAM (A501 trapdoor expansion)
 *   0x00dff000  custom chips
 *   0x00f80000  Kickstart ROM (512KB)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/char/goldfish_tty.h"
#include "hw/rtc/goldfish_rtc.h"
#include "hw/intc/m68k_irqc.h"
#include "hw/m68k/amiga.h"
#include "target/m68k/cpu.h"
#include "qom/object.h"

#define A500_SLOWRAM_BASE       0x00c00000
#define A500_SLOWRAM_MAX        (0x00d80000 - A500_SLOWRAM_BASE)  /* 1.5MB */
#define A500_ROM_BASE           0x00f80000

/*
 * Optional "fast RAM" expansion, at the classic Amiga 24-bit fast-RAM base
 * (0x00200000).  A stock A500 has none -- the 68000's 24-bit bus and the lack
 * of Ramsey/Fat Gary leave only chip + A501 slow RAM -- but a memory board
 * (or accelerator) adds fast RAM here, and a bare-metal 68000 payload (e.g. a
 * device-tree kernel loaded with -kernel) needs somewhere larger than the 2MB
 * chip window to live.  It runs from 0x00200000 up to the CIA page at
 * 0x00bfd000, so 8MB max.  NB: it is mapped as plain RAM, not AutoConfig, so
 * AmigaOS itself will not add it to its free list without an expansion board;
 * that (a Zorro II fast-RAM card) is a separate follow-up.
 */
#define A500_FASTRAM_BASE       0x00200000
#define A500_FASTRAM_MAX        (0x00a00000 - A500_FASTRAM_BASE)  /* 8MB */

/*
 * A goldfish console for MMU-less device-tree kernels, at a spare 24-bit
 * address the motherboard leaves as open bus (between the fast-RAM window
 * and the CIA/custom pages).  It is a QEMU-ism, not real A500 hardware:
 * the device-tree kernel binds its console to it (earlycon and the tty
 * driver), where AmigaOS would use Paula's serial port.  Only wired up for
 * a -kernel device-tree boot, so it never intrudes on a Kickstart boot.
 */
#define A500_GF_TTY_BASE        0x00e10000
#define A500_GF_RTC_BASE        0x00e20000

/*
 * Autovector levels (IPL) the goldfish devices interrupt on.  The kernel's
 * "motorola,mc68000-intc-vect" domain and the m68k-irqc GPIO inputs both
 * index these as (level - 1), which is what goes in the device tree.  The
 * timer sits high (INT6) and the console low (INT2), well away from the
 * Amiga chipset's own INT3/INT4 the kernel never touches here.
 */
#define A500_GF_TTY_IPL         2
#define A500_GF_RTC_IPL         6

#define TYPE_A500_MACHINE MACHINE_TYPE_NAME("a500")
OBJECT_DECLARE_SIMPLE_TYPE(A500MachineState, A500_MACHINE)

struct A500MachineState {
    AmigaMachineState parent_obj;

    uint64_t fastram_size;
    MemoryRegion fastram;
};

static void a500_board_init(AmigaMachineState *ams)
{
    A500MachineState *s = A500_MACHINE(ams);
    MachineState *machine = MACHINE(ams);
    MemoryRegion *sysmem = get_system_memory();

    /*
     * The A501 trapdoor expansion: "slow" RAM on the chip bus at
     * 0x00C00000, sized and added to the free list by Kickstart.
     */
    if (machine->ram_size > A500_SLOWRAM_MAX) {
        error_report("a500: trapdoor slow RAM is limited to 1.5MB");
        exit(1);
    }
    if (machine->ram_size) {
        memory_region_add_subregion(sysmem, A500_SLOWRAM_BASE, machine->ram);
    }

    /* optional fast-RAM expansion at 0x00200000 (see A500_FASTRAM_BASE) */
    if (s->fastram_size) {
        if (s->fastram_size > A500_FASTRAM_MAX) {
            error_report("a500: fast RAM is limited to 8MB (24-bit bus)");
            exit(1);
        }
        memory_region_init_ram(&s->fastram, NULL, "amiga.fastram",
                               s->fastram_size, &error_fatal);
        memory_region_add_subregion(sysmem, A500_FASTRAM_BASE, &s->fastram);
        ams->fastram_base = A500_FASTRAM_BASE;
        ams->fastram_size = s->fastram_size;
    }

    /*
     * The goldfish console for a device-tree kernel boot.  Its interrupt
     * line is left unconnected: earlycon and console output are pure MMIO
     * writes, and RX/an interrupt controller are a follow-up.
     */
    if (ams->kernel_nommu && s->fastram_size) {
        DeviceState *tty = qdev_new(TYPE_GOLDFISH_TTY);
        DeviceState *irqc, *rtc;

        /*
         * The m68k autovector interrupt controller: it drives the 68000's
         * IPL lines with autovectors, which is the root the kernel's
         * "motorola,mc68000-intc-vect" domain unflattens onto.  (Paula also
         * drives IPL, but a device-tree Linux never unmasks its interrupts,
         * so the two do not fight over the CPU here.)
         */
        irqc = qdev_new(TYPE_M68K_IRQC);
        object_property_set_link(OBJECT(irqc), "m68k-cpu",
                                 OBJECT(ams->cpu), &error_abort);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(irqc), &error_fatal);

        qdev_prop_set_chr(tty, "chardev", serial_hd(0));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(tty), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(tty), 0, A500_GF_TTY_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(tty), 0,
                           qdev_get_gpio_in(irqc, A500_GF_TTY_IPL - 1));
        ams->gf_tty_base = A500_GF_TTY_BASE;
        ams->gf_tty_irq = A500_GF_TTY_IPL - 1;

        /* the goldfish RTC block doubles as the kernel's system timer */
        rtc = qdev_new(TYPE_GOLDFISH_RTC);
        qdev_prop_set_bit(rtc, "big-endian", true);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(rtc), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(rtc), 0, A500_GF_RTC_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(rtc), 0,
                           qdev_get_gpio_in(irqc, A500_GF_RTC_IPL - 1));
        ams->gf_rtc_base = A500_GF_RTC_BASE;
        ams->gf_rtc_irq = A500_GF_RTC_IPL - 1;
    }
}

static void a500_get_fastram(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    A500MachineState *s = A500_MACHINE(obj);
    uint64_t value = s->fastram_size;

    visit_type_size(v, name, &value, errp);
}

static void a500_set_fastram(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    A500MachineState *s = A500_MACHINE(obj);
    uint64_t value;

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    s->fastram_size = value;
}

static void a500_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AmigaMachineClass *amc = AMIGA_MACHINE_CLASS(oc);

    mc->desc = "Commodore Amiga 500 (68000)";
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->default_ram_size = 512 * KiB;
    mc->default_ram_id = "amiga.slowram";

    amc->rom_base = A500_ROM_BASE;
    amc->rom_size = 512 * KiB;
    amc->chipram_size = 512 * KiB;
    /* the 68000's whole 24-bit address space terminates in open bus */
    amc->open_bus_size = 0x01000000;
    amc->board_init = a500_board_init;

    object_class_property_add(oc, "fastram", "size",
                              a500_get_fastram, a500_set_fastram, NULL, NULL);
    object_class_property_set_description(oc, "fastram",
        "Fast RAM expansion at 0x200000 (0 = none, up to 8MB)");
}

static const TypeInfo a500_machine_types[] = {
    {
        .name          = TYPE_A500_MACHINE,
        .parent        = TYPE_AMIGA_MACHINE,
        .instance_size = sizeof(A500MachineState),
        .class_init    = a500_machine_class_init,
    },
};

DEFINE_TYPES(a500_machine_types)
