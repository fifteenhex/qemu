/*
 *
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/system.h"

#include "hw/timer/dragonball_timer.h"

#define OSCFREQ 32768
#define NSPERTICK (NANOSECONDS_PER_SECOND / 32768)

static uint64_t dragonball_timer_read(void *opaque, hwaddr addr, unsigned int size)
{
    DragonBallTimerState *t = opaque;
    uint16_t ticks = (((qemu_clock_get_ns(rtc_clock) - t->start_offset) / NSPERTICK) / (t->tprer + 1)) & 0xffff;

    switch(addr) {
    case DRAGONBALL_TIMER_TCTL:
        return t->tctl;
    case DRAGONBALL_TIMER_TPRER:
        return t->tprer;
    case DRAGONBALL_TIMER_TCN:
        return ticks;
    case DRAGONBALL_TIMER_TSTAT:
        return t->tstat;
    }

    return 0;
}

static void dragonball_timer_cb(void *opaque)
{
    DragonBallTimerState *t = opaque;

    //printf("%s:%d\n", __func__, __LINE__);

    t->tstat |= DRAGONBALL_TIMER_TSTAT_COMP;

    if(t->tctl & DRAGONBALL_TIMER_TCTL_IRQEN)
        qemu_set_irq(t->irq, 1);
}


static bool dragonball_timer_running(DragonBallTimerState *t)
{
    if (t->tctl & DRAGONBALL_TIMER_TCTL_TEN) {
        /* If the clock source is 0 the clock is stopped at the current value */
        if ((t->tctl >> DRAGONBALL_TIMER_TCTL_CLKSOURCE_SHIFT) & DRAGONBALL_TIMER_TCTL_CLKSOURCE_MASK)
            return true;
    }

    return false;
}

static void dragonball_timer_update(DragonBallTimerState *t)
{
    /* Stop the IRQ if IRQEN has been cleared */
    if (!(t->tctl & DRAGONBALL_TIMER_TCTL_IRQEN))
        qemu_set_irq(t->irq, 0);

    ptimer_transaction_begin(t->timer);
    /* Configure the frequency and compare value */
    ptimer_set_freq(t->timer, OSCFREQ / (t->tprer + 1));
    ptimer_set_limit(t->timer, t->tcmp, 1);

    /* Start or stop the timer */
    if (dragonball_timer_running(t) && t->tcmp)
        ptimer_run(t->timer, 0);
    else
        ptimer_stop(t->timer);

    ptimer_transaction_commit(t->timer);
}

static void dragonball_timer_write(void *opaque, hwaddr addr,
                                   uint64_t value, unsigned int size)
{
    DragonBallTimerState *t = opaque;

    //printf("%s:%d %04x:%04x\n", __func__, __LINE__,
    //       (unsigned int) addr, (unsigned int) value);

    switch(addr){
    case DRAGONBALL_TIMER_TCTL:
        t->tctl = value;
	dragonball_timer_update(t);
        break;
    case DRAGONBALL_TIMER_TPRER:
        t->tprer = value;
	dragonball_timer_update(t);
        break;
    case DRAGONBALL_TIMER_TCMP:
        t->tcmp = value;
	dragonball_timer_update(t);
        break;
    case DRAGONBALL_TIMER_TSTAT:
    	t->tstat = value;
    	qemu_set_irq(t->irq, 0);
    	break;
    }
}

static const MemoryRegionOps timer_ops = {
    .read = dragonball_timer_read,
    .write = dragonball_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_timer_realize(DeviceState *dev, Error **errp)
{
    DragonBallTimerState *t = DRAGONBALL_TIMER(dev);

    memory_region_init_io(&t->iomem, OBJECT(t), &timer_ops, t, "dragonball.timer",0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &t->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &t->irq);
}

static void dragonball_timer_init(Object *obj)
{
    DragonBallTimerState *t = DRAGONBALL_TIMER(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &t->irq);
    t->timer = ptimer_init(dragonball_timer_cb, t, 0);
}

static void dragonball_timer_finalize(Object *obj)
{
    DragonBallTimerState *t = DRAGONBALL_TIMER(obj);

    ptimer_free(t->timer);
}

static void dragonball_timer_reset(DeviceState *dev)
{
    DragonBallTimerState *t = DRAGONBALL_TIMER(dev);

    t->start_offset = qemu_clock_get_ns(rtc_clock);
    t->tprer = 0;
    t->tcmp = ~0;
}

//static const Property dragonball_timer_properties[] = {
//    DEFINE_PROP_END_OF_LIST(),
//};

static void dragonball_timer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = dragonball_timer_realize;
    dc->legacy_reset = dragonball_timer_reset;
//    device_class_set_props(dc, dragonball_timer_properties);
}

static const TypeInfo dragonball_timer_info = {
    .name          = TYPE_DRAGONBALL_TIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DragonBallTimerState),
    .instance_init = dragonball_timer_init,
    .instance_finalize = dragonball_timer_finalize,
    .class_init    = dragonball_timer_class_init,
};

static void dragonball_timer_register_types(void)
{
    type_register_static(&dragonball_timer_info);
}

type_init(dragonball_timer_register_types)
