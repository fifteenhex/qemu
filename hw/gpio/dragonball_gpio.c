/*
 *
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/gpio/dragonball_gpio.h"
#include "migration/vmstate.h"
#include "trace.h"

/* the level a DATA read would return: latch for outputs, pin for inputs */
static uint8_t dragonball_gpio_pins(DragonBallGPIOState *s, unsigned int port)
{
    return (s->ports[port].data & s->ports[port].dir) |
           (s->ports[port].ext & ~s->ports[port].dir);
}

/*
 * Port D pins feed the INT0-3 and keyboard interrupt sources, gated
 * by the polarity/enable registers.  Level semantics only — the
 * edge configuration (PDIRQEG) is not modelled.
 */
static void dragonball_gpio_update_portd_irqs(DragonBallGPIOState *s)
{
    uint8_t dir = s->ports[DRAGONBALL_GPIO_PORTD].dir;
    uint8_t pins = s->ports[DRAGONBALL_GPIO_PORTD].ext & ~dir;
    /* a level interrupt fires while the pin matches the polarity */
    uint8_t active = ~(pins ^ s->pdpol) & s->pdirqen & ~dir;
    /* the keyboard interrupt is the OR of the raw enabled pins */
    uint8_t kb = pins & s->pdkben;
    int i;

    for (i = 0; i < 4; i++)
        qemu_set_irq(s->portd_int[i], (active >> i) & 1);
    qemu_set_irq(s->kb_int, kb != 0);
}

static uint64_t dragonball_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    DragonBallGPIOState *s = DRAGONBALL_GPIO(opaque);
    unsigned int port = DRAGONBALL_GPIO_ADDR2PORT(addr);
    unsigned int reg = DRAGONBALL_GPIO_ADDR2REG(addr);

    if (port >= s->num_ports) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }

    switch (reg) {
    case DRAGONBALL_GPIO_REG_DIR:
        return s->ports[port].dir;
    case DRAGONBALL_GPIO_REG_DATA:
        /* input pins read the external level, output pins the latch */
        return dragonball_gpio_pins(s, port);
    case DRAGONBALL_GPIO_REG_PUDEN:
        return s->ports[port].puden;
    case DRAGONBALL_GPIO_REG_SEL:
        return s->ports[port].sel;
    case DRAGONBALL_GPIO_REG_POL:
        if (port == DRAGONBALL_GPIO_PORTD)
            return s->pdpol;
        break;
    case DRAGONBALL_GPIO_REG_IRQEN:
        if (port == DRAGONBALL_GPIO_PORTD)
            return s->pdirqen;
        break;
    case DRAGONBALL_GPIO_REG_KBEN:
        if (port == DRAGONBALL_GPIO_PORTD)
            return s->pdkben;
        break;
    case DRAGONBALL_GPIO_REG_IRQEG:
        if (port == DRAGONBALL_GPIO_PORTD)
            return s->pdirqeg;
        break;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                  __func__, addr);
    return 0;
}

static void dragonball_gpio_update_outputs(DragonBallGPIOState *s, unsigned int p)
{
    int i;

    for (i = 0; i < DRAGONBALL_GPIO_NGPIOPERPORT; i++) {
        uint8_t mask = 1 << i;
        unsigned int pin = p * DRAGONBALL_GPIO_NGPIOPERPORT + i;
        int level;

        /*
         * Outputs drive the latch; pins tristated back to inputs rise
         * to the pull-up level (port C has pull-downs instead).  The
         * Palm keyboard scanner deselects matrix rows exactly that
         * way, by flipping them to inputs.
         */
        if (s->ports[p].dir & mask)
            level = (mask & s->ports[p].data) ? 1 : 0;
        else
            level = (p == 2) ? 0 : 1;

        qemu_set_irq(s->output[pin], level);
    }
}

static void dragonball_gpio_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned int size)
{
    DragonBallGPIOState *s = DRAGONBALL_GPIO(opaque);
    unsigned int port = DRAGONBALL_GPIO_ADDR2PORT(addr);
    unsigned int reg = DRAGONBALL_GPIO_ADDR2REG(addr);

    if (port >= s->num_ports) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    switch (reg) {
    case DRAGONBALL_GPIO_REG_DIR:
        s->ports[port].dir = value;
        dragonball_gpio_update_outputs(s, port);
        break;
    case DRAGONBALL_GPIO_REG_DATA:
        s->ports[port].data = value;
        dragonball_gpio_update_outputs(s, port);
        break;
    case DRAGONBALL_GPIO_REG_PUDEN:
        s->ports[port].puden = value;
        break;
    case DRAGONBALL_GPIO_REG_SEL:
        s->ports[port].sel = value;
        break;
    case DRAGONBALL_GPIO_REG_POL:
        if (port == DRAGONBALL_GPIO_PORTD) {
            s->pdpol = value;
            break;
        }
        /* fallthrough */
    case DRAGONBALL_GPIO_REG_IRQEN:
        if (port == DRAGONBALL_GPIO_PORTD) {
            s->pdirqen = value;
            break;
        }
        /* fallthrough */
    case DRAGONBALL_GPIO_REG_KBEN:
        if (port == DRAGONBALL_GPIO_PORTD) {
            s->pdkben = value;
            break;
        }
        /* fallthrough */
    case DRAGONBALL_GPIO_REG_IRQEG:
        if (port == DRAGONBALL_GPIO_PORTD) {
            s->pdirqeg = value;
            break;
        }
        /* fallthrough */
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
    }

    if (port == DRAGONBALL_GPIO_PORTD) {
        /* Bottom four bits of sel in port D are hardwire to zero */
        s->ports[port].sel &= ~0xf;
        dragonball_gpio_update_portd_irqs(s);
    }
}

static const MemoryRegionOps gpio_ops = {
    .read =  dragonball_gpio_read,
    .write = dragonball_gpio_write,
    /*
     * Every register is one byte at its own address; wider guest
     * accesses are split by the core.
     */
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_gpio_set(void *opaque, int line, int value)
{
    DragonBallGPIOState *s = DRAGONBALL_GPIO(opaque);
    unsigned int port = line / DRAGONBALL_GPIO_NGPIOPERPORT;
    uint8_t mask = 1 << (line % DRAGONBALL_GPIO_NGPIOPERPORT);

    if (value)
        s->ports[port].ext |= mask;
    else
        s->ports[port].ext &= ~mask;

    if (port == DRAGONBALL_GPIO_PORTD)
        dragonball_gpio_update_portd_irqs(s);
}

static void dragonball_gpio_reset(DeviceState *dev)
{
    DragonBallGPIOState *s = DRAGONBALL_GPIO(dev);

    int i;

    /*
     * Chip reset does not change the level on the pins: preserve the
     * externally driven state, clear only the register file.
     */
    for (i = 0; i < DRAGONBALL_GPIO_PORTS; i++) {
        s->ports[i].dir = 0;
        s->ports[i].data = 0;
        s->ports[i].puden = 0;
        s->ports[i].sel = 0;
    }

    s->pdpol = 0;
    s->pdirqen = 0;
    s->pdkben = 0;
    s->pdirqeg = 0;

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

static const Property dragonball_gpio_properties[] = {
    DEFINE_PROP_UINT8("num-ports", DragonBallGPIOState, num_ports, 7),
};

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
    qdev_init_gpio_out_named(DEVICE(s), s->portd_int, "portd-int", 4);
    qdev_init_gpio_out_named(DEVICE(s), &s->kb_int, "kb-int", 1);
}

static void dragonball_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, dragonball_gpio_properties);
    dc->vmsd = &vmstate_sifive_gpio;
    dc->realize = dragonball_gpio_realize;
    device_class_set_legacy_reset(dc, dragonball_gpio_reset);
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
