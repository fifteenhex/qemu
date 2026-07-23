/*
 * DragonBall on-chip RTC (0xfffffb00)
 */

#ifndef HW_DRAGONBALL_RTC_H
#define HW_DRAGONBALL_RTC_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_RTC "dragonball.rtc"

typedef struct DragonBallRTCState DragonBallRTCState;
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallRTCState, DRAGONBALL_RTC)

struct DragonBallRTCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    /*
     * The counter is "clock seconds + offset": setting the clock
     * adjusts the offset, reading derives day/h/m/s from the sum.
     */
    int64_t offset;

    uint32_t alarm;
    uint16_t dayalarm;
    uint16_t ctl;
    uint16_t isr;
    uint16_t ienr;
    uint16_t stpwch;
    uint16_t watchdog;

    /* last observed counter value, for edge detection in the tick */
    int64_t last_seconds;

    ptimer_state *timer;

    qemu_irq wdt_irq, rtc_irq;
};

#define DRAGONBALL_RTC_RTCTIME  0x00
#define DRAGONBALL_RTC_RTCALRM  0x04
#define DRAGONBALL_RTC_WATCHDOG 0x0a
#define DRAGONBALL_RTC_RTCCTL   0x0c
#define DRAGONBALL_RTC_RTCISR   0x0e
#define DRAGONBALL_RTC_RTCIENR  0x10
#define DRAGONBALL_RTC_STPWCH   0x12
/* VZ only */
#define DRAGONBALL_RTC_DAYR     0x1a
#define DRAGONBALL_RTC_DAYALARM 0x1c

#define DRAGONBALL_RTC_CTL_EN   (1 << 7)

#define DRAGONBALL_RTC_INT_SW   (1 << 0)
#define DRAGONBALL_RTC_INT_MIN  (1 << 1)
#define DRAGONBALL_RTC_INT_ALM  (1 << 2)
#define DRAGONBALL_RTC_INT_DAY  (1 << 3)
#define DRAGONBALL_RTC_INT_1HZ  (1 << 4)
#define DRAGONBALL_RTC_INT_HR   (1 << 5)

#endif
