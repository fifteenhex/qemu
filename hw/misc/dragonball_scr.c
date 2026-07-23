/*
 * DragonBall system control / ID register block.
 *
 * Only the identity registers matter to guests so far: PalmOS reads
 * the chip ID to pick its hardware path (the EZ boot code falls back
 * to a default when it reads 0, the VZ HAL expects 0x56).  SCR/PCR
 * are plain storage.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/dragonball_scr.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static uint64_t dragonball_scr_read(void *opaque, hwaddr addr, unsigned size)
{
    DragonBallSCRState *s = opaque;

    switch (addr) {
    case DRAGONBALL_SCR_SCR:
        return s->scr;
    case DRAGONBALL_SCR_PCR:
        return s->pcr;
    case DRAGONBALL_SCR_CHIPID:
        return s->chip_id;
    case DRAGONBALL_SCR_MASKID:
        return s->mask_id;
    case DRAGONBALL_SCR_SWID:
        return s->sw_id >> 8;
    case DRAGONBALL_SCR_SWID + 1:
        return s->sw_id & 0xff;
    default:
        return 0;
    }
}

static void dragonball_scr_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    DragonBallSCRState *s = opaque;

    switch (addr) {
    case DRAGONBALL_SCR_SCR:
        s->scr = value;
        break;
    case DRAGONBALL_SCR_PCR:
        s->pcr = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps dragonball_scr_ops = {
    .read = dragonball_scr_read,
    .write = dragonball_scr_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_scr_reset(DeviceState *dev)
{
    DragonBallSCRState *s = DRAGONBALL_SCR(dev);

    s->scr = 0x1c;
    s->pcr = 0;
}

static void dragonball_scr_realize(DeviceState *dev, Error **errp)
{
    DragonBallSCRState *s = DRAGONBALL_SCR(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &dragonball_scr_ops, s,
                          TYPE_DRAGONBALL_SCR, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static const VMStateDescription vmstate_dragonball_scr = {
    .name = "dragonball_scr",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(scr, DragonBallSCRState),
        VMSTATE_UINT8(pcr, DragonBallSCRState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property dragonball_scr_properties[] = {
    DEFINE_PROP_UINT8("chip-id", DragonBallSCRState, chip_id, 0),
    DEFINE_PROP_UINT8("mask-id", DragonBallSCRState, mask_id, 0),
    DEFINE_PROP_UINT16("sw-id", DragonBallSCRState, sw_id, 0),
};

static void dragonball_scr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, dragonball_scr_properties);
    device_class_set_legacy_reset(dc, dragonball_scr_reset);
    dc->realize = dragonball_scr_realize;
    dc->vmsd = &vmstate_dragonball_scr;
}

static const TypeInfo dragonball_scr_info = {
    .name          = TYPE_DRAGONBALL_SCR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DragonBallSCRState),
    .class_init    = dragonball_scr_class_init,
};

static void dragonball_scr_register_types(void)
{
    type_register_static(&dragonball_scr_info);
}

type_init(dragonball_scr_register_types)
