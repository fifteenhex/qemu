/*
 * MOS 8520 CIA (Complex Interface Adapter), as used in the Amiga line.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_M68K_MOS8520_H
#define HW_M68K_MOS8520_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_MOS8520 "mos8520"
OBJECT_DECLARE_SIMPLE_TYPE(MOS8520State, MOS8520)

struct MOS8520State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq port_a_out[8];
    qemu_irq port_b_out[8];

    uint32_t clock_hz;      /* E clock feeding the interval timers */
    uint32_t tod_hz;        /* tick rate of the TOD counter */

    uint8_t pra, prb, ddra, ddrb;
    uint8_t input_a, input_b;   /* external pin state (pulled up) */

    ptimer_state *timer_a;
    ptimer_state *timer_b;
    uint16_t ta_latch, tb_latch;
    uint16_t tb_count;          /* used when timer B counts TA underflows */
    uint8_t cra, crb;

    /* 24-bit TOD counter, computed from the virtual clock while running */
    uint32_t tod_base;
    int64_t tod_base_ns;
    bool tod_running;
    bool tod_latched;
    uint32_t tod_latch;
    uint32_t tod_alarm;
    QEMUTimer alarm_timer;

    uint8_t sdr;
    uint8_t icr_status, icr_mask;

    bool flag_prev;
};

/* Feed a byte into the serial port (keyboard input on CIA-A) */
void mos8520_sdr_input(MOS8520State *s, uint8_t data);

#endif
