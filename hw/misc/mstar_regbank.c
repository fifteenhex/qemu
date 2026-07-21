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
#include "trace.h"

/* Name of the register at `off`, or "?" if we have not decoded it yet. */
static const char *mstar_regbank_regname(MStarRegbankState *s, hwaddr off)
{
    const MStarRegName *r;

    for (r = s->regnames; r && r->name; r++) {
        if (r->offset == off) {
            return r->name;
        }
    }
    return "?";
}

static uint64_t mstar_regbank_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarRegbankState *s = MSTAR_REGBANK(opaque);
    uint64_t val = s->regs[addr / 4];

    if (trace_event_get_state_backends(TRACE_MSTAR_REGBANK_READ)) {
        trace_mstar_regbank_read(s->bankname ?: "regbank", s->base + addr,
                                 mstar_regbank_regname(s, addr), val);
    }
    return val;
}

static void mstar_regbank_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    MStarRegbankState *s = MSTAR_REGBANK(opaque);

    if (trace_event_get_state_backends(TRACE_MSTAR_REGBANK_WRITE)) {
        trace_mstar_regbank_write(s->bankname ?: "regbank", s->base + addr,
                                  mstar_regbank_regname(s, addr), val, size);
    }
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
    unsigned i;

    memset(s->regs, 0, sizeof(s->regs));
    for (i = 0; i < s->num_defaults; i++) {
        s->regs[s->defaults[i].offset / 4] = s->defaults[i].value;
    }
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
