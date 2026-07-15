/*
 * MStar/SigmaStar MIPI D-PHY
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/loader.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "hw/arm/mstar.h"

/* ------------------------------------------------------------- dphy (MIPI) */

/*
 * MIPI D-PHY (dphy@2a5000, "sstar,ssd20xd-dphy"; the vendor "DPHY_DSI" bank
 * 0x152800 - byte offset = vendor bank << 1). 16-bit registers at the usual
 * 4-byte RIU stride, i.e. register N is at offset N*4.
 *
 * This is a scaffold for developing a real Linux driver: the mainline
 * mstar_dphy.c is an empty stub, and the PHY is not required to render (the
 * DSI controller streams from DRAM, and there is no physical panel to drive).
 * The model just stores and returns register values so a driver can program
 * the PHY and read the configuration back.
 *
 * Documented registers, from mstar_dphy.c's header comment and the values the
 * vendor u-boot writes during panel bring-up (its init writes, for reference):
 *
 *   0x00 (REG 0x00)  bit0 = sw_rst, bit6 = pd_ldo        (u-boot: 0x0001)
 *   0x04 (REG 0x01)  bit0 = power-down HS mode,
 *                    bit1 = power-down whole DPHY analog  (u-boot: 0x0000)
 *   0x0c (REG 0x03)                                       (u-boot: 0x0090)
 *   0x18 (REG 0x06)                                       (u-boot: 0x2008)
 *   0x28 (REG 0x0a)                                       (u-boot: 0x4010)
 *   0x38 (REG 0x0e)                                       (u-boot: 0x300c)
 *   0x50 (REG 0x14)                                       (u-boot: 0x0080)
 *   0x70 (REG 0x1c)                                       (u-boot: 0xc000)
 *   0x74 (REG 0x1d)                                       (u-boot: 0x0080)
 *   0x88 (REG 0x22)                                       (u-boot: 0x1004)
 *   0x94 (REG 0x25)  read-modify-write by u-boot          (u-boot: 0x0000)
 *   0xd4 (REG 0x35)                                       (u-boot: 0x0000)
 *
 * The 0x0c/0x18/0x28/0x38/0x88/0x94 registers are read-modified-written by
 * u-boot; the remaining fields (lane timing/PLL) are not yet reverse
 * engineered. If a status/PLL-lock bit is found, model it here so a driver's
 * poll loop completes.
 */
static uint64_t mstar_dphy_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarDphyState *s = opaque;

    mstar_iolog(MSTAR_DPHY_BASE + addr, false, s->regs[addr / 4], size);
    return s->regs[addr / 4];
}

static void mstar_dphy_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MstarDphyState *s = opaque;

    mstar_iolog(MSTAR_DPHY_BASE + addr, true, val, size);
    s->regs[addr / 4] = val;
}

static const MemoryRegionOps mstar_dphy_ops = {
    .read = mstar_dphy_read,
    .write = mstar_dphy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void mstar_dphy_reset_hold(Object *obj, ResetType type)
{
    MstarDphyState *s = MSTAR_DPHY(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mstar_dphy_realize(DeviceState *dev, Error **errp)
{
    MstarDphyState *s = MSTAR_DPHY(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_dphy_ops, s,
                          "mstar.dphy", MSTAR_DPHY_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void mstar_dphy_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_dphy_realize;
    rc->phases.hold = mstar_dphy_reset_hold;
}

static const TypeInfo mstar_dphy_types[] = {
    {
        .name           = TYPE_MSTAR_DPHY,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MstarDphyState),
        .class_init     = mstar_dphy_class_init,
    },
};

DEFINE_TYPES(mstar_dphy_types)
