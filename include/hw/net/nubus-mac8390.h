/*
 * NuBus DP8390 ("mac8390") Ethernet card for classic 68k Macintoshes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_NET_NUBUS_MAC8390_H
#define HW_NET_NUBUS_MAC8390_H

#include "hw/nubus/nubus.h"
#include "hw/net/ne2000.h"
#include "qom/object.h"

#define TYPE_NUBUS_MAC8390 "nubus-mac8390"
OBJECT_DECLARE_TYPE(NubusMac8390State, NubusMac8390Class, NUBUS_MAC8390)

struct NubusMac8390State {
    NubusDevice parent_obj;

    NE2000State ne2000;     /* the shared DP8390 core */
    MemoryRegion reg;       /* DP8390 register file, "back4" spacing */
    MemoryRegion ram;       /* on-card shared ring RAM (aliased) */
    MemoryRegion reg_slot;  /* logical slot-space alias of reg */
    MemoryRegion ram_slot;  /* logical slot-space alias of ram */
    uint32_t ramsize;       /* populated RAM, power of two */
};

struct NubusMac8390Class {
    DeviceClass parent_class;
    DeviceRealize parent_realize;
};

#endif /* HW_NET_NUBUS_MAC8390_H */
