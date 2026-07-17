/*
 * MStar/SigmaStar CMDQ - command-queue engine (0x1f224000)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The register-write DMA / command-queue block (DT "msc313-cmdq"), present on
 * infinity / infinity2m / infinity3. Firmware builds a list of register-write
 * descriptors in DRAM, points this engine at it and triggers it; the engine
 * replays the writes and raises a completion. It is used by the display/scaler
 * compositing paths and, on the camera, by the SCLIRQ sensor-capture command
 * queue.
 *
 * Not yet functionally modelled: this is a named store/read-back region (so the
 * driver reads back what it wrote, instead of the catch-all's 0) that is being
 * used to MAP the register layout. Set MSTAR_CMDQ_DBG=1 to log every access
 * (offset, value, guest PC) - the Miyoo (infinity2m) exercises this block with
 * a small, clean set of accesses, which is the easiest way to reverse the
 * trigger/status/done registers.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/cpu.h"
#include "hw/core/resettable.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

static void mstar_cmdq_dbg(const char *rw, hwaddr addr, uint64_t val)
{
    uint64_t pc = 0;

    if (!getenv("MSTAR_CMDQ_DBG")) {
        return;
    }
    if (current_cpu) {
        pc = CPU_GET_CLASS(current_cpu)->get_pc(current_cpu);
    }
    fprintf(stderr, "CMDQ %s off=%03x val=%04x pc=%08x\n",
            rw, (int)addr, (unsigned)(val & 0xffff), (unsigned)pc);
}

static uint64_t mstar_cmdq_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarCmdqState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size && addr + i < s->size; i++) {
        val |= (uint64_t)s->store[addr + i] << (8 * i);
    }
    mstar_cmdq_dbg("R", addr, val);
    return val;
}

static void mstar_cmdq_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MstarCmdqState *s = opaque;
    unsigned i;

    mstar_cmdq_dbg("W", addr, val);
    for (i = 0; i < size && addr + i < s->size; i++) {
        s->store[addr + i] = (val >> (8 * i)) & 0xff;
    }
}

static const MemoryRegionOps mstar_cmdq_ops = {
    .read = mstar_cmdq_read,
    .write = mstar_cmdq_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_cmdq_reset_hold(Object *obj, ResetType type)
{
    MstarCmdqState *s = MSTAR_CMDQ(obj);

    memset(s->store, 0, s->size);
}

static void mstar_cmdq_realize(DeviceState *dev, Error **errp)
{
    MstarCmdqState *s = MSTAR_CMDQ(dev);

    if (s->size == 0) {
        s->size = MSTAR_CMDQ_SIZE;
    }
    s->store = g_malloc0(s->size);
    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_cmdq_ops, s,
                          "mstar.cmdq", s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mstar_cmdq = {
    .name = "mstar-cmdq",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VBUFFER_UINT32(store, MstarCmdqState, 0, NULL, size),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mstar_cmdq_props[] = {
    DEFINE_PROP_UINT32("size", MstarCmdqState, size, MSTAR_CMDQ_SIZE),
};

static void mstar_cmdq_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_cmdq_realize;
    dc->vmsd = &vmstate_mstar_cmdq;
    rc->phases.hold = mstar_cmdq_reset_hold;
    device_class_set_props(dc, mstar_cmdq_props);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo mstar_cmdq_types[] = {
    {
        .name           = TYPE_MSTAR_CMDQ,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MstarCmdqState),
        .class_init     = mstar_cmdq_class_init,
    },
};

DEFINE_TYPES(mstar_cmdq_types)
