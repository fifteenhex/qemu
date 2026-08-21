/*
 * Minimal VMEbus model - see include/hw/vme/vme.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/vme/vme.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"

/* ---------------------------------------------------------------- bus --- */

static const TypeInfo vme_bus_info = {
    .name = TYPE_VME_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(VMEBus),
};

VMEBus *vme_bus_new(DeviceState *bridge, const char *name, uint64_t size)
{
    VMEBus *bus = VME_BUS(qbus_new(TYPE_VME_BUS, bridge, name));

    memory_region_init(&bus->mmio, OBJECT(bus), "vme", size);
    return bus;
}

MemoryRegion *vme_bus_mmio(VMEBus *bus)
{
    return &bus->mmio;
}

void vme_bus_set_irq(VMEBus *bus, qemu_irq irq)
{
    bus->irq_out = irq;
}

/*
 * Recompute the request presented to the bridge: the highest level a card
 * is asserting wins and supplies its vector.  With more than one card up at
 * the same level a real bus daisy-chains the IACK; here the first found
 * wins, which is enough for a single-card backplane.
 */
static void vme_bus_update_irq(VMEBus *bus)
{
    BusState *b = BUS(bus);
    BusChild *kid;
    int best_level = 0, best_vector = 0;

    if (!bus->irq_out) {
        return;
    }
    QTAILQ_FOREACH(kid, &b->children, sibling) {
        VMEDevice *dev = VME_DEVICE(kid->child);

        if (dev->irq_state && (int)dev->irq_level > best_level) {
            best_level = dev->irq_level;
            best_vector = dev->irq_vector;
        }
    }
    qemu_set_irq(bus->irq_out,
                 best_level ? (best_level << 8) | (best_vector & 0xff) : 0);
}

/* ------------------------------------------------------------- device --- */

void vme_device_map(VMEDevice *dev, MemoryRegion *window)
{
    VMEBus *bus = VME_BUS(qdev_get_parent_bus(DEVICE(dev)));

    memory_region_add_subregion(&bus->mmio, dev->vme_base, window);
}

void vme_device_set_irq(VMEDevice *dev, int assert)
{
    VMEBus *bus = VME_BUS(qdev_get_parent_bus(DEVICE(dev)));

    dev->irq_state = assert;
    vme_bus_update_irq(bus);
}

static const Property vme_device_props[] = {
    DEFINE_PROP_UINT64("vme-base", VMEDevice, vme_base, 0),
    DEFINE_PROP_UINT32("irq-level", VMEDevice, irq_level, 0),
    DEFINE_PROP_UINT32("irq-vector", VMEDevice, irq_vector, 0),
};

static void vme_device_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->bus_type = TYPE_VME_BUS;
    device_class_set_props(dc, vme_device_props);
}

static const TypeInfo vme_device_info = {
    .name = TYPE_VME_DEVICE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(VMEDevice),
    .class_size = sizeof(VMEDeviceClass),
    .class_init = vme_device_class_init,
    .abstract = true,
};

/* ------------------------------------------------------------- bridge --- */

VMEBus *vme_bridge_get_bus(VMEBridge *br)
{
    return br->bus;
}

static void vme_bridge_realize(DeviceState *dev, Error **errp)
{
    VMEBridge *s = VME_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->bus = vme_bus_new(dev, "vme", s->size);
    sysbus_init_mmio(sbd, vme_bus_mmio(s->bus));
    sysbus_init_irq(sbd, &s->irq);
    vme_bus_set_irq(s->bus, s->irq);
}

static const Property vme_bridge_props[] = {
    DEFINE_PROP_UINT64("size", VMEBridge, size, 0x01000000),
};

static void vme_bridge_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = vme_bridge_realize;
    device_class_set_props(dc, vme_bridge_props);
}

static const TypeInfo vme_bridge_info = {
    .name = TYPE_VME_BRIDGE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VMEBridge),
    .class_init = vme_bridge_class_init,
};

static void vme_register_types(void)
{
    type_register_static(&vme_bus_info);
    type_register_static(&vme_device_info);
    type_register_static(&vme_bridge_info);
}

type_init(vme_register_types)
