/*
 * MStar/SigmaStar "bach" audio controller (dummy)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A scaffold for reverse engineering / developing the Linux sound driver
 * (sound/soc/mstar/msc313-bach.c). The bach block (mstar,msc313-bach) is the
 * SoC audio controller - a set of DMA sub-channels that move PCM between DRAM
 * and an analog codec, plus a sine generator and mixer. Its analog codec
 * registers live in a separate "audiotop" syscon, which the driver reaches
 * through the mstar,audiotop phandle (regmap offset REG_ATOP_OFFSET, 0x1000).
 *
 * This model does not produce sound. It stores and returns 16-bit register
 * values (RIU registers at the usual 4-byte stride) for both the bach block
 * and the audiotop syscon, and logs every access via mstar_iolog(), so the
 * driver's register programming (DMA setup, codec bring-up, playback trigger)
 * can be captured and studied. The DMA-completion interrupt line is wired but
 * not yet raised.
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

static uint64_t mstar_bach_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313BachState *s = opaque;

    mstar_iolog(MSTAR_BACH_BASE + addr, false, s->regs[addr / 4], size);
    return s->regs[addr / 4];
}

static void mstar_bach_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Msc313BachState *s = opaque;

    mstar_iolog(MSTAR_BACH_BASE + addr, true, val, size);
    s->regs[addr / 4] = val;
}

static const MemoryRegionOps mstar_bach_ops = {
    .read = mstar_bach_read,
    .write = mstar_bach_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static uint64_t mstar_bach_atop_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313BachState *s = opaque;

    mstar_iolog(MSTAR_AUDIOTOP_BASE + addr, false, s->atopregs[addr / 4], size);
    return s->atopregs[addr / 4];
}

static void mstar_bach_atop_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    Msc313BachState *s = opaque;

    mstar_iolog(MSTAR_AUDIOTOP_BASE + addr, true, val, size);
    s->atopregs[addr / 4] = val;
}

static const MemoryRegionOps mstar_bach_atop_ops = {
    .read = mstar_bach_atop_read,
    .write = mstar_bach_atop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void mstar_bach_reset_hold(Object *obj, ResetType type)
{
    Msc313BachState *s = MSC313_BACH(obj);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->atopregs, 0, sizeof(s->atopregs));
    qemu_set_irq(s->irq, 0);
}

static void mstar_bach_realize(DeviceState *dev, Error **errp)
{
    Msc313BachState *s = MSC313_BACH(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_bach_ops, s,
                          "mstar.bach", MSTAR_BACH_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    memory_region_init_io(&s->atop, OBJECT(dev), &mstar_bach_atop_ops, s,
                          "mstar.audiotop", MSTAR_AUDIOTOP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->atop);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void mstar_bach_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_bach_realize;
    rc->phases.hold = mstar_bach_reset_hold;
}

static const TypeInfo mstar_bach_types[] = {
    {
        .name           = TYPE_MSC313_BACH,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313BachState),
        .class_init     = mstar_bach_class_init,
    },
};

DEFINE_TYPES(mstar_bach_types)
