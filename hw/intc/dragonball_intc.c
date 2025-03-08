/*
 *
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/intc/dragonball_intc.h"
#include "migration/vmstate.h"
#include "hw/irq.h"

 /* A bit is set when the irq is active and not masked */
#define ISRVAL(_s) ((~s->imr) & s->ipr)

static const uint8_t dragonball_irq_levels[32] = {
    [DRAGONBALL_INTC_SPI] = 4,
    [DRAGONBALL_INTC_TMR] = 6,
    [DRAGONBALL_INTC_UART] = 4,
};

static void dragonball_intc_updateirqs(DragonBallINTCState *s)
{
    M68kCPU *cpu = M68K_CPU(s->cpu);
    uint32_t isr = ISRVAL(s);
    int i;
    int newlevelstates[8] = { 0 };

    /*
     * For each of the irqs figure out the state
     * and trigger that level if needed.
     */
    for (i = 0; i < 32; i++) {
        uint8_t irqlevel = dragonball_irq_levels[i];
        int thislevel = (isr >> i) & 1;

        /* Skip anything with the level 0 */
        if (!dragonball_irq_levels[i])
            continue;

        newlevelstates[irqlevel] |= thislevel;
    }

    /* Now set the new state of each of the levels from 1 to 7 */
    for (i = 1; i < ARRAY_SIZE(newlevelstates); i++) {
        uint8_t vector = s->ivr + i;
        int level = newlevelstates[i];

        /* Reduce noise by only doing anything if a change has happened */
        if (level == s->levelstates[i])
            continue;

        //if (vector != 70)
        //    printf("%d:%d\n", vector, level);
        m68k_set_irq_level(cpu, level, vector);
    }

    memcpy(s->levelstates, newlevelstates, sizeof(s->levelstates));
}

static uint64_t dragonball_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    DragonBallINTCState *s = opaque;

    //printf("%s:%d %04x\n", __func__, __LINE__, (unsigned int) addr);

    switch(addr){
    case DRAGONBALL_INTC_IMR:
        return s->imr;
    case DRAGONBALL_INTC_ISR:
        return ISRVAL(_s);
    case DRAGONBALL_INTC_IPR:
        return s->ipr;
    }

    return 0;
}

static void dragonball_intc_write(void *opaque, hwaddr addr, uint64_t value,
        unsigned size)
{
    DragonBallINTCState *s = opaque;

    //printf("%s:%d %04x:%08x\n", __func__, __LINE__,
    //           (unsigned int) addr, (unsigned int) value);

    switch(addr){
    case DRAGONBALL_INTC_IVR:
        s->ivr = value;
        break;
    case DRAGONBALL_INTC_IMR:
        s->imr = value;
        dragonball_intc_updateirqs(s);
        break;
    }
}

static const MemoryRegionOps dragonball_intc_ops = {
    .read = dragonball_intc_read,
    .write = dragonball_intc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_intc_reset(DeviceState *dev)
{
    DragonBallINTCState *s = DRAGONBALL_INTC(dev);

    s->imr = 0x00ffffff;
    memset(s->levelstates, 0, sizeof(s->levelstates));
}

//static void dragonball_intc_irq_request(void *opaque, int irq, int level)
//{
//    DragonBallINTCState *s = opaque;
//
//    dragonball_intc_set_pending(s, irq, level > 0);
//    dragonball_intc_update(s);
//}

static void dragonball_intc_set_irq(void *opaque, int irq, int level)
{
    DragonBallINTCState *s = opaque;
    uint32_t mask = (1 << irq);

    /* IPR doesn't care about the mask */
    if (level)
        s->ipr |= mask;
    else
        s->ipr &= ~mask;

    dragonball_intc_updateirqs(s);
}

static void dragonball_intc_realize(DeviceState *dev, Error **errp)
{
    DragonBallINTCState *s = DRAGONBALL_INTC(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &dragonball_intc_ops, s,
                          TYPE_DRAGONBALL_INTC, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    qdev_init_gpio_in_named(dev, dragonball_intc_set_irq, "peripheral_interrupts", 31);
}

static const VMStateDescription vmstate_dragonball_intc = {
    .name = "dragonball_intc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
            VMSTATE_END_OF_LIST()
        }
};

static const Property dragonball_intc_properties[] = {
    DEFINE_PROP_LINK("m68k-cpu", DragonBallINTCState, cpu,
                     TYPE_M68K_CPU, ArchCPU *),
};

static void dragonball_intc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->legacy_reset = dragonball_intc_reset;
    device_class_set_props(dc, dragonball_intc_properties);
    dc->realize = dragonball_intc_realize;
    dc->vmsd = &vmstate_dragonball_intc;
}

static const TypeInfo dragonball_intc_info = {
    .name          = TYPE_DRAGONBALL_INTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DragonBallINTCState),
    .class_init    = dragonball_intc_class_init,
};

static void dragonball_intc_register_types(void)
{
    type_register_static(&dragonball_intc_info);
}

type_init(dragonball_intc_register_types)
