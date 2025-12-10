/*
 *
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/dragonball_gpio.h"
#include "migration/vmstate.h"
#include "trace.h"

static uint64_t dragonball_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    DragonBallGPIOState *s = DRAGONBALL_GPIO(opaque);
    unsigned int port = DRAGONBALL_GPIO_ADDR2PORT(addr);
    unsigned int reg = DRAGONBALL_GPIO_ADDR2REG(addr);
    uint16_t val;

    //printf("%s:%d %x %u %u\n", __func__, __LINE__, (unsigned) addr, port, size);

    switch (reg) {
    case DRAGONBALL_GPIO_REG_DIR:
        val = s->ports[port].data;
        val |= ((uint16_t)s->ports[port].dir) << 8;
	//printf("0x%04x\n", (unsigned) val);
        return val;
    case DRAGONBALL_GPIO_REG_DATA:
        return s->ports[port].data;
    case DRAGONBALL_GPIO_REG_PUDEN:
        val = s->ports[port].sel;
        val |= ((uint16_t)s->ports[port].puden) << 8;
	//printf("0x%04x\n", (unsigned) val);
        return val;
    case DRAGONBALL_GPIO_REG_SEL:
        return s->ports[port].sel;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
    }

    return 0;
}

static void dragonball_gpio_update_outputs(DragonBallGPIOState *s, unsigned int p)
{
    int i;

    for (i = 0; i < DRAGONBALL_GPIO_NGPIOPERPORT; i++) {
        uint8_t mask = 1 << i;
        unsigned int pin = p * DRAGONBALL_GPIO_NGPIOPERPORT + i;
        int level = (mask & s->ports[p].data) ? 1 : 0;

        /* Is this pin an output? */
        if (s->ports[p].dir & mask) {
            qemu_set_irq(s->output[pin], level);
            //printf("%s:%d %u %02x %02x %d\n", __func__, __LINE__,
            //       pin, s->ports[p].dir, s->ports[p].data, level);
        }
    }
}

static void dragonball_gpio_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned int size)
{
    DragonBallGPIOState *s = DRAGONBALL_GPIO(opaque);
    unsigned int port = DRAGONBALL_GPIO_ADDR2PORT(addr);
    unsigned int reg = DRAGONBALL_GPIO_ADDR2REG(addr);

    //printf("%s:%d 0x%04x %u %u %04x\n", __func__, __LINE__,
    //        (unsigned) addr, port, size, (unsigned) value);

    switch (reg) {
    case DRAGONBALL_GPIO_REG_DIR:
        s->ports[port].dir = (value >> 8) & 0xff;
        s->ports[port].data = value & 0xff;
        dragonball_gpio_update_outputs(s, port);
        break;
    case DRAGONBALL_GPIO_REG_DATA:
        s->ports[port].data = value & 0xff;
        dragonball_gpio_update_outputs(s, port);
        break;
    case DRAGONBALL_GPIO_REG_PUDEN:
        s->ports[port].puden = (value >> 8) & 0xff;
        s->ports[port].sel = value & 0xff;
        break;
    case DRAGONBALL_GPIO_REG_SEL:
        s->ports[port].sel = value & 0xff;
        break;
    default:
    printf("%s:%d %x %u\n", __func__, __LINE__, (unsigned) addr, port);
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
    }

    if (port == DRAGONBALL_GPIO_PORTD)
        /* Bottom four bits of sel in port D are hardwire to zero */
        s->ports[port].sel &= ~0xf;
}

static const MemoryRegionOps gpio_ops = {
    .read =  dragonball_gpio_read,
    .write = dragonball_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_gpio_set(void *opaque, int line, int value)
{
    //DragonBallGPIOState *s = DRAGONBALL_GPIO(opaque);
}

static void dragonball_gpio_reset(DeviceState *dev)
{
    DragonBallGPIOState *s = DRAGONBALL_GPIO(dev);

    memset(s->ports, 0, sizeof(s->ports));

    s->ports[0].puden = DRAGONBALL_GPIO_PAPUEN_RESET;
    s->ports[1].puden = DRAGONBALL_GPIO_PBPUEN_RESET;
    s->ports[1].sel   = DRAGONBALL_GPIO_PBSEL_RESET;
    s->ports[2].puden = DRAGONBALL_GPIO_PCPDEN_RESET;
    s->ports[2].sel   = DRAGONBALL_GPIO_PCSEL_RESET;
    s->ports[DRAGONBALL_GPIO_PORTD].puden = DRAGONBALL_GPIO_PDPUEN_RESET;
    s->ports[DRAGONBALL_GPIO_PORTD].sel   = DRAGONBALL_GPIO_PDSEL_RESET;
    s->ports[4].puden = DRAGONBALL_GPIO_PEPUEN_RESET;
    s->ports[4].sel   = DRAGONBALL_GPIO_PESEL_RESET;
    s->ports[5].puden = DRAGONBALL_GPIO_PFPUEN_RESET;
    s->ports[6].puden = DRAGONBALL_GPIO_PGPUEN_RESET;
    s->ports[6].sel   = DRAGONBALL_GPIO_PGSEL_RESET;
}

static const VMStateDescription vmstate_sifive_gpio = {
    .name = TYPE_DRAGONBALL_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};

//static const Property dragonball_gpio_properties[] = {
//};

static void dragonball_gpio_realize(DeviceState *dev, Error **errp)
{
    DragonBallGPIOState *s = DRAGONBALL_GPIO(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &gpio_ops, s,
            TYPE_DRAGONBALL_GPIO, 0x100);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    //for (int i = 0; i < s->ngpio; i++) {
    //    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    //}

    qdev_init_gpio_in(DEVICE(s), dragonball_gpio_set, DRAGONBALL_GPIO_NGPIO);
    qdev_init_gpio_out(DEVICE(s), s->output, DRAGONBALL_GPIO_NGPIO);
}

static void dragonball_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    //device_class_set_props(dc, dragonball_gpio_properties);
    dc->vmsd = &vmstate_sifive_gpio;
    dc->realize = dragonball_gpio_realize;
    dc->legacy_reset = dragonball_gpio_reset;
}

static const TypeInfo dragonball_gpio_info = {
    .name = TYPE_DRAGONBALL_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DragonBallGPIOState),
    .class_init = dragonball_gpio_class_init
};

static void dragonball_gpio_register_types(void)
{
    type_register_static(&dragonball_gpio_info);
}

type_init(dragonball_gpio_register_types)
