/*
 * Amiga machine family.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_M68K_AMIGA_H
#define HW_M68K_AMIGA_H

#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/m68k/amiga_fdc.h"
#include "hw/m68k/amiga_kbd.h"
#include "target/m68k/cpu-qom.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_AMIGA_MACHINE MACHINE_TYPE_NAME("amiga-common")
OBJECT_DECLARE_TYPE(AmigaMachineState, AmigaMachineClass, AMIGA_MACHINE)

/* board devices that also listen to the CPU's /RSTO line */
#define AMIGA_RSTO_DEVS 4

/* the open-collector lines the floppy drives share */
enum {
    FLOPPY_LINE_CHNG,
    FLOPPY_LINE_WPRO,
    FLOPPY_LINE_TK0,
    FLOPPY_LINE_RDY,
    FLOPPY_LINE_INDEX,
    FLOPPY_LINE_COUNT,
};

struct AmigaMachineState {
    MachineState parent_obj;

    M68kCPU *cpu;
    /*
     * Base of the fast RAM bank backing machine->ram, recorded by the
     * board_init hook; a direct-boot Linux kernel is loaded here.
     */
    hwaddr fastram_base;
    /*
     * Size of that fast RAM bank.  On the big Amigas machine->ram is the
     * fast RAM, so this mirrors machine->ram_size; on the A500 the fast
     * RAM is a separate expansion (machine->ram is chip-bus slow RAM), so
     * the board records its size here for the direct-boot code.
     */
    uint64_t fastram_size;
    /* -kernel boot: entry point the CPU reset routes to instead of ROM */
    bool linux_boot;
    hwaddr kernel_entry;
    /*
     * MMU-less device-tree boot (a 68000 kernel): the CPU is entered with
     * the DT blob address in d7 rather than a bootinfo chain, and the
     * board exposes a goldfish console at gf_tty_base for the kernel's DT
     * drivers to bind to.  gf_tty_base 0 means the board has no such
     * console (and so cannot device-tree boot).
     */
    bool kernel_nommu;
    hwaddr dtb_addr;
    hwaddr gf_tty_base;
    hwaddr gf_rtc_base;
    /*
     * Device-tree interrupt cells for the goldfish console and timer: the
     * value is the m68k autovector index (IPL level - 1), matching both the
     * m68k-irqc GPIO input the device is wired to and the "motorola,
     * mc68000-intc-vect" domain the kernel unflattens.  -1 means the device
     * has no interrupt wired (e.g. the console is earlycon/TX only).
     */
    int gf_tty_irq;
    int gf_rtc_irq;
    /*
     * The Zorro III bridge, on machines with an expansion bus; the
     * kernel is loaded from a machine-done notifier so that boards
     * added with -device are described in the bootinfo.
     */
    DeviceState *zorro;
    Notifier machine_done;
    MemoryRegion chipram;
    MemoryRegion rom;
    MemoryRegion rom_overlay;
    MemoryRegion open_bus;
    DeviceState *ciaa, *ciab;
    DeviceState *custom;
    DeviceState *kbd;
    DeviceState *fdc[AMIGA_FLOPPY_DRIVES];
    /*
     * Board-specific chips on /RSTO: boards deposit devices here from
     * their board_init hook and the RESET instruction cold-resets them
     * along with the shared chips.
     */
    DeviceState *rsto_dev[AMIGA_RSTO_DEVS];

    /*
     * The floppy status lines are open collector and shared by all
     * drives: a wired AND of the per-drive levels feeds the CIA.
     */
    struct AmigaFloppyLine {
        uint8_t released;       /* one bit per drive */
        qemu_irq out;
    } floppy_line[FLOPPY_LINE_COUNT];
};

struct AmigaMachineClass {
    MachineClass parent_class;

    hwaddr rom_base;
    uint32_t rom_size;
    uint32_t chipram_size;
    uint32_t cia_clock_hz;
    uint32_t agnus_id;
    uint32_t denise_id;
    /*
     * Linux bootinfo identity (BI_AMIGA_MODEL / BI_AMIGA_CHIPSET from
     * standard-headers/asm-m68k/bootinfo-amiga.h).  Machines that leave
     * amiga_model at AMI_UNKNOWN do not support direct kernel boot.
     */
    uint32_t amiga_model;
    uint32_t chipset;
    /*
     * Size of the region (from address 0) where the glue logic always
     * terminates bus cycles, so accesses to unpopulated addresses read
     * open bus instead of faulting.
     */
    uint64_t open_bus_size;

    void (*board_init)(AmigaMachineState *ams);
};

/*
 * Filler for bus regions where the glue logic terminates every cycle:
 * reads return open bus (all ones), writes are ignored.  Autoconfig
 * relies on this to see 0xff ("no board") in empty config space.
 */
extern const MemoryRegionOps amiga_open_bus_ops;

/*
 * The Gayle IDE interface at its A600/A1200 addresses, interrupting
 * on INT2, with the IF_IDE drives attached; for the board_init hook
 * of the machines built around Gayle.
 */
void amiga_gayle_init(AmigaMachineState *ams);

/*
 * The A4000 onboard IDE (the same ATA core without the Gayle gate
 * array), at 0xdd2020/0xdd3020, interrupting on INT2, with the IF_IDE
 * drives attached; for the A4000/A4000T board_init hooks.
 */
void amiga_a4000ide_init(AmigaMachineState *ams);

#endif
