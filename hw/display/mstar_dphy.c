/*
 * MStar/SigmaStar MIPI D-PHY
 *
 * The analog MIPI D-PHY that sits between the DSI controller and the
 * panel (dphy@2a5000, "sstar,ssd20xd-dphy"; the vendor "DPHY_DSI"
 * bank). It serialises the DSI controller's output onto the physical
 * high-speed lanes. 16-bit registers on the usual 4 byte RIU stride.
 *
 * There is nothing to serialise in the model - the DSI controller
 * delivers packets straight to the panel model - so the D-PHY has no
 * behaviour here; it stores and returns register values so the driver
 * can program it and read it back. The register meanings below are
 * what the vendor u-boot programs during panel bring-up, recorded so
 * the block is documented rather than a nameless stub. No PLL-lock or
 * ready status register has been found that software polls, so none
 * is synthesised.
 *
 * Register (RIU reg N at offset N*4), value u-boot writes (``prev``):
 *   0x00  bit0 sw_rst, bit6 pd_ldo                 0x0001
 *   0x04  bit0 power-down HS mode, bit1 pd analog   0x0000
 *   0x0c                                            0x0090
 *   0x18                                            0x2008
 *   0x28                                            0x4010
 *   0x38                                            0x300c
 *   0x50                                            0x0080
 *   0x70                                            0xc000
 *   0x74                                            0x0080
 *   0x88                                            0x1004
 *   0x94  read-modify-written by u-boot             0x0000
 *   0xd4                                            0x0000
 * The lane-timing/PLL fields are not yet reverse engineered.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/display/mstar_dphy.h"

static uint64_t mstar_dphy_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarDphyState *s = MSTAR_DPHY(opaque);

    return s->regs[addr / 4];
}

static void mstar_dphy_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MStarDphyState *s = MSTAR_DPHY(opaque);

    s->regs[addr / 4] = val;
}

static const MemoryRegionOps mstar_dphy_ops = {
    .read = mstar_dphy_read,
    .write = mstar_dphy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void mstar_dphy_reset(DeviceState *dev)
{
    MStarDphyState *s = MSTAR_DPHY(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mstar_dphy_init(Object *obj)
{
    MStarDphyState *s = MSTAR_DPHY(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_dphy_ops, s,
                          TYPE_MSTAR_DPHY, MSTAR_DPHY_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void mstar_dphy_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_dphy_reset);
}

static const TypeInfo mstar_dphy_types[] = {
    {
        .name           = TYPE_MSTAR_DPHY,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarDphyState),
        .instance_init  = mstar_dphy_init,
        .class_init     = mstar_dphy_class_init,
    },
};

DEFINE_TYPES(mstar_dphy_types)
