/*
 *
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/dragonball_intc.h"
#include "migration/vmstate.h"

 /* A bit is set when the irq is active and not masked */
#define ISRVAL(_s) ((~(_s)->imr) & (_s)->ipr)

/* Interrupt levels are fixed on the DragonBall EZ */
static const uint8_t dragonball_irq_levels[32] = {
    [DRAGONBALL_INTC_SPI] = 4,
    [DRAGONBALL_INTC_TMR] = 6,
    [DRAGONBALL_INTC_UART] = 4,
    [DRAGONBALL_INTC_WDT] = 4,
    [DRAGONBALL_INTC_RTC] = 4,
    [DRAGONBALL_INTC_KB] = 4,
    [DRAGONBALL_INTC_PWM] = 4,
    [DRAGONBALL_INTC_INT0] = 4,
    [DRAGONBALL_INTC_INT1] = 4,
    [DRAGONBALL_INTC_INT2] = 4,
    [DRAGONBALL_INTC_INT3] = 4,
    [DRAGONBALL_INTC_IRQ1] = 1,
    [DRAGONBALL_INTC_IRQ2] = 2,
    [DRAGONBALL_INTC_IRQ3] = 3,
    [DRAGONBALL_INTC_IRQ6] = 6,
    [DRAGONBALL_INTC_IRQ5] = 5,
    [DRAGONBALL_INTC_SAM] = 4,
    [DRAGONBALL_INTC_EMIQ] = 7,
};

static void dragonball_intc_updateirqs(DragonBallINTCState *s)
{
    M68kCPU *cpu = M68K_CPU(s->cpu);
    uint32_t isr = ISRVAL(s);
    int i, top = 0;

    /*
     * The core sees a single interrupt level: the highest active one.
     * Presenting each level separately loses interrupts, because the
     * cpu only latches one pending level/vector pair — a lower level
     * being deasserted would cancel a still-pending higher one.
     */
    for (i = 0; i < 32; i++) {
        uint8_t irqlevel;

        /*
         * The VZ's ILCR sets the levels for TMR2/SPI1/UART2/PWM2; a
         * TMR2 field of 0 (the reset value, illegal to program) acts
         * as level 6 — PalmOS clears it before setting it and must
         * not lose ticks in between.
         */
        switch (i) {
        case DRAGONBALL_INTC_TMR2:
            irqlevel = s->ilcr & 0xf;
            if (!irqlevel)
                irqlevel = 6;
            break;
        case DRAGONBALL_INTC_PWM2:
            irqlevel = (s->ilcr >> 4) & 0xf;
            break;
        case DRAGONBALL_INTC_UART2:
            irqlevel = (s->ilcr >> 8) & 0xf;
            break;
        case DRAGONBALL_INTC_SPI1:
            irqlevel = (s->ilcr >> 12) & 0xf;
            break;
        default:
            irqlevel = dragonball_irq_levels[i];
            break;
        }

        if (((isr >> i) & 1) && irqlevel > top)
            top = irqlevel;
    }

    if (top != s->cpu_level) {
        m68k_set_irq_level(cpu, top, s->ivr + top);
        s->cpu_level = top;
    }
}

/*
 * Sources that can latch on an edge and are then cleared by writing a
 * one to their ISR bit: the external IRQ pins.  Note that PEN is NOT
 * one of these — its IPR bit follows the /PENIRQ pin level and PalmOS
 * polls it to track the pen, silencing the interrupt via IMR instead.
 */
#define DRAGONBALL_INTC_EDGE_SOURCES \
    ((1 << DRAGONBALL_INTC_IRQ1) | (1 << DRAGONBALL_INTC_IRQ2) | \
     (1 << DRAGONBALL_INTC_IRQ3) | (1 << DRAGONBALL_INTC_IRQ6) | \
     (1 << DRAGONBALL_INTC_EMIQ))

/*
 * PalmOS accesses the 32-bit registers as 8/16/32-bit chunks at any
 * offset within the register (the IMR notably as two 16-bit halves),
 * so the handlers reassemble full register values by hand.
 */
static uint64_t dragonball_intc_read(void *opaque, hwaddr addr, unsigned size)
{
    DragonBallINTCState *s = opaque;
    uint32_t reg;

    switch (addr & ~3) {
    case DRAGONBALL_INTC_IVR & ~3: /* IVR (8) at +0, ICR (16) at +2 */
        reg = ((uint32_t)s->ivr << 24) | s->icr;
        break;
    case DRAGONBALL_INTC_IMR:
        reg = s->imr;
        break;
    case DRAGONBALL_INTC_ISR:
        reg = ISRVAL(s);
        break;
    case DRAGONBALL_INTC_IPR:
        reg = s->ipr;
        break;
    case DRAGONBALL_INTC_ILCR:
        reg = (uint32_t)s->ilcr << 16;
        break;
    default:
        return 0;
    }

    return extract32(reg, (4 - size - (addr & 3)) * 8, size * 8);
}

static void dragonball_intc_write(void *opaque, hwaddr addr, uint64_t value,
        unsigned size)
{
    DragonBallINTCState *s = opaque;
    unsigned int shift = (4 - size - (addr & 3)) * 8;
    uint32_t mask = ((size == 4) ? 0xffffffff : ((1u << (size * 8)) - 1))
                    << shift;
    uint32_t val = ((uint32_t)value << shift) & mask;

    switch (addr & ~3) {
    case DRAGONBALL_INTC_IVR & ~3:
        if (mask & 0xff000000)
            s->ivr = val >> 24;
        if (mask & 0x0000ffff)
            s->icr = val & 0xffff;
        break;
    case DRAGONBALL_INTC_IMR:
        s->imr = (s->imr & ~mask) | val;
        dragonball_intc_updateirqs(s);
        break;
    case DRAGONBALL_INTC_ISR:
        /* writing ones acks latched edge sources */
        s->ipr &= ~(val & DRAGONBALL_INTC_EDGE_SOURCES);
        dragonball_intc_updateirqs(s);
        break;
    case DRAGONBALL_INTC_ILCR:
        if (mask & 0xffff0000) {
            s->ilcr = val >> 16;
            dragonball_intc_updateirqs(s);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps dragonball_intc_ops = {
    .read = dragonball_intc_read,
    .write = dragonball_intc_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_intc_reset(DeviceState *dev)
{
    DragonBallINTCState *s = DRAGONBALL_INTC(dev);

    s->imr = 0x00ffffff;
    /* default levels: SPI1=6, UART2=5, PWM2=3, TMR2=3 */
    s->ilcr = 0x6533;
    s->cpu_level = 0;
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

static void dragonball_intc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, dragonball_intc_reset);
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
