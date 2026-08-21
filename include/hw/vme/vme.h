/*
 * Minimal VMEbus model.
 *
 * Enough of a bus to hang a slave card off a processor board: the card
 * installs its VMEbus-accessible window into the bus's slave address
 * space at its (jumpered) base address, and drives a prioritised
 * interrupt request back towards the host bridge.  The bridge aliases
 * the slave space into its local CPU address space and turns the request
 * into a processor interrupt.  It deliberately models none of the DTB
 * arbitration, block transfers or geographical addressing of real VME -
 * just the master's view of a slave card and its interrupt.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_VME_VME_H
#define HW_VME_VME_H

#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_VME_BUS "vme-bus"
OBJECT_DECLARE_SIMPLE_TYPE(VMEBus, VME_BUS)

#define TYPE_VME_DEVICE "vme-device"
OBJECT_DECLARE_TYPE(VMEDevice, VMEDeviceClass, VME_DEVICE)

struct VMEBus {
    BusState parent_obj;

    /*
     * The modelled VME slave address space.  A bridge maps this (or an
     * alias of part of it) into its local address space; cards install
     * their slave windows here at their jumpered VME base address.
     */
    MemoryRegion mmio;

    /*
     * Aggregated interrupt request towards the host bridge, encoded as
     * (level << 8) | vector - the shape the m68k boards already consume -
     * or 0 when no card has a request up.
     */
    qemu_irq irq_out;
};

struct VMEDeviceClass {
    DeviceClass parent_class;
};

struct VMEDevice {
    DeviceState parent_obj;

    uint64_t vme_base;      /* base of this card's slave window in VME space */
    uint32_t irq_level;     /* VME IRQ level it drives, 1..7 (0 = none)      */
    uint32_t irq_vector;    /* the status/ID vector it returns at IACK       */
    bool irq_state;         /* whether its request is currently asserted     */
};

/* Bridge side ------------------------------------------------------------- */

/*
 * Create a VME bus under @bridge.  @size is the extent of the modelled
 * slave address space, e.g. 16 MiB for an A24 window.
 */
VMEBus *vme_bus_new(DeviceState *bridge, const char *name, uint64_t size);

/* The slave address space, for the bridge to map/alias into local memory. */
MemoryRegion *vme_bus_mmio(VMEBus *bus);

/* Where the bridge wants the aggregated (level<<8)|vector request delivered. */
void vme_bus_set_irq(VMEBus *bus, qemu_irq irq);

/*
 * A ready-made host bridge for boards that do not model their own VME
 * master.  It owns a VME bus and presents the slave address space as
 * sysbus MMIO region 0 (map it wherever the board sees VME) and the
 * aggregated (level<<8)|vector request as sysbus IRQ 0.  The modelled
 * slave-space size is the "size" property (default 16 MiB, A24).
 */
#define TYPE_VME_BRIDGE "vme-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(VMEBridge, VME_BRIDGE)

struct VMEBridge {
    SysBusDevice parent_obj;

    uint64_t size;
    VMEBus *bus;
    qemu_irq irq;
};

/* The bus new cards attach to. */
VMEBus *vme_bridge_get_bus(VMEBridge *br);

/* Card side --------------------------------------------------------------- */

/* Install a card's slave @window at its configured vme_base. */
void vme_device_map(VMEDevice *dev, MemoryRegion *window);

/* Assert (@assert != 0) or release the card's VME interrupt request. */
void vme_device_set_irq(VMEDevice *dev, int assert);

#endif /* HW_VME_VME_H */
