/*
 */

#include "qemu/osdep.h"
#include "hw/char/dragonball_uart.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#ifndef DEBUG_DRAGONBALL_UART
#define DEBUG_DRAGONBALL_UART 0
#endif

#define DPRINTF(fmt, args...) \
    do { \
        if (DEBUG_DRAGONBALL_UART) { \
            fprintf(stderr, "[%s]%s: " fmt , TYPE_DRAGONBALL_UART, \
                                             __func__, ##args); \
        } \
    } while (0)

static const VMStateDescription vmstate_dragonball_uart = {
    .name = TYPE_DRAGONBALL_UART,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(ucnt, DragonBallUartState),
        VMSTATE_UINT16(ubaud, DragonBallUartState),
        VMSTATE_UINT16(urx, DragonBallUartState),
        VMSTATE_UINT16(utx, DragonBallUartState),
        VMSTATE_UINT16(umisc, DragonBallUartState),
        VMSTATE_UINT16(nipr, DragonBallUartState),
        VMSTATE_END_OF_LIST()
    },
};

static int dragonball_uart_tx_empty(DragonBallUartState *s)
{
    return fifo8_is_empty(&s->tx_fifo) ? 1 : 0;
}

static int dragonball_uart_tx_half_empty(DragonBallUartState *s)
{
    return (fifo8_num_free(&s->tx_fifo) > (TXFIFOSZ / 2)) ? 1 : 0;
}


static int dragonball_uart_tx_avail(DragonBallUartState *s)
{
    return fifo8_is_full(&s->tx_fifo) ? 0 : 1;
}

static void dragonball_uart_update_irq(DragonBallUartState *s)
{
    int newlevel = 0;

    /* tx fifo interrupts */
    if (s->ucnt & UCNT_TXAE)
        newlevel |= dragonball_uart_tx_avail(s);
    if (s->ucnt & UCNT_TXHE)
        newlevel |= dragonball_uart_tx_half_empty(s);
    if (s->ucnt & UCNT_TXEE)
        newlevel |= dragonball_uart_tx_empty(s);

    /* rx fifo interrupts */
    if (s->ucnt & UCNT_RXRE)
        newlevel |= fifo8_is_empty(&s->rx_fifo) ? 0 : 1;
    if (s->ucnt & UCNT_RXHE)
        newlevel |= (fifo8_num_used(&s->rx_fifo) > (RXFIFOSZ / 2)) ? 1 : 0;
    if (s->ucnt & UCNT_RXFE)
        newlevel |= fifo8_is_full(&s->rx_fifo) ? 1 : 0;

    //if(newlevel)
    //    printf("%s:%d\n", __func__, __LINE__);

    qemu_set_irq(s->irq, newlevel);
}

static void dragonball_uart_timer_cb(void *opaque)
{
    DragonBallUartState *s = opaque;

    if ((s->ucnt & UCNT_TXEN) && !fifo8_is_empty(&s->tx_fifo)) {
        uint8_t ch = fifo8_pop(&s->tx_fifo);

        qemu_chr_fe_write_all(&s->chr, &ch, 1);
    }

    dragonball_uart_update_irq(s);
}

static void dragonball_uart_reset(DeviceState *dev)
{
    DragonBallUartState *s = DRAGONBALL_UART(dev);

    fifo8_reset(&s->rx_fifo);
    fifo8_reset(&s->tx_fifo);
}

static uint64_t dragonball_uart_read(void *opaque, hwaddr offset,
                                unsigned size)
{
    DragonBallUartState *s = (DragonBallUartState *)opaque;
    uint16_t urx = 0;
    DPRINTF("read(offset=0x%" HWADDR_PRIx ")\n", offset);

    switch (offset) {
    case UCNT:
        //printf("%s:%d 0x%04x\n", __func__, __LINE__, (unsigned) s->ucnt);
        return s->ucnt;
    case URX:
        if (!fifo8_is_empty(&s->rx_fifo)) {
            urx = fifo8_pop(&s->rx_fifo);
            urx |= URX_DATA_READY;
            qemu_chr_fe_accept_input(&s->chr);
        }
        return urx;
    case UTX:
        return (dragonball_uart_tx_empty(s) ? 0 : UTX_TX_BUSY) |
               (dragonball_uart_tx_empty(s) ? UTX_TX_EMPTY : 0) |
               (dragonball_uart_tx_half_empty(s) ? UTX_TX_HALF_EMPTY : 0) |
               (dragonball_uart_tx_avail(s) ? UTX_TX_AVAIL : 0);
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "[%s]%s: Bad register at offset 0x%"
                      HWADDR_PRIx "\n", TYPE_DRAGONBALL_UART, __func__, offset);
        return 0;
    }
}

static void dragonball_uart_update_timer(DragonBallUartState *s)
{
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, 9600 / 8);
    ptimer_set_limit(s->timer, 1, 1);

    /* Start or stop the timer */
    if(s->ucnt & UCNT_UEN)
        ptimer_run(s->timer, 0);
    else
        ptimer_stop(s->timer);

    ptimer_transaction_commit(s->timer);
}

static void dragonball_uart_updatebaud(DragonBallUartState *s, uint16_t value)
{
    unsigned int divider, prescaler;

    s->ubaud = value;
    divider = 1 << ((s->ubaud >> UBAUD_DIVIDE_SHIFT) & UBAUD_DIVIDE_MASK);
    prescaler = 65 - (s->ubaud & UBAUD_PRESCALER_MASK);

    DPRINTF("divider: %u, precaler: %u\n", divider, prescaler);
}

static void dragonball_uart_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    DragonBallUartState *s = (DragonBallUartState *)opaque;
    Chardev *chr = qemu_chr_fe_get_driver(&s->chr);
    unsigned char ch = value & 0xff;

    DPRINTF("write(offset=0x%" HWADDR_PRIx ", value = 0x%x) to %s\n",
            offset, (unsigned int)value, chr ? chr->label : "NODEV");

    switch (offset) {
    case UCNT:
        //printf("%s:%d 0x%04x\n", __func__, __LINE__, (unsigned) value);
        s->ucnt = value;
        dragonball_uart_update_timer(s);
        dragonball_uart_update_irq(s);
        break;
    case UBAUD:
        dragonball_uart_updatebaud(s, value);
        break;
    case 0x7:
        if (fifo8_is_full(&s->tx_fifo))
            printf("Tried to push to fifo despite being full\n");
        else
            fifo8_push(&s->tx_fifo, ch);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "[%s]%s: Bad register at offset 0x%"
                      HWADDR_PRIx "\n", TYPE_DRAGONBALL_UART, __func__, offset);
    }
}

static int dragonball_uart_can_receive(void *opaque)
{
    DragonBallUartState *s = (DragonBallUartState *)opaque;

    return !fifo8_is_full(&s->rx_fifo);
}

static void dragonball_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    DragonBallUartState *s = (DragonBallUartState *)opaque;
    int i;

    for (i = 0; i < size; i++) {
        if (fifo8_is_full(&s->rx_fifo))
            break;

        fifo8_push(&s->rx_fifo, buf[i]);
    }

    dragonball_uart_update_irq(s);
}

static void dragonball_uart_event(void *opaque, QEMUChrEvent event)
{
    if (event == CHR_EVENT_BREAK) {
        //imx_put_data(opaque, URXD_BRK | URXD_FRMERR | URXD_ERR);
    }
}


static const struct MemoryRegionOps dragonball_uart_ops = {
    .read = dragonball_uart_read,
    .write = dragonball_uart_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_uart_realize(DeviceState *dev, Error **errp)
{
    DragonBallUartState *s = DRAGONBALL_UART(dev);

    DPRINTF("char dev for uart: %p\n", qemu_chr_fe_get_driver(&s->chr));

    qemu_chr_fe_set_handlers(&s->chr, dragonball_uart_can_receive, dragonball_uart_receive,
                             dragonball_uart_event, NULL, s, NULL, true);
}

static void dragonball_uart_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DragonBallUartState *s = DRAGONBALL_UART(obj);

    s->timer = ptimer_init(dragonball_uart_timer_cb, s, 0);
    fifo8_create(&s->rx_fifo, RXFIFOSZ);
    fifo8_create(&s->tx_fifo, TXFIFOSZ);

    memory_region_init_io(&s->iomem, obj, &dragonball_uart_ops, s,
                          TYPE_DRAGONBALL_UART, 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static Property dragonball_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", DragonBallUartState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void dragonball_uart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = dragonball_uart_realize;
    dc->vmsd = &vmstate_dragonball_uart;
    dc->legacy_reset = dragonball_uart_reset;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->desc = "DragonBall UART";
    device_class_set_props(dc, dragonball_uart_properties);
}

static const TypeInfo dragonball_uart_info = {
    .name           = TYPE_DRAGONBALL_UART,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(DragonBallUartState),
    .instance_init  = dragonball_uart_init,
    .class_init     = dragonball_uart_class_init,
};

static void dragonball_uart_register_types(void)
{
    type_register_static(&dragonball_uart_info);
}

type_init(dragonball_uart_register_types)
