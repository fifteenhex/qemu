/*
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/rtc/dragonball_rtc.h"
#include "migration/vmstate.h"
#include "hw/irq.h"

static uint64_t dragonball_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    //DragonBallRTCState *plic = opaque;

    return 0;
}

static void dragonball_rtc_write(void *opaque, hwaddr addr, uint64_t value,
        unsigned size)
{
    //DragonBallRTCState *plic = opaque;
}

static const MemoryRegionOps dragonball_rtc_ops = {
    .read = dragonball_rtc_read,
    .write = dragonball_rtc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_rtc_reset(DeviceState *dev)
{
    //DragonBallRTCState *s = DRAGONBALL_RTC(dev);
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
}

static const VMStateDescription vmstate_dragonball_pll = {
    .name = "dragonball_pll",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
            VMSTATE_END_OF_LIST()
        }
};

static Property dragonball_rtc_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void dragonball_rtc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->legacy_reset = dragonball_rtc_reset;
    device_class_set_props(dc, dragonball_rtc_properties);
    dc->realize = dragonball_rtc_realize;
    dc->vmsd = &vmstate_dragonball_pll;
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
