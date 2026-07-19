/*
 * MStar/SigmaStar generic readback register bank
 *
 * Plenty of RIU banks (clkgen, pinctrl, ...) hold configuration that
 * only matters to the hardware: software programs them and later
 * reads back what it wrote - for example the vendor UART driver
 * derives its clock rate from the mux settings in the clkgen bank,
 * and a read-as-zero bank leaves it with a dead clock. This models
 * such a bank as plain 16-bit storage with no behaviour.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/mstar_regbank.h"

static uint64_t mstar_regbank_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarRegbankState *s = MSTAR_REGBANK(opaque);

    return s->regs[addr / 4];
}

static void mstar_regbank_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    MStarRegbankState *s = MSTAR_REGBANK(opaque);

    s->regs[addr / 4] = val;
}

static const MemoryRegionOps mstar_regbank_ops = {
    .read = mstar_regbank_read,
    .write = mstar_regbank_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_regbank_reset(DeviceState *dev)
{
    MStarRegbankState *s = MSTAR_REGBANK(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mstar_regbank_init(Object *obj)
{
    MStarRegbankState *s = MSTAR_REGBANK(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_regbank_ops, s,
                          TYPE_MSTAR_REGBANK, MSTAR_REGBANK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void mstar_regbank_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_regbank_reset);
}

static const TypeInfo mstar_regbank_types[] = {
    {
        .name           = TYPE_MSTAR_REGBANK,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarRegbankState),
        .instance_init  = mstar_regbank_init,
        .class_init     = mstar_regbank_class_init,
    },
};

DEFINE_TYPES(mstar_regbank_types)
