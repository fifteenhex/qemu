/*
 * Zorro III expansion bus (AutoConfig).
 *
 * Models the expansion.library side of the Zorro III bus: the
 * configuration space at 0xFF000000 where unconfigured boards answer
 * one at a time (the /CFGIN daisy chain is the registration order of
 * the boards).  Reads return the ExpansionRom/ExpansionControl
 * registers as nybbles - the high nybble of logical register byte N at
 * offset N*4 and the low nybble at N*4 + 0x100 (the Zorro III layout;
 * the Zorro II low-nybble offset of N*4 + 2 is accepted too), every
 * byte but er_Type presented inverted.  Writing the base address to
 * ec_BaseAddress/ec_Z3_HighWord at 0x44 (word: address bits 31-16, or
 * the Zorro II two-step of a byte each to 0x44 and 0x48) configures
 * the board: its body region appears at that base and the next board
 * in the chain takes over the config space.  A write to ec_Shutup at
 * 0x4C silences the board instead.  With the chain exhausted the
 * space reads open bus (0xFF, "no board").
 *
 * For direct kernel boot no ROM runs AutoConfig, so
 * zorro3_bridge_autoconfig() plays expansion.library: it assigns each
 * board a size-aligned base in the expansion space from 0x40000000 up,
 * for the machine to describe to Linux in BI_AMIGA_AUTOCON bootinfo
 * records.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/m68k/zorro3.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "trace.h"

/*
 * ExpansionRom/ExpansionControl register offsets in the configuration
 * space (logical byte index * 4).
 */
#define ZORRO3_EC_BASEADDRESS   0x44    /* ec_Z3_HighWord / ec_BaseAddress */
#define ZORRO3_EC_BASEADDRESS_Z2 0x48   /* Zorro II ec_BaseAddress step 2 */
#define ZORRO3_EC_SHUTUP        0x4c    /* any write: board disappears */
/* the low nybble of each register byte repeats 0x100 above the high */
#define ZORRO3_CONFIG_LOW_NYBBLE 0x100

static Zorro3Board *zorro3_current_board(Zorro3Bus *bus)
{
    Zorro3Board *board;

    QTAILQ_FOREACH(board, &bus->boards, chain) {
        if (!board->configured && !board->shutup) {
            return board;
        }
    }
    return NULL;
}

/* the logical ExpansionRom byte as expansion.library sees it */
static uint8_t zorro3_rom_byte(Zorro3Board *board, unsigned reg)
{
    switch (reg) {
    case 0x00:
        return board->er_type;
    case 0x01:
        return board->er_product;
    case 0x02:
        return board->er_flags;
    case 0x04:
        return board->er_manufacturer >> 8;
    case 0x05:
        return board->er_manufacturer;
    case 0x06:
        return board->er_serial >> 24;
    case 0x07:
        return board->er_serial >> 16;
    case 0x08:
        return board->er_serial >> 8;
    case 0x09:
        return board->er_serial;
    default:
        /* er_Reserved03, er_InitDiagVec (no driver ROM), the rest */
        return 0;
    }
}

static void zorro3_board_map(Zorro3Board *board)
{
    if (!board->mapped) {
        memory_region_add_subregion(get_system_memory(), board->base,
                                    board->body);
        board->mapped = true;
    }
}

static void zorro3_board_unmap(Zorro3Board *board)
{
    if (board->mapped) {
        memory_region_del_subregion(get_system_memory(), board->body);
        board->mapped = false;
    }
}

static void zorro3_board_configure(Zorro3Board *board, hwaddr base)
{
    trace_zorro3_configure(board->er_product, base);
    board->base = base;
    board->configured = true;
    zorro3_board_map(board);
}

static uint8_t zorro3_config_read_byte(Zorro3Board *board, hwaddr addr)
{
    unsigned reg = (addr & 0xfc) >> 2;
    bool low = (addr & ZORRO3_CONFIG_LOW_NYBBLE) || (addr & 2);
    uint8_t byte, result;

    byte = zorro3_rom_byte(board, reg);
    result = (low ? byte << 4 : byte) & 0xf0;
    /* every register but er_Type is presented inverted */
    if (reg != 0) {
        result = ~result;
    }
    return result;
}

static uint64_t zorro3_config_read(void *opaque, hwaddr addr, unsigned size)
{
    Zorro3Bridge *br = opaque;
    Zorro3Board *board = zorro3_current_board(&br->bus);
    uint64_t result = 0;
    unsigned i;

    if (!board) {
        /* chain exhausted: open bus, "no board here" */
        return (uint64_t)-1;
    }

    for (i = 0; i < size; i++) {
        result = (result << 8) | zorro3_config_read_byte(board, addr + i);
    }
    trace_zorro3_config_read(addr, size, result);
    return result;
}

static void zorro3_config_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    Zorro3Bridge *br = opaque;
    Zorro3Bus *bus = &br->bus;
    Zorro3Board *board = zorro3_current_board(bus);

    trace_zorro3_config_write(addr, size, val);
    if (!board) {
        return;
    }

    switch (addr) {
    case ZORRO3_EC_BASEADDRESS:
        /*
         * The write to 0x44 commits the configuration.  Kickstart's
         * WriteExpansionWord writes the two bytes separately, low
         * byte to 0x48 first, so a byte write here carries address
         * bits 31-24 above the 0x48 latch; a word write carries bits
         * 31-16 whole.
         */
        if (size >= 2) {
            zorro3_board_configure(board, (val & 0xffff) << 16);
        } else {
            zorro3_board_configure(board,
                                   ((val & 0xff) << 24) | bus->base_latch);
        }
        bus->base_latch = 0;
        break;
    case ZORRO3_EC_BASEADDRESS_Z2:
        /*
         * Address bits 23-16.  A Zorro II board would configure right
         * here, but for a Zorro III board this is only the low half:
         * hold it until the commit write to 0x44.
         */
        if ((board->er_type & ZORRO3_ERT_TYPEMASK) == ZORRO3_ERT_ZORROIII) {
            bus->base_latch = (val & 0xff) << 16;
        } else {
            zorro3_board_configure(board, (val & 0xff) << 16);
        }
        break;
    case ZORRO3_EC_SHUTUP:
        trace_zorro3_shutup(board->er_product);
        board->shutup = true;
        bus->base_latch = 0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "zorro3: unimplemented config write "
                      "0x%03" HWADDR_PRIx " = 0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps zorro3_config_ops = {
    .read = zorro3_config_read,
    .write = zorro3_config_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 2 },
};

/*
 * The write side must see word writes whole (the Zorro III configure
 * is a single word write to 0x44 carrying address bits 31-16), while
 * the read side wants bytes; impl 1-2 with the byte/word split done in
 * the handlers covers both.
 */

void zorro3_bus_add_board(Zorro3Bus *bus, Zorro3Board *board)
{
    QTAILQ_INSERT_TAIL(&bus->boards, board, chain);
}

void zorro3_device_set_int2(Zorro3Device *dev, int level)
{
    Zorro3Bus *bus = ZORRO3_BUS(qdev_get_parent_bus(DEVICE(dev)));
    Zorro3Bridge *br = ZORRO3_BRIDGE(BUS(bus)->parent);

    bus->int2_levels = deposit32(bus->int2_levels, dev->int2_bit, 1, !!level);
    qemu_set_irq(br->int2, bus->int2_levels != 0);
}

void zorro3_bridge_autoconfig(Zorro3Bridge *br, Error **errp)
{
    Zorro3Bus *bus = &br->bus;
    Zorro3Board *board;
    hwaddr next = ZORRO3_EXPANSION_BASE;

    QTAILQ_FOREACH(board, &bus->boards, chain) {
        hwaddr base = QEMU_ALIGN_UP(next, board->size);

        if (board->shutup) {
            continue;
        }
        if (base + board->size > ZORRO3_EXPANSION_END) {
            error_setg(errp, "zorro3: expansion space exhausted");
            return;
        }
        zorro3_board_configure(board, base);
        next = base + board->size;
    }
    bus->static_config = true;
}

static void zorro3_bridge_reset(DeviceState *dev)
{
    Zorro3Bridge *br = ZORRO3_BRIDGE(dev);
    Zorro3Bus *bus = &br->bus;
    Zorro3Board *board;

    /*
     * Bus reset unconfigures every board and restarts the config
     * chain, as /RST does on the real bus - except when QEMU itself
     * configured the boards for a direct kernel boot, where the
     * assignment is part of the machine description and must survive
     * the initial system reset.
     */
    bus->base_latch = 0;
    QTAILQ_FOREACH(board, &bus->boards, chain) {
        if (bus->static_config) {
            if (!board->shutup) {
                board->configured = true;
                zorro3_board_map(board);
            }
        } else {
            board->configured = false;
            board->shutup = false;
            zorro3_board_unmap(board);
        }
    }
}

static void zorro3_bridge_init(Object *obj)
{
    Zorro3Bridge *br = ZORRO3_BRIDGE(obj);

    qbus_init(&br->bus, sizeof(br->bus), TYPE_ZORRO3_BUS, DEVICE(obj), NULL);
    QTAILQ_INIT(&br->bus.boards);
}

static void zorro3_bridge_realize(DeviceState *dev, Error **errp)
{
    Zorro3Bridge *br = ZORRO3_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&br->config_mr, OBJECT(br), &zorro3_config_ops,
                          br, "zorro3.autoconfig", ZORRO3_CONFIG_SIZE);
    sysbus_init_mmio(sbd, &br->config_mr);
    sysbus_init_irq(sbd, &br->int2);
}

static const VMStateDescription vmstate_zorro3_bridge = {
    .name = "zorro3-bridge",
    .unmigratable = 1,
};

static void zorro3_bridge_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = zorro3_bridge_realize;
    device_class_set_legacy_reset(dc, zorro3_bridge_reset);
    dc->vmsd = &vmstate_zorro3_bridge;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static void zorro3_device_realize(DeviceState *dev, Error **errp)
{
    Zorro3Device *zdev = ZORRO3_DEVICE(dev);
    Zorro3Bus *bus = ZORRO3_BUS(qdev_get_parent_bus(dev));

    zdev->int2_bit = bus->num_devices++;
}

static void zorro3_device_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = zorro3_device_realize;
    dc->bus_type = TYPE_ZORRO3_BUS;
}

static const TypeInfo zorro3_types[] = {
    {
        .name          = TYPE_ZORRO3_BUS,
        .parent        = TYPE_BUS,
        .instance_size = sizeof(Zorro3Bus),
    },
    {
        .name          = TYPE_ZORRO3_BRIDGE,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Zorro3Bridge),
        .instance_init = zorro3_bridge_init,
        .class_init    = zorro3_bridge_class_init,
    },
    {
        .name          = TYPE_ZORRO3_DEVICE,
        .parent        = TYPE_DEVICE,
        .instance_size = sizeof(Zorro3Device),
        .class_size    = sizeof(Zorro3DeviceClass),
        .class_init    = zorro3_device_class_init,
        .abstract      = true,
    },
};

DEFINE_TYPES(zorro3_types)
