/*
 * See: https://www.analog.com/media/jp/technical-documentation/data-sheets/2686.pdf
 */

#include "qemu/osdep.h"
#include "hw/rtc/ds1305.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/qdev-properties.h"
#include "system/system.h"
#include "system/rtc.h"
#include "qemu/bcd.h"

static uint32_t ds1305_read(DS1305State *s)
{
    switch(s->address) {
    case DS1305_REG_SECONDS:
        return to_bcd(s->now.tm_sec);
    case DS1305_REG_MINUTES:
        return to_bcd(s->now.tm_min);
    case DS1305_REG_HOURS:
        return to_bcd(s->now.tm_hour);
    case DS1305_REG_DAY:
        return to_bcd(s->now.tm_wday + 1);
    case DS1305_REG_DATE:
        return to_bcd(s->now.tm_mday);
    case DS1305_REG_MONTH:
        return to_bcd(s->now.tm_mon + 1);
    case DS1305_REG_YEAR:
        return to_bcd(s->now.tm_year - 100);
    }

    return 0;
}

static void ds1305_write(DS1305State *s, uint8_t value)
{

}

static uint32_t ds1305_transfer(SSIPeripheral *dev, uint32_t value)
{
    DS1305State *s = DS1305(dev);
    uint32_t out = 0;

    printf("%d %d - in 0x%02x ", s->counter, s->address, value);

    if (s->counter == 0) {
        if (value & 0x80)
            s->write = true;
        s->address = value & 0x7f;
        goto inccounter;
    }

    if (s->write)
        ds1305_write(s, value);
    else
        out = ds1305_read(s);
    s->address++;

inccounter:
    printf("out 0x%02x\n", out);
    s->counter++;
    return out;
}

static const VMStateDescription vmstate_ds1305 = {
    .name = "ds1305",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, DS1305State),
        VMSTATE_END_OF_LIST()
    }
};

static void ds1305_realize(SSIPeripheral *dev, Error **errp)
{
//    DeviceState *dev = DEVICE(d);
//    DS1305State *s = MAX_111X(dev);
}

static void ds1305_reset(DeviceState *dev)
{
    //DS1305State *s = DS1305(dev);

    //s->counter = 0;
}

static int ds1305_set_cs(SSIPeripheral *dev, bool select)
{
    DS1305State *s = DS1305(dev);

    if (select) {
	qemu_get_timedate(&s->now, 0);
        s->counter = 0;
        s->write = false;
        s->address = 0;
    }
    return 0;
}

static void ds1305_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->transfer = ds1305_transfer;
    k->realize = ds1305_realize;
    k->cs_polarity = SSI_CS_HIGH;
    k->set_cs = ds1305_set_cs;
    dc->legacy_reset = ds1305_reset;
    dc->vmsd = &vmstate_ds1305;
}

static const TypeInfo ds1305_info = {
    .name          = TYPE_DS1305,
    .parent        = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(DS1305State),
    .class_init    = ds1305_class_init,
};

static void ds1305_register_types(void)
{
    type_register_static(&ds1305_info);
}

type_init(ds1305_register_types)
