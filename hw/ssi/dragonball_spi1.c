/*
 * DragonBall VZ SPI unit 1: the FIFO-based SPI controller.
 *
 * An 8-entry TX FIFO and 8-entry RX FIFO with word-at-a-time
 * exchange.  Software fills the TX FIFO through SPITXD, sets the XCH
 * bit in SPICONT1, and the controller shifts each queued word out to
 * the slave and pushes what comes back into the RX FIFO; SPIINTCS
 * reflects the FIFO fill levels and, masked, raises the SPI1
 * interrupt.  On the Palm m500 this bus carries the SD card.
 *
 * The transfer is done synchronously here rather than metered by the
 * sample-rate clock, which is what PalmOS polls SPIINTCS for anyway.
 * The register semantics follow POSE's EmRegsVZ SPI1 handlers.
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
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/dragonball_spi1.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void fifo_clear(DragonBallSPI1Fifo *f)
{
    f->count = 0;
    f->head = 0;
}

static void fifo_push(DragonBallSPI1Fifo *f, uint16_t v)
{
    if (f->count >= DRAGONBALL_SPI1_FIFO_DEPTH)
        return;
    f->data[(f->head + f->count) % DRAGONBALL_SPI1_FIFO_DEPTH] = v;
    f->count++;
}

static uint16_t fifo_pop(DragonBallSPI1Fifo *f)
{
    uint16_t v;

    if (f->count == 0)
        return 0;
    v = f->data[f->head];
    f->head = (f->head + 1) % DRAGONBALL_SPI1_FIFO_DEPTH;
    f->count--;
    return v;
}

static void dragonball_spi1_update_irq(DragonBallSPI1State *s)
{
    uint8_t flags = 0;

    if (s->rx.count == DRAGONBALL_SPI1_FIFO_DEPTH)
        flags |= DRAGONBALL_SPI1_ST_RXFULL;
    if (s->rx.count >= 4)
        flags |= DRAGONBALL_SPI1_ST_RXHALF;
    if (s->rx.count > 0)
        flags |= DRAGONBALL_SPI1_ST_RXAVAIL;
    if (s->tx.count == DRAGONBALL_SPI1_FIFO_DEPTH)
        flags |= DRAGONBALL_SPI1_ST_TXFULL;
    if (s->tx.count >= 4)
        flags |= DRAGONBALL_SPI1_ST_TXHALF;
    if (s->tx.count == 0)
        flags |= DRAGONBALL_SPI1_ST_TXEMPTY;

    /* keep the transfer-done bit (0x40) and the mask (high byte) */
    s->intcs = (s->intcs & 0xff40) | flags;

    qemu_set_irq(s->irq, (flags & (s->intcs >> 8)) != 0);
}

/* Shift the whole TX FIFO out to the slave, collecting the results. */
static void dragonball_spi1_exchange(DragonBallSPI1State *s)
{
    int bits = (s->cont1 & DRAGONBALL_SPI1_CONT_BITCOUNT) + 1;

    while (s->tx.count > 0) {
        uint16_t out = fifo_pop(&s->tx);
        uint16_t in;

        /*
         * A byte-oriented slave (the SD card) only shifts on 8- and
         * 16-bit words; sub-byte widths are timing pulses that must
         * NOT consume a byte from the slave, or its byte stream
         * desyncs.  Matches POSE's EmSPISlaveSD::DoExchange.
         */
        if (bits == 8) {
            in = ssi_transfer(s->ssi, out & 0xff);
        } else if (bits == 16) {
            in = ssi_transfer(s->ssi, (out >> 8) & 0xff) << 8;
            in |= ssi_transfer(s->ssi, out & 0xff);
        } else {
            in = 0xffff >> (16 - bits);
        }

        if (s->rx.count == 0)
            s->intcs |= DRAGONBALL_SPI1_ST_XFERDONE;
        fifo_push(&s->rx, in);
    }

    /* the exchange completes immediately; clear XCH */
    s->cont1 &= ~DRAGONBALL_SPI1_CONT_XCH;
    dragonball_spi1_update_irq(s);
}

static uint64_t dragonball_spi1_read(void *opaque, hwaddr addr, unsigned size)
{
    DragonBallSPI1State *s = opaque;
    uint16_t ret;

    switch (addr) {
    case DRAGONBALL_SPI1_RXD:
        ret = fifo_pop(&s->rx);
        dragonball_spi1_update_irq(s);
        return ret;
    case DRAGONBALL_SPI1_CONT:
        return s->cont1;
    case DRAGONBALL_SPI1_INTCS:
        ret = s->intcs;
        /* reading the status clears the transfer-done bit */
        s->intcs &= ~DRAGONBALL_SPI1_ST_XFERDONE;
        return ret;
    case DRAGONBALL_SPI1_TEST:
        return s->tx.count | (s->rx.count << 4);
    case DRAGONBALL_SPI1_SPC:
        return s->spc;
    default:
        return 0;
    }
}

static void dragonball_spi1_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned size)
{
    DragonBallSPI1State *s = opaque;
    uint16_t old;

    switch (addr) {
    case DRAGONBALL_SPI1_TXD:
        if (s->cont1 & DRAGONBALL_SPI1_CONT_SPIEN) {
            fifo_push(&s->tx, value);
            dragonball_spi1_update_irq(s);
        }
        break;
    case DRAGONBALL_SPI1_CONT:
        old = s->cont1;
        s->cont1 = value;

        /* disabling the unit flushes both FIFOs */
        if (!(value & DRAGONBALL_SPI1_CONT_SPIEN)) {
            fifo_clear(&s->tx);
            fifo_clear(&s->rx);
            dragonball_spi1_update_irq(s);
        }

        /* a transfer needs both enable and master set */
        if ((value & (DRAGONBALL_SPI1_CONT_SPIEN |
                      DRAGONBALL_SPI1_CONT_MASTER)) !=
            (DRAGONBALL_SPI1_CONT_SPIEN | DRAGONBALL_SPI1_CONT_MASTER)) {
            s->cont1 &= ~DRAGONBALL_SPI1_CONT_XCH;
            break;
        }

        /* rising edge of XCH starts the exchange */
        if ((value & ~old) & DRAGONBALL_SPI1_CONT_XCH)
            dragonball_spi1_exchange(s);
        break;
    case DRAGONBALL_SPI1_INTCS:
        /* only the interrupt mask (high byte) is writable */
        s->intcs = (s->intcs & 0x00ff) | (value & 0xff00);
        dragonball_spi1_update_irq(s);
        break;
    case DRAGONBALL_SPI1_SPC:
        s->spc = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps dragonball_spi1_ops = {
    .read = dragonball_spi1_read,
    .write = dragonball_spi1_write,
    .valid.min_access_size = 2,
    .valid.max_access_size = 2,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_spi1_reset(DeviceState *dev)
{
    DragonBallSPI1State *s = DRAGONBALL_SPI1(dev);

    s->cont1 = 0;
    s->intcs = 0;
    s->spc = 0;
    fifo_clear(&s->tx);
    fifo_clear(&s->rx);
    dragonball_spi1_update_irq(s);
}

static void dragonball_spi1_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DragonBallSPI1State *s = DRAGONBALL_SPI1(dev);

    s->ssi = ssi_create_bus(dev, "ssi");
    sysbus_init_irq(sbd, &s->irq);

    memory_region_init_io(&s->mmio, OBJECT(s), &dragonball_spi1_ops, s,
                          TYPE_DRAGONBALL_SPI1, 0x10);
    sysbus_init_mmio(sbd, &s->mmio);
}

static const VMStateDescription vmstate_dragonball_spi1_fifo = {
    .name = "dragonball_spi1_fifo",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(data, DragonBallSPI1Fifo,
                             DRAGONBALL_SPI1_FIFO_DEPTH),
        VMSTATE_UINT8(count, DragonBallSPI1Fifo),
        VMSTATE_UINT8(head, DragonBallSPI1Fifo),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_dragonball_spi1 = {
    .name = "dragonball_spi1",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(cont1, DragonBallSPI1State),
        VMSTATE_UINT16(intcs, DragonBallSPI1State),
        VMSTATE_UINT16(spc, DragonBallSPI1State),
        VMSTATE_STRUCT(tx, DragonBallSPI1State, 1,
                       vmstate_dragonball_spi1_fifo, DragonBallSPI1Fifo),
        VMSTATE_STRUCT(rx, DragonBallSPI1State, 1,
                       vmstate_dragonball_spi1_fifo, DragonBallSPI1Fifo),
        VMSTATE_END_OF_LIST()
    }
};

static void dragonball_spi1_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, dragonball_spi1_reset);
    dc->realize = dragonball_spi1_realize;
    dc->vmsd = &vmstate_dragonball_spi1;
}

static const TypeInfo dragonball_spi1_info = {
    .name          = TYPE_DRAGONBALL_SPI1,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DragonBallSPI1State),
    .class_init    = dragonball_spi1_class_init,
};

static void dragonball_spi1_register_types(void)
{
    type_register_static(&dragonball_spi1_info);
}

type_init(dragonball_spi1_register_types)
