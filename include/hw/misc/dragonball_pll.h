/*
 *
 */

#ifndef HW_DRAGONBALL_PLL_H
#define HW_DRAGONBALL_PLL_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_PLL "dragonball.pll"

typedef struct DragonBallPLLState DragonBallPLLState;
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallPLLState, DRAGONBALL_PLL)

struct DragonBallPLLState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    uint16_t pllcr;
    uint16_t pllfsr;
};

#define DRAGONBALL_PLL_PLLCR           0x0
#define DRAGONBALL_PLL_PLLCR_PRESC     (1 << 5)
#define DRAGONBALL_PLL_PLLFSR          0x2
#define DRAGONBALL_PLL_PLLFSR_PC_MASK  0xff
#define DRAGONBALL_PLL_PLLFSR_QC_MASK  0xf
#define DRAGONBALL_PLL_PLLFSR_QC_SHIFT 8

#endif
