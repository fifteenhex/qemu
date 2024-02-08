/*
 *
 */

#ifndef HW_DRAGONBALL_INTC_H
#define HW_DRAGONBALL_INTC_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_INTC "dragonball.intc"

typedef struct DragonBallINTCState DragonBallINTCState;
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallINTCState, DRAGONBALL_INTC)

struct DragonBallINTCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    uint8_t  ivr;
    uint16_t icr;
    uint32_t imr;
    uint32_t ipr;

    int levelstates[8];

    ArchCPU *cpu;
};

#define DRAGONBALL_INTC_IVR 0x0
#define DRAGONBALL_INTC_ICR 0x2
#define DRAGONBALL_INTC_IMR 0x4
#define DRAGONBALL_INTC_ISR 0xc
#define DRAGONBALL_INTC_IPR 0x10

#define DRAGONBALL_INTC_SPI  0
#define DRAGONBALL_INTC_TMR  1
#define DRAGONBALL_INTC_UART 2
#define DRAGONBALL_INTC_WDT  3
#define DRAGONBALL_INTC_RTC  4
#define DRAGONBALL_INTC_KB   6
#define DRAGONBALL_INTC_PWM  7
#define DRAGONBALL_INTC_INT0 8
#define DRAGONBALL_INTC_INT1 9
#define DRAGONBALL_INTC_INT2 10
#define DRAGONBALL_INTC_INT3 11
#define DRAGONBALL_INTC_IRQ1 16
#define DRAGONBALL_INTC_IRQ2 17
#define DRAGONBALL_INTC_IRQ3 18
#define DRAGONBALL_INTC_IRQ6 19
#define DRAGONBALL_INTC_IRQ5 20
#define DRAGONBALL_INTC_SAM  22
#define DRAGONBALL_INTC_EMIQ 23

#endif
