/*
 * MStar/SigmaStar SAR ADC
 *
 * A small successive-approximation ADC; boards hang keypads and the
 * like off its input channels. Register map from the mainline
 * msc313e_sar driver: select a channel and start a conversion in
 * CTRL, then read the per-channel result register. The vendor
 * u-boot instead pulses the LOAD bit and polls it clearing as its
 * done flag; both work here, and conversions are instantaneous.
 *
 * There is no analog world to sample, so each channel returns a
 * fixed value from its "channelN" property. The default is mid
 * scale, which reads as "nothing pulling the line anywhere" - in
 * particular the vendor u-boot's ADC keypad scan takes it as no key
 * held.
 *
 * The done interrupt is tracked in the status registers but not
 * wired to anything yet.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/adc/mstar_sar.h"

/* Register byte offsets (16-bit registers on a 4 byte stride) */
#define SAR_CTRL            0x00
#define SAR_CTRL_CHANNEL    0x0007
#define SAR_CTRL_START      (1 << 7)
#define SAR_CTRL_LOAD       (1 << 14)   /* self clears when converted */
#define SAR_GPIO_CTRL       0x44
#define SAR_GPIO_DATA       0x48
#define SAR_INT_MASK        0x50
#define SAR_INT_CLR         0x54
#define SAR_INT_FORCE       0x58
#define SAR_INT_STATUS      0x5c
#define SAR_INT_DONE        (1 << 0)
#define SAR_CH_RESULT(ch)   (0x100 + (ch) * 4)

/* 10-bit converter; mid scale means nothing is pulling the pin */
#define SAR_SAMPLE_IDLE     512

static void mstar_sar_update_irq(MStarSarState *s)
{
    uint16_t active = s->regs[SAR_INT_STATUS / 4] & ~s->regs[SAR_INT_MASK / 4];

    qemu_set_irq(s->irq, active != 0);
}

static uint64_t mstar_sar_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarSarState *s = MSTAR_SAR(opaque);

    return s->regs[addr / 4];
}

static void mstar_sar_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MStarSarState *s = MSTAR_SAR(opaque);
    unsigned int idx = addr / 4;
    uint16_t old = s->regs[idx];

    switch (addr) {
    case SAR_CTRL:
        /* Conversions are instantaneous, so LOAD reads back clear */
        s->regs[idx] = val & ~SAR_CTRL_LOAD;
        if (((val & SAR_CTRL_START) && !(old & SAR_CTRL_START)) ||
            (val & SAR_CTRL_LOAD)) {
            unsigned int ch = val & SAR_CTRL_CHANNEL;

            s->regs[SAR_CH_RESULT(ch) / 4] = s->chan_input[ch];
            s->regs[SAR_INT_STATUS / 4] |= SAR_INT_DONE;
            mstar_sar_update_irq(s);
        }
        break;
    case SAR_INT_CLR:
        s->regs[SAR_INT_STATUS / 4] &= ~(uint16_t)val;
        mstar_sar_update_irq(s);
        break;
    case SAR_INT_FORCE:
        s->regs[idx] = val;
        s->regs[SAR_INT_STATUS / 4] |= val;
        mstar_sar_update_irq(s);
        break;
    default:
        s->regs[idx] = val;
        break;
    }
}

static const MemoryRegionOps mstar_sar_ops = {
    .read = mstar_sar_read,
    .write = mstar_sar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_sar_reset(DeviceState *dev)
{
    MStarSarState *s = MSTAR_SAR(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mstar_sar_init(Object *obj)
{
    MStarSarState *s = MSTAR_SAR(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_sar_ops, s, TYPE_MSTAR_SAR,
                          MSTAR_SAR_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property mstar_sar_properties[] = {
    DEFINE_PROP_UINT16("channel0", MStarSarState, chan_input[0],
                       SAR_SAMPLE_IDLE),
    DEFINE_PROP_UINT16("channel1", MStarSarState, chan_input[1],
                       SAR_SAMPLE_IDLE),
    DEFINE_PROP_UINT16("channel2", MStarSarState, chan_input[2],
                       SAR_SAMPLE_IDLE),
    DEFINE_PROP_UINT16("channel3", MStarSarState, chan_input[3],
                       SAR_SAMPLE_IDLE),
    DEFINE_PROP_UINT16("channel4", MStarSarState, chan_input[4],
                       SAR_SAMPLE_IDLE),
    DEFINE_PROP_UINT16("channel5", MStarSarState, chan_input[5],
                       SAR_SAMPLE_IDLE),
    DEFINE_PROP_UINT16("channel6", MStarSarState, chan_input[6],
                       SAR_SAMPLE_IDLE),
    DEFINE_PROP_UINT16("channel7", MStarSarState, chan_input[7],
                       SAR_SAMPLE_IDLE),
};

static void mstar_sar_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_sar_reset);
    device_class_set_props(dc, mstar_sar_properties);
}

static const TypeInfo mstar_sar_types[] = {
    {
        .name           = TYPE_MSTAR_SAR,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarSarState),
        .instance_init  = mstar_sar_init,
        .class_init     = mstar_sar_class_init,
    },
};

DEFINE_TYPES(mstar_sar_types)
