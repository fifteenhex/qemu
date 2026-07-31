/*
 * Zorro III expansion bus (AutoConfig).
 *
 * A minimal model of the big-box Amiga expansion bus: unconfigured
 * boards answer one at a time in the AutoConfig configuration space at
 * 0xFF000000, where the OS reads the board's ExpansionRom description
 * nybble by nybble and then writes the board's base address (or tells
 * it to shut up).  Configured boards appear at their assigned base in
 * the Zorro III expansion space at 0x40000000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_M68K_ZORRO3_H
#define HW_M68K_ZORRO3_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ZORRO3_BUS "zorro3-bus"
OBJECT_DECLARE_SIMPLE_TYPE(Zorro3Bus, ZORRO3_BUS)

#define TYPE_ZORRO3_DEVICE "zorro3-device"
OBJECT_DECLARE_TYPE(Zorro3Device, Zorro3DeviceClass, ZORRO3_DEVICE)

#define TYPE_ZORRO3_BRIDGE "zorro3-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(Zorro3Bridge, ZORRO3_BRIDGE)

/* AutoConfig configuration space (Zorro III bus specification) */
#define ZORRO3_CONFIG_BASE      0xff000000
#define ZORRO3_CONFIG_SIZE      0x1000
/* Zorro III expansion space, where configured boards live */
#define ZORRO3_EXPANSION_BASE   0x40000000
#define ZORRO3_EXPANSION_END    0x80000000
/* how many boards a Linux BI_AMIGA_AUTOCON boot can describe */
#define ZORRO_NUM_AUTO          16

/* er_Type: board type in bits 7-6, physical size code in bits 2-0 */
#define ZORRO3_ERT_TYPEMASK     0xc0
#define ZORRO3_ERT_ZORROII      0xc0
#define ZORRO3_ERT_ZORROIII     0x80
#define ZORRO3_ERTF_MEMLIST     0x20
/* er_Type size codes when ERFF_EXTENDED is set: 16MB << code */
#define ZORRO3_ERT_EXT_16MB     0x00
#define ZORRO3_ERT_EXT_32MB     0x01
#define ZORRO3_ERT_EXT_64MB     0x02
#define ZORRO3_ERT_EXT_128MB    0x03
#define ZORRO3_ERT_EXT_256MB    0x04
#define ZORRO3_ERT_EXT_512MB    0x05
#define ZORRO3_ERT_EXT_1GB      0x06

/* er_Flags */
#define ZORRO3_ERFF_NOSHUTUP    0x40
#define ZORRO3_ERFF_EXTENDED    0x20    /* size code uses the extended table */
#define ZORRO3_ERFF_ZORRO_III   0x10
/* low nybble: logical size; 0 = matches the physical size */
#define ZORRO3_ERF_SUBSIZE_MATCH 0x00

/*
 * One logical AutoConfig board.  A card (Zorro3Device) may carry more
 * than one: the boards chain in registration order.  The body region
 * is mapped at the configured base address by the bus.
 */
typedef struct Zorro3Board {
    /* filled in by the device before zorro3_bus_add_board() */
    uint8_t er_type;
    uint8_t er_product;
    uint8_t er_flags;
    uint16_t er_manufacturer;
    uint32_t er_serial;
    uint64_t size;
    MemoryRegion *body;

    /* bus state */
    bool configured;
    bool shutup;
    bool mapped;
    hwaddr base;
    QTAILQ_ENTRY(Zorro3Board) chain;
} Zorro3Board;

struct Zorro3Bus {
    BusState qbus;

    QTAILQ_HEAD(, Zorro3Board) boards;
    /* base address bits collected from partial config-register writes */
    uint32_t base_latch;
    /*
     * QEMU played expansion.library (direct kernel boot): boards keep
     * their assigned bases across resets instead of unconfiguring.
     */
    bool static_config;
    /* open-collector /INT2, one bit per device, wired-OR */
    uint32_t int2_levels;
    int num_devices;
};

struct Zorro3Device {
    DeviceState qdev;
    int int2_bit;
};

struct Zorro3DeviceClass {
    DeviceClass parent_class;
    DeviceRealize parent_realize;
};

struct Zorro3Bridge {
    SysBusDevice parent_obj;

    Zorro3Bus bus;
    MemoryRegion config_mr;
    qemu_irq int2;
};

void zorro3_bus_add_board(Zorro3Bus *bus, Zorro3Board *board);
void zorro3_device_set_int2(Zorro3Device *dev, int level);
/*
 * Assign expansion-space bases to every board like expansion.library
 * would, for direct kernel boot where no ROM runs AutoConfig.  The
 * assignment sticks across resets.
 */
void zorro3_bridge_autoconfig(Zorro3Bridge *br, Error **errp);

#endif
