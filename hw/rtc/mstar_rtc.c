/*
 * MStar/SigmaStar msc313 RTC
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

/* ----------------------------------------------------------- msc313-rtc */

#define RTC_CTRL            0x00
#define RTC_CTRL_CNT_EN     (1 << 1)
#define RTC_CTRL_LOAD_EN    (1 << 3)
#define RTC_CTRL_READ_EN    (1 << 4)
#define RTC_CTRL_INT_MASK   (1 << 5)
#define RTC_CTRL_INT_CLEAR  (1 << 7)
#define RTC_FREQ_CW_L       0x04
#define RTC_FREQ_CW_H       0x08
#define RTC_LOAD_VAL_L      0x0c
#define RTC_LOAD_VAL_H      0x10
#define RTC_MATCH_VAL_L     0x14
#define RTC_MATCH_VAL_H     0x18
#define RTC_STATUS_INT      0x1c
#define RTC_STATUS_ALM_INT  (1 << 1)
#define RTC_CNT_VAL_L       0x20
#define RTC_CNT_VAL_H       0x24

static uint32_t msc313_rtc_count(Msc313RtcState *s)
{
    uint32_t v = s->base_count;

    if (s->ctrl & RTC_CTRL_CNT_EN) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        v += (now - s->base_ns) / NANOSECONDS_PER_SECOND;
    }
    return v;
}

static void msc313_rtc_update_irq(Msc313RtcState *s)
{
    bool alarm = (s->status & RTC_STATUS_ALM_INT) &&
                 !(s->ctrl & RTC_CTRL_INT_MASK);

    qemu_set_irq(s->irq, alarm);
}

static uint64_t msc313_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313RtcState *s = opaque;

    switch (addr) {
    case RTC_CTRL:
        return s->ctrl;
    case RTC_FREQ_CW_L:
        return s->freq_cw & 0xffff;
    case RTC_FREQ_CW_H:
        return s->freq_cw >> 16;
    case RTC_LOAD_VAL_L:
        return s->load_val & 0xffff;
    case RTC_LOAD_VAL_H:
        return s->load_val >> 16;
    case RTC_MATCH_VAL_L:
        return s->match_val & 0xffff;
    case RTC_MATCH_VAL_H:
        return s->match_val >> 16;
    case RTC_STATUS_INT:
        return s->status;
    case RTC_CNT_VAL_L:
        return s->cnt_latch & 0xffff;
    case RTC_CNT_VAL_H:
        return s->cnt_latch >> 16;
    }
    return 0;
}

static void msc313_rtc_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Msc313RtcState *s = opaque;

    switch (addr) {
    case RTC_CTRL:
        /*
         * LOAD_EN and READ_EN are "trigger" bits: the guest sets one and
         * polls the register until the hardware clears it. Perform the
         * transfer here and report the bit as already cleared.
         */
        if (val & RTC_CTRL_LOAD_EN) {
            s->base_count = s->load_val;
            s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        if (val & RTC_CTRL_READ_EN) {
            s->cnt_latch = msc313_rtc_count(s);
        }
        if (val & RTC_CTRL_INT_CLEAR) {
            s->status &= ~RTC_STATUS_ALM_INT;
        }
        s->ctrl = val & ~(RTC_CTRL_LOAD_EN | RTC_CTRL_READ_EN |
                          RTC_CTRL_INT_CLEAR);
        msc313_rtc_update_irq(s);
        break;
    case RTC_FREQ_CW_L:
        s->freq_cw = (s->freq_cw & 0xffff0000) | (val & 0xffff);
        break;
    case RTC_FREQ_CW_H:
        s->freq_cw = (s->freq_cw & 0xffff) | ((val & 0xffff) << 16);
        break;
    case RTC_LOAD_VAL_L:
        s->load_val = (s->load_val & 0xffff0000) | (val & 0xffff);
        break;
    case RTC_LOAD_VAL_H:
        s->load_val = (s->load_val & 0xffff) | ((val & 0xffff) << 16);
        break;
    case RTC_MATCH_VAL_L:
        s->match_val = (s->match_val & 0xffff0000) | (val & 0xffff);
        break;
    case RTC_MATCH_VAL_H:
        s->match_val = (s->match_val & 0xffff) | ((val & 0xffff) << 16);
        break;
    }
}

static const MemoryRegionOps msc313_rtc_ops = {
    .read = msc313_rtc_read,
    .write = msc313_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void msc313_rtc_reset_hold(Object *obj, ResetType type)
{
    Msc313RtcState *s = MSC313_RTC(obj);

    s->ctrl = 0;
    s->status = 0;
    s->freq_cw = 0;
    s->load_val = 0;
    s->match_val = 0;
    s->cnt_latch = 0;
    s->base_count = 0;
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    qemu_set_irq(s->irq, 0);
}

static void msc313_rtc_realize(DeviceState *dev, Error **errp)
{
    Msc313RtcState *s = MSC313_RTC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313_rtc_ops, s,
                          "mstar.msc313-rtc", MSTAR_RTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void msc313_rtc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313_rtc_realize;
    rc->phases.hold = msc313_rtc_reset_hold;
}

static const TypeInfo mstar_rtc_types[] = {
    {
        .name           = TYPE_MSC313_RTC,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313RtcState),
        .class_init     = msc313_rtc_class_init,
    },
};

DEFINE_TYPES(mstar_rtc_types)
