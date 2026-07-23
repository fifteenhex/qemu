/*
 * Gayle, the Amiga 600/1200 gate array: the IDE interface.
 *
 * Gayle glues an ATA port (and the PCMCIA slot, not modelled here)
 * onto the Amiga chip bus.  The register layout, as documented in the
 * Commodore Gayle specification and mirrored by the Linux driver
 * (drivers/ata/pata_gayle.c, arch/m68k/include/asm/amigayle.h):
 *
 *   0xda0000  ATA register block.  The 16-bit data port is at offset
 *             0; the eight task-file registers are byte-wide on a
 *             stride of 4 (error/feature at +0x06, sector count at
 *             +0x0a, ..., status/command at +0x1e, i.e. +2 + reg * 4),
 *             and the control/alt-status register is at +0x101a.
 *             Gayle does not decode address bits 0x2020 in this block,
 *             so it all mirrors at +0x20 and +0x2000 (Kickstart's
 *             scsi.device drives the mirror at 0xda2000).
 *   0xda8000  status: the live state of the interrupt lines, IDE INTRQ
 *             in bit 7 (the PCMCIA card lines sit in the lower bits and
 *             read 0 with no card).
 *   0xda9000  interrupt change: bit 7 latches an IDE INTRQ assertion.
 *             Writing clears the bits written as 0 (the low two bits
 *             are the IDEACK outputs and are stored as written).
 *   0xdaa000  interrupt enable: bit 7 enables the IDE interrupt onto
 *             the board's INT2 line.
 *   0xdab000  configuration (PCMCIA voltage/speed control; stored).
 *   0xde1000  Gayle ID, read bit-serially: a write resets the bit
 *             pointer, then each read returns the next bit of the ID
 *             starting with the MSB on D7.  A600/A1200 Gayle IDs as
 *             0xd0.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/dma.h"

#include "hw/ide/gayle.h"
#include "ide-internal.h"

/* address bits Gayle leaves undecoded in the ATA block */
#define GAYLE_ATA_MIRROR_MASK   0x2020
/* the control/alt-status register sits in the 0x1000 half */
#define GAYLE_ATA_CTRL_BLOCK    0x1000

/* interrupt bits shared by the status/change/enable registers */
#define GAYLE_INT_IDE           0x80
/* the IDEACK output bits in the change register store their value */
#define GAYLE_INT_IDEACK        0x03

OBJECT_DECLARE_SIMPLE_TYPE(GayleIDEState, GAYLE_IDE)

struct GayleIDEState {
    SysBusDevice parent_obj;

    IDEBus bus;
    MemoryRegion ata;
    MemoryRegion ctrl;
    MemoryRegion id;
    qemu_irq irq;

    uint8_t gayle_id;           /* the serially-read ID value */
    uint8_t id_shift;           /* current bit position of the ID read */
    uint8_t intreq;             /* 0xda9000: latched interrupt bits */
    uint8_t intena;             /* 0xdaa000: interrupt enable bits */
    uint8_t cfg;                /* 0xdab000 */
    bool ide_level;             /* live INTRQ from the drive */
};

static void gayle_update_irq(GayleIDEState *s)
{
    qemu_set_irq(s->irq, !!(s->intreq & s->intena & GAYLE_INT_IDE));
}

static void gayle_ide_intrq(void *opaque, int n, int level)
{
    GayleIDEState *s = opaque;

    /* the change register latches the INTRQ assertion edge */
    if (level && !s->ide_level) {
        s->intreq |= GAYLE_INT_IDE;
    }
    s->ide_level = level;
    gayle_update_irq(s);
}

static uint64_t gayle_ata_read(void *opaque, hwaddr addr, unsigned size)
{
    GayleIDEState *s = opaque;
    unsigned reg;

    addr &= ~GAYLE_ATA_MIRROR_MASK;
    /* +2 + reg * 4 for the task file, so the register index is addr >> 2 */
    reg = (addr >> 2) & 7;
    if (addr & GAYLE_ATA_CTRL_BLOCK) {
        if (reg == 6) {
            return ide_status_read(&s->bus, 0);     /* alt status */
        }
        return 0xff;
    }
    if (reg == 0) {
        return ide_data_readw(&s->bus, 0);
    }
    return ide_ioport_read(&s->bus, reg);
}

static void gayle_ata_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    GayleIDEState *s = opaque;
    unsigned reg;

    addr &= ~GAYLE_ATA_MIRROR_MASK;
    reg = (addr >> 2) & 7;
    if (addr & GAYLE_ATA_CTRL_BLOCK) {
        if (reg == 6) {
            ide_ctrl_write(&s->bus, 0, val);        /* device control */
        }
        return;
    }
    if (reg == 0) {
        ide_data_writew(&s->bus, 0, val);
        return;
    }
    ide_ioport_write(&s->bus, reg, val);
}

/*
 * Little-endian so that the 16-bit data port's low byte (the first
 * byte of a sector) lands in the big-endian guest's high lane, i.e.
 * at the even address — the straight D15..D0 wiring of the real port.
 * The byte-wide task-file registers don't care.  32-bit data-port
 * accesses split into two word cycles like the real 16-bit bus.
 */
static const MemoryRegionOps gayle_ata_ops = {
    .read = gayle_ata_read,
    .write = gayle_ata_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

/* the four control registers each decode a 0x1000 page */
enum {
    GAYLE_CTRL_STATUS = 0,
    GAYLE_CTRL_CHANGE = 1,
    GAYLE_CTRL_ENABLE = 2,
    GAYLE_CTRL_CONFIG = 3,
};

static uint64_t gayle_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    GayleIDEState *s = opaque;

    switch (addr >> 12) {
    case GAYLE_CTRL_STATUS:
        /* live INTRQ; the PCMCIA card-status lines read 0 (no card) */
        return s->ide_level ? GAYLE_INT_IDE : 0;
    case GAYLE_CTRL_CHANGE:
        return s->intreq;
    case GAYLE_CTRL_ENABLE:
        return s->intena;
    case GAYLE_CTRL_CONFIG:
        return s->cfg;
    }
    return 0;
}

static void gayle_ctrl_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    GayleIDEState *s = opaque;

    switch (addr >> 12) {
    case GAYLE_CTRL_STATUS:
        /* the status register reflects the lines; nothing to store */
        break;
    case GAYLE_CTRL_CHANGE:
        /* write-0-to-clear, except the IDEACK bits which store */
        s->intreq = (s->intreq & val & ~GAYLE_INT_IDEACK) |
                    (val & GAYLE_INT_IDEACK);
        /* an INTRQ still pending past the ack latches right back */
        if (s->ide_level) {
            s->intreq |= GAYLE_INT_IDE;
        }
        gayle_update_irq(s);
        break;
    case GAYLE_CTRL_ENABLE:
        s->intena = val;
        gayle_update_irq(s);
        break;
    case GAYLE_CTRL_CONFIG:
        s->cfg = val;
        break;
    }
}

static const MemoryRegionOps gayle_ctrl_ops = {
    .read = gayle_ctrl_read,
    .write = gayle_ctrl_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static uint64_t gayle_id_read(void *opaque, hwaddr addr, unsigned size)
{
    GayleIDEState *s = opaque;

    /* one ID bit per read on D7, MSB first, then zeros */
    return (s->gayle_id << (s->id_shift++ & 31)) & 0x80;
}

static void gayle_id_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    GayleIDEState *s = opaque;

    /* any write restarts the serial read at the MSB */
    s->id_shift = 0;
}

static const MemoryRegionOps gayle_id_ops = {
    .read = gayle_id_read,
    .write = gayle_id_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void gayle_ide_reset(DeviceState *dev)
{
    GayleIDEState *s = GAYLE_IDE(dev);

    ide_bus_reset(&s->bus);
    s->id_shift = 0;
    s->intreq = 0;
    s->intena = 0;
    s->cfg = 0;
    s->ide_level = false;
    gayle_update_irq(s);
}

static const VMStateDescription vmstate_gayle_ide = {
    .name = "gayle-ide",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_IDE_BUS(bus, GayleIDEState),
        VMSTATE_IDE_DRIVES(bus.ifs, GayleIDEState),
        VMSTATE_UINT8(id_shift, GayleIDEState),
        VMSTATE_UINT8(intreq, GayleIDEState),
        VMSTATE_UINT8(intena, GayleIDEState),
        VMSTATE_UINT8(cfg, GayleIDEState),
        VMSTATE_BOOL(ide_level, GayleIDEState),
        VMSTATE_END_OF_LIST()
    }
};

static void gayle_ide_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *d = SYS_BUS_DEVICE(dev);
    GayleIDEState *s = GAYLE_IDE(dev);

    ide_bus_init_output_irq(&s->bus,
                            qemu_allocate_irq(gayle_ide_intrq, s, 0));

    memory_region_init_io(&s->ata, OBJECT(s), &gayle_ata_ops, s,
                          "gayle.ata", GAYLE_IDE_ATA_SIZE);
    memory_region_init_io(&s->ctrl, OBJECT(s), &gayle_ctrl_ops, s,
                          "gayle.ctrl", GAYLE_IDE_CTRL_SIZE);
    memory_region_init_io(&s->id, OBJECT(s), &gayle_id_ops, s,
                          "gayle.id", GAYLE_IDE_ID_SIZE);
    sysbus_init_mmio(d, &s->ata);
    sysbus_init_mmio(d, &s->ctrl);
    sysbus_init_mmio(d, &s->id);
}

static void gayle_ide_initfn(Object *obj)
{
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    GayleIDEState *s = GAYLE_IDE(obj);

    ide_bus_init(&s->bus, sizeof(s->bus), DEVICE(obj), 0, 2);
    sysbus_init_irq(d, &s->irq);
}

void gayle_ide_init_drives(DeviceState *dev, DriveInfo *hd0, DriveInfo *hd1)
{
    GayleIDEState *s = GAYLE_IDE(dev);

    if (hd0) {
        ide_bus_create_drive(&s->bus, 0, hd0);
    }
    if (hd1) {
        ide_bus_create_drive(&s->bus, 1, hd1);
    }
}

static const Property gayle_ide_properties[] = {
    /* the A600/A1200 Gayle identifies as 0xd0 */
    DEFINE_PROP_UINT8("gayle-id", GayleIDEState, gayle_id, 0xd0),
};

static void gayle_ide_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = gayle_ide_realize;
    device_class_set_legacy_reset(dc, gayle_ide_reset);
    device_class_set_props(dc, gayle_ide_properties);
    dc->vmsd = &vmstate_gayle_ide;
}

static const TypeInfo gayle_ide_types[] = {
    {
        .name = TYPE_GAYLE_IDE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(GayleIDEState),
        .instance_init = gayle_ide_initfn,
        .class_init = gayle_ide_class_init,
    },
};

DEFINE_TYPES(gayle_ide_types)
