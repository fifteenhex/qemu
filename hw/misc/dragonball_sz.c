/*
 * MC68SZ328 "DragonBall Super VZ" SoC register block + on-chip enhanced
 * TFT color LCD controller (Sony CLIE PEG-SJ33 / NR70V).
 *
 * Register map, reset values and peripheral semantics from
 * Cloudpilot-emu's hardware/EmRegsSZ.{h,cpp},
 * hardware/clie/EmRegsSZRedwood.cpp and the HwrM68SZ328Type struct in
 * palm/.../IncsPrv/M68SZ328Hwr.h (see CLIE-POC-NOTES.md's SJ33/SZ328
 * sections).  Register offsets below use the Palm struct's "$1xxxx"
 * labels; note the classic-peripheral cluster ($10000..) actually
 * decodes at 0xFFFFF000 -- see sz_reg_off() and dragonball_sz.h.
 *
 * Modelled dynamically (enough to run the Palm OS 4.1 HAL off a real
 * SZ328 ROM to the UI):
 *   - the interrupt controller ($10300: vector/mask/status/pending +
 *     the 7 level-control registers), driving the m68k core's
 *     level/vector pair;
 *   - general-purpose timers 1 and 2 ($10600/$10610), clocked from the
 *     guest-programmed MCU PLL (pllFreqSel0/1 -> DMA/CPU clock ->
 *     pllControl sysclk divider), the source of the Palm OS tick;
 *   - UART 1 status ($10900) so the HAL serial-debug path drains
 *     against a real transmitter-ready bit (TX data goes to a chardev);
 *   - GPIO port data reads ($10400..) composed from direction/select
 *     and the PEG-NR70V ("Redwood") board input levels, plus the
 *     per-pin port interrupts (D/E/F/G/J/K/M/N/P/R);
 *   - RTC time-of-day reads and the interrupt plumbing for the
 *     alarm/second status bits ($10B00);
 *   - the Sony Memory Stick DSP coprocessor's IPC mailbox (a second
 *     MMIO region the board maps at 0x11000000);
 *   - the bit-banged ADS7846 touch/battery ADC hanging off GPIOs.
 * Everything else is plain big-endian storage with the documented
 * reset values seeded, and the enhanced on-chip LCDC scanout (reads
 * the framebuffer straight out of main DRAM via address_space_read,
 * 1/2/4/8/16bpp, 12-bit CLUT).  NB the NR70V/SJ33 display is NOT this
 * on-chip LCDC (which their HAL leaves unprogrammed) but the external
 * "T2" MediaQ-class controller, see hw/display/clie_lcd.c.
 *
 * Debug aids (env-gated, off by default): SZ_TRACE_DSP traces the DSP
 * IPC mailbox, SZ_TRACE_PEN the completed ADC bit-bang transactions.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "exec/cpu-common.h"
#include "system/address-spaces.h"
#include "system/rtc.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/dragonball_sz.h"
#include "migration/vmstate.h"
#include "ui/pixel_ops.h"
#include "ui/input.h"
#include "cpu.h"

/* Register struct offsets (from the 0xFFFE0000 window base). */
#define SZ_LCD_START_ADDR   0x00800  /* u32: framebuffer base (in DRAM) */
#define SZ_LCD_SCREEN_SIZE  0x00804  /* u16: [15:9]=width/8, [8:0]=height */
#define SZ_LCD_PAGE_WIDTH   0x00806  /* u16: [9:0]=virtual page width /2 */
#define SZ_LCD_PANEL_CTL1   0x00814  /* u16: [11:9]=bpp selector */
#define SZ_LCD_PANNING      0x0081E  /* u16: [3:0]=panning offset */
#define SZ_LCD_CLUT         0x00A00  /* u16[256]: 12-bit colour LUT */
#define SZ_SCR              0x10000  /* u8:  system control */
#define SZ_CHIP_ID          0x10004  /* u8:  chip ID */
#define SZ_MASK_ID          0x10005  /* u8:  mask ID */
#define SZ_PLL_CONTROL      0x10200  /* u16 */
#define SZ_PLL_FREQSEL0     0x10202  /* u16 */
#define SZ_PLL_FREQSEL1     0x10204  /* u16 */
#define SZ_PWR_CONTROL      0x10207  /* u8 */
#define SZ_CLOCK_SRC_CTL    0x1020C  /* u16 */

/* Interrupt controller */
#define SZ_INT_VECTOR       0x10300  /* u8:  vector base (bits 7:3) */
#define SZ_INT_CONTROL      0x10302  /* u16: IRQ1/2/3/6 edge/polarity */
#define SZ_INT_MASK_HI      0x10304  /* u16 */
#define SZ_INT_MASK_LO      0x10306  /* u16 */
#define SZ_INT_STATUS_HI    0x1030C  /* u16: pending & ~mask (read), ack (write) */
#define SZ_INT_STATUS_LO    0x1030E  /* u16 */
#define SZ_INT_PENDING_HI   0x10310  /* u16 (read-only) */
#define SZ_INT_PENDING_LO   0x10312  /* u16 (read-only) */
#define SZ_INT_LEVEL(n)     (0x10314 + ((n) - 1) * 2)   /* n = 1..7 */

/*
 * Interrupt source bit numbers in the combined (hi << 16) | lo word,
 * per M68SZ328Hwr.h's hwrSZ328Int{Hi,Lo}* masks.
 */
#define SZ_INT_LCDC         0
#define SZ_INT_TMR1         1
#define SZ_INT_UART1        2
#define SZ_INT_WDT          3
#define SZ_INT_RTC          4
#define SZ_INT_TMR2         5
#define SZ_INT_IRQ1         16
#define SZ_INT_IRQ2         17
#define SZ_INT_IRQ3         18
#define SZ_INT_IRQ6         19
#define SZ_INT_EMU          23

/* GPIO: 13 ports (A B C D E F G J K M N P R), 8 bytes each */
#define SZ_PORT_BASE        0x10400
#define SZ_PORT_COUNT       13
#define SZ_PORT_DIR         0        /* +0: direction */
#define SZ_PORT_DATA        1        /* +1: data */
#define SZ_PORT_PUEN        2        /* +2: pull-up/down enable */
#define SZ_PORT_SEL         3        /* +3: select (1 = GPIO) */

/* General-purpose timers */
#define SZ_TMR_BASE(i)      (0x10600 + (i) * 0x10)
#define SZ_TMR_CONTROL      0x0
#define SZ_TMR_PRESCALER    0x2
#define SZ_TMR_COMPARE      0x4
#define SZ_TMR_CAPTURE      0x6
#define SZ_TMR_COUNTER      0x8
#define SZ_TMR_STATUS       0xA

#define SZ_TMR_CTL_FREERUN  0x0100
#define SZ_TMR_CTL_IRQEN    0x0010
#define SZ_TMR_CTL_ENABLE   0x0001
#define SZ_TMR_STAT_COMP    0x0001

/* UARTs */
#define SZ_UART_BASE(i)     (0x10900 + (i) * 0x10)
#define SZ_UART_CONTROL     0x0
#define SZ_UART_BAUD        0x2
#define SZ_UART_RECEIVE     0x4
#define SZ_UART_TRANSMIT    0x6
#define SZ_UART_MISC        0x8

/*
 * uTransmit status: FIFOEmpty | FIFOHalf | TxAvail.  CTSStatus is
 * deliberately NOT set: the NR70V ROM's debug stub takes an asserted
 * CTS as "a debugger host is attached to the cradle" and parks in its
 * wait-for-command loop instead of booting.
 */
#define SZ_UART_TX_READY    0xE000

/* RTC */
#define SZ_RTC_HOURMINSEC   0x10B00  /* u32: hours<<24 | min<<16 | sec */
#define SZ_RTC_ALARM        0x10B04  /* u32 */
#define SZ_RTC_WATCHDOG     0x10B0A  /* u16 */
#define SZ_RTC_CONTROL      0x10B0C  /* u16: bit 7 = RTC enable */
#define SZ_RTC_INT_STATUS   0x10B0E  /* u16: write-1-to-clear */
#define SZ_RTC_INT_ENABLE   0x10B10  /* u16 */
#define SZ_RTC_DAY          0x10B1A  /* u16 */

#define SZ_RTC_CTL_ENABLE   0x0080
/* Sec | 24Hr | Alarm | Minute | StopWatch: the bits that post the RTC irq */
#define SZ_RTC_INT_BITS     0x001F

static void sz_put16(DragonBallSZState *s, unsigned int off, uint16_t v)
{
    s->regs[off] = v >> 8;
    s->regs[off + 1] = v;
}

static uint16_t sz_get16(DragonBallSZState *s, unsigned int off)
{
    return (s->regs[off] << 8) | s->regs[off + 1];
}

static uint32_t sz_get32(DragonBallSZState *s, unsigned int off)
{
    return (s->regs[off] << 24) | (s->regs[off + 1] << 16) |
           (s->regs[off + 2] << 8) | s->regs[off + 3];
}

/* Does the [addr, addr+size) access touch [off, off+len)? */
static bool sz_overlap(hwaddr addr, unsigned size, unsigned int off,
                       unsigned int len)
{
    return addr < off + len && addr + size > off;
}

/*
 * ------------------------- Interrupt controller -------------------------
 */

/*
 * Interrupt level of source bit <idx>, per EmRegsSZ::GetInterruptLevel:
 * IRQ1/2/3/6 and EMU are fixed, everything else comes from a 4-bit
 * field in one of the seven intLevelControl registers.
 */
static unsigned int sz_int_level(DragonBallSZState *s, unsigned int idx)
{
    unsigned int level;

    switch (idx) {
    case SZ_INT_LCDC:  level = sz_get16(s, SZ_INT_LEVEL(2)) >> 4;  break;
    case SZ_INT_TMR1:  level = sz_get16(s, SZ_INT_LEVEL(7)) >> 0;  break;
    case SZ_INT_UART1: level = sz_get16(s, SZ_INT_LEVEL(4)) >> 0;  break;
    case SZ_INT_WDT:   level = sz_get16(s, SZ_INT_LEVEL(4)) >> 4;  break;
    case SZ_INT_RTC:   level = sz_get16(s, SZ_INT_LEVEL(4)) >> 8;  break;
    case SZ_INT_TMR2:  level = sz_get16(s, SZ_INT_LEVEL(1)) >> 0;  break;
    case 6:  /* port J */ level = sz_get16(s, SZ_INT_LEVEL(6)) >> 0;  break;
    case 7:  /* PWM 1 */  level = sz_get16(s, SZ_INT_LEVEL(3)) >> 0;  break;
    case 8:  /* port G */ level = sz_get16(s, SZ_INT_LEVEL(6)) >> 4;  break;
    case 9:  /* port F */ level = sz_get16(s, SZ_INT_LEVEL(6)) >> 8;  break;
    case 10: /* port E */ level = sz_get16(s, SZ_INT_LEVEL(6)) >> 12; break;
    case 11: /* port D */ level = sz_get16(s, SZ_INT_LEVEL(5)) >> 0;  break;
    case 12: /* UART 2 */ level = sz_get16(s, SZ_INT_LEVEL(1)) >> 8;  break;
    case 13: /* PWM 2 */  level = sz_get16(s, SZ_INT_LEVEL(1)) >> 4;  break;
    case 14: /* DMA 2 */  level = sz_get16(s, SZ_INT_LEVEL(3)) >> 4;  break;
    case 15: /* DMA 1 */  level = sz_get16(s, SZ_INT_LEVEL(3)) >> 8;  break;
    case SZ_INT_IRQ1:  return 1;
    case SZ_INT_IRQ2:  return 2;
    case SZ_INT_IRQ3:  return 3;
    case SZ_INT_IRQ6:  return 6;
    case 20: /* port R */ level = sz_get16(s, SZ_INT_LEVEL(7)) >> 4;  break;
    case 21: /* CSPI */   level = sz_get16(s, SZ_INT_LEVEL(1)) >> 12; break;
    case 22: /* RTC sample timer */
                          level = sz_get16(s, SZ_INT_LEVEL(3)) >> 12; break;
    case SZ_INT_EMU:   return 7;
    case 24: /* ADC */    level = sz_get16(s, SZ_INT_LEVEL(2)) >> 0;  break;
    case 25: /* port P */ level = sz_get16(s, SZ_INT_LEVEL(5)) >> 4;  break;
    case 26: /* port N */ level = sz_get16(s, SZ_INT_LEVEL(5)) >> 8;  break;
    case 27: /* port M */ level = sz_get16(s, SZ_INT_LEVEL(5)) >> 12; break;
    case 28: /* port K */ level = sz_get16(s, SZ_INT_LEVEL(4)) >> 12; break;
    case 29: /* MMC */    level = sz_get16(s, SZ_INT_LEVEL(2)) >> 8;  break;
    case 30: /* I2C */    level = sz_get16(s, SZ_INT_LEVEL(2)) >> 12; break;
    case 31: /* USB */    level = sz_get16(s, SZ_INT_LEVEL(7)) >> 8;  break;
    default: return 0;
    }

    level &= 0xf;
    return level > 7 ? 7 : level;
}

static uint32_t sz_int_status(DragonBallSZState *s)
{
    uint32_t mask = ((uint32_t)sz_get16(s, SZ_INT_MASK_HI) << 16) |
                    sz_get16(s, SZ_INT_MASK_LO);

    return s->int_pending & ~mask;
}

/*
 * Recompute intStatus and present the highest active level (and the
 * matching vector, intVector[7:3] + level) to the CPU core.
 */
static void sz_intc_update(DragonBallSZState *s)
{
    uint32_t status = sz_int_status(s);
    unsigned int i, top = 0;

    sz_put16(s, SZ_INT_STATUS_HI, status >> 16);
    sz_put16(s, SZ_INT_STATUS_LO, status);
    sz_put16(s, SZ_INT_PENDING_HI, s->int_pending >> 16);
    sz_put16(s, SZ_INT_PENDING_LO, s->int_pending);

    for (i = 0; i < 32; i++) {
        if (status & (1u << i)) {
            unsigned int level = sz_int_level(s, i);

            if (level > top) {
                top = level;
            }
        }
    }

    if (s->cpu && top != s->cpu_level) {
        m68k_set_irq_level(M68K_CPU(s->cpu),
                           top, (s->regs[SZ_INT_VECTOR] & 0xF8) + top);
        s->cpu_level = top;
    }
}

static void sz_int_set_pending(DragonBallSZState *s, unsigned int idx,
                               bool set)
{
    if (set) {
        s->int_pending |= 1u << idx;
    } else {
        s->int_pending &= ~(1u << idx);
    }
    sz_intc_update(s);
}

/*
 * Writes to intStatusHi ack the edge-latched external sources: an IRQn
 * pending bit is cleared by writing a one to its status bit while its
 * edge bit in intControl is set; EMU (and, per Cloudpilot, IRQ6 - the
 * power key) are cleared unconditionally (EmRegsSZ::intStatusHiWrite).
 */
static void sz_int_status_ack(DragonBallSZState *s, uint16_t value)
{
    uint16_t ctl = sz_get16(s, SZ_INT_CONTROL);
    uint32_t clear = 0;

    if ((ctl & 0x0800) && (value & 0x0001)) clear |= 1u << SZ_INT_IRQ1;
    if ((ctl & 0x0400) && (value & 0x0002)) clear |= 1u << SZ_INT_IRQ2;
    if ((ctl & 0x0200) && (value & 0x0004)) clear |= 1u << SZ_INT_IRQ3;
    if ((ctl & 0x0100) && (value & 0x0008)) clear |= 1u << SZ_INT_IRQ6;
    if (value & 0x0080) clear |= 1u << SZ_INT_EMU;
    clear |= 1u << SZ_INT_IRQ6;

    s->int_pending &= ~clear;
    sz_intc_update(s);
}

/*
 * ------------------------------- Timers -------------------------------
 */

/*
 * The MCU PLL clock from the guest-programmed frequency-select
 * registers, divided down to the DMA/CPU clock
 * (EmRegsSZ::GetSystemClockFrequency).  With the HAL-programmed PLL
 * values this lands at the SJ33/NR70V's 66MHz.
 */
static uint32_t sz_cpu_clock(DragonBallSZState *s)
{
    uint32_t mpfsr0 = sz_get16(s, SZ_PLL_FREQSEL0);
    uint32_t mpfsr1 = sz_get16(s, SZ_PLL_FREQSEL1);
    uint32_t mmfi = (mpfsr0 >> 11) & 0x0f;
    uint32_t mmfn = mpfsr0 & 0x3ff;
    uint32_t mmfd = (mpfsr1 + 1) & 0x3ff;
    uint32_t mpdf = ((mpfsr1 >> 11) + 1) & 0x0f;
    uint32_t cscr = sz_get16(s, SZ_CLOCK_SRC_CTL);
    uint32_t div = (cscr & (1 << 10)) ? 1 : 2 << ((cscr >> 8) & 0x03);
    uint64_t pll;

    if (!mmfd || !mpdf) {
        return 0;
    }
    pll = (2ull * 32768 * 512 * ((uint64_t)mmfi * mmfd + mmfn)) /
          ((uint64_t)mpdf * mmfd);

    return pll / div;
}

/* The system clock feeding the timers (EmRegsSZ::GetSysClk). */
static uint32_t sz_sys_clock(DragonBallSZState *s)
{
    uint32_t pllcr = sz_get16(s, SZ_PLL_CONTROL);
    uint32_t div = (pllcr & (1 << 10)) ? 1 : 2 << ((pllcr >> 8) & 0x03);

    return sz_cpu_clock(s) / div;
}

/* Timer input clock after the clock-source mux and prescaler. */
static uint32_t sz_timer_freq(DragonBallSZState *s, int i)
{
    uint16_t ctl = sz_get16(s, SZ_TMR_BASE(i) + SZ_TMR_CONTROL);
    uint32_t presc = (sz_get16(s, SZ_TMR_BASE(i) + SZ_TMR_PRESCALER) & 0xff) + 1;
    uint32_t freq;

    switch ((ctl >> 1) & 0x7) {
    case 1:
        freq = sz_sys_clock(s);
        break;
    case 2:
        freq = sz_sys_clock(s) / 16;
        break;
    default:
        /* 1xx: the 32.768kHz crystal; 0/3 (stop/TIN): no clock */
        freq = (ctl & 0x8) ? 32768 : 0;
        break;
    }

    return freq / presc;
}

/* TCN at a given virtual-clock time (16-bit, free-running). */
static uint16_t sz_timer_tcn_at(DragonBallSZState *s, int i, int64_t now)
{
    if (!s->tmr_running[i] || !s->tmr_freq[i]) {
        return s->tmr_frozen_tcn[i];
    }
    return (uint16_t)muldiv64(now - s->tmr_anchor_ns[i], s->tmr_freq[i],
                              NANOSECONDS_PER_SECOND);
}

/*
 * (Re)arm the compare deadline from the current TCN.  A TCMP equal to
 * the current TCN means "one full 16-bit wrap away" (the counter has
 * just matched or must run all the way around), never "now": the HAL's
 * tick ISR advances TCMP *after* the match it is servicing.
 */
static void sz_timer_arm(DragonBallSZState *s, int i, int64_t now)
{
    uint16_t cmp = sz_get16(s, SZ_TMR_BASE(i) + SZ_TMR_COMPARE);
    uint16_t tcn = sz_timer_tcn_at(s, i, now);
    uint32_t delta = (uint16_t)(cmp - tcn);

    if (delta == 0) {
        delta = 0x10000;
    }
    s->tmr_deadline_ns[i] = now + muldiv64(delta, NANOSECONDS_PER_SECOND,
                                           s->tmr_freq[i]);
    timer_mod(s->tmr_qt[i], s->tmr_deadline_ns[i]);
    if (getenv("SZ_TRACE_TMR")) {
        fprintf(stderr, "TMR%d arm tcn=%04x cmp=%04x delta=%x dl=%" PRId64 "\n",
                i + 1, tcn, cmp, delta, s->tmr_deadline_ns[i]);
    }
}

/*
 * Re-evaluate a timer after a guest write to its registers (or a PLL /
 * clock-source change).  The counter value is carried across the
 * change: only a 0->1 edge of the TCTL enable bit resets TCN to 0
 * (TEN reset per the DragonBall datasheet); TCMP/TPRER writes leave
 * TCN alone.
 */
static void sz_timer_update(DragonBallSZState *s, int i)
{
    uint16_t ctl = sz_get16(s, SZ_TMR_BASE(i) + SZ_TMR_CONTROL);
    uint32_t freq = sz_timer_freq(s, i);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint16_t tcn = sz_timer_tcn_at(s, i, now);  /* under the old clock */
    bool enable = ctl & SZ_TMR_CTL_ENABLE;

    if (enable && !s->tmr_enabled[i]) {
        tcn = 0;                                /* TEN 0->1 resets TCN */
    }
    s->tmr_enabled[i] = enable;
    s->tmr_freq[i] = freq;

    if (enable && freq) {
        s->tmr_anchor_ns[i] = now - muldiv64(tcn, NANOSECONDS_PER_SECOND,
                                             freq);
        s->tmr_running[i] = true;
        sz_timer_arm(s, i, now);
    } else {
        s->tmr_frozen_tcn[i] = tcn;
        s->tmr_running[i] = false;
        timer_del(s->tmr_qt[i]);
        if (getenv("SZ_TRACE_TMR")) {
            fprintf(stderr, "TMR%d stop ctl=%04x freq=%u tcn=%04x\n",
                    i + 1, ctl, freq, tcn);
        }
    }
}

static uint16_t sz_timer_counter(DragonBallSZState *s, int i)
{
    return sz_timer_tcn_at(s, i, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void sz_timer_hit(DragonBallSZState *s, int i)
{
    uint16_t ctl = sz_get16(s, SZ_TMR_BASE(i) + SZ_TMR_CONTROL);
    int64_t now = s->tmr_deadline_ns[i];

    s->tmr_status[i] |= SZ_TMR_STAT_COMP;
    if (ctl & SZ_TMR_CTL_IRQEN) {
        sz_int_set_pending(s, i ? SZ_INT_TMR2 : SZ_INT_TMR1, true);
    }
    if (getenv("SZ_TRACE_TMR")) {
        fprintf(stderr, "TMR%d hit ctl=%04x run=%d freq=%u\n",
                i + 1, ctl, s->tmr_running[i], s->tmr_freq[i]);
    }

    if (!s->tmr_running[i] || !s->tmr_freq[i]) {
        return;
    }
    if (!(ctl & SZ_TMR_CTL_FREERUN)) {
        /* restart mode: the match resets TCN to 0 (drift-free) */
        s->tmr_anchor_ns[i] = now;
    } else {
        /* keep the anchor within one wrap so the muldiv can't overflow */
        uint16_t tcn = sz_timer_tcn_at(s, i, now);

        s->tmr_anchor_ns[i] = now - muldiv64(tcn, NANOSECONDS_PER_SECOND,
                                             s->tmr_freq[i]);
    }
    sz_timer_arm(s, i, now);
}

static void sz_timer1_hit(void *opaque) { sz_timer_hit(opaque, 0); }
static void sz_timer2_hit(void *opaque) { sz_timer_hit(opaque, 1); }

/*
 * TSTAT ack, per EmRegsSZ::tmr1StatusWrite: a status bit may only be
 * cleared (by writing zero to it) if the guest has read it as set
 * since the last ack - otherwise a compare that fired between the read
 * and the write would be lost.
 */
static void sz_timer_status_write(DragonBallSZState *s, int i, uint16_t value)
{
    s->tmr_status[i] &= value | ~s->tmr_last_status[i];
    s->tmr_last_status[i] = 0;

    if (!(s->tmr_status[i] & SZ_TMR_STAT_COMP)) {
        sz_int_set_pending(s, i ? SZ_INT_TMR2 : SZ_INT_TMR1, false);
    }
}

/*
 * -------------------------------- GPIO --------------------------------
 *
 * Board input levels of the PEG-NR70V "Redwood" (the SJ33's SoC
 * sibling), from EmRegsSzRedwood::GetPortInputValue /
 * GetPortInternalValue and the board wiring: port C (keyboard rows)
 * reads all-high, port G bit 1 high, port J all-high, port P 0x0A plus
 * DC_IN (bit 7, "on the charger"), and the dedicated-function level of
 * port D is 0x80 - /LOWB_IRQ (power fail, active low) deasserted.
 *
 * Port D input idles at 0x97: the keyboard columns (bits 0-2) and the
 * dock/HotSync button (bit 4) are active-low with pull-ups, and bit 7
 * doubles as the power-fail sense in GPIO mode.  The columns idling
 * high is what the debug big ROM's boot-key scan (trap #15 $A3B6 ->
 * 0x1008df44) needs: all-keys-up ORs to boot-flag word 0xC7F, whose
 * bit 2 makes the debugger stub run its console once and then trap #8
 * into the real Palm OS boot.  Columns reading 0 look like "every key
 * held" and park the ROM in its debugger console forever.
 */
/*
 * Port F bit 5 is the wired remote-commander (headphone remote) key
 * line, polled by the HAL's ExtKeyTickHandler (0x102cf300): a
 * debounced LOW means "remote key pressed" and enqueues the Sony
 * remote vchr 0x170C at tick rate -- with the line stuck low the
 * post-Setup Sony apps drown in remote-key events and loop an
 * (invisible) Rmc-library error dialog forever.  Idle HIGH = no key.
 */
                                  /*  A     B     C     D     E     F    */
static const uint8_t sz_port_input[SZ_PORT_COUNT] = {
    0x00, 0x00, 0xff, 0x97, 0x00, 0x20,
                                  /*  G     J     K     M     N     P    R */
    0x02, 0xff, 0x00, 0x00, 0x00, 0x8a, 0x00,
};
static const uint8_t sz_port_internal[SZ_PORT_COUNT] = {
    [3] = 0x80,                   /* D: /LOWB_IRQ high = battery OK */
};

/*
 * Board input levels, plus the live DSP irq line on port D bit 3 and
 * the digitizer pen-down line.  The HAL arms rising-edge pin
 * interrupts on port G bit 5 and port K bit 4 (traced live); both are
 * driven from the ADS784x PENIRQ.
 */
static uint8_t sz_port_input_value(DragonBallSZState *s, int port)
{
    uint8_t v = sz_port_input[port];

    if (port == 3 && s->dsp_irq) {
        v |= 0x08;
    }
    if (port == 6) {
        if (s->pen_down) {
            v |= 0x20;
        }
        if (s->bb_din) {
            v |= 0x10;      /* ADS7846 DIN, shifted out by the clock */
        }
    }
    if (port == 8 && s->pen_down) {
        v |= 0x10;
    }
    return v;
}

/*
 * Compose a port data read (EmRegsSZ::portXDataRead): output latch
 * where SEL and DIR say output, board input where SEL says GPIO input,
 * dedicated-function level where SEL is zero.
 */
static uint8_t sz_port_data(DragonBallSZState *s, int port)
{
    unsigned int base = SZ_PORT_BASE + port * 8;
    uint8_t dir = s->regs[base + SZ_PORT_DIR];
    uint8_t sel = s->regs[base + SZ_PORT_SEL];
    uint8_t out = s->port_out[port];

    return (out & sel & dir) |
           (sz_port_input_value(s, port) & sel & ~dir) |
           (sz_port_internal[port] & ~sel);
}

/* INTC source bit of each GPIO port's interrupt, 0 = none (ports A-C). */
static const uint8_t sz_port_int_bit[SZ_PORT_COUNT] = {
    /* A  B  C  D   E   F  G  J  K   M   N   P   R */
    0, 0, 0, 11, 10, 9, 8, 6, 28, 27, 26, 25, 20,
};

/*
 * Port pin interrupts (EmRegsSZ::UpdatePortXInterrupts): level mode
 * follows the pin against the polarity bit, edge mode uses the latched
 * rising edges; gated per-pin by the mask register and input direction.
 */
static void sz_port_irq_update(DragonBallSZState *s, int port)
{
    unsigned int base = SZ_PORT_BASE + port * 8;
    uint8_t dir = s->regs[base + SZ_PORT_DIR];
    uint8_t mask = s->regs[base + 4];
    uint8_t edge = s->regs[base + 6];
    uint8_t pol = s->regs[base + 7];
    uint8_t data = sz_port_input_value(s, port);
    uint8_t bits;

    if (!sz_port_int_bit[port]) {
        return;
    }
    bits = (~edge & data & ~pol) |
           (~edge & ~data & pol) |
           (edge & s->port_edge[port] & pol);
    /*
     * The pen pins (G5/K4) request as long as the pen is held: the
     * NR70V pen ISR (0x1008f520) acks the edge latch and then keeps
     * re-reading intPendingHi's port-K bit as its "pen still down"
     * level, so the raw line must keep the request asserted.
     */
    if (port == 6 && s->pen_down) {
        bits |= 0x20;
    }
    if (port == 8 && s->pen_down) {
        bits |= 0x10;
    }
    bits &= mask & ~dir;
    s->regs[base + 5] = bits;   /* interrupt status register */
    sz_int_set_pending(s, sz_port_int_bit[port], bits != 0);
}

/*
 * ---------------------- Sony DSP (Memory Stick) ----------------------
 *
 * The NR70/SJ33 board hangs Sony's Memory Stick DSP coprocessor at
 * chip select 0x11000000 (64KB: IPC mailbox registers + 32KB shared
 * memory at +0x8000).  The SZ HAL's boot path blocks on its IPC
 * handshake (cmpiw #0xFC00, 0x11000C06 at 0x10094070) before it will
 * bring up the rest of the hardware, so model the mailbox after
 * Cloudpilot's EmRegsSonyDSP: command register dispatch that sets the
 * status word and raises the IPC-done interrupt (the DSP irq line is
 * port D bit 3).  No Memory Stick is inserted: the media commands
 * answer with the "error" status nibble and the sense results stay 0.
 */
#define SZ_DSP_RESET        0x0204
#define SZ_DSP_INT_STATUS   0x0220
#define SZ_DSP_INT_MASK     0x0222
#define SZ_DSP_IPC_COMMAND  0x0C04
#define SZ_DSP_IPC_STATUS   0x0C06
#define SZ_DSP_IPC_RESULT_1 0x0C14

#define SZ_DSP_INT_IPC_DONE 0x0001
#define SZ_DSP_INT_MS_EJECT 0x0008
#define SZ_DSP_STATUS_MASK  0xFC00

static void sz_dsp_put16(DragonBallSZState *s, unsigned int off, uint16_t v)
{
    s->dsp[off] = v >> 8;
    s->dsp[off + 1] = v;
}

static uint16_t sz_dsp_get16(DragonBallSZState *s, unsigned int off)
{
    return (s->dsp[off] << 8) | s->dsp[off + 1];
}

/*
 * ---------------- Bit-banged ADS7846 touch/battery ADC ----------------
 *
 * The NR70V HAL samples the digitizer by bit-banging an ADS7846-class
 * ADC over GPIOs (routine at ROM 0x1008f400): chip select on port B
 * bit 5 (active low), the command shifted out on port P bit 6, the
 * clock on port N bit 7, and the 12-bit response read back on port G
 * bit 4 -- one leading busy bit, then the sample MSB-first, sampled
 * while the clock is high.  Command bytes seen: 0x9A (channel 1, Y)
 * and 0xDA (channel 5, X).
 */
static uint16_t sz_adc_sample(DragonBallSZState *s, unsigned int channel)
{
    /*
     * Convert the normalized pointer (0..32767 over the 320x480
     * console) to panel pixels and to an ADS7846 raw count.  Three
     * things must hold for the NR70V HAL's two-point PenCalibrate
     * (ROM 0x102d04a6) to accept the calibration:
     *
     *  - Channel mapping: the sampler issues command 0xDA (channel 5)
     *    for the X plate and 0x9A (channel 1) for the Y plate -- and it
     *    stores the 0xDA result as the point's X, the 0x9A result as Y
     *    (0x1008f3ca).  So channel 5 must carry X, channel 1 must carry
     *    Y (they are NOT x=1/y=5).
     *  - Polarity: a real resistive panel's raw count *decreases* as the
     *    screen coordinate grows (the Sony HAL inverts it again before
     *    PenCalibrate, giving screen-increasing calibration coords that
     *    the routine's ordering test -- point-2 > point-1 -- requires).
     *  - Range: top 0xF00 with uniform density k=8 keeps both axes in
     *    the 12-bit range (Y max 0xF00-0*8=0xF00, min 0xF00-479*8=8),
     *    so nothing wraps -- an earlier version overflowed Y and the
     *    calibration collected garbage.
     */
    uint32_t px = ((uint32_t)s->pen_x * 320) >> 15;     /* 0..319 */
    uint32_t py = ((uint32_t)s->pen_y * 480) >> 15;     /* 0..479 */

    switch (channel) {
    case 5:                     /* command 0xDA: X plate */
        return 0xF00 - px * 8;
    case 1:                     /* command 0x9A: Y plate */
        return 0xF00 - py * 8;
    case 3:                     /* Z1 (pressure) */
        return s->pen_down ? 0x400 : 0;
    case 4:                     /* Z2 */
        return 0x400;
    case 2:                     /* battery: healthy */
        return 0xB80;
    default:                    /* AUX etc: mid-scale */
        return 0x800;
    }
}

/* CS (port B bit 5, active low) edge: reset the shifter state. */
static void sz_adc_cs_update(DragonBallSZState *s)
{
    if (s->port_out[1] & 0x20) {
        s->bb_cmd = 0;
        s->bb_cmd_bits = 0;
        s->bb_resp_bits = 0;
        s->bb_din = false;
    }
}

/*
 * Clock (port N bit 7) rising edge: run the ADS7846 serial state
 * machine.  A single CS assertion carries *several* back-to-back
 * conversions -- the NR70V sampler (0x1008f400) reads the same axis up
 * to 10 times and keeps sampling until two consecutive results match
 * (stability filter), all under one CS-low.  So the responder cannot
 * assume one command per CS: it idles until the START bit (DIN high,
 * command bit 7) appears, collects the 8-bit command, then emits one
 * busy bit + 12 data bits (MSB first) on DOUT, and returns to idle so
 * the next conversion in the same frame decodes afresh.  (Modelling
 * only the first conversion made every retry after the first read back
 * 0, so the "stable" value converged on 0 and calibration collected
 * (0,0) for every tap.)
 */
static void sz_adc_clk_update(DragonBallSZState *s)
{
    bool clk = s->port_out[10] & 0x80;

    if (clk && !s->bb_clk && !(s->port_out[1] & 0x20)) {
        if (s->bb_resp_bits > 0) {
            /* shifting the response out, MSB (busy bit) first */
            s->bb_resp_bits--;
            s->bb_din = (s->bb_resp >> s->bb_resp_bits) & 1;
        } else {
            bool din_in = s->port_out[11] & 0x40;   /* guest DIN = port P6 */

            if (s->bb_cmd_bits == 0) {
                /* idle: a set START bit begins a command, else ignore */
                if (din_in) {
                    s->bb_cmd = 1;
                    s->bb_cmd_bits = 1;
                }
            } else {
                s->bb_cmd = (s->bb_cmd << 1) | din_in;
                if (++s->bb_cmd_bits == 8) {
                    s->bb_resp = sz_adc_sample(s, (s->bb_cmd >> 4) & 7) & 0xFFF;
                    s->bb_resp_bits = 13;   /* 1 busy + 12 data */
                    s->bb_cmd_bits = 0;
                    if (getenv("SZ_TRACE_PEN")) {
                        fprintf(stderr, "PEN cmd=0x%02x resp=0x%03x down=%d\n",
                                s->bb_cmd, s->bb_resp, s->pen_down);
                    }
                }
            }
            s->bb_din = false;
        }
    }
    s->bb_clk = clk;
}

/* Digitizer pen-down line (ADS784x PENIRQ, level = 1 while touched). */
static void sz_pen_irq(void *opaque, int n, int level)
{
    DragonBallSZState *s = opaque;
    bool down = level != 0;

    if (getenv("SZ_TRACE_PEN")) {
        fprintf(stderr, "PEN irq level=%d (was %d)\n", down, s->pen_down);
    }
    if (down && !s->pen_down) {
        s->port_edge[6] |= 0x20;    /* port G bit 5 */
        s->port_edge[8] |= 0x10;    /* port K bit 4 */
        /*
         * ...and the falling edge on the external IRQ1 pin (the HAL
         * programs intControl for an edge on IRQ1); the latch is
         * cleared by the guest's intStatusHi ack write.
         */
        s->int_pending |= 1u << SZ_INT_IRQ1;
    }
    s->pen_down = down;
    sz_port_irq_update(s, 6);
    sz_port_irq_update(s, 8);
    sz_intc_update(s);
}

static void sz_dsp_irq_update(DragonBallSZState *s)
{
    bool line = sz_dsp_get16(s, SZ_DSP_INT_STATUS) &
                sz_dsp_get16(s, SZ_DSP_INT_MASK);

    if (line && !s->dsp_irq) {
        s->port_edge[3] |= 0x08;    /* latch the rising edge on PD3 */
    }
    s->dsp_irq = line;
    sz_port_irq_update(s, 3);
}

static void sz_dsp_dispatch(DragonBallSZState *s, uint16_t cmd)
{
    int i;

    if (getenv("SZ_TRACE_DSP")) {
        fprintf(stderr, "SZDSP cmd=0x%04x args=%04x %04x %04x %04x %04x %04x\n",
                cmd, sz_dsp_get16(s, 0xC08), sz_dsp_get16(s, 0xC0A),
                sz_dsp_get16(s, 0xC0C), sz_dsp_get16(s, 0xC0E),
                sz_dsp_get16(s, 0xC10), sz_dsp_get16(s, 0xC12));
    }

    for (i = 0; i < 6; i++) {
        sz_dsp_put16(s, SZ_DSP_IPC_RESULT_1 + i * 2, 0);
    }

    switch (cmd) {
    case 0x0036:                /* firmware upload, type 1 */
    case 0x0037:                /* DSP init */
        sz_dsp_put16(s, SZ_DSP_IPC_STATUS, (cmd << 10) & SZ_DSP_STATUS_MASK);
        break;
    case 0x0C85:
        /*
         * Firmware upload, type 2.  Cloudpilot answers 0xFC00, but the
         * NR70V ROM's Pa1Lib (Sony audio) init polls for the command's
         * own status-field echo -- the same convention its 'IC' 0x4943
         * setup command uses -- and tears its 'SeAo' registration back
         * down when it times out (the very next system beep then
         * fatals on the missing registration).
         */
        sz_dsp_put16(s, SZ_DSP_IPC_STATUS, cmd & SZ_DSP_STATUS_MASK);
        break;
    case 0x1E01:                /* Memory Stick sense: results 0 = no card */
        sz_dsp_put16(s, SZ_DSP_IPC_STATUS, cmd & SZ_DSP_STATUS_MASK);
        break;
    case 0x0800:                /* IPC escape: latch args, no completion irq */
    case 0xC800:
        return;
    default:
        if ((cmd & 0x00FF) == 0x0001) {
            /* Memory Stick media commands: error, no card in the slot */
            sz_dsp_put16(s, SZ_DSP_IPC_STATUS,
                         (cmd & SZ_DSP_STATUS_MASK) | 0x000F);
        } else {
            /*
             * Anything else (e.g. the audio firmware's 0x4943 'IC'
             * setup command the NR70V HAL issues after the upload
             * sequence): acknowledge cleanly, or the Sony Audio
             * library aborts its install and the very first system
             * beep fatals on the missing 'SeAo' registration.
             */
            sz_dsp_put16(s, SZ_DSP_IPC_STATUS, cmd & SZ_DSP_STATUS_MASK);
        }
        break;
    }

    sz_dsp_put16(s, SZ_DSP_INT_STATUS,
                 sz_dsp_get16(s, SZ_DSP_INT_STATUS) | SZ_DSP_INT_IPC_DONE);
    sz_dsp_irq_update(s);
}

static uint64_t dragonball_sz_dsp_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    DragonBallSZState *s = opaque;
    uint64_t val = 0;
    unsigned int i;

    for (i = 0; i < size; i++) {
        val = (val << 8) | s->dsp[addr + i];
    }
    if (getenv("SZ_TRACE_DSP") && addr >= 0x200 && addr < 0xC20) {
        fprintf(stderr, "SZDSP rd off=0x%04x size=%u val=0x%" PRIx64 "\n",
                (unsigned)addr, size, val);
    }
    return val;
}

static void dragonball_sz_dsp_write(void *opaque, hwaddr addr, uint64_t value,
                                    unsigned size)
{
    DragonBallSZState *s = opaque;
    unsigned int i;
    uint16_t int_status = sz_dsp_get16(s, SZ_DSP_INT_STATUS);

    for (i = 0; i < size; i++) {
        s->dsp[addr + i] = value >> (8 * (size - 1 - i));
    }

    if (sz_overlap(addr, size, SZ_DSP_INT_STATUS, 2)) {
        /* read-only from the guest */
        sz_dsp_put16(s, SZ_DSP_INT_STATUS, int_status);
    }
    if (sz_overlap(addr, size, SZ_DSP_IPC_STATUS, 2)) {
        /* an IPC status write acks the IPC-done interrupt */
        sz_dsp_put16(s, SZ_DSP_INT_STATUS,
                     sz_dsp_get16(s, SZ_DSP_INT_STATUS) & ~SZ_DSP_INT_IPC_DONE);
        sz_dsp_irq_update(s);
    }
    if (sz_overlap(addr, size, SZ_DSP_INT_MASK, 2)) {
        sz_dsp_irq_update(s);
    }
    if (sz_overlap(addr, size, SZ_DSP_RESET, 2)) {
        sz_dsp_put16(s, SZ_DSP_IPC_STATUS, SZ_DSP_STATUS_MASK);
    }
    if (sz_overlap(addr, size, SZ_DSP_IPC_COMMAND, 2)) {
        sz_dsp_dispatch(s, sz_dsp_get16(s, SZ_DSP_IPC_COMMAND));
    }
}

static const MemoryRegionOps dragonball_sz_dsp_ops = {
    .read = dragonball_sz_dsp_read,
    .write = dragonball_sz_dsp_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * --------------------------------- RTC ---------------------------------
 */

static void sz_rtc_time_refresh(DragonBallSZState *s)
{
    struct tm tm;

    qemu_get_timedate(&tm, 0);
    s->regs[SZ_RTC_HOURMINSEC] = tm.tm_hour & 0x1f;
    s->regs[SZ_RTC_HOURMINSEC + 1] = 0;
    s->regs[SZ_RTC_HOURMINSEC + 2] = tm.tm_min & 0x3f;
    s->regs[SZ_RTC_HOURMINSEC + 3] = tm.tm_sec & 0x3f;
}

/* Post/clear the RTC interrupt source (EmRegsSZ::UpdateRTCInterrupts). */
static void sz_rtc_update(DragonBallSZState *s)
{
    bool enabled = sz_get16(s, SZ_RTC_CONTROL) & SZ_RTC_CTL_ENABLE;
    uint16_t pending = sz_get16(s, SZ_RTC_INT_STATUS) &
                       sz_get16(s, SZ_RTC_INT_ENABLE) & SZ_RTC_INT_BITS;

    sz_int_set_pending(s, SZ_INT_RTC, enabled && pending);
}

/*
 * ------------------------------ LCD scanout ------------------------------
 */

/* CLUT entry: 12-bit 0x0RGB -> XRGB8888 (EmRegsSZ::convertColor_12bit). */
static uint32_t sz_clut_pixel(DragonBallSZState *s, unsigned int idx)
{
    uint16_t v = sz_get16(s, SZ_LCD_CLUT + idx * 2);
    uint8_t r = (v >> 8) & 0xf;
    uint8_t g = (v >> 4) & 0xf;
    uint8_t b = v & 0xf;

    return rgb_to_pixel32(r | (r << 4), g | (g << 4), b | (b << 4));
}

static bool dragonball_sz_update_display(void *opaque)
{
    DragonBallSZState *s = opaque;
    DisplaySurface *surface;
    uint32_t fb = sz_get32(s, SZ_LCD_START_ADDR) & ~1u;
    uint16_t screen = sz_get16(s, SZ_LCD_SCREEN_SIZE);
    unsigned int width = (screen >> 9) * 8;
    unsigned int height = screen & 0x1ff;
    unsigned int vpw = (sz_get16(s, SZ_LCD_PAGE_WIDTH) & 0x3ff) * 2; /* bytes/line */
    unsigned int bpp = 1u << ((sz_get16(s, SZ_LCD_PANEL_CTL1) >> 9) & 0x7);
    uint32_t palette[256];
    uint8_t *line;
    uint32_t *dest;
    size_t linelen;
    unsigned int x, y;

    /*
     * Always finish the update (return true) even when there is nothing
     * to scan out yet -- otherwise a screendump / VNC refresh coroutine
     * parked in qemu_console_co_wait_update() wedges forever (the same
     * gfx_update() contract fixed for clie_lcd.c, see that file).
     */
    if (!width || !height || width > 1024 || height > 1024) {
        goto blank;
    }
    if (!vpw) {
        vpw = width * bpp / 8;
    }

    surface = qemu_console_surface(s->con);
    if (width != surface_width(surface) || height != surface_height(surface)) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
    }
    if (surface_bits_per_pixel(surface) != 32) {
        goto blank;
    }

    if (bpp > 16) {
        /* only 1/2/4/8/16bpp are real LCD modes; anything else is a
         * mid-programming transient -- don't try to scan it out */
        goto blank;
    }

    if (bpp <= 8) {
        for (x = 0; x < 256; x++) {
            palette[x] = sz_clut_pixel(s, x);
        }
    }

    /*
     * Allocate the line buffer big enough for every pixel access this
     * scanline makes (width * bytes-per-pixel, +2 slack for the 16bpp
     * high-byte read), independent of the programmed virtual page width
     * -- a too-small vpw (transient, mid-programming) must not cause an
     * out-of-bounds read.  Zero-fill, then copy in the real framebuffer
     * bytes (vpw of them) capped to the buffer.
     */
    linelen = (size_t)width * ((bpp <= 8) ? 1 : 2) + 2;
    if (vpw > linelen) {
        linelen = vpw;
    }
    line = g_malloc0(linelen);
    dest = surface_data(surface);
    for (y = 0; y < height; y++) {
        /* the framebuffer lives in main DRAM at the programmed address */
        address_space_read(&address_space_memory, fb + (hwaddr)y * vpw,
                           MEMTXATTRS_UNSPECIFIED, line, vpw ? vpw : linelen);
        for (x = 0; x < width; x++) {
            uint32_t pixel;

            switch (bpp) {
            case 1:
                pixel = palette[(line[x / 8] >> (7 - x % 8)) & 1];
                break;
            case 2:
                pixel = palette[(line[x / 4] >> (6 - x % 4 * 2)) & 3];
                break;
            case 4:
                pixel = palette[x % 2 ? line[x / 2] & 0xf : line[x / 2] >> 4];
                break;
            case 8:
                pixel = palette[line[x]];
                break;
            default: {
                /* 16bpp big-endian RGB565 (EmMemDoGet16 is big-endian) */
                uint16_t v = (line[x * 2] << 8) | line[x * 2 + 1];

                pixel = rgb_to_pixel32((v >> 11) << 3,
                                       ((v >> 5) & 0x3f) << 2,
                                       (v & 0x1f) << 3);
                break;
            }
            }
            *dest++ = pixel;
        }
    }
    g_free(line);

    qemu_console_update_full(s->con);
    return true;

blank:
    /*
     * Nothing valid to scan out yet (LCD controller not programmed, or
     * mid-programming transient): present a clean black 320x480 frame
     * (the SJ33/NR70V panel size) instead of leaving QEMU's default
     * "guest has not initialized the display" placeholder.  Always
     * return true so the screendump / VNC waiter is released.
     */
    surface = qemu_console_surface(s->con);
    if (!surface || surface_width(surface) != 320 ||
        surface_height(surface) != 480) {
        qemu_console_resize(s->con, 320, 480);
        surface = qemu_console_surface(s->con);
    }
    if (surface && surface_bits_per_pixel(surface) == 32) {
        memset(surface_data(surface), 0,
               (size_t)surface_width(surface) * surface_height(surface) * 4);
    }
    qemu_console_update_full(s->con);
    return true;
}

/*
 * ----------------------------- MMIO handlers -----------------------------
 */

/*
 * Refresh the regs[] bytes of any dynamically-computed register the
 * access touches, so the generic byte extraction below returns live
 * values whatever the access size/alignment.
 */
static void sz_read_refresh(DragonBallSZState *s, hwaddr addr, unsigned size)
{
    int i;

    if (sz_overlap(addr, size, SZ_INT_STATUS_HI, 8)) {
        uint32_t status = sz_int_status(s);

        sz_put16(s, SZ_INT_STATUS_HI, status >> 16);
        sz_put16(s, SZ_INT_STATUS_LO, status);
        sz_put16(s, SZ_INT_PENDING_HI, s->int_pending >> 16);
        sz_put16(s, SZ_INT_PENDING_LO, s->int_pending);
    }

    for (i = 0; i < 2; i++) {
        if (sz_overlap(addr, size, SZ_TMR_BASE(i) + SZ_TMR_COUNTER, 2)) {
            sz_put16(s, SZ_TMR_BASE(i) + SZ_TMR_COUNTER, sz_timer_counter(s, i));
        }
        if (sz_overlap(addr, size, SZ_TMR_BASE(i) + SZ_TMR_STATUS, 2)) {
            /* remember what the guest saw - the TSTAT ack gate */
            s->tmr_last_status[i] |= s->tmr_status[i];
            sz_put16(s, SZ_TMR_BASE(i) + SZ_TMR_STATUS, s->tmr_status[i]);
        }

        if (sz_overlap(addr, size, SZ_UART_BASE(i) + SZ_UART_RECEIVE, 2)) {
            /* no RX data pending, no errors */
            sz_put16(s, SZ_UART_BASE(i) + SZ_UART_RECEIVE, 0);
        }
        if (sz_overlap(addr, size, SZ_UART_BASE(i) + SZ_UART_TRANSMIT, 2)) {
            /* the transmitter always has room: the HAL debug path drains */
            sz_put16(s, SZ_UART_BASE(i) + SZ_UART_TRANSMIT, SZ_UART_TX_READY);
        }
    }

    if (sz_overlap(addr, size, SZ_PORT_BASE, SZ_PORT_COUNT * 8)) {
        for (i = 0; i < SZ_PORT_COUNT; i++) {
            if (sz_overlap(addr, size, SZ_PORT_BASE + i * 8 + SZ_PORT_DATA, 1)) {
                s->regs[SZ_PORT_BASE + i * 8 + SZ_PORT_DATA] = sz_port_data(s, i);
            }
        }
    }

    if (sz_overlap(addr, size, SZ_RTC_HOURMINSEC, 4)) {
        sz_rtc_time_refresh(s);
    }
}

/*
 * Translate a window offset to the internal (Palm "$1xxxx"-labelled)
 * register-file offset: the classic peripheral cluster at window
 * offset 0x1F000 (= 0xFFFFF000) is stored at 0x10000.  Accesses to the
 * hole in between hit nothing (~0 is returned as a no-register mark).
 */
static unsigned int sz_reg_off(hwaddr addr)
{
    if (addr >= 0x1F000) {
        return addr - 0x1F000 + 0x10000;
    }
    if (addr >= DRAGONBALL_SZ_REGS_SIZE) {
        return ~0u;
    }
    return addr;
}

static uint64_t dragonball_sz_read(void *opaque, hwaddr window_addr,
                                   unsigned size)
{
    DragonBallSZState *s = opaque;
    unsigned int addr = sz_reg_off(window_addr);
    uint64_t val = 0;
    unsigned int i;

    if (addr == ~0u) {
        return 0;
    }
    sz_read_refresh(s, addr, size);

    for (i = 0; i < size; i++) {
        val = (val << 8) | s->regs[addr + i];
    }
    return val;
}

static void dragonball_sz_write(void *opaque, hwaddr window_addr,
                                uint64_t value, unsigned size)
{
    DragonBallSZState *s = opaque;
    unsigned int addr = sz_reg_off(window_addr);
    unsigned int i;
    uint16_t rtc_status_old = 0;
    bool rtc_status_hit;

    if (addr == ~0u) {
        return;
    }
    rtc_status_hit = sz_overlap(addr, size, SZ_RTC_INT_STATUS, 2);
    if (rtc_status_hit) {
        rtc_status_old = sz_get16(s, SZ_RTC_INT_STATUS);
    }

    for (i = 0; i < size; i++) {
        s->regs[addr + i] = value >> (8 * (size - 1 - i));
    }

    /* --- interrupt controller --- */
    if (sz_overlap(addr, size, SZ_INT_STATUS_HI, 2)) {
        sz_int_status_ack(s, sz_get16(s, SZ_INT_STATUS_HI));
    }
    if (sz_overlap(addr, size, SZ_INT_VECTOR, 1) ||
        sz_overlap(addr, size, SZ_INT_CONTROL, 2) ||
        sz_overlap(addr, size, SZ_INT_MASK_HI, 4) ||
        sz_overlap(addr, size, SZ_INT_LEVEL(1), 14)) {
        sz_intc_update(s);
    }

    /* --- PLL / clocking: retime the timers --- */
    if (sz_overlap(addr, size, SZ_PLL_CONTROL, 6) ||
        sz_overlap(addr, size, SZ_CLOCK_SRC_CTL, 2)) {
        sz_timer_update(s, 0);
        sz_timer_update(s, 1);
    }

    /* --- timers --- */
    for (i = 0; i < 2; i++) {
        if (sz_overlap(addr, size, SZ_TMR_BASE(i), 6)) {
            sz_timer_update(s, i);
        }
        if (sz_overlap(addr, size, SZ_TMR_BASE(i) + SZ_TMR_STATUS, 2)) {
            sz_timer_status_write(s, i,
                                  sz_get16(s, SZ_TMR_BASE(i) + SZ_TMR_STATUS));
        }
    }

    /* --- UART 1/2 TX data --- */
    for (i = 0; i < 2; i++) {
        unsigned int txlo = SZ_UART_BASE(i) + SZ_UART_TRANSMIT + 1;

        if (sz_overlap(addr, size, txlo, 1)) {
            uint8_t byte = s->regs[txlo];

            if (i == 0 && qemu_chr_fe_backend_connected(&s->chr)) {
                qemu_chr_fe_write_all(&s->chr, &byte, 1);
            }
        }
    }

    /* --- GPIO output latches + pin interrupt configuration --- */
    if (sz_overlap(addr, size, SZ_PORT_BASE, SZ_PORT_COUNT * 8)) {
        for (i = 0; i < SZ_PORT_COUNT; i++) {
            unsigned int base = SZ_PORT_BASE + i * 8;

            if (sz_overlap(addr, size, base + SZ_PORT_DATA, 1)) {
                s->port_out[i] = s->regs[base + SZ_PORT_DATA];
                if (i == 1) {
                    sz_adc_cs_update(s);    /* ADS7846 CS on port B5 */
                }
                if (i == 10) {
                    sz_adc_clk_update(s);   /* ADS7846 CLK on port N7 */
                }
            }
            if (sz_overlap(addr, size, base + 5, 1)) {
                /* an int-status write acks the latched edges */
                s->port_edge[i] &= ~s->regs[base + 5];
                if (i == 8 && getenv("SZ_TRACE_PEN")) {
                    fprintf(stderr, "PEN Kack=0x%02x down=%d\n",
                            s->regs[base + 5], s->pen_down);
                }
            }
            if (i == 11 && sz_overlap(addr, size, base + SZ_PORT_DATA, 1) &&
                !(s->port_out[11] & 0x40)) {
                /*
                 * PP6 (ADC_CNVST, active low) driven low: the ADS784x
                 * conversion completes immediately -- latch the
                 * end-of-conversion rising edge the HAL waits for on
                 * PP3 (it dozes on that pin interrupt mid-measure).
                 */
                s->port_edge[11] |= 0x08;
            }
            if (sz_overlap(addr, size, base, 8)) {
                sz_port_irq_update(s, i);
            }
        }
    }

    /* --- RTC --- */
    if (rtc_status_hit) {
        /* status bits are cleared by writing ones (rtcIntStatusWrite) */
        sz_put16(s, SZ_RTC_INT_STATUS,
                 rtc_status_old & ~sz_get16(s, SZ_RTC_INT_STATUS));
    }
    if (rtc_status_hit ||
        sz_overlap(addr, size, SZ_RTC_CONTROL, 2) ||
        sz_overlap(addr, size, SZ_RTC_INT_ENABLE, 2)) {
        sz_rtc_update(s);
    }
}

static const MemoryRegionOps dragonball_sz_ops = {
    .read = dragonball_sz_read,
    .write = dragonball_sz_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};


/* QEMU pointer events -> pen position + PENIRQ. */
static void sz_pen_input_event(DeviceState *dev, QemuConsole *src,
                               QemuInputEvent *evt)
{
    DragonBallSZState *s = DRAGONBALL_SZ(dev);

    switch (evt->type) {
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            s->pen_btn = evt->btn.down;
        }
        break;
    case INPUT_EVENT_KIND_ABS: {
        int val = qemu_input_scale_axis(evt->abs.value,
                                        INPUT_EVENT_ABS_MIN,
                                        INPUT_EVENT_ABS_MAX, 0, 32767);

        if (evt->abs.axis == INPUT_AXIS_X) {
            s->pen_x = val;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            s->pen_y = val;
        }
        break;
    }
    default:
        break;
    }
}

static void sz_pen_input_sync(DeviceState *dev)
{
    DragonBallSZState *s = DRAGONBALL_SZ(dev);

    if (s->pen_btn != s->pen_down) {
        sz_pen_irq(s, 0, s->pen_btn);
    }
}

static const QemuInputHandler sz_pen_input_handler = {
    .name = "clie-sj33 pen",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = sz_pen_input_event,
    .sync = sz_pen_input_sync,
};

static void dragonball_sz_invalidate(void *opaque)
{
}

static const GraphicHwOps dragonball_sz_gfx_ops = {
    .invalidate = dragonball_sz_invalidate,
    .gfx_update = dragonball_sz_update_display,
};

static void dragonball_sz_reset(DeviceState *dev)
{
    DragonBallSZState *s = DRAGONBALL_SZ(dev);
    static const uint8_t port_reset[SZ_PORT_COUNT][4] = {
        /* dir, data, puen, sel - kInitial68SZ328RegisterValues */
        { 0x00, 0xff, 0xff, 0x00 },     /* A (no select register) */
        { 0x00, 0xff, 0xff, 0xff },     /* B */
        { 0x00, 0x00, 0xff, 0xff },     /* C */
        { 0x00, 0xff, 0xff, 0xff },     /* D */
        { 0x00, 0xff, 0xff, 0xff },     /* E */
        { 0x00, 0xff, 0xff, 0x87 },     /* F */
        { 0x00, 0x3f, 0x3d, 0x08 },     /* G */
        { 0x00, 0xff, 0xff, 0xef },     /* J */
        { 0x00, 0x0f, 0xff, 0x00 },     /* K */
        { 0x00, 0x20, 0x3f, 0x3f },     /* M */
        { 0x00, 0x0f, 0xff, 0xff },     /* N */
        { 0x00, 0x0f, 0xff, 0xff },     /* P */
        { 0x00, 0x0f, 0xff, 0xff },     /* R */
    };
    int i;

    memset(s->regs, 0, sizeof(s->regs));
    /* Identity + PLL reset values (EmRegsSZ kInitial68SZ328RegisterValues). */
    s->regs[SZ_SCR] = 0x1c;
    s->regs[SZ_CHIP_ID] = 0x56;
    s->regs[SZ_MASK_ID] = 0x01;
    s->regs[SZ_PWR_CONTROL] = 0x1f;
    sz_put16(s, SZ_PLL_CONTROL, 0x2414);
    sz_put16(s, SZ_PLL_FREQSEL0, 0x3ce8);
    sz_put16(s, SZ_PLL_FREQSEL1, 0x0900);
    sz_put16(s, SZ_CLOCK_SRC_CTL, 0x8a03);

    /* INTC: everything masked, default levels */
    sz_put16(s, SZ_INT_MASK_HI, 0x00ff);
    sz_put16(s, SZ_INT_MASK_LO, 0xffff);
    sz_put16(s, SZ_INT_LEVEL(1), 0x6533);
    sz_put16(s, SZ_INT_LEVEL(2), 0x6533);
    sz_put16(s, SZ_INT_LEVEL(3), 0x4666);
    sz_put16(s, SZ_INT_LEVEL(4), 0x4444);
    sz_put16(s, SZ_INT_LEVEL(5), 0x4444);
    sz_put16(s, SZ_INT_LEVEL(6), 0x4444);
    sz_put16(s, SZ_INT_LEVEL(7), 0x0546);
    s->int_pending = 0;
    s->cpu_level = 0;

    /* timers */
    for (i = 0; i < 2; i++) {
        sz_put16(s, SZ_TMR_BASE(i) + SZ_TMR_COMPARE, 0xffff);
        s->tmr_status[i] = 0;
        s->tmr_last_status[i] = 0;
        s->tmr_running[i] = false;
        s->tmr_enabled[i] = false;
        s->tmr_frozen_tcn[i] = 0;
        s->tmr_freq[i] = 0;
        timer_del(s->tmr_qt[i]);
    }

    /* GPIO */
    for (i = 0; i < SZ_PORT_COUNT; i++) {
        memcpy(&s->regs[SZ_PORT_BASE + i * 8], port_reset[i], 4);
        s->port_out[i] = port_reset[i][1];
        s->port_edge[i] = 0;
    }

    /* Sony DSP: IPC handshake ready, no Memory Stick in the slot */
    memset(s->dsp, 0, sizeof(s->dsp));
    s->dsp_irq = false;
    sz_dsp_put16(s, SZ_DSP_IPC_STATUS, SZ_DSP_STATUS_MASK);
    sz_dsp_put16(s, SZ_DSP_INT_STATUS, SZ_DSP_INT_MS_EJECT);

    /* RTC + assorted identity/status defaults */
    sz_put16(s, SZ_RTC_WATCHDOG, 0x0001);
    sz_put16(s, SZ_RTC_CONTROL, 0x0080);
    s->regs[0x1080C] = 0x81;    /* i2cStatus */
}

static void dragonball_sz_init(Object *obj)
{
    DragonBallSZState *s = DRAGONBALL_SZ(obj);

    s->tmr_qt[0] = timer_new_ns(QEMU_CLOCK_VIRTUAL, sz_timer1_hit, s);
    s->tmr_qt[1] = timer_new_ns(QEMU_CLOCK_VIRTUAL, sz_timer2_hit, s);
}

static void dragonball_sz_finalize(Object *obj)
{
    DragonBallSZState *s = DRAGONBALL_SZ(obj);

    timer_free(s->tmr_qt[0]);
    timer_free(s->tmr_qt[1]);
}

static void dragonball_sz_realize(DeviceState *dev, Error **errp)
{
    DragonBallSZState *s = DRAGONBALL_SZ(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &dragonball_sz_ops, s,
                          TYPE_DRAGONBALL_SZ, DRAGONBALL_SZ_WINDOW);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    memory_region_init_io(&s->dsp_mr, OBJECT(dev), &dragonball_sz_dsp_ops, s,
                          TYPE_DRAGONBALL_SZ ".sony-dsp", sizeof(s->dsp));
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->dsp_mr);

    qdev_init_gpio_in_named(dev, sz_pen_irq, "pen-irq", 1);
    s->hs = qemu_input_handler_register(dev, &sz_pen_input_handler);
    qemu_input_handler_activate(s->hs);

    s->con = qemu_graphic_console_create(dev, 0, &dragonball_sz_gfx_ops, s);
}

static const VMStateDescription vmstate_dragonball_sz = {
    .name = TYPE_DRAGONBALL_SZ,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, DragonBallSZState, DRAGONBALL_SZ_REGS_SIZE),
        VMSTATE_UINT32(int_pending, DragonBallSZState),
        VMSTATE_UINT8(cpu_level, DragonBallSZState),
        VMSTATE_TIMER_PTR(tmr_qt[0], DragonBallSZState),
        VMSTATE_TIMER_PTR(tmr_qt[1], DragonBallSZState),
        VMSTATE_INT64_ARRAY(tmr_anchor_ns, DragonBallSZState, 2),
        VMSTATE_INT64_ARRAY(tmr_deadline_ns, DragonBallSZState, 2),
        VMSTATE_UINT16_ARRAY(tmr_frozen_tcn, DragonBallSZState, 2),
        VMSTATE_BOOL_ARRAY(tmr_running, DragonBallSZState, 2),
        VMSTATE_BOOL_ARRAY(tmr_enabled, DragonBallSZState, 2),
        VMSTATE_UINT32_ARRAY(tmr_freq, DragonBallSZState, 2),
        VMSTATE_UINT16_ARRAY(tmr_status, DragonBallSZState, 2),
        VMSTATE_UINT16_ARRAY(tmr_last_status, DragonBallSZState, 2),
        VMSTATE_UINT8_ARRAY(port_out, DragonBallSZState, 13),
        VMSTATE_UINT8_ARRAY(dsp, DragonBallSZState, 0x10000),
        VMSTATE_BOOL(dsp_irq, DragonBallSZState),
        VMSTATE_UINT8_ARRAY(port_edge, DragonBallSZState, 13),
        VMSTATE_END_OF_LIST()
    }
};

static const Property dragonball_sz_properties[] = {
    DEFINE_PROP_LINK("m68k-cpu", DragonBallSZState, cpu,
                     TYPE_M68K_CPU, ArchCPU *),
    DEFINE_PROP_CHR("chardev", DragonBallSZState, chr),
};

static void dragonball_sz_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, dragonball_sz_reset);
    device_class_set_props(dc, dragonball_sz_properties);
    dc->realize = dragonball_sz_realize;
    dc->vmsd = &vmstate_dragonball_sz;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo dragonball_sz_info = {
    .name          = TYPE_DRAGONBALL_SZ,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DragonBallSZState),
    .instance_init = dragonball_sz_init,
    .instance_finalize = dragonball_sz_finalize,
    .class_init    = dragonball_sz_class_init,
};

static void dragonball_sz_register_types(void)
{
    type_register_static(&dragonball_sz_info);
}

type_init(dragonball_sz_register_types)
