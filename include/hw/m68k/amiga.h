/*
 * Amiga machine family.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_M68K_AMIGA_H
#define HW_M68K_AMIGA_H

#include "hw/core/boards.h"
#include "target/m68k/cpu-qom.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_AMIGA_MACHINE MACHINE_TYPE_NAME("amiga-common")
OBJECT_DECLARE_TYPE(AmigaMachineState, AmigaMachineClass, AMIGA_MACHINE)

struct AmigaMachineState {
    MachineState parent_obj;

    M68kCPU *cpu;
    MemoryRegion chipram;
    MemoryRegion rom;
    MemoryRegion rom_overlay;
    MemoryRegion open_bus;
    DeviceState *ciaa, *ciab;
    DeviceState *custom;
    DeviceState *fdc;
};

struct AmigaMachineClass {
    MachineClass parent_class;

    hwaddr rom_base;
    uint32_t rom_size;
    uint32_t chipram_size;
    uint32_t cia_clock_hz;
    uint32_t agnus_id;
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

#endif
