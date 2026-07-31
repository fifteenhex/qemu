/*
 * Zorro III RAM expansion board.
 *
 * A generic AutoConfig fast-RAM card (the ZorRAM / BigRamPlus class of
 * board): one Zorro III board with the MEMLIST flag, so AmigaOS links
 * the space into the free-memory list and a direct-booted Linux gets it
 * as an extra memory chunk.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/m68k/zorro3.h"
#include "system/memory.h"
#include "qom/object.h"

/* AutoConfig manufacturer 0x07DB is reserved for testing/unassigned */
#define Z3RAM_MANUFACTURER      0x07db
#define Z3RAM_PRODUCT           0x01

#define TYPE_Z3RAM "z3ram"
OBJECT_DECLARE_SIMPLE_TYPE(Z3RamState, Z3RAM)

struct Z3RamState {
    Zorro3Device parent_obj;

    uint64_t size;
    MemoryRegion ram;
    Zorro3Board board;
};

static void z3ram_realize(DeviceState *dev, Error **errp)
{
    Z3RamState *s = Z3RAM(dev);
    Zorro3Bus *bus = ZORRO3_BUS(qdev_get_parent_bus(dev));
    uint8_t size_code;

    switch (s->size) {
    case 16 * MiB:  size_code = ZORRO3_ERT_EXT_16MB;  break;
    case 32 * MiB:  size_code = ZORRO3_ERT_EXT_32MB;  break;
    case 64 * MiB:  size_code = ZORRO3_ERT_EXT_64MB;  break;
    case 128 * MiB: size_code = ZORRO3_ERT_EXT_128MB; break;
    case 256 * MiB: size_code = ZORRO3_ERT_EXT_256MB; break;
    case 512 * MiB: size_code = ZORRO3_ERT_EXT_512MB; break;
    case 1 * GiB:   size_code = ZORRO3_ERT_EXT_1GB;   break;
    default:
        error_setg(errp, "z3ram: size must be a power of two between "
                   "16M and 1G");
        return;
    }

    memory_region_init_ram(&s->ram, OBJECT(dev), "z3ram", s->size, errp);
    if (*errp) {
        return;
    }

    s->board = (Zorro3Board) {
        .er_type = ZORRO3_ERT_ZORROIII | ZORRO3_ERTF_MEMLIST | size_code,
        .er_flags = ZORRO3_ERFF_ZORRO_III | ZORRO3_ERFF_EXTENDED |
                    ZORRO3_ERF_SUBSIZE_MATCH,
        .er_product = Z3RAM_PRODUCT,
        .er_manufacturer = Z3RAM_MANUFACTURER,
        .size = s->size,
        .body = &s->ram,
    };
    zorro3_bus_add_board(bus, &s->board);
}

static const Property z3ram_properties[] = {
    DEFINE_PROP_SIZE("size", Z3RamState, size, 128 * MiB),
};

static void z3ram_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = z3ram_realize;
    dc->desc = "Zorro III RAM expansion";
    device_class_set_props(dc, z3ram_properties);
}

static const TypeInfo z3ram_info = {
    .name = TYPE_Z3RAM,
    .parent = TYPE_ZORRO3_DEVICE,
    .instance_size = sizeof(Z3RamState),
    .class_init = z3ram_class_init,
};

static void z3ram_register_types(void)
{
    type_register_static(&z3ram_info);
}

type_init(z3ram_register_types);
