/*
 * DragonBall VZ SPI unit 1 — the FIFO-based SPI controller (0xfffff700)
 */

#ifndef HW_SSI_DRAGONBALL_SPI1_H
#define HW_SSI_DRAGONBALL_SPI1_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_SPI1 "dragonball.spi1"

typedef struct DragonBallSPI1State DragonBallSPI1State;
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallSPI1State, DRAGONBALL_SPI1)

#define DRAGONBALL_SPI1_FIFO_DEPTH 8

typedef struct DragonBallSPI1Fifo {
    uint16_t data[DRAGONBALL_SPI1_FIFO_DEPTH];
    uint8_t count;
    uint8_t head;
} DragonBallSPI1Fifo;

struct DragonBallSPI1State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    SSIBus *ssi;
    qemu_irq irq;

    uint16_t cont1;
    uint16_t intcs;
    uint16_t spc;

    DragonBallSPI1Fifo tx;
    DragonBallSPI1Fifo rx;
};

#define DRAGONBALL_SPI1_RXD    0x0
#define DRAGONBALL_SPI1_TXD    0x2
#define DRAGONBALL_SPI1_CONT   0x4
#define DRAGONBALL_SPI1_INTCS  0x6
#define DRAGONBALL_SPI1_TEST   0x8
#define DRAGONBALL_SPI1_SPC    0xa

#define DRAGONBALL_SPI1_CONT_BITCOUNT   0x000f
#define DRAGONBALL_SPI1_CONT_XCH        0x0100
#define DRAGONBALL_SPI1_CONT_SPIEN      0x0200
#define DRAGONBALL_SPI1_CONT_MASTER     0x0400
#define DRAGONBALL_SPI1_CONT_RATE_SHIFT 13

/* spiIntCS status flags (low byte) */
#define DRAGONBALL_SPI1_ST_RXFULL   0x20
#define DRAGONBALL_SPI1_ST_RXHALF   0x10
#define DRAGONBALL_SPI1_ST_RXAVAIL  0x08
#define DRAGONBALL_SPI1_ST_TXFULL   0x04
#define DRAGONBALL_SPI1_ST_TXHALF   0x02
#define DRAGONBALL_SPI1_ST_TXEMPTY  0x01
#define DRAGONBALL_SPI1_ST_XFERDONE 0x40

#endif
