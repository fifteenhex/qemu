/*
 *
 */

#ifndef HW_DRAGONBALL_SPI_H
#define HW_DRAGONBALL_SPI_H

#include "hw/sysbus.h"
#include "hw/ptimer.h"

#define TYPE_DRAGONBALL_SPI "dragonball.spi"
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallSPIState, DRAGONBALL_SPI)

struct DragonBallSPIState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    uint16_t data_out, data_in;
    uint16_t cont;

    SSIBus *spi;

    ptimer_state *timer;
    bool running;
};

#define DRAGONBALL_SPI_REG_SPIMDATA               0x0
#define DRAGONBALL_SPI_REG_SPIMCONT               0x2
#define DRAGONBALL_SPI_REG_SPIMCONT_DATARATE
#define DRAGONBALL_SPI_REG_SPIMCONT_ENABLE        (1 << 9)
#define DRAGONBALL_SPI_REG_SPIMCONT_XCH           (1 << 8)
#define DRAGONBALL_SPI_REG_SPIMCONT_IRQ           (1 << 7)
#define DRAGONBALL_SPI_REG_SPIMCONT_IRQEN         (1 << 6)
#define DRAGONBALL_SPI_REG_SPIMCONT_PHA
#define DRAGONBALL_SPI_REG_SPIMCONT_POL
#define DRAGONBALL_SPI_REG_SPIMCONT_BITCOUNT_MASK 0xf

#endif /* HW_DRAGONBALL_SPI_H */
