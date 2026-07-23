/*
 * Commodore A2065 Zorro II Ethernet card.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_M68K_AMIGA_A2065_H
#define HW_M68K_AMIGA_A2065_H

#include "hw/core/sysbus.h"
#include "net/net.h"
#include "hw/net/pcnet.h"
#include "qom/object.h"

#define TYPE_AMIGA_A2065 "amiga-a2065"
OBJECT_DECLARE_SIMPLE_TYPE(A2065State, AMIGA_A2065)

struct A2065State {
    SysBusDevice parent_obj;

    PCNetState state;           /* the Am7990 LANCE core */

    MemoryRegion body;          /* the 64KB Zorro II board window */
    MemoryRegion regs;          /* LANCE RDP/RAP at body+0x4000 */
    MemoryRegion ram;           /* 32KB onboard RAM at body+0x8000 */
    MemoryRegion autoconfig;    /* nibble config ROM at 0xe80000 */
    uint8_t *rambuf;            /* direct pointer into the onboard RAM */

    uint8_t rom[64];            /* the ExpansionRom autoconfig bytes */
    uint32_t base;              /* Zorro II base the OS assigned */
    bool configured;
};

#endif
