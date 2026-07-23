/*
 *
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/ssi/dragonball_spi.h"

#define DATABITS(_s) ((_s->cont & DRAGONBALL_SPI_REG_SPIMCONT_BITCOUNT_MASK) + 1)

static void dragonball_spi_complete(DragonBallSPIState *s);

static void dragonball_spi_reset(DeviceState *d)
{
    DragonBallSPIState *s = DRAGONBALL_SPI(d);

    s->running = false;
}

static uint64_t dragonball_spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    DragonBallSPIState *s = opaque;

    //printf("%s:%d %x %u\n", __func__, __LINE__, (unsigned) addr, size);

    switch(addr) {
    case DRAGONBALL_SPI_REG_SPIMDATA:
        return s->data_in;
    case DRAGONBALL_SPI_REG_SPIMCONT:
        return s->cont | (s->running ? DRAGONBALL_SPI_REG_SPIMCONT_XCH : 0);
    }

    return 0;
}

static void dragonball_spi_do_transfer(DragonBallSPIState *s)
{
    int bits = DATABITS(s);
    uint16_t out = s->data_out & (0xffff >> (16 - bits));

    s->running = true;

    /*
     * Any count from 1 to 16 bits is allowed (PalmOS uses odd sizes
     * to talk to the touchscreen ADC).
     *
     * In bitwise mode every ssi_transfer() call carries a single bit,
     * MSB first, which preserves the frame alignment for slaves that
     * hunt for a start bit in the stream (the ADS7843: PalmOS places
     * the command at bits 14..7 of a frame, which byte chunking would
     * mangle).  Otherwise transfer byte-wise, high byte first, for
     * conventional slaves like the SD card or DS1305.
     */
    s->data_in = 0;
    if (s->bitwise) {
        int i;

        for (i = bits - 1; i >= 0; i--) {
            s->data_in = (s->data_in << 1) |
                         (ssi_transfer(s->spi, (out >> i) & 1) & 1);
        }
    } else {
        if (bits > 8) {
            s->data_in = ssi_transfer(s->spi, (out >> 8) & 0xff);
            s->data_in <<= 8;
        }
        s->data_in |= ssi_transfer(s->spi, out & 0xff);
        s->data_in &= 0xffff >> (16 - bits);
    }

    //printf("out: 0x%04x in: 0x%04x (%d bits)\n",
    //        (unsigned) s->data_out, (unsigned) s->data_in, DATABITS(s));

    /* Clear running flag later to simulate the real speed */
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, 4000000);
    ptimer_set_limit(s->timer, 16, 1);
    ptimer_run(s->timer, 1);
    ptimer_transaction_commit(s->timer);
}

static void dragonball_spi_write(void *opaque, hwaddr addr,
                             uint64_t value, unsigned int size)
{
    DragonBallSPIState *s = opaque;

    //printf("%s:%d %x\n", __func__, __LINE__, (unsigned) addr);

    switch(addr) {
	case DRAGONBALL_SPI_REG_SPIMDATA:
	s->data_out = value;
	break;
        case DRAGONBALL_SPI_REG_SPIMCONT:
             s->cont = value & ~DRAGONBALL_SPI_REG_SPIMCONT_XCH;

             /* Clear the interrupt */
             if (!(s->cont & DRAGONBALL_SPI_REG_SPIMCONT_IRQ))
                 qemu_set_irq(s->irq, 0);

             if (!(s->cont & DRAGONBALL_SPI_REG_SPIMCONT_ENABLE))
		break;
             /* Has the XCH bit toggled ? */
             if (value & DRAGONBALL_SPI_REG_SPIMCONT_XCH) {
                 /*
                  * The data was exchanged synchronously; only the
                  * completion flag is delayed.  A new exchange while
                  * the flag is pending must not be lost — flush the
                  * old completion and start over, like real silicon
                  * that is simply clocked by the new request.
                  */
                 if (s->running) {
                     ptimer_transaction_begin(s->timer);
                     ptimer_stop(s->timer);
                     ptimer_transaction_commit(s->timer);
                     dragonball_spi_complete(s);
                 }
                 dragonball_spi_do_transfer(s);
             }
        break;
    }
}

static const MemoryRegionOps dragonball_spi_ops = {
    .read = dragonball_spi_read,
    .write = dragonball_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_spi_complete(DragonBallSPIState *s)
{
    s->running = false;

    if (s->cont & DRAGONBALL_SPI_REG_SPIMCONT_IRQEN) {
        s->cont |= DRAGONBALL_SPI_REG_SPIMCONT_IRQ;
        qemu_set_irq(s->irq, 1);
    }
}

static void dragonball_spi_timer_cb(void *opaque)
{
    dragonball_spi_complete(opaque);
}

static void dragonball_spi_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DragonBallSPIState *s = DRAGONBALL_SPI(dev);

    s->spi = ssi_create_bus(dev, "ssi");
    sysbus_init_irq(sbd, &s->irq);
    s->timer = ptimer_init(dragonball_spi_timer_cb, s, 0);

    memory_region_init_io(&s->mmio, OBJECT(s), &dragonball_spi_ops, s,
                          TYPE_DRAGONBALL_SPI, 0x100);
    sysbus_init_mmio(sbd, &s->mmio);
}

static const Property dragonball_spi_properties[] = {
    DEFINE_PROP_BOOL("bitwise", DragonBallSPIState, bitwise, false),
};

static void dragonball_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, dragonball_spi_properties);
    device_class_set_legacy_reset(dc, dragonball_spi_reset);
    dc->realize = dragonball_spi_realize;
}

static const TypeInfo dragonball_spi_info = {
    .name           = TYPE_DRAGONBALL_SPI,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(DragonBallSPIState),
    .class_init     = dragonball_spi_class_init,
};

static void dragonball_spi_register_types(void)
{
    type_register_static(&dragonball_spi_info);
}

type_init(dragonball_spi_register_types)
