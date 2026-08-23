/*
 * Radius PrecisionColor 24Xp NuBus video card (reverse-engineering model)
 *
 * Register map recovered from the card's declaration ROM (PrimaryInit and
 * video driver, firmware V1.32 "by Steve Lemke"):
 *
 * Slot space (offsets from 0xFs000000):
 *   0x000000-0x3FFFFF  VRAM (1/2/4 MB; ROM sResources for each size)
 *   0xC80000           RAMDAC (Bt473-style, byte lane 2, addr+data inverted)
 *     +0x06  command register        (guest writes 0xCF/0xCB/0xC7)
 *     +0x0A  overlay palette data
 *     +0x0E  overlay palette address
 *     +0x16  pixel read mask
 *     +0x1A  palette data (R,G,B)
 *     +0x1E  palette write address
 *   0xD00002 (r)       status: bit4 = CLUT busy/retrace, bits5,6 polled
 *                      by PrimaryInit (memory config sense?)
 *   0xD00003 (w)       control: bits0-2 depth code (4/5/6/7/0 =
 *                      1/4/8/16/32 bpp), bit4 = blank while reprogramming,
 *                      bit5 = enable (set at end of PrimaryInit),
 *                      bit7 = VBL IRQ clear/inhibit (1 = clear+hold)
 *   0xD80001 (rw)      config: bits5-7 depth/fetch code (0x00/0x20/0x40/
 *                      0xA0/0xC0 = 1/4/8/16/32 bpp), bits0-4 preserved
 *   0xD80005 (w)       mode config byte (from per-monitor mode table)
 *   0xD8000D (rw)      multi-function: read bits5-7 = monitor sense lines
 *                      (active low); write 0x80/0x40/0x20 drives individual
 *                      sense lines; write bits1-3 bit-bang a serial clock
 *                      synthesizer; write 0x08 then 0x00 = CRTC restart
 *   0xD80015..0xD80035 CRTC timing registers (9 bytes, written high
 *                      address first from mode table)
 *
 * The v2.0 ROM ("24Xp Series", BoardId 0x52B) and Radius' QuickColor
 * software additionally use a VRAM block-transfer aperture (fills AND
 * copies, recovered from the resident QCOD image's fill/blit fast paths):
 *
 *   0x400000-0x7FFFFF  blit aperture: address = VRAM offset + 0x400000.
 *     The engine is a 128-byte data buffer (ring) with an input cursor
 *     (advanced by aperture reads) and an output cursor (advanced by
 *     aperture writes), both mod 128:
 *     - Both reads and writes carry a naturally aligned power-of-two
 *       *span* encoded in the low address bits: with e = (addr | 1)
 *       (A0 is ignored - a byte access covers its 16-bit halfword),
 *       span = 2 * 2^(number of trailing 1s of e), base = addr & ~(span-1).
 *       The last covered halfword of an access sits at the end of the
 *       first half of its span: +1/+2 -> 4, +2..3/+6 -> 8, +6..7/+0xE ->
 *       16, +0xE..F -> 32, +0x1E -> 64, +0x3E..0x3F -> 128, ...
 *     - A read latches ("appends") the span at VRAM[base] into the ring
 *       at the input cursor.  The drivers always append exactly 128 bytes
 *       per load: 8 reads at +6 (8 x 16), 4 reads at +0xE (4 x 32), or
 *       reads at +0x1E and +0x5E (2 x 64).
 *     - A byte/word write block-transfers ring bytes (sequentially from
 *       the output cursor, wrapping mod 128) into the span at VRAM[base].
 *       The data value written is ignored (the drivers write whatever is
 *       in the loop counter).
 *     - Fills: seed the pattern in VRAM (usually a staging block at the
 *       reserved top of VRAM, 0x3FF800+), append it 128 bytes worth, then
 *       span-writes over the destination; consumption wraps mod 128, so
 *       the pattern replicates (dither/PixPat phase is kept by staging the
 *       pattern pre-rotated to the destination phase).
 *     - Copies (QuickColor screen->screen CopyBits, i.e. window moves and
 *       scrolls): interleaved span reads at the *source* and span writes
 *       at the *destination* (both walk an alignment ladder 4/8/16/32,
 *       then 64-byte blocks 1:1, with the source running ahead by at most
 *       124 bytes).  Only src == dst (mod 4) is required - the ring is a
 *       FIFO, not address-indexed.  Overlap is handled in software: a
 *       bottom-up row loop, and a separate right-to-left variant (which
 *       requires src == dst modulo the span size and pairs descending
 *       uniform-span read/writes).
 *     - A long write is a command: value 0x40000000|N block-transfers N
 *       ring bytes (mod-128 wrap) to the written offset; a small value N
 *       (the ROM driver uses 8) loads the N bytes at VRAM[off] replicated
 *       across the whole ring and resets both cursors.
 *     - A write to the control register 0xD00003 with bit5 (display
 *       enable) LOW resets both cursors.  This is the QCOD "depth pulse"
 *       (read D00002, write depth, write depth|0x20) that precedes/follows
 *       every engine batch: it re-synchronizes the ring before a transfer
 *       sequence.  The VBL ack (bit7 toggle with bit5 high) does not
 *       disturb the cursors.
 *   0xD40402 (r)       blit configuration: bit0 selects the maximum engine
 *                      chunk (0=0x80, 1=0x200 bytes) - VRAM bank layout
 *   0xD40403 (w)       blit engine arm/reset (ROM driver writes 1)
 *
 * All MMIO accesses are logged to stderr to capture the register protocol.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "hw/nubus/nubus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "ui/console.h"
#include "framebuffer.h"

#define TYPE_RADIUS_24XP "radius-24xp"
OBJECT_DECLARE_TYPE(Radius24XpState, Radius24XpClass, RADIUS_24XP)

#define R24XP_VRAM_SIZE     (4 * MiB)

#define R24XP_FILL_BASE     0x400000
#define R24XP_FILL_SIZE     R24XP_VRAM_SIZE

#define R24XP_DAC_BASE      0xC80000
#define R24XP_CTRL_BASE     0xD00000
#define R24XP_CRTC_BASE     0xD80000
#define R24XP_MMIO_BASE     R24XP_DAC_BASE
#define R24XP_MMIO_SIZE     (0xE00000 - R24XP_DAC_BASE)

/* 60.15Hz VBL like other Mac video hardware */
#define R24XP_VBL_PERIOD_NS (1000000000 / 60)

struct Radius24XpState {
    NubusDevice parent_obj;

    MemoryRegion vram;
    MemoryRegion deadbank;
    MemoryRegion fillaper;
    MemoryRegion mmio;
    MemoryRegion catchall;
    QemuConsole *con;
    QEMUTimer *vbl_timer;

    uint8_t *vram_ptr;

    /* recovered registers */
    uint8_t ctrl;           /* 0xD00003 shadow */
    uint8_t crtc_cfg;       /* 0xD80001 */
    uint8_t mode_cfg;       /* 0xD80005 */
    uint8_t sense_out;      /* last write to 0xD8000D */
    uint8_t crtc_regs[16];  /* 0xD80015.. index (off-0x15)/4 */

    /* RAMDAC state (values stored de-inverted, i.e. real DAC values) */
    uint8_t dac_cmd;
    uint8_t dac_mask;
    uint8_t clut_index;
    uint8_t clut_phase;
    uint8_t clut_rgb[3];
    uint32_t clut[256];
    uint8_t ovl_index;
    uint8_t ovl_phase;

    /* config */
    uint8_t sense;          /* monitor sense code (Apple style, 0-7) */
    bool trace;
    uint32_t width, height; /* current mode, derived from the CRTC regs */
    int htotal, vtotal;     /* total pixels/lines from the CRTC regs */
    double pixel_clock_hz;  /* from the SC11410CV serial word */
    int64_t vbl_period_ns;  /* derived from CRTC totals + pixel clock */

    /* SC11410CV serial bit-bang capture state */
    uint32_t clk_ds;
    int clk_nbits;
    bool clk_armed;
    int clk_prev;
    uint32_t vram_mb;       /* populated VRAM banks in MiB (1/2/4) */
    uint8_t status56;       /* probe: bitmask, which of D00002 bits5/6 toggle */
    bool boot_init;         /* leave the card in a valid 640x480x16 mode at
                             * reset, as the Mac ROM would, so a -kernel Linux
                             * boot (which skips the ROM) can drive it */
    uint32_t status_reads;  /* free-running counter to make bits5/6 toggle */

    /* VRAM block-transfer ("blit aperture") state: 128-byte ring FIFO */
    uint8_t blit_ring[128]; /* data buffer: reads append, writes consume */
    uint8_t ring_in;        /* input cursor (mod 128), advanced by reads */
    uint8_t ring_out;       /* output cursor (mod 128), advanced by writes */
    bool trace_fill;        /* log every fill-aperture op (very chatty) */

    bool invalidated;
};

static void r24xp_ring_reset(Radius24XpState *s);

static const char *r24xp_reg_name(hwaddr addr)
{
    switch (addr + R24XP_MMIO_BASE) {
    case 0xC80006: return "DAC.command";
    case 0xC8000A: return "DAC.ovl_data";
    case 0xC8000E: return "DAC.ovl_addr";
    case 0xC80016: return "DAC.pixel_mask";
    case 0xC8001A: return "DAC.clut_data";
    case 0xC8001E: return "DAC.clut_addr";
    case 0xD00002: return "CTRL.status";
    case 0xD00003: return "CTRL.control";
    case 0xD40402: return "FILL.config";
    case 0xD40403: return "FILL.arm";
    case 0xD80001: return "CRTC.config";
    case 0xD80005: return "CRTC.mode_cfg";
    case 0xD8000D: return "CRTC.sense_clk";
    case 0xD80015: case 0xD80019: case 0xD8001D: case 0xD80021:
    case 0xD80025: case 0xD80029: case 0xD8002D: case 0xD80031:
    case 0xD80035: return "CRTC.timing";
    default:       return "?";
    }
}

static void r24xp_log(Radius24XpState *s, const char *dir, hwaddr addr,
                      uint64_t val, unsigned size)
{
    static hwaddr last_addr = -1;
    static uint64_t last_val = -1;
    static char last_dir;
    static unsigned repeat;
    uint64_t pc = 0;

    if (!s->trace) {
        return;
    }

    /* collapse polling loops / VBL ack spam */
    if (addr == last_addr && val == last_val && dir[0] == last_dir) {
        repeat++;
        return;
    }
    if (repeat) {
        fprintf(stderr, "radius24xp:  ... last access repeated %u times\n",
                repeat);
        repeat = 0;
    }
    last_addr = addr;
    last_val = val;
    last_dir = dir[0];

    if (current_cpu) {
        pc = CPU_GET_CLASS(current_cpu)->get_pc(current_cpu);
    }
    fprintf(stderr, "radius24xp: %s %s size %u addr 0x%06" PRIx64
            " val 0x%02" PRIx64 " (~val 0x%02" PRIx64 ") pc 0x%" PRIx64 "\n",
            dir, r24xp_reg_name(addr), size,
            (uint64_t)(addr + R24XP_MMIO_BASE), val,
            (uint64_t)(~val & ((1ULL << (size * 8)) - 1)), pc);
}

static int r24xp_depth(Radius24XpState *s)
{
    /* D00003 bits 0-2: 4/5/6/7/0 => 1/4/8/16/32 bpp */
    switch (s->ctrl & 7) {
    case 4: return 1;
    case 5: return 4;
    case 6: return 8;
    case 7: return 16;
    case 0: return 32;
    default: return 8;
    }
}

/* --- RAMDAC --- */

static void r24xp_clut_write(Radius24XpState *s, uint8_t val)
{
    s->clut_rgb[s->clut_phase++] = val;
    if (s->clut_phase == 3) {
        s->clut_phase = 0;
        s->clut[s->clut_index] = (s->clut_rgb[0] << 16) |
                                 (s->clut_rgb[1] << 8) | s->clut_rgb[2];
        if (s->trace) {
            fprintf(stderr, "radius24xp: CLUT[%3u] = %06x\n",
                    s->clut_index, s->clut[s->clut_index]);
        }
        s->clut_index++;
        s->invalidated = true;
    }
}

static void r24xp_dac_write(Radius24XpState *s, hwaddr off, uint8_t cpuval)
{
    uint8_t val = ~cpuval; /* DAC data lines are inverted on the board */

    switch (off) {
    case 0x06:
        s->dac_cmd = val;
        s->invalidated = true;
        break;
    case 0x0A:
        if (++s->ovl_phase == 3) {
            s->ovl_phase = 0;
            s->ovl_index++;
        }
        break;
    case 0x0E:
        s->ovl_index = val;
        s->ovl_phase = 0;
        break;
    case 0x16:
        s->dac_mask = val;
        break;
    case 0x1A:
        r24xp_clut_write(s, val);
        break;
    case 0x1E:
        s->clut_index = val;
        s->clut_phase = 0;
        break;
    default:
        break;
    }
}

/* --- MMIO --- */

static uint64_t r24xp_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Radius24XpState *s = opaque;
    uint64_t val = 0;
    hwaddr full = addr + R24XP_MMIO_BASE;

    /*
     * The control/status page is decoded ignoring address bit 10, so the
     * 0xD00400 block aliases the 0xD00000 registers.  The v1.32 ROM only
     * used 0xD00000; the v2.0 ROM runs its VBL/depth poll at the 0xD00400
     * alias, so fold it here or that wait spins forever.
     */
    if (full >= R24XP_CTRL_BASE + 0x400 && full < R24XP_CTRL_BASE + 0x500) {
        full -= 0x400;
    }

    switch (full) {
    case 0xD00002:
        /*
         * bit4: CLUT/DAC write-ready ("busy") strobe.  The driver polls in a
         * bounded wait-until-clear loop immediately before every CLUT entry
         * write, so 0 == ready.  We always return ready.
         * bits5,6: two sense lines sampled by PrimaryInit over a long window
         * ("does it toggle?") that feed the *monitor id*, not VRAM.  The
         * status56 property lets us toggle each to probe the mapping.
         * bits0-3: read back the current depth code from the control
         * register.  Radius' QuickColor accelerator (QCOD 2, traced live)
         * relies on this: at the end of some blit fast paths it reads
         * D00002, masks 0x0F, and rewrites D00003 = depth then depth|0x20,
         * expecting the low nibble to mirror the programmed depth.
         */
        val = s->ctrl & 0x0f;
        s->status_reads++;
        if (s->status56 & 1) {          /* bit5 toggles */
            val |= (s->status_reads & 1) ? 0x20 : 0x00;
        }
        if (s->status56 & 2) {          /* bit6 toggles */
            val |= (s->status_reads & 1) ? 0x40 : 0x00;
        }
        break;
    case 0xD40402:
        /*
         * Fill-engine configuration.  The v2.0 ROM driver reads bit0 to pick
         * the maximum fill chunk (0 = 0x80 bytes, 1 = 0x200); our fill model
         * accepts any length, so report the small-bank layout.
         */
        val = 0;
        break;
    case 0xD80001:
        val = s->crtc_cfg;
        break;
    case 0xD8000D:
        /*
         * Monitor sense: bits7,6,5 are three open-drain sense lines
         * (read back directly; PrimaryInit computes code = (~val >> 5) & 7).
         * Apple sense code s->sense: line i reads low iff bit i of code set.
         * Lower bits read back the last written value (clock synth loop).
         */
        val = (s->sense_out & 0x1f) |
              ((~s->sense & 1) ? 0x20 : 0) |
              ((~s->sense & 2) ? 0x40 : 0) |
              ((~s->sense & 4) ? 0x80 : 0);
        break;
    default:
        break;
    }

    r24xp_log(s, "R", addr, val, size);
    return val;
}

/* Standard PC dot-clock crystal feeding the SC11410CV (datasheet Fosc). */
#define R24XP_FOSC_HZ 14318280.0

/*
 * Recompute the VBL period from the CRTC totals and the pixel clock the
 * guest programmed into the SC11410CV (captured by the bit-bang decoder).
 * refresh = pixel_clock / (Htotal * Vtotal).
 */
static void r24xp_recompute_vbl(Radius24XpState *s)
{
    if (s->htotal > 0 && s->vtotal > 0 && s->pixel_clock_hz > 1e6) {
        double refresh = s->pixel_clock_hz / (double)(s->htotal * s->vtotal);
        if (refresh >= 30 && refresh <= 120) {
            s->vbl_period_ns = (int64_t)(1e9 / refresh);
            return;
        }
    }
    s->vbl_period_ns = 1000000000 / 60;
}

/*
 * Recompute width/height/totals from the 9 CRTC registers.  Units recovered
 * from the ROM mode tables (verified against Apple/VGA standard timings):
 *   horizontal regs are in 8-pixel character clocks, stored as (count - 1);
 *   vertical regs are scanlines, stored as (count - 1); vertical display is a
 *   16-bit value split across two byte registers, with sync/blank-polarity
 *   flags in the top bits of the high byte.
 */
static void r24xp_update_timing(Radius24XpState *s)
{
    uint8_t *r = s->crtc_regs;
    int hchar = (r[8] + 1) + (r[7] + 1) + (r[6] + 1) + (r[5] + 1);
    int hact  = (r[8] + 1) * 8;
    int htot  = hchar * 8;
    int vact  = (((r[3] & 7) << 8) | r[4]) + 1;
    int vtot  = vact + (r[2] + 1) + (r[1] + 1) + (r[0] + 1);

    if (hact < 256 || hact > 2048 || vact < 200 || vact > 1200) {
        return;   /* not a plausible programmed mode yet */
    }

    s->width = hact;
    s->height = vact;
    s->htotal = htot;
    s->vtotal = vtot;
    s->invalidated = true;
    r24xp_recompute_vbl(s);
}

/*
 * SC11410CV serial-clock bit-bang decoder (port 0xD8000D).
 * Bit 1 = serial clock, bit 3 = serial data (board-inverted, like ROM/DAC),
 * bit 2 = start/latch pulse.  DS0 is shifted in first.  Datasheet fields:
 *   DS0 mode, DS[2:1] post-scaler P=2^sel, DS3 cclk, DS[10:4] N (numerator,
 *   VCO/feedback divider), DS[17:11] M (reference divider).
 *   CLKOUT = (N / (M*P)) * Fosc.
 */
static void r24xp_clockgen_write(Radius24XpState *s, uint8_t val)
{
    int clk = (val >> 1) & 1;
    int start = (val >> 2) & 1;

    if (clk && !s->clk_prev) {                 /* rising edge of serial clock */
        if (start) {                           /* start/latch: begin a frame */
            s->clk_ds = 0;
            s->clk_nbits = 0;
            s->clk_armed = true;
        } else if (s->clk_armed && s->clk_nbits < 18) {
            /* board inverts the data line, so the chip sees NOT(bit3) */
            int data = 1 - ((val >> 3) & 1);
            s->clk_ds |= (uint32_t)(data & 1) << s->clk_nbits;   /* DS0 first */
            if (++s->clk_nbits == 18) {
                uint32_t ds = s->clk_ds;
                unsigned p = 1u << ((ds >> 1) & 3);
                unsigned n = (ds >> 4) & 0x7f;
                unsigned m = (ds >> 11) & 0x7f;
                s->clk_armed = false;
                if (m && p) {
                    s->pixel_clock_hz = (double)n / (double)(m * p) *
                                        R24XP_FOSC_HZ;
                    if (s->trace) {
                        fprintf(stderr, "radius24xp: SC11410 ds=%05x "
                                "M=%u N=%u P=%u -> %.3f MHz\n", ds, m, n, p,
                                s->pixel_clock_hz / 1e6);
                    }
                    r24xp_recompute_vbl(s);
                }
            }
        }
    }
    s->clk_prev = clk;
}

static void r24xp_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Radius24XpState *s = opaque;
    hwaddr full = addr + R24XP_MMIO_BASE;

    r24xp_log(s, "W", addr, val, size);

    if (full >= R24XP_DAC_BASE && full < R24XP_DAC_BASE + 0x40) {
        r24xp_dac_write(s, full - R24XP_DAC_BASE, val);
        return;
    }

    /* control page decoded ignoring A10: fold the 0xD00400 alias (v2.0 ROM) */
    if (full >= R24XP_CTRL_BASE + 0x400 && full < R24XP_CTRL_BASE + 0x500) {
        full -= 0x400;
    }

    switch (full) {
    case 0xD40403:
        /* blit engine arm/reset (v2.0 ROM writes 1 before engine fills) */
        r24xp_ring_reset(s);
        break;
    case 0xD00003:
        if (val & 0x80) {
            /* VBL IRQ clear/inhibit */
            nubus_set_irq(&s->parent_obj, 0);
        }
        if (!(val & 0x20)) {
            /*
             * Display-enable bit low: this is the front half of the QCOD
             * "depth pulse" (R D00002 / W depth / W depth|0x20) issued
             * before or after every blit-aperture batch - it resets the
             * blit ring cursors.  The VBL ack (bit7 toggle, bit5 high)
             * does not, so an interrupt cannot corrupt a transfer.
             */
            r24xp_ring_reset(s);
        }
        if ((s->ctrl & 7) != (val & 7)) {
            s->invalidated = true;
        }
        s->ctrl = val;
        break;
    case 0xD80001:
        s->crtc_cfg = val;
        r24xp_update_timing(s);   /* last CRTC write in the program sequence */
        break;
    case 0xD80005:
        s->mode_cfg = val;
        break;
    case 0xD8000D:
        s->sense_out = val;
        r24xp_clockgen_write(s, val);
        break;
    case 0xD80015: case 0xD80019: case 0xD8001D: case 0xD80021:
    case 0xD80025: case 0xD80029: case 0xD8002D: case 0xD80031:
    case 0xD80035:
        s->crtc_regs[(full - 0xD80015) >> 2] = val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps r24xp_mmio_ops = {
    .read = r24xp_mmio_read,
    .write = r24xp_mmio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* Catch accesses to unimplemented parts of the slot space */
static uint64_t r24xp_catchall_read(void *opaque, hwaddr addr, unsigned size)
{
    Radius24XpState *s = opaque;

    if (s->trace) {
        uint64_t pc = current_cpu ?
            CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0;
        fprintf(stderr, "radius24xp: R UNMAPPED size %u addr 0x%06" PRIx64
                " pc 0x%" PRIx64 "\n", size, (uint64_t)addr, pc);
    }
    return 0;
}

static void r24xp_catchall_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    Radius24XpState *s = opaque;

    if (s->trace) {
        uint64_t pc = current_cpu ?
            CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0;
        fprintf(stderr, "radius24xp: W UNMAPPED size %u addr 0x%06" PRIx64
                " val 0x%" PRIx64 " pc 0x%" PRIx64 "\n",
                size, (uint64_t)addr, val, pc);
    }
}

static const MemoryRegionOps r24xp_catchall_ops = {
    .read = r24xp_catchall_read,
    .write = r24xp_catchall_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * --- VRAM block-transfer ("blit") aperture at slot 0x400000 ---
 *
 * The second 4 MB of the 8 MB memory decode is not more VRAM: it is a
 * command alias of the framebuffer.  Address 0x400000+X operates on VRAM
 * offset X.  The engine is a 128-byte ring buffer between VRAM and VRAM:
 *
 *   - an aperture READ appends the span-encoded block at the source
 *     address to the ring (input cursor advances mod 128),
 *   - an aperture WRITE stores ring bytes (from the output cursor,
 *     advancing mod 128) to the span-encoded block at the destination
 *     (the data value is ignored),
 *   - a write to control 0xD00003 with bit5 low ("depth pulse") resets
 *     both cursors.
 *
 * Fills append a staged pattern (loads always total exactly 128 bytes:
 * 8x16, 4x32 or 2x64) and then consume it cyclically over the target;
 * copies interleave source reads and destination writes 1:1 (source at
 * most 124 bytes ahead).  Span encoding for both directions:
 * e = (addr + access_size - 1) | 1 (A0 ignored: a byte access covers its
 * halfword), span = 2 * 2^trailing_ones(e), base = addr & ~(span - 1).
 * Observed spans: 4 (+1), 8 (+2/+3), 16 (+6/+7), 32 (+0xE/+0xF),
 * 64 (+0x1E/+0x1F), 128 (+0x3F), 1024 (+0x1FF).
 */

static void r24xp_fill_span(hwaddr addr, unsigned size,
                            hwaddr *base, hwaddr *len)
{
    hwaddr e = (addr + size - 1) | 1;
    hwaddr m = 1;

    while (e & m) {
        m <<= 1;
    }
    *len = 2 * m;
    *base = addr & ~(2 * m - 1);
}

static void r24xp_ring_reset(Radius24XpState *s)
{
    s->ring_in = 0;
    s->ring_out = 0;
}

/* Append a VRAM block to the ring (aperture read side). */
static void r24xp_fill_latch(Radius24XpState *s, hwaddr base, hwaddr len)
{
    hwaddr t;

    base &= R24XP_VRAM_SIZE - 1;
    for (t = 0; t < len && base + t < R24XP_VRAM_SIZE; t++) {
        s->blit_ring[(s->ring_in + t) & 127] = s->vram_ptr[base + t];
    }
    s->ring_in = (s->ring_in + len) & 127;
    if (s->trace_fill) {
        fprintf(stderr, "radius24xp: FILL latch @%06" PRIx64 " len 0x%"
                PRIx64 " in %u\n", (uint64_t)base, (uint64_t)len, s->ring_in);
    }
}

/* Store ring bytes to a VRAM block (aperture write side). */
static void r24xp_fill_run(Radius24XpState *s, hwaddr base, uint64_t len)
{
    hwaddr t;

    base &= R24XP_VRAM_SIZE - 1;
    for (t = 0; t < len && base + t < R24XP_VRAM_SIZE; t++) {
        s->vram_ptr[base + t] = s->blit_ring[(s->ring_out + t) & 127];
    }
    s->ring_out = (s->ring_out + len) & 127;
    s->invalidated = true;
    if (s->trace_fill) {
        fprintf(stderr, "radius24xp: FILL run @%06" PRIx64 " len 0x%"
                PRIx64 " out %u\n", (uint64_t)base, (uint64_t)len,
                s->ring_out);
    }
}

static void r24xp_fill_log(Radius24XpState *s, const char *dir, hwaddr addr,
                           uint64_t val, unsigned size)
{
    if (s->trace_fill) {
        uint64_t pc = current_cpu ?
            CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0;
        fprintf(stderr, "radius24xp: FILLAP %s size %u addr 0x%06" PRIx64
                " val 0x%" PRIx64 " pc 0x%" PRIx64 "\n",
                dir, size, (uint64_t)addr, val, pc);
    }
}

static uint64_t r24xp_fill_read(void *opaque, hwaddr addr, unsigned size)
{
    Radius24XpState *s = opaque;
    hwaddr base, len;

    /*
     * Reads append the span at the read address to the ring.  The drivers
     * discard the returned data (fixed-count dbf loops, not status polls).
     */
    r24xp_fill_log(s, "R", addr, 0, size);
    r24xp_fill_span(addr, size, &base, &len);
    r24xp_fill_latch(s, base, len);
    return 0;
}

static void r24xp_fill_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Radius24XpState *s = opaque;
    hwaddr base, len;

    r24xp_fill_log(s, "W", addr, val, size);

    if (size == 4) {
        /* engine command word at VRAM offset addr */
        if (val & 0x40000000) {
            r24xp_fill_run(s, addr, val & 0xffff);
        } else if (val) {
            /*
             * Observed: 8 = load the 8 pattern bytes at the offset.  The
             * ROM driver then issues length commands far larger than 8, so
             * the load replicates the bytes across the whole ring.
             */
            hwaddr off = addr & ~(hwaddr)7 & (R24XP_VRAM_SIZE - 1);
            unsigned n = MIN((unsigned)val, 8u);
            int j;

            r24xp_ring_reset(s);
            for (j = 0; j < 128; j++) {
                s->blit_ring[j] = s->vram_ptr[off + (j % n)];
            }
        }
    } else {
        /* span-encoded block store from the ring; data value ignored */
        r24xp_fill_span(addr, size, &base, &len);
        r24xp_fill_run(s, base, len);
    }
}

static const MemoryRegionOps r24xp_fill_ops = {
    .read = r24xp_fill_read,
    .write = r24xp_fill_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* Unpopulated VRAM banks: reads float to 0, writes vanish (probe aid). */
static uint64_t r24xp_deadbank_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}
static void r24xp_deadbank_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
}
static const MemoryRegionOps r24xp_deadbank_ops = {
    .read = r24xp_deadbank_read,
    .write = r24xp_deadbank_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- display --- */

static void r24xp_draw_line(Radius24XpState *s, uint8_t *d, uint8_t *src,
                            int width, int depth)
{
    int x;

    switch (depth) {
    case 1:
        /* the 1-bit pixel drives the MSB of the DAC pixel index */
        for (x = 0; x < width; x++) {
            int bit = (src[x >> 3] >> (7 - (x & 7))) & 1;
            uint32_t rgb = s->clut[(bit ? 0x80 : 0x00) & s->dac_mask];
            ((uint32_t *)d)[x] = rgb | 0xff000000;
        }
        break;
    case 4:
        /* the 4-bit pixel drives the high nibble of the DAC pixel index */
        for (x = 0; x < width; x++) {
            int idx = (src[x >> 1] >> (x & 1 ? 0 : 4)) & 0xf;
            ((uint32_t *)d)[x] = s->clut[(idx << 4) & s->dac_mask] |
                0xff000000;
        }
        break;
    case 8:
        for (x = 0; x < width; x++) {
            ((uint32_t *)d)[x] = s->clut[src[x] & s->dac_mask] | 0xff000000;
        }
        break;
    case 16:
        for (x = 0; x < width; x++) {
            uint16_t pix = (src[x * 2] << 8) | src[x * 2 + 1];
            uint32_t r = (pix >> 10) & 0x1f;
            uint32_t g = (pix >> 5) & 0x1f;
            uint32_t b = pix & 0x1f;
            ((uint32_t *)d)[x] = 0xff000000 |
                ((r << 3 | r >> 2) << 16) |
                ((g << 3 | g >> 2) << 8) |
                (b << 3 | b >> 2);
        }
        break;
    case 32:
        for (x = 0; x < width; x++) {
            /* big-endian xRGB */
            ((uint32_t *)d)[x] = 0xff000000 | (src[x * 4 + 1] << 16) |
                (src[x * 4 + 2] << 8) | src[x * 4 + 3];
        }
        break;
    }
}

static bool r24xp_update_display(void *opaque)
{
    Radius24XpState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int depth = r24xp_depth(s);
    int rowbytes = s->width * depth / 8;
    int y;

    if (surface_width(surface) != s->width ||
        surface_height(surface) != s->height) {
        qemu_console_resize(s->con, s->width, s->height);
        surface = qemu_console_surface(s->con);
        s->invalidated = true;
    }

    /* simple full redraw; fine for RE purposes */
    for (y = 0; y < s->height; y++) {
        uint8_t *d = surface_data(surface) +
            y * surface_stride(surface);
        r24xp_draw_line(s, d, s->vram_ptr + y * rowbytes, s->width, depth);
    }
    qemu_console_update(s->con, 0, 0, s->width, s->height);
    s->invalidated = false;

    return true;
}

static void r24xp_invalidate_display(void *opaque)
{
    Radius24XpState *s = opaque;
    s->invalidated = true;
}

static const GraphicHwOps r24xp_gfx_ops = {
    .invalidate = r24xp_invalidate_display,
    .gfx_update = r24xp_update_display,
};

/* --- VBL --- */

static void r24xp_vbl(void *opaque)
{
    Radius24XpState *s = opaque;

    /* bit7 of control high = IRQ inhibited */
    if (!(s->ctrl & 0x80)) {
        nubus_set_irq(&s->parent_obj, 1);
    }
    timer_mod(s->vbl_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->vbl_period_ns);
}

/* --- device --- */

static void r24xp_reset(DeviceState *dev)
{
    Radius24XpState *s = RADIUS_24XP(dev);
    int i;

    /*
     * IRQ inhibited (bit 7).  With boot-init the ROM is modelled as having
     * left the card in 640x480x16 (depth code 7) so a -kernel Linux, which
     * never runs the Mac ROM's Slot Manager, still finds a live truecolor
     * framebuffer to drive; otherwise the depth is code 0 (32bpp).
     */
    s->ctrl = s->boot_init ? (0x80 | 7) : 0x80;
    s->crtc_cfg = 0;
    s->clut_phase = 0;
    s->dac_mask = 0xff;
    s->vbl_period_ns = R24XP_VBL_PERIOD_NS;
    for (i = 0; i < 256; i++) {
        /* inverse gray ramp so 1bpp works before the CLUT is loaded */
        uint8_t v = 255 - i;
        s->clut[i] = (v << 16) | (v << 8) | v;
    }
    s->invalidated = true;
}

struct Radius24XpClass {
    DeviceClass parent_class;
    DeviceRealize parent_realize;
};

static void r24xp_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    NubusDevice *nd = NUBUS_DEVICE(dev);
    Radius24XpState *s = RADIUS_24XP(dev);
    Radius24XpClass *rc = RADIUS_24XP_GET_CLASS(dev);

    /* chain to TYPE_NUBUS_DEVICE realize (loads romfile, maps slot) */
    rc->parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    memory_region_init_ram(&s->vram, OBJECT(dev), "radius24xp-vram",
                           R24XP_VRAM_SIZE, errp);
    if (*errp) {
        return;
    }
    s->vram_ptr = memory_region_get_ram_ptr(&s->vram);
    memory_region_add_subregion(&nd->slot_mem, 0, &s->vram);

    /* Simulate a card with only vram_mb of populated VRAM: shadow the
     * unpopulated top banks with a dead region so read-back probes see 0. */
    if (s->vram_mb < 4) {
        hwaddr pop = (hwaddr)s->vram_mb * MiB;
        memory_region_init_io(&s->deadbank, OBJECT(dev), &r24xp_deadbank_ops,
                              s, "radius24xp-deadbank", R24XP_VRAM_SIZE - pop);
        memory_region_add_subregion_overlap(&nd->slot_mem, pop,
                                             &s->deadbank, 1);
    }

    memory_region_init_io(&s->fillaper, OBJECT(dev), &r24xp_fill_ops, s,
                          "radius24xp-fill", R24XP_FILL_SIZE);
    memory_region_add_subregion(&nd->slot_mem, R24XP_FILL_BASE, &s->fillaper);

    memory_region_init_io(&s->mmio, OBJECT(dev), &r24xp_mmio_ops, s,
                          "radius24xp-mmio", R24XP_MMIO_SIZE);
    memory_region_add_subregion(&nd->slot_mem, R24XP_MMIO_BASE, &s->mmio);

    memory_region_init_io(&s->catchall, OBJECT(dev), &r24xp_catchall_ops, s,
                          "radius24xp-catchall", NUBUS_SLOT_SIZE);
    memory_region_add_subregion_overlap(&nd->slot_mem, 0, &s->catchall, -1);

    /* initial mode until the guest programs the CRTC (sense-6 640x480) */
    s->width = 640;
    s->height = 480;
    s->vbl_period_ns = R24XP_VBL_PERIOD_NS;

    s->con = qemu_graphic_console_create(dev, 0, &r24xp_gfx_ops, s);

    s->vbl_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, r24xp_vbl, s);
    timer_mod(s->vbl_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->vbl_period_ns);
}

static const Property r24xp_properties[] = {
    /* Apple-style sense code: 6 = 13"/14" 640x480 RGB */
    DEFINE_PROP_UINT8("sense", Radius24XpState, sense, 6),
    DEFINE_PROP_BOOL("trace", Radius24XpState, trace, true),
    DEFINE_PROP_BOOL("trace_fill", Radius24XpState, trace_fill, false),
    /* probe knobs */
    DEFINE_PROP_UINT32("vram_mb", Radius24XpState, vram_mb, 4),
    DEFINE_PROP_UINT8("status56", Radius24XpState, status56, 0),
    DEFINE_PROP_BOOL("boot-init", Radius24XpState, boot_init, false),
};

static void r24xp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    Radius24XpClass *rc = RADIUS_24XP_CLASS(oc);

    dc->desc = "Radius PrecisionColor 24Xp";
    device_class_set_parent_realize(dc, r24xp_realize, &rc->parent_realize);
    device_class_set_legacy_reset(dc, r24xp_reset);
    device_class_set_props(dc, r24xp_properties);
}

static const TypeInfo r24xp_type_info = {
    .name = TYPE_RADIUS_24XP,
    .parent = TYPE_NUBUS_DEVICE,
    .instance_size = sizeof(Radius24XpState),
    .class_size = sizeof(Radius24XpClass),
    .class_init = r24xp_class_init,
};

static void r24xp_register_types(void)
{
    type_register_static(&r24xp_type_info);
}

type_init(r24xp_register_types)
