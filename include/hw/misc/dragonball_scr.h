/*
 * DragonBall system control / ID register block (0xfffff000)
 */

#ifndef HW_MISC_DRAGONBALL_SCR_H
#define HW_MISC_DRAGONBALL_SCR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_SCR "dragonball.scr"

typedef struct DragonBallSCRState DragonBallSCRState;
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallSCRState, DRAGONBALL_SCR)

struct DragonBallSCRState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    uint8_t scr;
    uint8_t pcr;

    /* fixed silicon identity */
    uint8_t chip_id;
    uint8_t mask_id;
    uint16_t sw_id;
};

#define DRAGONBALL_SCR_SCR    0x0
#define DRAGONBALL_SCR_PCR    0x3
#define DRAGONBALL_SCR_CHIPID 0x4
#define DRAGONBALL_SCR_MASKID 0x5
#define DRAGONBALL_SCR_SWID   0x6

#endif
