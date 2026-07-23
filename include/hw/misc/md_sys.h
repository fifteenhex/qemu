/* SPDX-License-Identifier: MIT */
/* QEMU Sega MegaDrive Z80 area + system control (bus request / reset) */

#ifndef MD_SYS_H
#define MD_SYS_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MD_SYS "md-sys"
OBJECT_DECLARE_SIMPLE_TYPE(MDSysState, MD_SYS)

#define MD_SYS_Z80_RAM_SIZE (8 * 1024)

struct MDSysState {
    SysBusDevice parent_obj;

    MemoryRegion z80_iomem;     /* 0xA00000-0xA0FFFF */
    MemoryRegion ctrl_iomem;    /* 0xA11000-0xA112FF */

    uint8_t z80_ram[MD_SYS_Z80_RAM_SIZE];

    bool z80_busreq;            /* 68k has requested the Z80 bus */
    bool z80_reset;             /* Z80 held in reset */
    uint8_t ym_addr[2];         /* YM2612 latched register addresses */
};

#endif
