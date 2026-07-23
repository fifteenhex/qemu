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

#endif
