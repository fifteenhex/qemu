/*
 * Motorola/Freescale MC68SZ328 "DragonBall Super VZ" on-chip register
 * block + enhanced TFT color LCD controller.
 *
 * The SZ328 is the SoC of the Sony CLIE PEG-NR70/NR70V and PEG-SJ33.
 * Unlike the EZ/VZ DragonBalls (whose on-chip peripherals live at
 * 0xfffffxxx), the SZ moves its entire register window to 0xFFFE0000
 * ("different than in previous chips!!!" -- Cloudpilot-emu's
 * EmRegsSZPrv.h, kMemoryStart = 0xFFFE0000).  This single device models
 * that whole window as one MMIO region and additionally scans out the
 * on-chip LCD controller's framebuffer (which lives in main DRAM at the
 * address the guest programs into lcdStartAddr).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HW_MISC_DRAGONBALL_SZ_H
#define HW_MISC_DRAGONBALL_SZ_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "chardev/char-fe.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_SZ "dragonball-sz"
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallSZState, DRAGONBALL_SZ)

/*
 * Register window base and size.  The window is 128KB: the SZ's *new*
 * peripherals (DMA/ADC/MMC/enhanced LCDC + CLUT) occupy 0xFFFE0000..,
 * and the classic DragonBall peripheral cluster (SCR/PLL/INTC/GPIO/
 * timers/UART/RTC) still sits at its traditional 0xFFFFF000 page --
 * window offset 0x1F000.  (Palm's HwrM68SZ328Type labels that cluster
 * "$10000.." but places it at struct offset 0x1F000 via ___filler38;
 * the NR70V ROM demonstrably addresses it at 0xFFFFFxxx.)  Internally
 * the register file is stored at the Palm "$1xxxx" offsets, and the
 * MMIO window translates 0x1F000.. accesses onto it.
 */
#define DRAGONBALL_SZ_BASE      0xFFFE0000
#define DRAGONBALL_SZ_WINDOW    0x20000
#define DRAGONBALL_SZ_REGS_SIZE 0x11000

struct DragonBallSZState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    MemoryRegion dsp_mr;        /* Sony DSP (Memory Stick coprocessor) */
    uint8_t dsp[0x10000];
    bool dsp_irq;               /* the DSP's irq line (port D bit 3) */
    bool pen_down;              /* digitizer PENIRQ (ports G5/K4) */
    uint8_t port_edge[13];      /* latched rising edges per GPIO port */

    /*
     * Bit-banged ADS7846 touch ADC (CS = port B5, DOUT = port P6,
     * CLK = port N7, DIN = port G4 -- the NR70V HAL's pen sampler at
     * 0x1008f400) plus the QEMU pointer state feeding it.
     */
    QemuInputHandlerState *hs;
    int pen_x, pen_y;           /* 0..32767 */
    bool pen_btn;
    uint8_t bb_cmd;
    int bb_cmd_bits;
    uint16_t bb_resp;
    int bb_resp_bits;
    bool bb_clk;
    bool bb_din;
    MemoryRegion *fbmem;        /* main system memory, for LCD scanout */
    QemuConsole *con;

    /* the CPU whose interrupt level/vector the on-chip INTC drives */
    ArchCPU *cpu;
    /* UART 1 (the cradle serial / HAL debug port) */
    CharFrontend chr;

    /*
     * Dynamic peripheral state (authoritative; the corresponding
     * bytes in regs[] are refreshed from these on guest reads).
     */
    uint32_t int_pending;       /* INTC: (hi << 16) | lo */
    uint8_t cpu_level;          /* interrupt level currently presented */
    /*
     * General-purpose timers 1/2.  Each is a free-running 16-bit
     * up-counter (TCN) modelled as an anchor timestamp on the virtual
     * clock plus a deadline timer for the TCMP compare event.  TCN
     * must NOT be disturbed by TCMP writes: the Palm HAL implements
     * its sub-tick (pen-sampling etc.) timers by rewriting
     * TCMP = TCN + delta on the fly and relies on the counter
     * free-running underneath (an earlier ptimer-based model reloaded
     * the counter on every TCMP write, which eventually wedged the
     * timer and with it the whole 100Hz system tick).
     */
    QEMUTimer *tmr_qt[2];       /* TCMP compare deadline */
    int64_t tmr_anchor_ns[2];   /* when TCN was 0 (while running) */
    int64_t tmr_deadline_ns[2]; /* armed deadline (drift-free re-anchor) */
    uint16_t tmr_frozen_tcn[2]; /* TCN while stopped (disabled/no clock) */
    bool tmr_running[2];
    bool tmr_enabled[2];        /* last seen TCTL enable bit (edge detect) */
    uint32_t tmr_freq[2];       /* cached input clock after prescaler */
    uint16_t tmr_status[2];
    uint16_t tmr_last_status[2]; /* status bits the guest has read (ack gate) */
    uint8_t port_out[13];       /* GPIO output latches, ports A..R */

    uint8_t regs[DRAGONBALL_SZ_REGS_SIZE];
};

#endif
