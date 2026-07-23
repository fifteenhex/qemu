/*
 * QEMU Atari 1040STF hardware system emulator
 *
 * 68000 at 8 MHz, up to 4MB of RAM in two banks behind the GLUE/MMU
 * pair, TOS in 192KB of ROM at 0xFC0000, and the ST chip set: the
 * Shifter (paletted planar video), the MFP 68901 (GPIP interrupts,
 * timers, the 200Hz system tick), two MC6850 ACIAs carrying the IKBD
 * keyboard/mouse controller and MIDI, the YM2149 PSG (whose port A
 * drives the floppy select lines), and the WD1772 FDC behind the ST's
 * DMA controller.
 *
 * Memory map (68000, 24-bit bus):
 *   0x000000  RAM (the first 8 bytes read from ROM: the GLUE feeds the
 *             reset vector fetch from ROM, writes still land in RAM)
 *   0x400000  unassigned - bus errors, TOS sizes hardware by probing
 *   0xFA0000  cartridge port, reads float to 0xFF with nothing plugged
 *   0xFC0000  ROM 192KB
 *   0xFF8000  MMU memory config
 *   0xFF8200  Shifter video base/counter, sync mode
 *   0xFF8240  Shifter palette, 0xFF8260 resolution
 *   0xFF8604  FDC/HDC access port + DMA control (0xFF8606 mode/status,
 *             0xFF8609/0B/0D DMA base address)
 *   0xFF8800  YM2149 PSG
 *   0xFFFA01  MFP 68901 (odd bytes)
 *   0xFFFC00  keyboard ACIA, 0xFFFC04 MIDI ACIA
 *
 * Interrupts: MFP at IPL6 (vectored), GLUE VBL at IPL4 and HBL at IPL2
 * (autovectored, latched until the CPU's interrupt acknowledge - see
 * the "iack-out" lines on the m68k core).
 *
 * The RAM sizing dance is modelled for real: the MMU config at
 * 0xFF8001 selects a decoded window size per bank, and when the
 * configured size exceeds the installed chips the DRAM drops the top
 * column address bits, so TOS's probe pattern reappears at +0x208
 * (128K chips) or +0x408 (512K chips).  Absent banks read as zero
 * without bus faulting (the probe runs without any exception handler),
 * while ST-RAM addresses beyond the configured windows bus-error, which
 * is how TOS's phystop scan terminates.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/datadir.h"
#include "system/system.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "target/m68k/cpu.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/input.h"
#include "hw/display/framebuffer.h"
#include "system/qtest.h"
#include "system/reset.h"

#define ATARIST_RAM_WINDOW    0x400000
#define ATARIST_ADDR_SPACE    0x1000000   /* 24-bit bus */

#define ATARIST_CART_BASE     0xFA0000
#define ATARIST_CART_SIZE     0x20000

#define ATARIST_ROM_BASE      0xFC0000
#define ATARIST_ROM_SIZE      0x30000     /* 192KB TOS 1.0x */
#define ATARIST_ROM_FILENAME  "tos104uk.rom"

#define ATARIST_MEMCFG_BASE   0xFF8000
#define ATARIST_VIDEO_BASE    0xFF8200
#define ATARIST_DMA_BASE      0xFF8604
#define ATARIST_PSG_BASE      0xFF8800
#define ATARIST_MFP_BASE      0xFFFA00
#define ATARIST_ACIA_BASE     0xFFFC00

/* the MMU config: bank 0 in bits 3-2, bank 1 in bits 1-0 */
#define MEMCFG_BANK0(cfg)     (((cfg) >> 2) & 3)
#define MEMCFG_BANK1(cfg)     ((cfg) & 3)
#define MEMCFG_RESET          0x0a        /* 2MB/2MB */

/* the MFP timers run from a 2.4576 MHz XTAL */
#define MFP_XTAL_HZ           2457600

/* frame timings per the GLUE sync mode (colour: 50 or 60 Hz) */
#define FRAME_NS_50HZ         20000000
#define FRAME_LINES_50HZ      313
#define FRAME_NS_60HZ         16666667
#define FRAME_LINES_60HZ      263
#define FRAME_VISIBLE_LINES   200
#define FRAME_BYTES           32000
#define FRAME_LINE_BYTES      160

/*
 * ---------------------------------------------------------------------
 * MFP 68901
 * ---------------------------------------------------------------------
 */

#define TYPE_ATARIST_MFP "atarist-mfp"
OBJECT_DECLARE_SIMPLE_TYPE(AtaristMfpState, ATARIST_MFP)

/* register index = (odd address offset) >> 1 */
enum {
    MFP_REG_GPIP,
    MFP_REG_AER,
    MFP_REG_DDR,
    MFP_REG_IERA,
    MFP_REG_IERB,
    MFP_REG_IPRA,
    MFP_REG_IPRB,
    MFP_REG_ISRA,
    MFP_REG_ISRB,
    MFP_REG_IMRA,
    MFP_REG_IMRB,
    MFP_REG_VR,
    MFP_REG_TACR,
    MFP_REG_TBCR,
    MFP_REG_TCDCR,
    MFP_REG_TADR,
    MFP_REG_TBDR,
    MFP_REG_TCDR,
    MFP_REG_TDDR,
    MFP_REG_SCR,
    MFP_REG_UCR,
    MFP_REG_RSR,
    MFP_REG_TSR,
    MFP_REG_UDR,
};

/* interrupt channels, 15 = highest priority */
#define MFP_CH_GPIP7          15          /* monochrome detect */
#define MFP_CH_GPIP6          14          /* RS232 ring */
#define MFP_CH_TIMER_A        13
#define MFP_CH_RX_FULL        12
#define MFP_CH_RX_ERROR       11
#define MFP_CH_TX_EMPTY       10
#define MFP_CH_TX_ERROR       9
#define MFP_CH_TIMER_B        8
#define MFP_CH_GPIP5          7           /* FDC/HDC interrupt */
#define MFP_CH_GPIP4          6           /* keyboard/MIDI ACIAs */
#define MFP_CH_TIMER_C        5           /* the 200Hz system tick */
#define MFP_CH_TIMER_D        4
#define MFP_CH_GPIP3          3
#define MFP_CH_GPIP2          2
#define MFP_CH_GPIP1          1
#define MFP_CH_GPIP0          0

/* GPIP line assignment on the ST */
#define MFP_GPIP_BUSY         0           /* parallel port busy */
#define MFP_GPIP_DCD          1
#define MFP_GPIP_CTS          2
#define MFP_GPIP_GPU          3
#define MFP_GPIP_ACIA         4           /* active low */
#define MFP_GPIP_FDC          5           /* active low */
#define MFP_GPIP_RING         6
#define MFP_GPIP_MONO         7           /* low = mono monitor */

#define MFP_VR_SOFT_EOI       0x08

#define MFP_TIMER_A           0
#define MFP_TIMER_B           1
#define MFP_TIMER_C           2
#define MFP_TIMER_D           3

typedef struct AtaristVideoState AtaristVideoState;

typedef struct MfpTimer {
    uint8_t ctrl;               /* effective control bits (0-15) */
    uint8_t reload;             /* TxDR as written */
    uint8_t counter;            /* latched count while stopped */
    bool running;               /* delay mode active */
    int64_t expiry;             /* next terminal count */
    int64_t period_ns;
    /* event count mode (timer B counting Shifter DE lines) */
    bool event_mode;
    int64_t event_start;
    uint16_t event_count0;
} MfpTimer;

struct AtaristMfpState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;               /* to the GLUE, level */

    AtaristVideoState *video;   /* timer B counts its DE lines */

    uint8_t gpip_latch;
    uint8_t gpip_ext;           /* external line levels, not reset */
    uint8_t aer;
    uint8_t ddr;
    uint16_t ier, ipr, isr, imr;    /* A in the high byte */
    uint8_t vr;
    uint8_t scr, ucr, rsr, tsr, udr;

    MfpTimer t[4];
    QEMUTimer *qtimer[4];
};

static const uint16_t mfp_prescale[8] = { 0, 4, 10, 16, 50, 64, 100, 200 };

static const uint8_t mfp_gpip_channel[8] = {
    MFP_CH_GPIP0, MFP_CH_GPIP1, MFP_CH_GPIP2, MFP_CH_GPIP3,
    MFP_CH_GPIP4, MFP_CH_GPIP5, MFP_CH_GPIP6, MFP_CH_GPIP7,
};

static const uint8_t mfp_timer_channel[4] = {
    MFP_CH_TIMER_A, MFP_CH_TIMER_B, MFP_CH_TIMER_C, MFP_CH_TIMER_D,
};

static int64_t atarist_video_de_events(AtaristVideoState *v, int64_t t);
static int64_t atarist_video_de_event_time(AtaristVideoState *v,
                                           int64_t target);
static uint8_t mfp_timer_count(AtaristMfpState *s, int i);

static int mfp_highest(uint16_t bits)
{
    return bits ? 31 - clz32(bits) : -1;
}

/* the channel an IACK would take right now, or -1 */
static int mfp_pending_channel(AtaristMfpState *s)
{
    int ch = mfp_highest(s->ipr & s->imr);

    if (ch >= 0 && ch <= mfp_highest(s->isr)) {
        ch = -1;                /* blocked by an in-service channel */
    }
    return ch;
}

static void mfp_update_irq(AtaristMfpState *s)
{
    qemu_set_irq(s->irq, mfp_pending_channel(s) >= 0);
}

static uint8_t atarist_mfp_current_vector(AtaristMfpState *s)
{
    int ch = mfp_pending_channel(s);

    if (ch < 0) {
        /* raced away: hand out the spurious-interrupt vector */
        return 24;
    }
    return (s->vr & 0xf0) | ch;
}

/* the CPU acknowledged our IPL6 request */
static void atarist_mfp_iack(AtaristMfpState *s)
{
    int ch = mfp_pending_channel(s);

    if (ch < 0) {
        return;
    }
    s->ipr &= ~BIT(ch);
    if (s->vr & MFP_VR_SOFT_EOI) {
        s->isr |= BIT(ch);
    }
    mfp_update_irq(s);
}

static void mfp_set_pending(AtaristMfpState *s, int ch)
{
    if (s->ier & BIT(ch)) {
        s->ipr |= BIT(ch);
        mfp_update_irq(s);
    }
}

/* external GPIP lines */
static void mfp_gpip_set(void *opaque, int line, int level)
{
    AtaristMfpState *s = opaque;
    uint8_t old = (s->gpip_ext >> line) & 1;

    if (old == !!level) {
        return;
    }
    s->gpip_ext = deposit32(s->gpip_ext, line, 1, !!level);

    /* AER bit set: interrupt on the rising edge, clear: falling */
    if (!!level == !!(s->aer & BIT(line))) {
        mfp_set_pending(s, mfp_gpip_channel[line]);
    }
}

static int64_t mfp_tick_ns(int prescale_code)
{
    return (int64_t)mfp_prescale[prescale_code] * NANOSECONDS_PER_SECOND
           / MFP_XTAL_HZ;
}

static unsigned mfp_reload_ticks(uint8_t reload)
{
    return reload ? reload : 256;
}

static void mfp_timer_arm(AtaristMfpState *s, int i)
{
    MfpTimer *t = &s->t[i];

    t->period_ns = mfp_reload_ticks(t->reload) * mfp_tick_ns(t->ctrl & 7);
    t->expiry = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + t->period_ns;
    t->running = true;
    timer_mod(s->qtimer[i], t->expiry);
}

/*
 * Timer B event-count interrupts: with the channel enabled, schedule
 * the terminal count from the video frame phase (its event input is
 * the Shifter DE line, one event per visible scanline - games use it
 * as a raster interrupt).
 */
static void mfp_timer_b_event_arm(AtaristMfpState *s)
{
    MfpTimer *t = &s->t[MFP_TIMER_B];
    int64_t now, target;

    if (!t->event_mode || !s->video) {
        return;
    }
    if (!(s->ier & BIT(MFP_CH_TIMER_B))) {
        timer_del(s->qtimer[MFP_TIMER_B]);
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    target = atarist_video_de_events(s->video, now) + mfp_timer_count(s, MFP_TIMER_B);
    timer_mod(s->qtimer[MFP_TIMER_B],
              atarist_video_de_event_time(s->video, target));
}

static void mfp_timer_fire(void *opaque, int i)
{
    AtaristMfpState *s = opaque;
    MfpTimer *t = &s->t[i];

    if (t->event_mode) {
        mfp_set_pending(s, mfp_timer_channel[i]);
        /* reload and rearm on the frame phase */
        t->event_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        t->event_count0 = mfp_reload_ticks(t->reload);
        if (i == MFP_TIMER_B) {
            mfp_timer_b_event_arm(s);
        }
        return;
    }
    if (!t->running) {
        return;
    }
    mfp_set_pending(s, mfp_timer_channel[i]);
    /* reload */
    t->period_ns = mfp_reload_ticks(t->reload) * mfp_tick_ns(t->ctrl & 7);
    t->expiry += t->period_ns;
    if (t->expiry < qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        t->expiry = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + t->period_ns;
    }
    timer_mod(s->qtimer[i], t->expiry);
}

static void mfp_timer_fire_a(void *opaque) { mfp_timer_fire(opaque, 0); }
static void mfp_timer_fire_b(void *opaque) { mfp_timer_fire(opaque, 1); }
static void mfp_timer_fire_c(void *opaque) { mfp_timer_fire(opaque, 2); }
static void mfp_timer_fire_d(void *opaque) { mfp_timer_fire(opaque, 3); }

/* current countdown value for a running delay-mode timer */
static uint8_t mfp_timer_count(AtaristMfpState *s, int i)
{
    MfpTimer *t = &s->t[i];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (t->event_mode) {
        /*
         * Event count mode: timer B counts the Shifter's DE line (one
         * event per visible scanline).  TOS's pre-boot 50Hz switch
         * polls this to find the vertical blank, so compute the count
         * from the video frame phase instead of running a 15.7kHz
         * timer.
         */
        int64_t events = atarist_video_de_events(s->video, now)
                         - atarist_video_de_events(s->video, t->event_start);
        unsigned n0 = t->event_count0 ? t->event_count0 : 256;
        unsigned r = mfp_reload_ticks(t->reload);

        if (events < n0) {
            return n0 - events;
        }
        return r - ((events - n0) % r);
    }
    if (t->running) {
        int64_t rem = t->expiry - now;
        int64_t ticks;

        if (rem <= 0) {
            return 1;
        }
        ticks = DIV_ROUND_UP(rem, mfp_tick_ns(t->ctrl & 7));
        return MIN((int64_t)mfp_reload_ticks(t->reload), MAX(ticks, 1));
    }
    return t->counter;
}

static void mfp_timer_set_ctrl(AtaristMfpState *s, int i, uint8_t ctrl)
{
    MfpTimer *t = &s->t[i];
    uint8_t mode = ctrl & 0x0f;

    if (t->ctrl == mode) {
        return;
    }

    /* freeze the current count before switching */
    t->counter = mfp_timer_count(s, i);
    t->running = false;
    t->event_mode = false;
    timer_del(s->qtimer[i]);
    t->ctrl = mode;

    if (mode == 0) {
        return;                 /* stopped */
    }
    if (mode < 8) {
        mfp_timer_arm(s, i);
        return;
    }
    /* event or pulse mode: only timer B's DE event counting is real */
    if (i == MFP_TIMER_B && s->video) {
        t->event_mode = true;
        t->event_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        t->event_count0 = t->counter;
        mfp_timer_b_event_arm(s);
    } else {
        qemu_log_mask(LOG_UNIMP,
                      "atarist mfp: timer %c event/pulse mode 0x%x\n",
                      'A' + i, mode);
    }
}

static uint64_t atarist_mfp_read(void *opaque, hwaddr addr, unsigned size)
{
    AtaristMfpState *s = opaque;
    unsigned reg;

    if (!(addr & 1)) {
        return 0xff;            /* only the low data byte is wired */
    }
    reg = addr >> 1;
    switch (reg) {
    case MFP_REG_GPIP:
        return (s->gpip_ext & ~s->ddr) | (s->gpip_latch & s->ddr);
    case MFP_REG_AER:
        return s->aer;
    case MFP_REG_DDR:
        return s->ddr;
    case MFP_REG_IERA:
        return s->ier >> 8;
    case MFP_REG_IERB:
        return s->ier;
    case MFP_REG_IPRA:
        return s->ipr >> 8;
    case MFP_REG_IPRB:
        return s->ipr;
    case MFP_REG_ISRA:
        return s->isr >> 8;
    case MFP_REG_ISRB:
        return s->isr;
    case MFP_REG_IMRA:
        return s->imr >> 8;
    case MFP_REG_IMRB:
        return s->imr;
    case MFP_REG_VR:
        return s->vr;
    case MFP_REG_TACR:
        return s->t[MFP_TIMER_A].ctrl;
    case MFP_REG_TBCR:
        return s->t[MFP_TIMER_B].ctrl;
    case MFP_REG_TCDCR:
        return (s->t[MFP_TIMER_C].ctrl << 4) | s->t[MFP_TIMER_D].ctrl;
    case MFP_REG_TADR:
    case MFP_REG_TBDR:
    case MFP_REG_TCDR:
    case MFP_REG_TDDR:
        return mfp_timer_count(s, reg - MFP_REG_TADR);
    case MFP_REG_SCR:
        return s->scr;
    case MFP_REG_UCR:
        return s->ucr;
    case MFP_REG_RSR:
        return s->rsr & ~0x80;  /* receive buffer never full */
    case MFP_REG_TSR:
        return s->tsr | 0x80;   /* transmit buffer always empty */
    case MFP_REG_UDR:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "atarist mfp: read reg %u\n", reg);
        return 0;
    }
}

static void atarist_mfp_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    AtaristMfpState *s = opaque;
    unsigned reg;

    if (!(addr & 1)) {
        return;
    }
    reg = addr >> 1;
    switch (reg) {
    case MFP_REG_GPIP:
        s->gpip_latch = val;
        break;
    case MFP_REG_AER:
        s->aer = val;
        break;
    case MFP_REG_DDR:
        s->ddr = val;
        break;
    case MFP_REG_IERA:
        s->ier = deposit32(s->ier, 8, 8, val);
        s->ipr &= s->ier;       /* disabling a channel drops its pending */
        mfp_timer_b_event_arm(s);
        mfp_update_irq(s);
        break;
    case MFP_REG_IERB:
        s->ier = deposit32(s->ier, 0, 8, val);
        s->ipr &= s->ier;
        mfp_update_irq(s);
        break;
    case MFP_REG_IPRA:
        s->ipr &= (val << 8) | 0xff;    /* write zeroes to clear */
        mfp_update_irq(s);
        break;
    case MFP_REG_IPRB:
        s->ipr &= val | 0xff00;
        mfp_update_irq(s);
        break;
    case MFP_REG_ISRA:
        s->isr &= (val << 8) | 0xff;
        mfp_update_irq(s);
        break;
    case MFP_REG_ISRB:
        s->isr &= val | 0xff00;
        mfp_update_irq(s);
        break;
    case MFP_REG_IMRA:
        s->imr = deposit32(s->imr, 8, 8, val);
        mfp_update_irq(s);
        break;
    case MFP_REG_IMRB:
        s->imr = deposit32(s->imr, 0, 8, val);
        mfp_update_irq(s);
        break;
    case MFP_REG_VR:
        s->vr = val;
        if (!(s->vr & MFP_VR_SOFT_EOI)) {
            s->isr = 0;
        }
        mfp_update_irq(s);
        break;
    case MFP_REG_TACR:
        mfp_timer_set_ctrl(s, MFP_TIMER_A, val);
        break;
    case MFP_REG_TBCR:
        mfp_timer_set_ctrl(s, MFP_TIMER_B, val);
        break;
    case MFP_REG_TCDCR:
        mfp_timer_set_ctrl(s, MFP_TIMER_C, (val >> 4) & 7);
        mfp_timer_set_ctrl(s, MFP_TIMER_D, val & 7);
        break;
    case MFP_REG_TADR:
    case MFP_REG_TBDR:
    case MFP_REG_TCDR:
    case MFP_REG_TDDR: {
        int i = reg - MFP_REG_TADR;

        s->t[i].reload = val;
        if (!s->t[i].running && !s->t[i].event_mode) {
            s->t[i].counter = val;
        }
        break;
    }
    case MFP_REG_SCR:
        s->scr = val;
        break;
    case MFP_REG_UCR:
        s->ucr = val;
        break;
    case MFP_REG_RSR:
        s->rsr = val;
        break;
    case MFP_REG_TSR:
        s->tsr = val;
        break;
    case MFP_REG_UDR:
        s->udr = val;           /* serial transmit: discarded */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "atarist mfp: write reg %u <- 0x%02x\n",
                      reg, (unsigned)val);
        break;
    }
}

static const MemoryRegionOps atarist_mfp_ops = {
    .read = atarist_mfp_read,
    .write = atarist_mfp_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void atarist_mfp_reset_hold(Object *obj, ResetType type)
{
    AtaristMfpState *s = ATARIST_MFP(obj);
    int i;

    s->gpip_latch = 0;
    s->aer = 0;
    s->ddr = 0;
    s->ier = s->ipr = s->isr = s->imr = 0;
    s->vr = 0;
    s->scr = s->ucr = s->rsr = s->tsr = s->udr = 0;
    for (i = 0; i < 4; i++) {
        s->t[i].ctrl = 0;
        s->t[i].reload = 0;
        s->t[i].counter = 0;
        s->t[i].running = false;
        s->t[i].event_mode = false;
        timer_del(s->qtimer[i]);
    }
    /* gpip_ext is external line state and survives the chip reset */
    mfp_update_irq(s);
}

static void atarist_mfp_init(Object *obj)
{
    AtaristMfpState *s = ATARIST_MFP(obj);

    /* idle line levels: ACIA/FDC interrupts high, colour monitor high */
    s->gpip_ext = BIT(MFP_GPIP_ACIA) | BIT(MFP_GPIP_FDC) |
                  BIT(MFP_GPIP_MONO);

    memory_region_init_io(&s->iomem, obj, &atarist_mfp_ops, s,
                          "atarist.mfp", 0x40);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), mfp_gpip_set, "gpip", 8);
}

static void atarist_mfp_realize(DeviceState *dev, Error **errp)
{
    AtaristMfpState *s = ATARIST_MFP(dev);

    s->qtimer[0] = timer_new_ns(QEMU_CLOCK_VIRTUAL, mfp_timer_fire_a, s);
    s->qtimer[1] = timer_new_ns(QEMU_CLOCK_VIRTUAL, mfp_timer_fire_b, s);
    s->qtimer[2] = timer_new_ns(QEMU_CLOCK_VIRTUAL, mfp_timer_fire_c, s);
    s->qtimer[3] = timer_new_ns(QEMU_CLOCK_VIRTUAL, mfp_timer_fire_d, s);
}

static const Property atarist_mfp_properties[] = {
    DEFINE_PROP_LINK("video", AtaristMfpState, video, "atarist-video",
                     AtaristVideoState *),
};

static void atarist_mfp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = atarist_mfp_realize;
    device_class_set_props(dc, atarist_mfp_properties);
    rc->phases.hold = atarist_mfp_reset_hold;
}

/*
 * ---------------------------------------------------------------------
 * Shifter video + the GLUE's video address counter and sync mode
 * ---------------------------------------------------------------------
 */

#define TYPE_ATARIST_VIDEO "atarist-video"
OBJECT_DECLARE_SIMPLE_TYPE(AtaristVideoState, ATARIST_VIDEO)

/* byte offsets into the 0xFF8200 region */
#define VID_REG_BASE_HI       0x01
#define VID_REG_BASE_MID      0x03
#define VID_REG_COUNT_HI      0x05
#define VID_REG_COUNT_MID     0x07
#define VID_REG_COUNT_LO      0x09
#define VID_REG_SYNC          0x0a
#define VID_REG_PALETTE       0x40
#define VID_REG_PALETTE_END   0x5f
#define VID_REG_RES           0x60

#define VID_SYNC_50HZ         0x02

#define VID_RES_LOW           0
#define VID_RES_MED           1
#define VID_RES_HIGH          2

struct AtaristVideoState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *ram;
    uint32_t ram_size;
    qemu_irq vbl;

    uint8_t base_hi;
    uint8_t base_mid;
    uint8_t sync;
    uint8_t res;
    uint8_t pal[32];

    QEMUTimer *vbl_timer;
    int64_t last_vbl_ns;
    uint32_t scan_base;

    QemuConsole *con;
    MemoryRegionSection fbsection;
    int invalidate;
    uint32_t fb_base;
    int fb_mode;
};

static int64_t video_frame_ns(AtaristVideoState *s)
{
    return (s->sync & VID_SYNC_50HZ) ? FRAME_NS_50HZ : FRAME_NS_60HZ;
}

static int video_frame_lines(AtaristVideoState *s)
{
    return (s->sync & VID_SYNC_50HZ) ? FRAME_LINES_50HZ : FRAME_LINES_60HZ;
}

static uint32_t video_base(AtaristVideoState *s)
{
    return ((uint32_t)s->base_hi << 16) | ((uint32_t)s->base_mid << 8);
}

/*
 * Monotonic count of display-enable lines (one per visible scanline)
 * since the epoch; MFP timer B counts these in event mode.  Frames are
 * anchored at t=0 like the VBL timer, the 200 visible lines sit in the
 * middle of the frame.
 */
static int64_t atarist_video_de_events(AtaristVideoState *s, int64_t t)
{
    int64_t period = video_frame_ns(s);
    int lines = video_frame_lines(s);
    int64_t line_ns = period / lines;
    int64_t frames = t / period;
    int64_t line = (t % period) / line_ns;
    int64_t first = (lines - FRAME_VISIBLE_LINES) / 2;
    int64_t vis = MIN(MAX(line - first, 0), FRAME_VISIBLE_LINES);

    return frames * FRAME_VISIBLE_LINES + vis;
}

/* the time at which the cumulative DE event count reaches @target */
static int64_t atarist_video_de_event_time(AtaristVideoState *s,
                                           int64_t target)
{
    int64_t period = video_frame_ns(s);
    int lines = video_frame_lines(s);
    int64_t line_ns = period / lines;
    int64_t first = (lines - FRAME_VISIBLE_LINES) / 2;
    int64_t frames = target / FRAME_VISIBLE_LINES;
    int64_t rem = target % FRAME_VISIBLE_LINES;

    /* the event completes at the end of its scanline */
    return frames * period + (first + rem + 1) * line_ns;
}

static void atarist_video_vbl(void *opaque)
{
    AtaristVideoState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t period = video_frame_ns(s);

    qemu_irq_pulse(s->vbl);
    s->scan_base = video_base(s);
    s->last_vbl_ns = now;
    /* keep the frames anchored at multiples of the period */
    timer_mod(s->vbl_timer, (now + period) / period * period);
}

/* the video address counter, read at 0xFF8205/07/09 */
static uint32_t video_counter(AtaristVideoState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t off = (now - s->last_vbl_ns) * FRAME_BYTES / video_frame_ns(s);

    off = MIN(MAX(off, 0), FRAME_BYTES - 2) & ~1;
    return s->scan_base + off;
}

static uint64_t atarist_video_read(void *opaque, hwaddr addr, unsigned size)
{
    AtaristVideoState *s = opaque;

    switch (addr) {
    case VID_REG_BASE_HI:
        return s->base_hi;
    case VID_REG_BASE_MID:
        return s->base_mid;
    case VID_REG_COUNT_HI:
        return (video_counter(s) >> 16) & 0x3f;
    case VID_REG_COUNT_MID:
        return video_counter(s) >> 8;
    case VID_REG_COUNT_LO:
        return video_counter(s);
    case VID_REG_SYNC:
        return s->sync;
    case VID_REG_PALETTE ... VID_REG_PALETTE_END:
        return s->pal[addr - VID_REG_PALETTE];
    case VID_REG_RES:
        return s->res;
    default:
        qemu_log_mask(LOG_UNIMP, "atarist video: read +0x%02x\n",
                      (unsigned)addr);
        return 0;
    }
}

static void atarist_video_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    AtaristVideoState *s = opaque;

    switch (addr) {
    case VID_REG_BASE_HI:
        s->base_hi = val & 0x3f;
        s->invalidate = 1;
        break;
    case VID_REG_BASE_MID:
        s->base_mid = val;
        s->invalidate = 1;
        break;
    case VID_REG_SYNC:
        s->sync = val;
        break;
    case VID_REG_PALETTE ... VID_REG_PALETTE_END:
        s->pal[addr - VID_REG_PALETTE] = val & ((addr & 1) ? 0x77 : 0x07);
        break;
    case VID_REG_RES:
        s->res = val & 3;
        s->invalidate = 1;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "atarist video: write +0x%02x <- 0x%02x\n",
                      (unsigned)addr, (unsigned)val);
        break;
    }
}

static const MemoryRegionOps atarist_video_ops = {
    .read = atarist_video_read,
    .write = atarist_video_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* 3-bit STF colour channel to 8 bits */
static inline uint32_t st_col(unsigned c)
{
    c &= 7;
    return (c << 5) | (c << 2) | (c >> 1);
}

static void video_palette_rgb(AtaristVideoState *s, uint32_t *rgb, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        uint16_t e = (s->pal[i * 2] << 8) | s->pal[i * 2 + 1];

        rgb[i] = 0xff000000 | (st_col(e >> 8) << 16) |
                 (st_col(e >> 4) << 8) | st_col(e);
    }
}

static void atarist_fb_draw_line_low(void *opaque, uint8_t *d,
                                     const uint8_t *src, int width,
                                     int pitch)
{
    AtaristVideoState *s = opaque;
    uint32_t *buf = (uint32_t *)d;
    uint32_t rgb[16];
    int g, b;

    video_palette_rgb(s, rgb, 16);
    for (g = 0; g < FRAME_LINE_BYTES / 8; g++) {
        uint16_t p0 = lduw_be_p(src + g * 8);
        uint16_t p1 = lduw_be_p(src + g * 8 + 2);
        uint16_t p2 = lduw_be_p(src + g * 8 + 4);
        uint16_t p3 = lduw_be_p(src + g * 8 + 6);

        for (b = 15; b >= 0; b--) {
            unsigned idx = ((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) |
                           (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3);

            *buf++ = rgb[idx];
        }
    }
}

static void atarist_fb_draw_line_med(void *opaque, uint8_t *d,
                                     const uint8_t *src, int width,
                                     int pitch)
{
    AtaristVideoState *s = opaque;
    uint32_t *buf = (uint32_t *)d;
    uint32_t rgb[4];
    int g, b;

    video_palette_rgb(s, rgb, 4);
    for (g = 0; g < FRAME_LINE_BYTES / 4; g++) {
        uint16_t p0 = lduw_be_p(src + g * 4);
        uint16_t p1 = lduw_be_p(src + g * 4 + 2);

        for (b = 15; b >= 0; b--) {
            unsigned idx = ((p0 >> b) & 1) | (((p1 >> b) & 1) << 1);

            *buf++ = rgb[idx];
        }
    }
}

static void atarist_fb_draw_line_high(void *opaque, uint8_t *d,
                                      const uint8_t *src, int width,
                                      int pitch)
{
    AtaristVideoState *s = opaque;
    uint32_t *buf = (uint32_t *)d;
    /* palette register 0 bit 0 set: set bits paint black on white */
    uint32_t fg = (s->pal[1] & 1) ? 0xff000000 : 0xffffffff;
    uint32_t bg = (s->pal[1] & 1) ? 0xffffffff : 0xff000000;
    int g, b;

    for (g = 0; g < FRAME_LINE_BYTES / 2; g++) {
        uint16_t p0 = lduw_be_p(src + g * 2);

        for (b = 15; b >= 0; b--) {
            *buf++ = ((p0 >> b) & 1) ? fg : bg;
        }
    }
}

static const struct {
    int width, height, lines;
    drawfn fn;
} atarist_fb_modes[3] = {
    [VID_RES_LOW]  = { 320, 200, 200, atarist_fb_draw_line_low },
    [VID_RES_MED]  = { 640, 200, 200, atarist_fb_draw_line_med },
    [VID_RES_HIGH] = { 640, 400, 400, atarist_fb_draw_line_high },
};

static bool atarist_fb_update(void *opaque)
{
    AtaristVideoState *s = ATARIST_VIDEO(opaque);
    DisplaySurface *surface;
    int mode = s->res & 3;
    uint32_t base;
    int first = 0, last = 0;

    if (mode == 3) {
        mode = VID_RES_LOW;
    }
    if (mode != s->fb_mode) {
        s->fb_mode = mode;
        qemu_console_resize(s->con, atarist_fb_modes[mode].width,
                            atarist_fb_modes[mode].height);
        s->invalidate = 1;
    }

    base = video_base(s) & (ATARIST_RAM_WINDOW - 1);
    if (base + FRAME_BYTES > s->ram_size) {
        base = s->ram_size - FRAME_BYTES;
    }

    if (s->invalidate || base != s->fb_base) {
        /*
         * Address the RAM region directly instead of resolving the
         * address through the CPU-visible flatview: the video scanner
         * reads RAM regardless of what the CPU decode currently maps
         * there (see the mac128k overlay lesson).
         */
        s->fb_base = base;
        s->fbsection = (MemoryRegionSection) {
            .mr = s->ram,
            .offset_within_region = base,
            .size = int128_make64(FRAME_BYTES),
        };
        s->invalidate = 0;
    }

    surface = qemu_console_surface(s->con);
    framebuffer_update_display(surface, &s->fbsection,
                               atarist_fb_modes[mode].width,
                               atarist_fb_modes[mode].lines,
                               FRAME_LINE_BYTES,
                               atarist_fb_modes[mode].width * 4,
                               0, 1, atarist_fb_modes[mode].fn, s,
                               &first, &last);
    qemu_console_update(s->con, 0, 0, atarist_fb_modes[mode].width,
                        atarist_fb_modes[mode].height);
    return true;
}

static void atarist_fb_invalidate(void *opaque)
{
    AtaristVideoState *s = ATARIST_VIDEO(opaque);

    s->invalidate = 1;
}

static const GraphicHwOps atarist_fb_ops = {
    .invalidate = atarist_fb_invalidate,
    .gfx_update = atarist_fb_update,
};

static void atarist_video_reset_hold(Object *obj, ResetType type)
{
    AtaristVideoState *s = ATARIST_VIDEO(obj);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t period;

    s->base_hi = 0;
    s->base_mid = 0;
    s->sync = 0;
    s->res = 0;
    memset(s->pal, 0, sizeof(s->pal));
    s->invalidate = 1;
    s->scan_base = 0;
    s->last_vbl_ns = now;
    period = video_frame_ns(s);
    timer_mod(s->vbl_timer, (now + period) / period * period);
}

static void atarist_video_init(Object *obj)
{
    AtaristVideoState *s = ATARIST_VIDEO(obj);

    memory_region_init_io(&s->iomem, obj, &atarist_video_ops, s,
                          "atarist.video", 0x80);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->vbl);
}

static void atarist_video_realize(DeviceState *dev, Error **errp)
{
    AtaristVideoState *s = ATARIST_VIDEO(dev);

    s->vbl_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, atarist_video_vbl, s);
    s->fb_mode = -1;
    s->con = qemu_graphic_console_create(dev, 0, &atarist_fb_ops, s);
    qemu_console_resize(s->con, 320, 200);
}

static void atarist_video_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->realize = atarist_video_realize;
    rc->phases.hold = atarist_video_reset_hold;
}

/*
 * ---------------------------------------------------------------------
 * GLUE interrupt priority encoder
 *
 * MFP on IPL6 (vectored), VBL on IPL4 and HBL on IPL2 (autovectored).
 * VBL/HBL are edge-latched and cleared by the CPU's interrupt
 * acknowledge, which also drives the MFP's pending->in-service
 * transition.
 * ---------------------------------------------------------------------
 */

#define TYPE_ATARIST_GLUE "atarist-glue"
OBJECT_DECLARE_SIMPLE_TYPE(AtaristGlueState, ATARIST_GLUE)

#define GLUE_IRQ_HBL          0
#define GLUE_IRQ_VBL          1
#define GLUE_IRQ_MFP          2

#define M68K_AUTOVECTOR(level) (24 + (level))

struct AtaristGlueState {
    SysBusDevice parent_obj;

    M68kCPU *cpu;
    AtaristMfpState *mfp;
    bool hbl;
    bool vbl;
    bool mfp_req;
};

static void atarist_glue_update(AtaristGlueState *s)
{
    if (s->mfp_req) {
        m68k_set_irq_level(s->cpu, 6, atarist_mfp_current_vector(s->mfp));
    } else if (s->vbl) {
        m68k_set_irq_level(s->cpu, 4, M68K_AUTOVECTOR(4));
    } else if (s->hbl) {
        m68k_set_irq_level(s->cpu, 2, M68K_AUTOVECTOR(2));
    } else {
        m68k_set_irq_level(s->cpu, 0, 0);
    }
}

static void atarist_glue_set_irq(void *opaque, int irq, int level)
{
    AtaristGlueState *s = opaque;

    switch (irq) {
    case GLUE_IRQ_HBL:
        if (level) {
            s->hbl = true;      /* latched until IACK */
        }
        break;
    case GLUE_IRQ_VBL:
        if (level) {
            s->vbl = true;
        }
        break;
    case GLUE_IRQ_MFP:
        s->mfp_req = level;
        break;
    }
    atarist_glue_update(s);
}

static void atarist_glue_iack(void *opaque, int level, int active)
{
    AtaristGlueState *s = opaque;

    if (!active) {
        return;
    }
    switch (level) {
    case 2:
        s->hbl = false;
        break;
    case 4:
        s->vbl = false;
        break;
    case 6:
        atarist_mfp_iack(s->mfp);
        break;
    }
    atarist_glue_update(s);
}

static void atarist_glue_init(Object *obj)
{
    qdev_init_gpio_in_named(DEVICE(obj), atarist_glue_set_irq, "irq", 3);
    qdev_init_gpio_in_named(DEVICE(obj), atarist_glue_iack, "iack", 8);
}

static const Property atarist_glue_properties[] = {
    DEFINE_PROP_LINK("cpu", AtaristGlueState, cpu, TYPE_M68K_CPU, M68kCPU *),
    DEFINE_PROP_LINK("mfp", AtaristGlueState, mfp, TYPE_ATARIST_MFP,
                     AtaristMfpState *),
};

static void atarist_glue_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_props(DEVICE_CLASS(klass), atarist_glue_properties);
}

/*
 * ---------------------------------------------------------------------
 * YM2149 PSG: a plain register file (no sound), but port A carries the
 * floppy side-select and drive-select lines.
 * ---------------------------------------------------------------------
 */

#define TYPE_ATARIST_PSG "atarist-psg"
OBJECT_DECLARE_SIMPLE_TYPE(AtaristPsgState, ATARIST_PSG)

#define PSG_REG_PORTA         14

#define PSG_PORTA_SIDE        0x01        /* 0 = side 1 */
#define PSG_PORTA_DRIVE0      0x02        /* 0 = selected */
#define PSG_PORTA_DRIVE1      0x04

struct AtaristPsgState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t sel;
    uint8_t regs[16];
    qemu_irq porta_out[3];      /* side, drive0, drive1 (raw levels) */
};

static void atarist_psg_porta_update(AtaristPsgState *s)
{
    uint8_t v = s->regs[PSG_REG_PORTA];

    qemu_set_irq(s->porta_out[0], !!(v & PSG_PORTA_SIDE));
    qemu_set_irq(s->porta_out[1], !!(v & PSG_PORTA_DRIVE0));
    qemu_set_irq(s->porta_out[2], !!(v & PSG_PORTA_DRIVE1));
}

static uint64_t atarist_psg_read(void *opaque, hwaddr addr, unsigned size)
{
    AtaristPsgState *s = opaque;

    if ((addr & 2) == 0) {
        return s->regs[s->sel & 0x0f];
    }
    return 0xff;
}

static void atarist_psg_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    AtaristPsgState *s = opaque;

    if ((addr & 2) == 0) {
        s->sel = val;
    } else {
        s->regs[s->sel & 0x0f] = val;
        if ((s->sel & 0x0f) == PSG_REG_PORTA) {
            atarist_psg_porta_update(s);
        }
    }
}

static const MemoryRegionOps atarist_psg_ops = {
    .read = atarist_psg_read,
    .write = atarist_psg_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void atarist_psg_reset_hold(Object *obj, ResetType type)
{
    AtaristPsgState *s = ATARIST_PSG(obj);

    s->sel = 0;
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[PSG_REG_PORTA] = PSG_PORTA_SIDE | PSG_PORTA_DRIVE0 |
                             PSG_PORTA_DRIVE1;
    atarist_psg_porta_update(s);
}

static void atarist_psg_init(Object *obj)
{
    AtaristPsgState *s = ATARIST_PSG(obj);

    memory_region_init_io(&s->iomem, obj, &atarist_psg_ops, s,
                          "atarist.psg", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), s->porta_out, "porta", 3);
}

static void atarist_psg_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = atarist_psg_reset_hold;
}


/*
 * ---------------------------------------------------------------------
 * The two MC6850 ACIAs and the IKBD (HD6301) keyboard controller,
 * modelled at protocol level.  Both ACIA interrupt outputs wire-OR
 * into MFP GPIP4 (active low).  The keyboard ACIA's serial line runs
 * at 7812.5 baud, so consecutive IKBD bytes are paced rather than
 * delivered back-to-back - TOS's interrupt handler takes one byte per
 * GPIP edge.
 * ---------------------------------------------------------------------
 */

#define TYPE_ATARIST_IKBD "atarist-ikbd"
OBJECT_DECLARE_SIMPLE_TYPE(AtaristIkbdState, ATARIST_IKBD)

/* byte offsets in the 0xFFFC00 region */
#define ACIA_KBD_CTRL         0x00
#define ACIA_KBD_DATA         0x02
#define ACIA_MIDI_CTRL        0x04
#define ACIA_MIDI_DATA        0x06

/* MC6850 status bits */
#define ACIA_SR_RDRF          0x01
#define ACIA_SR_TDRE          0x02
#define ACIA_SR_IRQ           0x80

/* MC6850 control bits */
#define ACIA_CR_RESET_MASK    0x03        /* 11 = master reset */
#define ACIA_CR_RX_IRQ_EN     0x80

/* IKBD commands */
#define IKBD_CMD_RESET        0x80        /* param 0x01 */
#define IKBD_CMD_MOUSE_ACTION 0x07
#define IKBD_CMD_MOUSE_REL    0x08
#define IKBD_CMD_MOUSE_ABS    0x09
#define IKBD_CMD_MOUSE_KEYCD  0x0a
#define IKBD_CMD_MOUSE_THRESH 0x0b
#define IKBD_CMD_MOUSE_SCALE  0x0c
#define IKBD_CMD_MOUSE_POLL   0x0d
#define IKBD_CMD_MOUSE_LOAD   0x0e
#define IKBD_CMD_MOUSE_YBOT   0x0f
#define IKBD_CMD_MOUSE_YTOP   0x10
#define IKBD_CMD_RESUME       0x11
#define IKBD_CMD_MOUSE_OFF    0x12
#define IKBD_CMD_PAUSE        0x13
#define IKBD_CMD_JOY_EVENT    0x14
#define IKBD_CMD_JOY_POLLMODE 0x15
#define IKBD_CMD_JOY_POLL     0x16
#define IKBD_CMD_JOY_MONITOR  0x17
#define IKBD_CMD_JOY_FIRE     0x18
#define IKBD_CMD_JOY_KEYCD    0x19
#define IKBD_CMD_JOY_OFF      0x1a
#define IKBD_CMD_SET_TIME     0x1b
#define IKBD_CMD_GET_TIME     0x1c

/* IKBD report headers */
#define IKBD_REPLY_VERSION    0xf1        /* self-test done, version */
#define IKBD_HDR_STATUS       0xf6
#define IKBD_HDR_MOUSE_ABS    0xf7
#define IKBD_HDR_MOUSE_REL    0xf8        /* + button bits */
#define IKBD_HDR_TIME         0xfc
#define IKBD_HDR_JOY0         0xfd

#define IKBD_MOUSE_BTN_LEFT   0x02
#define IKBD_MOUSE_BTN_RIGHT  0x01

#define IKBD_KEY_RELEASE      0x80

#define IKBD_BUF_SIZE         256

/* 7812.5 baud, 10 bits on the wire */
#define IKBD_BYTE_NS          1280000
/* the HD6301 self-test after a reset command */
#define IKBD_RESET_DELAY_NS   (40 * SCALE_MS)
/* relative mouse packet pacing */
#define IKBD_MOUSE_PACKET_NS  (5 * SCALE_MS)

/* QEMU qcode -> Atari ST scancode (largely the XT layout + ST extras) */
static const uint8_t atarist_keymap[Q_KEY_CODE__MAX] = {
    [Q_KEY_CODE_ESC] = 0x01,
    [Q_KEY_CODE_1] = 0x02, [Q_KEY_CODE_2] = 0x03, [Q_KEY_CODE_3] = 0x04,
    [Q_KEY_CODE_4] = 0x05, [Q_KEY_CODE_5] = 0x06, [Q_KEY_CODE_6] = 0x07,
    [Q_KEY_CODE_7] = 0x08, [Q_KEY_CODE_8] = 0x09, [Q_KEY_CODE_9] = 0x0a,
    [Q_KEY_CODE_0] = 0x0b,
    [Q_KEY_CODE_MINUS] = 0x0c, [Q_KEY_CODE_EQUAL] = 0x0d,
    [Q_KEY_CODE_BACKSPACE] = 0x0e,
    [Q_KEY_CODE_TAB] = 0x0f,
    [Q_KEY_CODE_Q] = 0x10, [Q_KEY_CODE_W] = 0x11, [Q_KEY_CODE_E] = 0x12,
    [Q_KEY_CODE_R] = 0x13, [Q_KEY_CODE_T] = 0x14, [Q_KEY_CODE_Y] = 0x15,
    [Q_KEY_CODE_U] = 0x16, [Q_KEY_CODE_I] = 0x17, [Q_KEY_CODE_O] = 0x18,
    [Q_KEY_CODE_P] = 0x19,
    [Q_KEY_CODE_BRACKET_LEFT] = 0x1a, [Q_KEY_CODE_BRACKET_RIGHT] = 0x1b,
    [Q_KEY_CODE_RET] = 0x1c,
    [Q_KEY_CODE_CTRL] = 0x1d, [Q_KEY_CODE_CTRL_R] = 0x1d,
    [Q_KEY_CODE_A] = 0x1e, [Q_KEY_CODE_S] = 0x1f, [Q_KEY_CODE_D] = 0x20,
    [Q_KEY_CODE_F] = 0x21, [Q_KEY_CODE_G] = 0x22, [Q_KEY_CODE_H] = 0x23,
    [Q_KEY_CODE_J] = 0x24, [Q_KEY_CODE_K] = 0x25, [Q_KEY_CODE_L] = 0x26,
    [Q_KEY_CODE_SEMICOLON] = 0x27, [Q_KEY_CODE_APOSTROPHE] = 0x28,
    [Q_KEY_CODE_GRAVE_ACCENT] = 0x2b,   /* ST "#~", nearest match */
    [Q_KEY_CODE_SHIFT] = 0x2a,
    [Q_KEY_CODE_BACKSLASH] = 0x60,      /* ST ISO key next to lshift */
    [Q_KEY_CODE_Z] = 0x2c, [Q_KEY_CODE_X] = 0x2d, [Q_KEY_CODE_C] = 0x2e,
    [Q_KEY_CODE_V] = 0x2f, [Q_KEY_CODE_B] = 0x30, [Q_KEY_CODE_N] = 0x31,
    [Q_KEY_CODE_M] = 0x32,
    [Q_KEY_CODE_COMMA] = 0x33, [Q_KEY_CODE_DOT] = 0x34,
    [Q_KEY_CODE_SLASH] = 0x35,
    [Q_KEY_CODE_SHIFT_R] = 0x36,
    [Q_KEY_CODE_ALT] = 0x38, [Q_KEY_CODE_ALT_R] = 0x38,
    [Q_KEY_CODE_SPC] = 0x39,
    [Q_KEY_CODE_CAPS_LOCK] = 0x3a,
    [Q_KEY_CODE_F1] = 0x3b, [Q_KEY_CODE_F2] = 0x3c, [Q_KEY_CODE_F3] = 0x3d,
    [Q_KEY_CODE_F4] = 0x3e, [Q_KEY_CODE_F5] = 0x3f, [Q_KEY_CODE_F6] = 0x40,
    [Q_KEY_CODE_F7] = 0x41, [Q_KEY_CODE_F8] = 0x42, [Q_KEY_CODE_F9] = 0x43,
    [Q_KEY_CODE_F10] = 0x44,
    [Q_KEY_CODE_HOME] = 0x47,           /* ClrHome */
    [Q_KEY_CODE_UP] = 0x48,
    [Q_KEY_CODE_KP_SUBTRACT] = 0x4a,
    [Q_KEY_CODE_LEFT] = 0x4b, [Q_KEY_CODE_RIGHT] = 0x4d,
    [Q_KEY_CODE_KP_ADD] = 0x4e,
    [Q_KEY_CODE_DOWN] = 0x50,
    [Q_KEY_CODE_INSERT] = 0x52,
    [Q_KEY_CODE_DELETE] = 0x53,
    [Q_KEY_CODE_END] = 0x61,            /* Undo */
    [Q_KEY_CODE_PGUP] = 0x62,           /* Help */
    [Q_KEY_CODE_KP_DIVIDE] = 0x65,
    [Q_KEY_CODE_KP_MULTIPLY] = 0x66,
    [Q_KEY_CODE_KP_7] = 0x67, [Q_KEY_CODE_KP_8] = 0x68,
    [Q_KEY_CODE_KP_9] = 0x69, [Q_KEY_CODE_KP_4] = 0x6a,
    [Q_KEY_CODE_KP_5] = 0x6b, [Q_KEY_CODE_KP_6] = 0x6c,
    [Q_KEY_CODE_KP_1] = 0x6d, [Q_KEY_CODE_KP_2] = 0x6e,
    [Q_KEY_CODE_KP_3] = 0x6f, [Q_KEY_CODE_KP_0] = 0x70,
    [Q_KEY_CODE_KP_DECIMAL] = 0x71,
    [Q_KEY_CODE_KP_ENTER] = 0x72,
};

struct AtaristIkbdState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;               /* to MFP GPIP4, active low */

    /* keyboard ACIA */
    uint8_t kbd_ctrl;
    uint8_t kbd_rx;
    bool kbd_rdrf;
    /* MIDI ACIA: transmit is discarded, nothing ever received */
    uint8_t midi_ctrl;

    /* IKBD output queue towards the ACIA */
    uint8_t obuf[IKBD_BUF_SIZE];
    unsigned ohead, ocount;
    bool paused;
    QEMUTimer *xmit_timer;
    QEMUTimer *reset_timer;

    /* IKBD command parser */
    uint8_t cmd;
    uint8_t params[8];
    unsigned nparams, want_params;

    /* mouse */
    bool mouse_rel;             /* relative reporting enabled */
    bool mouse_y_bottom;
    int mouse_dx, mouse_dy;
    uint8_t mouse_buttons;
    uint8_t mouse_sent_buttons;
    QEMUTimer *mouse_timer;

    QemuInputHandlerState *mouse_hs;
    QemuInputHandlerState *kbd_hs;
};

static void ikbd_acia_update_irq(AtaristIkbdState *s)
{
    bool kbd_irq = s->kbd_rdrf && (s->kbd_ctrl & ACIA_CR_RX_IRQ_EN);

    /* wire-OR into GPIP4, active low */
    qemu_set_irq(s->irq, !kbd_irq);
}

/* move the next queued byte into the ACIA receive register */
static void ikbd_deliver(AtaristIkbdState *s)
{
    if (s->kbd_rdrf || !s->ocount || s->paused) {
        return;
    }
    s->kbd_rx = s->obuf[s->ohead];
    s->ohead = (s->ohead + 1) % IKBD_BUF_SIZE;
    s->ocount--;
    s->kbd_rdrf = true;
    ikbd_acia_update_irq(s);
}

static void ikbd_xmit_cb(void *opaque)
{
    ikbd_deliver(opaque);
}

static void ikbd_queue(AtaristIkbdState *s, const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i < len && s->ocount < IKBD_BUF_SIZE; i++) {
        s->obuf[(s->ohead + s->ocount) % IKBD_BUF_SIZE] = data[i];
        s->ocount++;
    }
    if (!s->kbd_rdrf && !timer_pending(s->xmit_timer)) {
        ikbd_deliver(s);
    }
}

static void ikbd_queue_byte(AtaristIkbdState *s, uint8_t b)
{
    ikbd_queue(s, &b, 1);
}

/* emit one relative mouse packet, rescheduling while movement remains */
static void ikbd_mouse_flush(AtaristIkbdState *s)
{
    uint8_t pkt[3];
    int dx, dy;

    if (!s->mouse_rel || s->paused) {
        return;
    }
    if (!s->mouse_dx && !s->mouse_dy &&
        s->mouse_buttons == s->mouse_sent_buttons) {
        return;
    }
    if (timer_pending(s->mouse_timer)) {
        return;
    }

    dx = MIN(MAX(s->mouse_dx, -127), 127);
    dy = MIN(MAX(s->mouse_dy, -127), 127);
    s->mouse_dx -= dx;
    s->mouse_dy -= dy;
    s->mouse_sent_buttons = s->mouse_buttons;

    pkt[0] = IKBD_HDR_MOUSE_REL | s->mouse_buttons;
    pkt[1] = dx;
    pkt[2] = s->mouse_y_bottom ? -dy : dy;
    ikbd_queue(s, pkt, sizeof(pkt));

    if (s->mouse_dx || s->mouse_dy) {
        timer_mod(s->mouse_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  IKBD_MOUSE_PACKET_NS);
    }
}

static void ikbd_mouse_timer_cb(void *opaque)
{
    ikbd_mouse_flush(opaque);
}

static void ikbd_reset_state(AtaristIkbdState *s)
{
    s->ohead = s->ocount = 0;
    s->paused = false;
    s->cmd = 0;
    s->nparams = s->want_params = 0;
    s->mouse_rel = true;        /* power-up default: relative mode */
    s->mouse_y_bottom = false;
    s->mouse_dx = s->mouse_dy = 0;
    s->mouse_buttons = 0;
    s->mouse_sent_buttons = 0;
}

static void ikbd_reset_done_cb(void *opaque)
{
    AtaristIkbdState *s = opaque;

    ikbd_queue_byte(s, IKBD_REPLY_VERSION);
}

/* number of parameter bytes each IKBD command takes */
static unsigned ikbd_cmd_params(uint8_t cmd)
{
    switch (cmd) {
    case IKBD_CMD_RESET:
    case IKBD_CMD_MOUSE_ACTION:
        return 1;
    case IKBD_CMD_MOUSE_KEYCD:
    case IKBD_CMD_MOUSE_THRESH:
    case IKBD_CMD_MOUSE_SCALE:
    case IKBD_CMD_JOY_MONITOR:
        return 2;
    case IKBD_CMD_MOUSE_ABS:
        return 4;
    case IKBD_CMD_MOUSE_LOAD:
        return 5;
    case IKBD_CMD_JOY_KEYCD:
    case IKBD_CMD_SET_TIME:
        return 6;
    default:
        return 0;
    }
}

static void ikbd_run_cmd(AtaristIkbdState *s)
{
    switch (s->cmd) {
    case IKBD_CMD_RESET:
        if (s->params[0] == 0x01) {
            ikbd_reset_state(s);
            timer_mod(s->reset_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      IKBD_RESET_DELAY_NS);
        }
        break;
    case IKBD_CMD_MOUSE_REL:
        s->mouse_rel = true;
        break;
    case IKBD_CMD_MOUSE_ABS:
    case IKBD_CMD_MOUSE_KEYCD:
    case IKBD_CMD_MOUSE_OFF:
        s->mouse_rel = false;
        break;
    case IKBD_CMD_MOUSE_YBOT:
        s->mouse_y_bottom = true;
        break;
    case IKBD_CMD_MOUSE_YTOP:
        s->mouse_y_bottom = false;
        break;
    case IKBD_CMD_MOUSE_POLL: {
        uint8_t pkt[6] = { IKBD_HDR_MOUSE_ABS, s->mouse_buttons ^ 0,
                           0, 0, 0, 0 };
        ikbd_queue(s, pkt, sizeof(pkt));
        break;
    }
    case IKBD_CMD_RESUME:
        s->paused = false;
        ikbd_deliver(s);
        break;
    case IKBD_CMD_PAUSE:
        s->paused = true;
        break;
    case IKBD_CMD_JOY_POLL: {
        uint8_t pkt[3] = { IKBD_HDR_JOY0, 0, 0 };
        ikbd_queue(s, pkt, sizeof(pkt));
        break;
    }
    case IKBD_CMD_GET_TIME: {
        /* BCD YY MM DD hh mm ss; a real HD6301 wakes up with a sane
         * (if arbitrary) clock, and TOS feeds the reply through its
         * date conversion unchecked */
        uint8_t pkt[7] = { IKBD_HDR_TIME, 0x86, 0x07, 0x22, 0x12, 0x00,
                           0x00 };
        ikbd_queue(s, pkt, sizeof(pkt));
        break;
    }
    case IKBD_CMD_MOUSE_ACTION:
    case IKBD_CMD_MOUSE_THRESH:
    case IKBD_CMD_MOUSE_SCALE:
    case IKBD_CMD_MOUSE_LOAD:
    case IKBD_CMD_JOY_EVENT:
    case IKBD_CMD_JOY_POLLMODE:
    case IKBD_CMD_JOY_MONITOR:
    case IKBD_CMD_JOY_FIRE:
    case IKBD_CMD_JOY_KEYCD:
    case IKBD_CMD_JOY_OFF:
    case IKBD_CMD_SET_TIME:
        break;                  /* accepted, nothing to model */
    default:
        qemu_log_mask(LOG_UNIMP, "atarist ikbd: command 0x%02x\n", s->cmd);
        break;
    }
}

/* a command byte arrived from the keyboard ACIA transmit register */
static void ikbd_rx_byte(AtaristIkbdState *s, uint8_t b)
{
    if (s->want_params) {
        s->params[s->nparams++] = b;
        if (s->nparams >= s->want_params) {
            s->want_params = 0;
            ikbd_run_cmd(s);
        }
        return;
    }
    s->cmd = b;
    s->nparams = 0;
    s->want_params = ikbd_cmd_params(b);
    if (s->want_params > sizeof(s->params)) {
        s->want_params = sizeof(s->params);
    }
    if (!s->want_params) {
        ikbd_run_cmd(s);
    }
}

static uint64_t atarist_ikbd_read(void *opaque, hwaddr addr, unsigned size)
{
    AtaristIkbdState *s = opaque;

    switch (addr) {
    case ACIA_KBD_CTRL: {
        uint8_t sr = ACIA_SR_TDRE;

        if (s->kbd_rdrf) {
            sr |= ACIA_SR_RDRF;
        }
        if (s->kbd_rdrf && (s->kbd_ctrl & ACIA_CR_RX_IRQ_EN)) {
            sr |= ACIA_SR_IRQ;
        }
        return sr;
    }
    case ACIA_KBD_DATA:
        s->kbd_rdrf = false;
        ikbd_acia_update_irq(s);
        if (s->ocount) {
            timer_mod(s->xmit_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + IKBD_BYTE_NS);
        }
        return s->kbd_rx;
    case ACIA_MIDI_CTRL:
        return ACIA_SR_TDRE;
    case ACIA_MIDI_DATA:
        return 0;
    default:
        return 0xff;
    }
}

static void atarist_ikbd_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    AtaristIkbdState *s = opaque;

    switch (addr) {
    case ACIA_KBD_CTRL:
        s->kbd_ctrl = val;
        if ((val & ACIA_CR_RESET_MASK) == ACIA_CR_RESET_MASK) {
            s->kbd_rdrf = false;
        }
        ikbd_acia_update_irq(s);
        break;
    case ACIA_KBD_DATA:
        ikbd_rx_byte(s, val);
        break;
    case ACIA_MIDI_CTRL:
        s->midi_ctrl = val;
        break;
    case ACIA_MIDI_DATA:
        break;                  /* MIDI out: discarded */
    default:
        break;
    }
}

static const MemoryRegionOps atarist_ikbd_ops = {
    .read = atarist_ikbd_read,
    .write = atarist_ikbd_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void atarist_ikbd_mouse_event(DeviceState *dev, QemuConsole *src,
                                     QemuInputEvent *evt)
{
    AtaristIkbdState *s = ATARIST_IKBD(dev);

    switch (evt->type) {
    case INPUT_EVENT_KIND_REL:
        if (evt->rel.axis == INPUT_AXIS_X) {
            s->mouse_dx += evt->rel.value;
        } else if (evt->rel.axis == INPUT_AXIS_Y) {
            s->mouse_dy += evt->rel.value;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_LEFT) {
            if (evt->btn.down) {
                s->mouse_buttons |= IKBD_MOUSE_BTN_LEFT;
            } else {
                s->mouse_buttons &= ~IKBD_MOUSE_BTN_LEFT;
            }
        } else if (evt->btn.button == INPUT_BUTTON_RIGHT) {
            if (evt->btn.down) {
                s->mouse_buttons |= IKBD_MOUSE_BTN_RIGHT;
            } else {
                s->mouse_buttons &= ~IKBD_MOUSE_BTN_RIGHT;
            }
        }
        break;
    default:
        break;
    }
}

static void atarist_ikbd_mouse_sync(DeviceState *dev)
{
    ikbd_mouse_flush(ATARIST_IKBD(dev));
}

static const QemuInputHandler atarist_ikbd_mouse_handler = {
    .name  = "Atari ST Mouse",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_REL,
    .event = atarist_ikbd_mouse_event,
    .sync  = atarist_ikbd_mouse_sync,
};

static void atarist_ikbd_key_event(DeviceState *dev, QemuConsole *src,
                                   QemuInputEvent *evt)
{
    AtaristIkbdState *s = ATARIST_IKBD(dev);
    int qcode;
    uint8_t code;

    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    /* handlers receive the host (Linux) keycode; map it to a QKeyCode */
    qcode = qemu_input_linux_to_qcode(evt->key.key);
    if (qcode <= 0 || qcode >= Q_KEY_CODE__MAX) {
        return;
    }
    code = atarist_keymap[qcode];
    if (!code) {
        return;
    }
    ikbd_queue_byte(s, code | (evt->key.down ? 0 : IKBD_KEY_RELEASE));
}

static const QemuInputHandler atarist_ikbd_key_handler = {
    .name  = "Atari ST Keyboard",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = atarist_ikbd_key_event,
};

static void atarist_ikbd_reset_hold(Object *obj, ResetType type)
{
    AtaristIkbdState *s = ATARIST_IKBD(obj);

    s->kbd_ctrl = 0;
    s->kbd_rdrf = false;
    s->midi_ctrl = 0;
    ikbd_reset_state(s);
    timer_del(s->xmit_timer);
    timer_del(s->reset_timer);
    timer_del(s->mouse_timer);
    ikbd_acia_update_irq(s);
}

static void atarist_ikbd_init(Object *obj)
{
    AtaristIkbdState *s = ATARIST_IKBD(obj);

    memory_region_init_io(&s->iomem, obj, &atarist_ikbd_ops, s,
                          "atarist.acia", 8);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void atarist_ikbd_realize(DeviceState *dev, Error **errp)
{
    AtaristIkbdState *s = ATARIST_IKBD(dev);

    s->xmit_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ikbd_xmit_cb, s);
    s->reset_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ikbd_reset_done_cb, s);
    s->mouse_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ikbd_mouse_timer_cb, s);
    s->mouse_hs = qemu_input_handler_register(dev,
                                              &atarist_ikbd_mouse_handler);
    s->kbd_hs = qemu_input_handler_register(dev, &atarist_ikbd_key_handler);
}

static void atarist_ikbd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = atarist_ikbd_realize;
    rc->phases.hold = atarist_ikbd_reset_hold;
}


/*
 * ---------------------------------------------------------------------
 * WD1772 FDC behind the ST DMA controller
 *
 * The FDC/HDC registers are reached through 0xFF8604 (word), with the
 * DMA mode register at 0xFF8606 selecting which register the access
 * hits; 0xFF8606 reads back as DMA status.  The DMA base address is
 * three byte registers at 0xFF8609/0B/0D and counts up as sectors move.
 * Drive/side selection comes from the PSG's port A.  Command
 * completion raises INTRQ into MFP GPIP5 (active low); reading the
 * status register or writing a command clears it.
 * ---------------------------------------------------------------------
 */

#define TYPE_ATARIST_FDC "atarist-fdc"
OBJECT_DECLARE_SIMPLE_TYPE(AtaristFdcState, ATARIST_FDC)

/* byte offsets in the 0xFF8604 region */
#define DMA_REG_DATA          0x00        /* FDC/HDC/sector count access */
#define DMA_REG_MODE          0x02        /* write: mode, read: status */
#define DMA_REG_ADDR_HI       0x05
#define DMA_REG_ADDR_MID      0x07
#define DMA_REG_ADDR_LO       0x09

/* DMA mode register bits */
#define DMA_MODE_A0           0x0002
#define DMA_MODE_A1           0x0004
#define DMA_MODE_HDC          0x0008      /* 1 = HDC, 0 = FDC */
#define DMA_MODE_SECCOUNT     0x0010      /* access the sector counter */
#define DMA_MODE_DISABLED     0x0080
#define DMA_MODE_WRITE        0x0100      /* 1 = RAM -> disk */

/* DMA status register bits */
#define DMA_STATUS_OK         0x01        /* 1 = no error */
#define DMA_STATUS_SECCOUNT   0x02        /* 1 = sector count nonzero */
#define DMA_STATUS_DRQ        0x04

/* WD1772 registers, selected by mode bits A1/A0 */
#define FDC_REG_STATUS_CMD    0
#define FDC_REG_TRACK         1
#define FDC_REG_SECTOR        2
#define FDC_REG_DATA          3

/* WD1772 status bits */
#define FDC_SR_BUSY           0x01
#define FDC_SR_DRQ_INDEX      0x02
#define FDC_SR_TRACK0_LOST    0x04
#define FDC_SR_CRC_ERROR      0x08
#define FDC_SR_RNF            0x10
#define FDC_SR_SPINUP_RECTYPE 0x20
#define FDC_SR_WPROT          0x40
#define FDC_SR_MOTOR_ON       0x80

/* command classes */
#define FDC_CMD_RESTORE       0x00
#define FDC_CMD_SEEK          0x10
#define FDC_CMD_STEP          0x20
#define FDC_CMD_STEP_IN       0x40
#define FDC_CMD_STEP_OUT      0x60
#define FDC_CMD_READ_SECTOR   0x80
#define FDC_CMD_WRITE_SECTOR  0xa0
#define FDC_CMD_READ_ADDRESS  0xc0
#define FDC_CMD_FORCE_INT     0xd0
#define FDC_CMD_READ_TRACK    0xe0
#define FDC_CMD_WRITE_TRACK   0xf0

#define FDC_CMD_FLAG_UPDATE   0x10        /* type I step: update track reg */
#define FDC_CMD_FLAG_MULTIPLE 0x10        /* type II: multiple sectors */
#define FDC_FORCE_INT_NOW     0x08

#define FDC_NUM_DRIVES        2
#define FDC_MAX_TRACK         85
#define FDC_SECTOR_SIZE       512

#define FDC_STEP_NS           (3 * SCALE_MS)
#define FDC_SETTLE_NS         (15 * SCALE_MS)
#define FDC_SECTOR_NS         (2 * SCALE_MS)
#define FDC_MOTOR_SPINDOWN_NS ((int64_t)1800 * SCALE_MS)

typedef struct AtaristFloppy {
    BlockBackend *blk;
    unsigned spt, sides, tracks;
    unsigned phys_track;
} AtaristFloppy;

struct AtaristFdcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;               /* to MFP GPIP5, active low */

    AtaristFloppy drive[FDC_NUM_DRIVES];

    /* PSG port A levels (raw, selects are active low) */
    bool sel_drive[FDC_NUM_DRIVES];
    bool side1;

    uint8_t track, sector, data, status, cmd;
    bool busy;
    bool intrq;
    int64_t motor_off_ns;

    uint16_t dma_mode;
    uint32_t dma_addr;
    uint16_t dma_seccount;
    bool dma_ok;

    QEMUTimer *cmd_timer;
};

static void fdc_update_irq(AtaristFdcState *s)
{
    qemu_set_irq(s->irq, !s->intrq);
}

static void fdc_set_intrq(AtaristFdcState *s)
{
    s->intrq = true;
    fdc_update_irq(s);
}

static void fdc_clear_intrq(AtaristFdcState *s)
{
    s->intrq = false;
    fdc_update_irq(s);
}

static AtaristFloppy *fdc_selected(AtaristFdcState *s)
{
    int i;

    for (i = 0; i < FDC_NUM_DRIVES; i++) {
        if (!s->sel_drive[i]) {         /* active low */
            return &s->drive[i];
        }
    }
    return NULL;
}

static void fdc_motor_on(AtaristFdcState *s)
{
    s->motor_off_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      FDC_MOTOR_SPINDOWN_NS;
}

static bool fdc_motor_running(AtaristFdcState *s)
{
    return s->busy ||
           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->motor_off_ns;
}

/* DMA one buffer into/out of RAM, advancing the address register */
static void fdc_dma_transfer(AtaristFdcState *s, uint8_t *buf, size_t len,
                             bool to_ram)
{
    if (!s->dma_seccount) {
        return;                 /* the DMA FIFO is not listening */
    }
    if (to_ram) {
        address_space_write(&address_space_memory,
                            s->dma_addr & (ATARIST_ADDR_SPACE - 1),
                            MEMTXATTRS_UNSPECIFIED, buf, len);
    } else {
        address_space_read(&address_space_memory,
                           s->dma_addr & (ATARIST_ADDR_SPACE - 1),
                           MEMTXATTRS_UNSPECIFIED, buf, len);
    }
    s->dma_addr += len;
}

static void fdc_sector_done(AtaristFdcState *s)
{
    if (s->dma_seccount) {
        s->dma_seccount--;
    }
}

/* type II/III payload work, run from the command completion timer */
static void fdc_do_read_sectors(AtaristFdcState *s, AtaristFloppy *fl)
{
    bool multiple = s->cmd & FDC_CMD_FLAG_MULTIPLE;
    unsigned side = fl->sides > 1 && s->side1 ? 1 : 0;
    unsigned transferred = 0;
    uint8_t buf[FDC_SECTOR_SIZE];

    if (!fl->blk || s->track != fl->phys_track ||
        fl->phys_track >= fl->tracks) {
        s->status |= FDC_SR_RNF;
        return;
    }

    while (s->sector >= 1 && s->sector <= fl->spt) {
        int64_t lba = ((int64_t)fl->phys_track * fl->sides + side) *
                      fl->spt + (s->sector - 1);

        if (blk_pread(fl->blk, lba * FDC_SECTOR_SIZE, FDC_SECTOR_SIZE,
                      buf, 0) < 0) {
            s->status |= FDC_SR_RNF;
            return;
        }
        fdc_dma_transfer(s, buf, FDC_SECTOR_SIZE, true);
        fdc_sector_done(s);
        transferred++;
        if (!multiple) {
            return;
        }
        s->sector++;
        if (!s->dma_seccount) {
            return;             /* the ST always force-stops here anyway */
        }
    }
    if (!transferred) {
        s->status |= FDC_SR_RNF;
    }
}

static void fdc_do_write_sectors(AtaristFdcState *s, AtaristFloppy *fl)
{
    bool multiple = s->cmd & FDC_CMD_FLAG_MULTIPLE;
    unsigned side = fl->sides > 1 && s->side1 ? 1 : 0;
    unsigned transferred = 0;
    uint8_t buf[FDC_SECTOR_SIZE];

    if (!fl->blk || s->track != fl->phys_track ||
        fl->phys_track >= fl->tracks) {
        s->status |= FDC_SR_RNF;
        return;
    }
    if (!blk_supports_write_perm(fl->blk)) {
        s->status |= FDC_SR_WPROT;
        return;
    }

    while (s->sector >= 1 && s->sector <= fl->spt && s->dma_seccount) {
        int64_t lba = ((int64_t)fl->phys_track * fl->sides + side) *
                      fl->spt + (s->sector - 1);

        fdc_dma_transfer(s, buf, FDC_SECTOR_SIZE, false);
        if (blk_pwrite(fl->blk, lba * FDC_SECTOR_SIZE, FDC_SECTOR_SIZE,
                       buf, 0) < 0) {
            s->status |= FDC_SR_RNF;
            return;
        }
        fdc_sector_done(s);
        transferred++;
        if (!multiple) {
            return;
        }
        s->sector++;
    }
    if (!transferred) {
        s->status |= FDC_SR_RNF;
    }
}

static void fdc_do_read_address(AtaristFdcState *s, AtaristFloppy *fl)
{
    uint8_t id[6];

    if (!fl->blk || fl->phys_track >= fl->tracks) {
        s->status |= FDC_SR_RNF;
        return;
    }
    id[0] = fl->phys_track;
    id[1] = fl->sides > 1 && s->side1 ? 1 : 0;
    id[2] = 1;
    id[3] = 2;                  /* 512 bytes/sector */
    id[4] = 0;                  /* CRC, not modelled */
    id[5] = 0;
    fdc_dma_transfer(s, id, sizeof(id), true);
    /* the 1772 copies the track number into the sector register */
    s->sector = fl->phys_track;
}

static void fdc_cmd_done(void *opaque)
{
    AtaristFdcState *s = opaque;
    AtaristFloppy *fl = fdc_selected(s);
    uint8_t type = s->cmd & 0xf0;

    s->status = 0;

    switch (type) {
    case FDC_CMD_RESTORE:
    case FDC_CMD_SEEK:
    case FDC_CMD_STEP:
    case FDC_CMD_STEP | FDC_CMD_FLAG_UPDATE:
    case FDC_CMD_STEP_IN:
    case FDC_CMD_STEP_IN | FDC_CMD_FLAG_UPDATE:
    case FDC_CMD_STEP_OUT:
    case FDC_CMD_STEP_OUT | FDC_CMD_FLAG_UPDATE: {
        unsigned phys = fl ? fl->phys_track : 0;

        s->status |= FDC_SR_SPINUP_RECTYPE;
        if (phys == 0) {
            s->status |= FDC_SR_TRACK0_LOST;
        }
        break;
    }
    case FDC_CMD_READ_SECTOR:
    case FDC_CMD_READ_SECTOR | FDC_CMD_FLAG_MULTIPLE:
        if (fl) {
            fdc_do_read_sectors(s, fl);
        } else {
            s->status |= FDC_SR_RNF;
        }
        break;
    case FDC_CMD_WRITE_SECTOR:
    case FDC_CMD_WRITE_SECTOR | FDC_CMD_FLAG_MULTIPLE:
        if (fl) {
            fdc_do_write_sectors(s, fl);
        } else {
            s->status |= FDC_SR_RNF;
        }
        break;
    case FDC_CMD_READ_ADDRESS:
        if (fl) {
            fdc_do_read_address(s, fl);
        } else {
            s->status |= FDC_SR_RNF;
        }
        break;
    case FDC_CMD_READ_TRACK:
    case FDC_CMD_WRITE_TRACK:
        qemu_log_mask(LOG_UNIMP, "atarist fdc: command 0x%02x\n", s->cmd);
        break;
    }

    if (fdc_motor_running(s)) {
        s->status |= FDC_SR_MOTOR_ON;
    }
    s->busy = false;
    fdc_motor_on(s);
    fdc_set_intrq(s);
}

static void fdc_command(AtaristFdcState *s, uint8_t cmd)
{
    AtaristFloppy *fl = fdc_selected(s);
    uint8_t type = cmd & 0xf0;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t delay;
    int steps;

    if (type == FDC_CMD_FORCE_INT) {
        timer_del(s->cmd_timer);
        s->busy = false;
        s->status &= ~FDC_SR_BUSY;
        if (cmd & FDC_FORCE_INT_NOW) {
            fdc_set_intrq(s);
        }
        return;
    }
    if (s->busy) {
        return;                 /* only force interrupt gets through */
    }

    s->cmd = cmd;
    s->busy = true;
    fdc_clear_intrq(s);
    fdc_motor_on(s);

    switch (type) {
    case FDC_CMD_RESTORE:
        steps = fl ? fl->phys_track : 0;
        if (fl) {
            fl->phys_track = 0;
        }
        s->track = 0;
        delay = FDC_SETTLE_NS + steps * FDC_STEP_NS;
        break;
    case FDC_CMD_SEEK: {
        unsigned target = MIN(s->data, FDC_MAX_TRACK);

        steps = fl ? abs((int)fl->phys_track - (int)target) : 0;
        if (fl) {
            fl->phys_track = target;
        }
        s->track = s->data;
        delay = FDC_SETTLE_NS + steps * FDC_STEP_NS;
        break;
    }
    case FDC_CMD_STEP:
    case FDC_CMD_STEP | FDC_CMD_FLAG_UPDATE:
    case FDC_CMD_STEP_IN:
    case FDC_CMD_STEP_IN | FDC_CMD_FLAG_UPDATE:
    case FDC_CMD_STEP_OUT:
    case FDC_CMD_STEP_OUT | FDC_CMD_FLAG_UPDATE: {
        int dir = (type & 0xe0) == FDC_CMD_STEP_OUT ? -1 : 1;

        if (fl) {
            int t = (int)fl->phys_track + dir;

            fl->phys_track = MIN(MAX(t, 0), FDC_MAX_TRACK);
            if (cmd & FDC_CMD_FLAG_UPDATE) {
                s->track = fl->phys_track;
            }
        }
        delay = FDC_STEP_NS;
        break;
    }
    case FDC_CMD_READ_SECTOR:
    case FDC_CMD_READ_SECTOR | FDC_CMD_FLAG_MULTIPLE:
    case FDC_CMD_WRITE_SECTOR:
    case FDC_CMD_WRITE_SECTOR | FDC_CMD_FLAG_MULTIPLE:
        delay = FDC_SECTOR_NS *
                ((cmd & FDC_CMD_FLAG_MULTIPLE) ? MAX(s->dma_seccount, 1)
                                               : 1);
        break;
    default:
        delay = FDC_SETTLE_NS;
        break;
    }

    timer_mod(s->cmd_timer, now + delay);
}

static uint8_t fdc_status(AtaristFdcState *s)
{
    uint8_t sr = s->status;

    if (s->busy) {
        sr |= FDC_SR_BUSY;
    }
    if (fdc_motor_running(s)) {
        sr |= FDC_SR_MOTOR_ON;
    } else {
        sr &= ~FDC_SR_MOTOR_ON;
    }
    return sr;
}

static uint64_t atarist_fdc_read(void *opaque, hwaddr addr, unsigned size)
{
    AtaristFdcState *s = opaque;

    switch (addr & ~1) {
    case DMA_REG_DATA:
        if (s->dma_mode & DMA_MODE_SECCOUNT) {
            return s->dma_seccount;
        }
        if (s->dma_mode & DMA_MODE_HDC) {
            return 0xff;        /* no ACSI device: floating bus */
        }
        switch ((s->dma_mode & (DMA_MODE_A1 | DMA_MODE_A0)) >> 1) {
        case FDC_REG_STATUS_CMD:
            fdc_clear_intrq(s);
            return fdc_status(s);
        case FDC_REG_TRACK:
            return s->track;
        case FDC_REG_SECTOR:
            return s->sector;
        default:
            return s->data;
        }
    case DMA_REG_MODE:
        return (s->dma_ok ? DMA_STATUS_OK : 0) |
               (s->dma_seccount ? DMA_STATUS_SECCOUNT : 0);
    case DMA_REG_ADDR_HI & ~1:
        return (s->dma_addr >> 16) & 0x3f;
    case DMA_REG_ADDR_MID & ~1:
        return (s->dma_addr >> 8) & 0xff;
    case DMA_REG_ADDR_LO & ~1:
        return s->dma_addr & 0xff;
    default:
        return 0xff;
    }
}

static void atarist_fdc_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    AtaristFdcState *s = opaque;

    switch (addr & ~1) {
    case DMA_REG_DATA:
        if (s->dma_mode & DMA_MODE_SECCOUNT) {
            s->dma_seccount = val & 0xff;
            return;
        }
        if (s->dma_mode & DMA_MODE_HDC) {
            return;             /* no ACSI device */
        }
        switch ((s->dma_mode & (DMA_MODE_A1 | DMA_MODE_A0)) >> 1) {
        case FDC_REG_STATUS_CMD:
            fdc_clear_intrq(s);
            fdc_command(s, val);
            return;
        case FDC_REG_TRACK:
            s->track = val;
            return;
        case FDC_REG_SECTOR:
            s->sector = val;
            return;
        default:
            s->data = val;
            return;
        }
    case DMA_REG_MODE:
        /* flipping the direction bit resets the DMA state machine */
        if ((s->dma_mode ^ val) & DMA_MODE_WRITE) {
            s->dma_ok = true;
        }
        s->dma_mode = val;
        return;
    case DMA_REG_ADDR_HI & ~1:
        s->dma_addr = deposit32(s->dma_addr, 16, 8, val & 0x3f);
        return;
    case DMA_REG_ADDR_MID & ~1:
        s->dma_addr = deposit32(s->dma_addr, 8, 8, val);
        return;
    case DMA_REG_ADDR_LO & ~1:
        s->dma_addr = deposit32(s->dma_addr, 0, 8, val);
        return;
    default:
        return;
    }
}

static const MemoryRegionOps atarist_fdc_ops = {
    .read = atarist_fdc_read,
    .write = atarist_fdc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

/* PSG port A lines: 0 = side select, 1/2 = drive selects (active low) */
static void atarist_fdc_porta(void *opaque, int line, int level)
{
    AtaristFdcState *s = opaque;

    if (line == 0) {
        s->side1 = !level;
    } else {
        s->sel_drive[line - 1] = level;
    }
}

/*
 * Geometry from the image: trust the boot sector BPB when sane
 * (sectors/track at 0x18, sides at 0x1A, both little-endian), fall
 * back to deriving it from the image size.
 */
static void fdc_probe_geometry(AtaristFloppy *fl, Error **errp)
{
    int64_t len = blk_getlength(fl->blk);
    int64_t total;
    uint8_t boot[FDC_SECTOR_SIZE];
    static const unsigned try_sides[] = { 2, 1 };
    static const unsigned try_spt[] = { 9, 10, 11, 12 };
    unsigned i, j;

    if (len <= 0) {
        fl->blk = NULL;         /* no medium */
        return;
    }
    if (len % FDC_SECTOR_SIZE) {
        error_setg(errp, "floppy image size %" PRId64
                   " is not a multiple of 512", len);
        return;
    }
    total = len / FDC_SECTOR_SIZE;

    if (blk_pread(fl->blk, 0, FDC_SECTOR_SIZE, boot, 0) == 0) {
        unsigned spt = lduw_le_p(boot + 0x18);
        unsigned sides = lduw_le_p(boot + 0x1a);

        if (spt >= 8 && spt <= 12 && sides >= 1 && sides <= 2 &&
            total % (spt * sides) == 0) {
            unsigned tracks = total / (spt * sides);

            if (tracks >= 75 && tracks <= FDC_MAX_TRACK + 1) {
                fl->spt = spt;
                fl->sides = sides;
                fl->tracks = tracks;
                return;
            }
        }
    }

    for (i = 0; i < ARRAY_SIZE(try_sides); i++) {
        for (j = 0; j < ARRAY_SIZE(try_spt); j++) {
            unsigned per = try_sides[i] * try_spt[j];
            unsigned tracks = total / per;

            if (total % per == 0 && tracks >= 75 &&
                tracks <= FDC_MAX_TRACK + 1) {
                fl->spt = try_spt[j];
                fl->sides = try_sides[i];
                fl->tracks = tracks;
                return;
            }
        }
    }
    error_setg(errp, "cannot derive a floppy geometry from %" PRId64
               " sectors (raw .ST images only)", total);
}

static void atarist_fdc_reset_hold(Object *obj, ResetType type)
{
    AtaristFdcState *s = ATARIST_FDC(obj);

    s->track = s->sector = s->data = s->status = s->cmd = 0;
    s->busy = false;
    s->intrq = false;
    s->motor_off_ns = 0;
    s->dma_mode = 0;
    s->dma_addr = 0;
    s->dma_seccount = 0;
    s->dma_ok = true;
    timer_del(s->cmd_timer);
    fdc_update_irq(s);
}

static void atarist_fdc_init(Object *obj)
{
    AtaristFdcState *s = ATARIST_FDC(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &atarist_fdc_ops, s,
                          "atarist.fdc-dma", 0x0c);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), atarist_fdc_porta, "porta", 3);
    for (i = 0; i < FDC_NUM_DRIVES; i++) {
        s->sel_drive[i] = true;         /* deselected */
    }
}

static void atarist_fdc_realize(DeviceState *dev, Error **errp)
{
    AtaristFdcState *s = ATARIST_FDC(dev);
    int i;

    s->cmd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fdc_cmd_done, s);
    for (i = 0; i < FDC_NUM_DRIVES; i++) {
        if (!s->drive[i].blk) {
            continue;
        }
        fdc_probe_geometry(&s->drive[i], errp);
        if (*errp) {
            return;
        }
    }
}

static const Property atarist_fdc_properties[] = {
    DEFINE_PROP_DRIVE("driveA", AtaristFdcState, drive[0].blk),
    DEFINE_PROP_DRIVE("driveB", AtaristFdcState, drive[1].blk),
};

static void atarist_fdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = atarist_fdc_realize;
    device_class_set_props(dc, atarist_fdc_properties);
    rc->phases.hold = atarist_fdc_reset_hold;
}

/*
 * ---------------------------------------------------------------------
 * Machine
 * ---------------------------------------------------------------------
 */

#define TYPE_ATARIST_MACHINE MACHINE_TYPE_NAME("atarist")
OBJECT_DECLARE_SIMPLE_TYPE(AtaristMachineState, ATARIST_MACHINE)

typedef struct AtaristBank {
    struct AtaristMachineState *m;
    uint32_t ram_off;           /* offset of this bank in machine->ram */
    uint32_t chip_size;         /* installed bytes, 0 = empty */
    unsigned chip_colbits;      /* DRAM column bits of the chips */
    unsigned cfg_colbits;       /* column bits the MMU drives */
    MemoryRegion *mr;           /* currently mapped window */
} AtaristBank;

struct AtaristMachineState {
    MachineState parent_obj;

    M68kCPU cpu;
    AtaristGlueState glue;
    AtaristMfpState mfp;
    AtaristVideoState video;
    AtaristPsgState psg;
    AtaristIkbdState ikbd;
    AtaristFdcState fdc;

    MemoryRegion rom;
    MemoryRegion rom_low;       /* reset vectors readable at 0 */
    MemoryRegion cart;
    MemoryRegion berr;          /* everything unassigned bus-errors */
    MemoryRegion ram_window;    /* container for the decoded banks */
    MemoryRegion memcfg_mr;

    uint8_t memcfg;
    AtaristBank bank[2];
    uint8_t *ram_ptr;

    uint32_t reset_sp;
    uint32_t reset_pc;
    uint8_t rom_head[8];
};

/* bank-size code (per the MMU config register) to bytes/column bits */
static const uint32_t memcfg_bank_bytes[4] = {
    128 * KiB, 512 * KiB, 2 * MiB, 2 * MiB,
};
static const unsigned memcfg_bank_colbits[4] = { 8, 9, 10, 10 };

static unsigned atarist_chip_colbits(uint32_t size)
{
    switch (size) {
    case 128 * KiB:
        return 8;
    case 512 * KiB:
        return 9;
    default:
        return 10;
    }
}

static uint32_t atarist_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

/* absent bank: reads float low, writes vanish, no bus fault */
static uint64_t atarist_bank_empty_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    return 0;
}

static void atarist_bank_empty_write(void *opaque, hwaddr addr,
                                     uint64_t val, unsigned size)
{
}

static const MemoryRegionOps atarist_bank_empty_ops = {
    .read = atarist_bank_empty_read,
    .write = atarist_bank_empty_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * A bank whose configured window exceeds the installed chips: the DRAM
 * ignores the top column/row address bits the MMU multiplexes out, so
 * the word address folds - TOS's sizing probes at +0x208/+0x408 rely
 * on exactly this.
 */
static uint32_t atarist_bank_fold(AtaristBank *b, hwaddr addr)
{
    unsigned ccol = b->cfg_colbits;
    unsigned scol = b->chip_colbits;
    uint32_t word = ((addr >> 1) & ((1 << scol) - 1)) |
                    (((addr >> (1 + ccol)) & ((1 << scol) - 1)) << scol);

    return b->ram_off + word * 2;
}

static uint64_t atarist_bank_alias_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    AtaristBank *b = opaque;
    uint8_t *p = b->m->ram_ptr + atarist_bank_fold(b, addr);

    if (size == 1) {
        return p[addr & 1];
    }
    return ((uint64_t)p[0] << 8) | p[1];
}

static void atarist_bank_alias_write(void *opaque, hwaddr addr,
                                     uint64_t val, unsigned size)
{
    AtaristBank *b = opaque;
    uint8_t *p = b->m->ram_ptr + atarist_bank_fold(b, addr);

    if (size == 1) {
        p[addr & 1] = val;
    } else {
        p[0] = val >> 8;
        p[1] = val;
    }
}

static const MemoryRegionOps atarist_bank_alias_ops = {
    .read = atarist_bank_alias_read,
    .write = atarist_bank_alias_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

/* rebuild the two decoded RAM bank windows for the current config */
static void atarist_apply_memcfg(AtaristMachineState *m)
{
    MachineState *machine = MACHINE(m);
    uint32_t base = 0;
    int i;

    for (i = 0; i < 2; i++) {
        AtaristBank *b = &m->bank[i];
        unsigned code = i ? MEMCFG_BANK1(m->memcfg) : MEMCFG_BANK0(m->memcfg);
        uint32_t cfg_size = memcfg_bank_bytes[code];
        MemoryRegion *mr = g_new0(MemoryRegion, 1);

        if (b->mr) {
            memory_region_del_subregion(&m->ram_window, b->mr);
            object_unparent(OBJECT(b->mr));
            b->mr = NULL;
        }

        b->cfg_colbits = memcfg_bank_colbits[code];
        if (b->chip_size == 0) {
            memory_region_init_io(mr, OBJECT(machine),
                                  &atarist_bank_empty_ops, b,
                                  "atarist.bank-empty", cfg_size);
        } else if (cfg_size <= b->chip_size) {
            memory_region_init_alias(mr, OBJECT(machine), "atarist.bank",
                                     machine->ram, b->ram_off, cfg_size);
        } else {
            memory_region_init_io(mr, OBJECT(machine),
                                  &atarist_bank_alias_ops, b,
                                  "atarist.bank-folded", cfg_size);
        }
        memory_region_add_subregion(&m->ram_window, base, mr);
        b->mr = mr;
        base += cfg_size;
    }
}

static uint64_t atarist_memcfg_read(void *opaque, hwaddr addr, unsigned size)
{
    AtaristMachineState *m = opaque;

    return (addr == 1) ? m->memcfg : 0xff;
}

static void atarist_memcfg_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    AtaristMachineState *m = opaque;

    if (addr == 1) {
        m->memcfg = val & 0x0f;
        atarist_apply_memcfg(m);
    }
}

static const MemoryRegionOps atarist_memcfg_ops = {
    .read = atarist_memcfg_read,
    .write = atarist_memcfg_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * The first 8 bytes of the address space: the GLUE feeds the 68000's
 * reset vector fetch from ROM, but writes still go to RAM (whose
 * contents are simply invisible here).
 */
static uint64_t atarist_rom_low_read(void *opaque, hwaddr addr,
                                     unsigned size)
{
    AtaristMachineState *m = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | m->rom_head[addr + i];
    }
    return v;
}

static void atarist_rom_low_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    AtaristMachineState *m = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        m->ram_ptr[addr + size - 1 - i] = val & 0xff;
        val >>= 8;
    }
}

static const MemoryRegionOps atarist_rom_low_ops = {
    .read = atarist_rom_low_read,
    .write = atarist_rom_low_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* the cartridge port with nothing plugged in: data lines float high */
static uint64_t atarist_cart_read(void *opaque, hwaddr addr, unsigned size)
{
    return (1ULL << (size * 8)) - 1;
}

static void atarist_cart_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
}

static const MemoryRegionOps atarist_cart_ops = {
    .read = atarist_cart_read,
    .write = atarist_cart_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* everything unassigned bus-errors; TOS probes hardware this way */
static MemTxResult atarist_berr_read(void *opaque, hwaddr addr,
                                     uint64_t *data, unsigned size,
                                     MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP,
                  "atarist: read  0x%06x (%d) -> BERR pc=0x%06x\n",
                  (unsigned)addr, size, atarist_trace_pc());
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult atarist_berr_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned size,
                                      MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP,
                  "atarist: write 0x%06x (%d) <- 0x%" PRIx64
                  " -> BERR pc=0x%06x\n",
                  (unsigned)addr, size, val, atarist_trace_pc());
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps atarist_berr_ops = {
    .read_with_attrs = atarist_berr_read,
    .write_with_attrs = atarist_berr_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};


static void atarist_machine_reset(void *opaque)
{
    AtaristMachineState *m = opaque;
    M68kCPU *cpu = &m->cpu;

    cpu_reset(CPU(cpu));
    /*
     * SP/PC come from the first two ROM longwords (the GLUE maps them
     * over address 0 for the reset fetch).
     */
    cpu->env.aregs[7] = m->reset_sp;
    cpu->env.pc = m->reset_pc;

    m->memcfg = MEMCFG_RESET;
    atarist_apply_memcfg(m);
}

/* the RESET instruction: reset the chip set, not the CPU */
static void atarist_reset_out(void *opaque, int n, int level)
{
    AtaristMachineState *m = opaque;

    if (!level) {
        return;
    }
    device_cold_reset(DEVICE(&m->mfp));
    device_cold_reset(DEVICE(&m->video));
    device_cold_reset(DEVICE(&m->psg));
    device_cold_reset(DEVICE(&m->ikbd));
    device_cold_reset(DEVICE(&m->fdc));
}

static void atarist_machine_init(MachineState *machine)
{
    AtaristMachineState *m = ATARIST_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    ram_addr_t ram_size = machine->ram_size;
    const char *bios_name = machine->firmware ?: ATARIST_ROM_FILENAME;
    char *filename;
    int bios_size;
    uint32_t bank_sizes[2];
    int i;

    /* the two DRAM banks take 128K, 512K or 2MB chip sets */
    switch (ram_size) {
    case 256 * KiB:
        bank_sizes[0] = bank_sizes[1] = 128 * KiB;
        break;
    case 512 * KiB:
        bank_sizes[0] = 512 * KiB;
        bank_sizes[1] = 0;
        break;
    case 1 * MiB:
        bank_sizes[0] = bank_sizes[1] = 512 * KiB;
        break;
    case 2 * MiB:
        bank_sizes[0] = 2 * MiB;
        bank_sizes[1] = 0;
        break;
    case 2 * MiB + 512 * KiB:
        bank_sizes[0] = 2 * MiB;
        bank_sizes[1] = 512 * KiB;
        break;
    case 4 * MiB:
        bank_sizes[0] = bank_sizes[1] = 2 * MiB;
        break;
    default:
        error_report("RAM size must be 256K, 512K, 1M, 2M, 2.5M or 4M");
        exit(1);
    }

    /* CPU */
    object_initialize_child(OBJECT(machine), "cpu", &m->cpu,
                            machine->cpu_type);
    qdev_realize(DEVICE(&m->cpu), NULL, &error_fatal);
    qdev_connect_gpio_out_named(DEVICE(&m->cpu), "reset-out", 0,
                                qemu_allocate_irq(atarist_reset_out, m, 0));

    /* unassigned addresses deliver bus errors */
    memory_region_init_io(&m->berr, OBJECT(machine), &atarist_berr_ops,
                          NULL, "atarist.berr", ATARIST_ADDR_SPACE);
    memory_region_add_subregion_overlap(sysmem, 0, &m->berr, -2);

    /* RAM: machine->ram split over the two banks, decoded by config */
    m->ram_ptr = memory_region_get_ram_ptr(machine->ram);
    memory_region_init(&m->ram_window, OBJECT(machine),
                       "atarist.ram-window", ATARIST_RAM_WINDOW);
    memory_region_add_subregion(sysmem, 0, &m->ram_window);
    for (i = 0; i < 2; i++) {
        m->bank[i].m = m;
        m->bank[i].chip_size = bank_sizes[i];
        m->bank[i].ram_off = i ? bank_sizes[0] : 0;
        m->bank[i].chip_colbits = atarist_chip_colbits(bank_sizes[i]);
    }
    m->memcfg = MEMCFG_RESET;
    atarist_apply_memcfg(m);

    /* ROM */
    memory_region_init_rom(&m->rom, NULL, "atarist.rom", ATARIST_ROM_SIZE,
                           &error_abort);
    memory_region_add_subregion(sysmem, ATARIST_ROM_BASE, &m->rom);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, ATARIST_ROM_BASE,
                                        ATARIST_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }
    if (!qtest_enabled()) {
        uint8_t *ptr;

        if (bios_size <= 0 || bios_size > ATARIST_ROM_SIZE) {
            error_report("could not load TOS ROM '%s' (192KB TOS 1.0x "
                         "images map at 0xFC0000)", bios_name);
            exit(1);
        }
        ptr = rom_ptr(ATARIST_ROM_BASE, 8);
        assert(ptr != NULL);
        memcpy(m->rom_head, ptr, 8);
        m->reset_sp = ldl_be_p(ptr);
        m->reset_pc = ldl_be_p(ptr + 4);
    }

    /* reset vectors at address 0 read from ROM, writes land in RAM */
    memory_region_init_io(&m->rom_low, OBJECT(machine),
                          &atarist_rom_low_ops, m, "atarist.rom-low", 8);
    memory_region_add_subregion_overlap(sysmem, 0, &m->rom_low, 1);

    /* the empty cartridge port must not bus-error (TOS probes it
     * before it has a stack) */
    memory_region_init_io(&m->cart, OBJECT(machine), &atarist_cart_ops,
                          NULL, "atarist.cart", ATARIST_CART_SIZE);
    memory_region_add_subregion(sysmem, ATARIST_CART_BASE, &m->cart);

    /* MMU memory config */
    memory_region_init_io(&m->memcfg_mr, OBJECT(machine),
                          &atarist_memcfg_ops, m, "atarist.memcfg", 2);
    memory_region_add_subregion(sysmem, ATARIST_MEMCFG_BASE, &m->memcfg_mr);

    /* GLUE priority encoder, fed back from the CPU's IACK lines */
    object_initialize_child(OBJECT(machine), "glue", &m->glue,
                            TYPE_ATARIST_GLUE);
    object_property_set_link(OBJECT(&m->glue), "cpu", OBJECT(&m->cpu),
                             &error_abort);

    /* video */
    object_initialize_child(OBJECT(machine), "video", &m->video,
                            TYPE_ATARIST_VIDEO);
    m->video.ram = machine->ram;
    m->video.ram_size = ram_size;
    sysbus_realize(SYS_BUS_DEVICE(&m->video), &error_fatal);
    memory_region_add_subregion(sysmem, ATARIST_VIDEO_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->video),
                                                       0));

    /* MFP */
    object_initialize_child(OBJECT(machine), "mfp", &m->mfp,
                            TYPE_ATARIST_MFP);
    object_property_set_link(OBJECT(&m->mfp), "video", OBJECT(&m->video),
                             &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&m->mfp), &error_fatal);
    memory_region_add_subregion(sysmem, ATARIST_MFP_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->mfp),
                                                       0));

    object_property_set_link(OBJECT(&m->glue), "mfp", OBJECT(&m->mfp),
                             &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&m->glue), &error_fatal);

    sysbus_connect_irq(SYS_BUS_DEVICE(&m->mfp), 0,
                       qdev_get_gpio_in_named(DEVICE(&m->glue), "irq",
                                              GLUE_IRQ_MFP));
    sysbus_connect_irq(SYS_BUS_DEVICE(&m->video), 0,
                       qdev_get_gpio_in_named(DEVICE(&m->glue), "irq",
                                              GLUE_IRQ_VBL));
    for (i = 2; i <= 6; i += 2) {
        qdev_connect_gpio_out_named(DEVICE(&m->cpu), "iack-out", i,
                                    qdev_get_gpio_in_named(DEVICE(&m->glue),
                                                           "iack", i));
    }

    /* PSG */
    object_initialize_child(OBJECT(machine), "psg", &m->psg,
                            TYPE_ATARIST_PSG);
    sysbus_realize(SYS_BUS_DEVICE(&m->psg), &error_fatal);
    memory_region_add_subregion(sysmem, ATARIST_PSG_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->psg),
                                                       0));

    /* the ACIAs and the IKBD behind them */
    object_initialize_child(OBJECT(machine), "ikbd", &m->ikbd,
                            TYPE_ATARIST_IKBD);
    sysbus_realize(SYS_BUS_DEVICE(&m->ikbd), &error_fatal);
    memory_region_add_subregion(sysmem, ATARIST_ACIA_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->ikbd),
                                                       0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&m->ikbd), 0,
                       qdev_get_gpio_in_named(DEVICE(&m->mfp), "gpip",
                                              MFP_GPIP_ACIA));

    /* WD1772 + DMA */
    object_initialize_child(OBJECT(machine), "fdc", &m->fdc,
                            TYPE_ATARIST_FDC);
    for (i = 0; i < FDC_NUM_DRIVES; i++) {
        DriveInfo *dinfo = drive_get(IF_FLOPPY, 0, i);

        if (dinfo) {
            qdev_prop_set_drive(DEVICE(&m->fdc), i ? "driveB" : "driveA",
                                blk_by_legacy_dinfo(dinfo));
        }
    }
    sysbus_realize(SYS_BUS_DEVICE(&m->fdc), &error_fatal);
    memory_region_add_subregion(sysmem, ATARIST_DMA_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->fdc),
                                                       0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&m->fdc), 0,
                       qdev_get_gpio_in_named(DEVICE(&m->mfp), "gpip",
                                              MFP_GPIP_FDC));
    for (i = 0; i < 3; i++) {
        qdev_connect_gpio_out_named(DEVICE(&m->psg), "porta", i,
                                    qdev_get_gpio_in_named(DEVICE(&m->fdc),
                                                           "porta", i));
    }

    qemu_register_reset(atarist_machine_reset, m);
}

static void atarist_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68000"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Atari 1040STF";
    mc->init = atarist_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->block_default_type = IF_FLOPPY;
    mc->default_ram_size = 1 * MiB;
    mc->default_ram_id = "atarist.ram";
}

static const TypeInfo atarist_machine_typeinfo[] = {
    {
        .name       = TYPE_ATARIST_MFP,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(AtaristMfpState),
        .instance_init = atarist_mfp_init,
        .class_init = atarist_mfp_class_init,
    },
    {
        .name       = TYPE_ATARIST_VIDEO,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(AtaristVideoState),
        .instance_init = atarist_video_init,
        .class_init = atarist_video_class_init,
    },
    {
        .name       = TYPE_ATARIST_GLUE,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(AtaristGlueState),
        .instance_init = atarist_glue_init,
        .class_init = atarist_glue_class_init,
    },
    {
        .name       = TYPE_ATARIST_IKBD,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(AtaristIkbdState),
        .instance_init = atarist_ikbd_init,
        .class_init = atarist_ikbd_class_init,
    },
    {
        .name       = TYPE_ATARIST_FDC,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(AtaristFdcState),
        .instance_init = atarist_fdc_init,
        .class_init = atarist_fdc_class_init,
    },
    {
        .name       = TYPE_ATARIST_PSG,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(AtaristPsgState),
        .instance_init = atarist_psg_init,
        .class_init = atarist_psg_class_init,
    },
    {
        .name       = TYPE_ATARIST_MACHINE,
        .parent     = TYPE_MACHINE,
        .instance_size = sizeof(AtaristMachineState),
        .class_init = atarist_machine_class_init,
    },
};

DEFINE_TYPES(atarist_machine_typeinfo)
