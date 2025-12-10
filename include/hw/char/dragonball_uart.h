/*
 */

#ifndef DRAGONBALL_UART_H
#define DRAGONBALL_UART_H

#include "chardev/char-fe.h"
#include "hw/sysbus.h"
#include "hw/ptimer.h"
#include "qemu/fifo8.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_UART "dragonball.uart"
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallUartState, DRAGONBALL_UART)

struct DragonBallUartState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    Fifo8 rx_fifo;
    Fifo8 tx_fifo;
    ptimer_state *timer;

    uint16_t ucnt;
    uint16_t ubaud;
    uint16_t urx;
    uint16_t utx;
    uint16_t umisc;
    uint16_t nipr;

    qemu_irq irq;
    CharFrontend chr;
};

#define TXFIFOSZ        8
#define RXFIFOSZ        12

#define UCNT            0x0
#define UCNT_TXAE       (1 << 0)
#define UCNT_TXHE       (1 << 1)
#define UCNT_TXEE       (1 << 2)
#define UCNT_RXRE       (1 << 3)
#define UCNT_RXHE       (1 << 4)
#define UCNT_RXFE       (1 << 5)
#define UCNT_TXEN       (1 << 13)
#define UCNT_RXEN       (1 << 14)
#define UCNT_UEN        (1 << 15)
#define UBAUD                0x2
#define UBAUD_DIVIDE_MASK    0x3
#define UBAUD_DIVIDE_SHIFT   8
#define UBAUD_PRESCALER_MASK 0x3f
#define URX             0x4
#define URX_DATA_READY  (1 << 13)
#define UTX             0x6
#define UTX_TX_BUSY       (1 << 10)
#define UTX_TX_AVAIL      (1 << 13)
#define UTX_TX_HALF_EMPTY (1 << 14)
#define UTX_TX_EMPTY      (1 << 15)

#endif
