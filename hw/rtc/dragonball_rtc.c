/*
 * DragonBall on-chip RTC.
 *
 * Time comes from the rtc clock: the counter is the clock's seconds
 * plus a guest-set offset, so setting it never disturbs the host.
 * The EZ counts hours/minutes/seconds and wraps daily; the VZ adds
 * the day counter and day alarm.  A 1Hz tick derives the second/
 * minute/hour/day/alarm interrupts; the sample-rate interrupts
 * (2..512Hz) and the watchdog are not modelled.
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
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/rtc/dragonball_rtc.h"
#include "migration/vmstate.h"
#include "system/rtc.h"
#include "system/system.h"

#define SECSPERDAY 86400

static int64_t dragonball_rtc_seconds(DragonBallRTCState *s)
{
    return qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND + s->offset;
}

/* hours in 28:24, minutes in 21:16, seconds in 5:0 */
static uint32_t dragonball_rtc_hms(int64_t daysecs)
{
    return ((daysecs / 3600) << 24) |
           (((daysecs / 60) % 60) << 16) |
           (daysecs % 60);
}

static void dragonball_rtc_update_irq(DragonBallRTCState *s)
{
    qemu_set_irq(s->rtc_irq, (s->isr & s->ienr) != 0);
}

static void dragonball_rtc_tick(void *opaque)
{
    DragonBallRTCState *s = opaque;
    int64_t now = dragonball_rtc_seconds(s);
    uint16_t set = DRAGONBALL_RTC_INT_1HZ;

    if (now / 60 != s->last_seconds / 60)
        set |= DRAGONBALL_RTC_INT_MIN;
    if (now / 3600 != s->last_seconds / 3600)
        set |= DRAGONBALL_RTC_INT_HR;
    if (now / SECSPERDAY != s->last_seconds / SECSPERDAY)
        set |= DRAGONBALL_RTC_INT_DAY;

    /* the alarm matches on time-of-day (and, on the VZ, the day) */
    if (dragonball_rtc_hms(now % SECSPERDAY) == s->alarm &&
        (!s->dayalarm || (now / SECSPERDAY) % 512 == s->dayalarm))
        set |= DRAGONBALL_RTC_INT_ALM;

    s->last_seconds = now;
    s->isr |= set;
    dragonball_rtc_update_irq(s);
}

static uint64_t dragonball_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    DragonBallRTCState *s = opaque;
    int64_t now = dragonball_rtc_seconds(s);

    switch (addr) {
    case DRAGONBALL_RTC_RTCTIME:
        return dragonball_rtc_hms(now % SECSPERDAY);
    case DRAGONBALL_RTC_RTCALRM:
        return s->alarm;
    case DRAGONBALL_RTC_WATCHDOG:
        return s->watchdog;
    case DRAGONBALL_RTC_RTCCTL:
        return s->ctl;
    case DRAGONBALL_RTC_RTCISR:
        return s->isr;
    case DRAGONBALL_RTC_RTCIENR:
        return s->ienr;
    case DRAGONBALL_RTC_STPWCH:
        return s->stpwch;
    case DRAGONBALL_RTC_DAYR:
        return (now / SECSPERDAY) % 512;
    case DRAGONBALL_RTC_DAYALARM:
        return s->dayalarm;
    default:
        return 0;
    }
}

static void dragonball_rtc_write(void *opaque, hwaddr addr, uint64_t value,
        unsigned size)
{
    DragonBallRTCState *s = opaque;
    int64_t now = dragonball_rtc_seconds(s);
    int64_t newsecs;

    switch (addr) {
    case DRAGONBALL_RTC_RTCTIME:
        newsecs = ((value >> 24) & 0x1f) * 3600 +
                  ((value >> 16) & 0x3f) * 60 +
                  (value & 0x3f);
        s->offset += newsecs - now % SECSPERDAY;
        s->last_seconds = dragonball_rtc_seconds(s);
        break;
    case DRAGONBALL_RTC_RTCALRM:
        s->alarm = value;
        break;
    case DRAGONBALL_RTC_WATCHDOG:
        s->watchdog = value;
        break;
    case DRAGONBALL_RTC_RTCCTL:
        s->ctl = value;
        break;
    case DRAGONBALL_RTC_RTCISR:
        /* write ones to clear */
        s->isr &= ~value;
        dragonball_rtc_update_irq(s);
        break;
    case DRAGONBALL_RTC_RTCIENR:
        s->ienr = value;
        dragonball_rtc_update_irq(s);
        break;
    case DRAGONBALL_RTC_STPWCH:
        s->stpwch = value;
        break;
    case DRAGONBALL_RTC_DAYR:
        s->offset += ((int64_t)(value & 0x1ff) - (now / SECSPERDAY) % 512) *
                     SECSPERDAY;
        s->last_seconds = dragonball_rtc_seconds(s);
        break;
    case DRAGONBALL_RTC_DAYALARM:
        s->dayalarm = value & 0x1ff;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps dragonball_rtc_ops = {
    .read = dragonball_rtc_read,
    .write = dragonball_rtc_write,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_rtc_reset(DeviceState *dev)
{
    DragonBallRTCState *s = DRAGONBALL_RTC(dev);
    struct tm tm;

    /* start from the configured guest RTC time */
    qemu_get_timedate(&tm, 0);
    s->offset = 0;
    s->offset = (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec) -
                dragonball_rtc_seconds(s) % SECSPERDAY;

    s->alarm = 0;
    s->dayalarm = 0;
    s->ctl = DRAGONBALL_RTC_CTL_EN;
    s->isr = 0;
    s->ienr = 0;
    s->stpwch = 0x3f;
    s->watchdog = 0;
    s->last_seconds = dragonball_rtc_seconds(s);

    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, 1);
    ptimer_set_limit(s->timer, 1, 1);
    ptimer_run(s->timer, 0);
    ptimer_transaction_commit(s->timer);
}

static void dragonball_rtc_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DragonBallRTCState *s = DRAGONBALL_RTC(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &dragonball_rtc_ops, s,
                          TYPE_DRAGONBALL_RTC, 0x100);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->wdt_irq);
    sysbus_init_irq(sbd, &s->rtc_irq);

    s->timer = ptimer_init(dragonball_rtc_tick, s, 0);
}

static const VMStateDescription vmstate_dragonball_rtc = {
    .name = "dragonball_rtc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(offset, DragonBallRTCState),
        VMSTATE_UINT32(alarm, DragonBallRTCState),
        VMSTATE_UINT16(dayalarm, DragonBallRTCState),
        VMSTATE_UINT16(ctl, DragonBallRTCState),
        VMSTATE_UINT16(isr, DragonBallRTCState),
        VMSTATE_UINT16(ienr, DragonBallRTCState),
        VMSTATE_UINT16(stpwch, DragonBallRTCState),
        VMSTATE_UINT16(watchdog, DragonBallRTCState),
        VMSTATE_INT64(last_seconds, DragonBallRTCState),
        VMSTATE_PTIMER(timer, DragonBallRTCState),
        VMSTATE_END_OF_LIST()
    }
};

static void dragonball_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, dragonball_rtc_reset);
    dc->realize = dragonball_rtc_realize;
    dc->vmsd = &vmstate_dragonball_rtc;
}

static const TypeInfo dragonball_rtc_info = {
    .name          = TYPE_DRAGONBALL_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DragonBallRTCState),
    .class_init    = dragonball_rtc_class_init,
};

static void dragonball_rtc_register_types(void)
{
    type_register_static(&dragonball_rtc_info);
}

type_init(dragonball_rtc_register_types)
