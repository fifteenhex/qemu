/*
 * MStar/SigmaStar clkgen clock block
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

/* ---------------------------------------------------------------- clkgen */

/*
 * What the reverse-engineered Linux clkgen driver knows about: for each mux
 * register (byte offset in the block), a mask of the bits it describes (the
 * gate bit, the parent-select field and, where present, the deglitch bit).
 * Anything the firmware reads/writes outside this table is logged as UNIMP so
 * we can find the pieces the driver (and therefore we) don't understand yet.
 * Derived from drivers/clk/mstar/clk-msc313-clkgen-msc313.h.
 */
typedef struct {
    unsigned int offset;
    const char *name;
    uint16_t known;
} Msc313ClkgenReg;

static const Msc313ClkgenReg msc313_clkgen_known[] = {
    { 0x04, "mcu/riubrdg", 0x0d1d },
    { 0x5c, "miu",         0x001d },
    { 0x64, "ddr_syn",     0x000d },
    { 0xc4, "uart0/1",     0x0d0d },
    { 0xc8, "spi",         0x001d },
    { 0xcc, "mspi0/1",     0x0d0d },
    { 0xd0, "fuart",       0x00dd },
    { 0xdc, "miic0/1",     0x0d0d },
    { 0x108, "emac_ahb",   0x000d },
    { 0x114, "sdio",       0x001d },
    { 0x180, "bdma",       0x001d },
    { 0x184, "aesdma/isp", 0x1d1d },
    { 0x1a8, "jpe",        0x000d },
};

static const Msc313ClkgenReg *msc313_clkgen_lookup(hwaddr off)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(msc313_clkgen_known); i++) {
        if (msc313_clkgen_known[i].offset == off) {
            return &msc313_clkgen_known[i];
        }
    }
    return NULL;
}

static uint64_t msc313_clkgen_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313ClkgenState *s = opaque;

    if (!msc313_clkgen_lookup(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "mstar-clkgen: read of undescribed register 0x%03x\n",
                      (unsigned)addr);
    }
    return s->regs[addr / 4];
}

static void msc313_clkgen_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    Msc313ClkgenState *s = opaque;
    const Msc313ClkgenReg *reg = msc313_clkgen_lookup(addr);

    if (!reg) {
        qemu_log_mask(LOG_UNIMP,
                      "mstar-clkgen: write 0x%04x to undescribed register "
                      "0x%03x\n", (unsigned)(val & 0xffff), (unsigned)addr);
    } else if (val & ~(uint64_t)reg->known) {
        qemu_log_mask(LOG_UNIMP,
                      "mstar-clkgen: write 0x%04x to %s (0x%03x) sets bits "
                      "0x%04x the driver does not describe (known 0x%04x)\n",
                      (unsigned)(val & 0xffff), reg->name, (unsigned)addr,
                      (unsigned)(val & ~reg->known & 0xffff), reg->known);
    }
    s->regs[addr / 4] = val;
}

static const MemoryRegionOps msc313_clkgen_ops = {
    .read = msc313_clkgen_read,
    .write = msc313_clkgen_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void msc313_clkgen_reset_hold(Object *obj, ResetType type)
{
    Msc313ClkgenState *s = MSC313_CLKGEN(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void msc313_clkgen_realize(DeviceState *dev, Error **errp)
{
    Msc313ClkgenState *s = MSC313_CLKGEN(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313_clkgen_ops, s,
                          "mstar.clkgen", MSTAR_CLKGEN_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void msc313_clkgen_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313_clkgen_realize;
    rc->phases.hold = msc313_clkgen_reset_hold;
}

static const TypeInfo mstar_clkgen_types[] = {
    {
        .name           = TYPE_MSC313_CLKGEN,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313ClkgenState),
        .class_init     = msc313_clkgen_class_init,
    },
};

DEFINE_TYPES(mstar_clkgen_types)
