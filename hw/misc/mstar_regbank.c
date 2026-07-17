/*
 * MStar/SigmaStar generic passive register bank
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A small reusable model for the many RIU sub-blocks that are just passive
 * register banks with no active side effects: the guest writes configuration
 * and reads it back. It is backed by a flat byte array so read-after-write is
 * consistent for any access size (unlike the catch-all, which reads back 0).
 *
 * Used for blocks the firmware only stores to / reads from, e.g. the DT
 * "syscon"/"simple-mfd" bank at 0x1f226600. With "readonly" set it also models
 * a read-only fuse array (efuse@4000, "mstar,msc313-efuse"): the guest only
 * reads it, and real fuse contents can be injected later via the backing store.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

static uint64_t mstar_regbank_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarRegbankState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size && addr + i < s->size; i++) {
        val |= (uint64_t)s->store[addr + i] << (8 * i);
    }
    return val;
}

static void mstar_regbank_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    MstarRegbankState *s = opaque;
    unsigned i;

    if (s->readonly) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write %0*x to read-only register 0x%03x ignored\n",
                      s->name, size * 2, (unsigned)val, (unsigned)addr);
        return;
    }
    for (i = 0; i < size && addr + i < s->size; i++) {
        s->store[addr + i] = (val >> (8 * i)) & 0xff;
    }
}

static const MemoryRegionOps mstar_regbank_ops = {
    .read = mstar_regbank_read,
    .write = mstar_regbank_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_regbank_reset_hold(Object *obj, ResetType type)
{
    MstarRegbankState *s = MSTAR_REGBANK(obj);

    memset(s->store, 0, s->size);
}

static void mstar_regbank_realize(DeviceState *dev, Error **errp)
{
    MstarRegbankState *s = MSTAR_REGBANK(dev);

    if (s->size == 0) {
        error_setg(errp, "mstar-regbank: size must be non-zero");
        return;
    }
    if (!s->name) {
        s->name = g_strdup("mstar-regbank");
    }
    s->store = g_malloc0(s->size);
    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_regbank_ops, s,
                          s->name, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mstar_regbank = {
    .name = "mstar-regbank",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VBUFFER_UINT32(store, MstarRegbankState, 0, NULL, size),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mstar_regbank_props[] = {
    DEFINE_PROP_UINT32("size", MstarRegbankState, size, 0x200),
    DEFINE_PROP_BOOL("readonly", MstarRegbankState, readonly, false),
    DEFINE_PROP_STRING("name", MstarRegbankState, name),
};

static void mstar_regbank_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_regbank_realize;
    dc->vmsd = &vmstate_mstar_regbank;
    rc->phases.hold = mstar_regbank_reset_hold;
    device_class_set_props(dc, mstar_regbank_props);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo mstar_regbank_types[] = {
    {
        .name           = TYPE_MSTAR_REGBANK,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MstarRegbankState),
        .class_init     = mstar_regbank_class_init,
    },
};

DEFINE_TYPES(mstar_regbank_types)
