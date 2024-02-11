/*
 *
 */

#ifndef HW_DRAGONBALL_RTC_H
#define HW_DRAGONBALL_RTC_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_RTC "dragonball.rtc"

typedef struct DragonBallRTCState DragonBallRTCState;
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallRTCState, DRAGONBALL_RTC)

struct DragonBallRTCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    qemu_irq wdt_irq, rtc_irq;
};

#endif
