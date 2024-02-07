/*
 *
 */

#ifndef DRAGONBALL_TIMER_H
#define DRAGONBALL_TIMER_H

#include "hw/sysbus.h"
#include "hw/ptimer.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_TIMER "dragonball.timer"
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallTimerState, DRAGONBALL_TIMER)

struct DragonBallTimerState
{
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;

    uint16_t tctl;
    uint16_t tprer;
    uint16_t tcmp;
    uint16_t tstat;

    uint64_t start_offset;
};

#define DRAGONBALL_TIMER_TCTL                 0x0
#define DRAGONBALL_TIMER_TCTL_CLKSOURCE_SHIFT 1
#define DRAGONBALL_TIMER_TCTL_CLKSOURCE_MASK  0x7
#define DRAGONBALL_TIMER_TCTL_TEN             (1 << 0)
#define DRAGONBALL_TIMER_TCTL_IRQEN           (1 << 4)
#define DRAGONBALL_TIMER_TCTL_FRR             (1 << 8)
#define DRAGONBALL_TIMER_TPRER                0x2
#define DRAGONBALL_TIMER_TCMP                 0x4
#define DRAGONBALL_TIMER_TCR                  0x6
#define DRAGONBALL_TIMER_TCN                  0x8
#define DRAGONBALL_TIMER_TSTAT                0xa
#define DRAGONBALL_TIMER_TSTAT_COMP           (1 << 0)
#define DRAGONBALL_TIMER_TSTAT_CAPT           (1 << 1)

#endif /* DRAGONBALL_TIMER_H */
