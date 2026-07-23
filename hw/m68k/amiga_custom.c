/*
 * Amiga custom chip register block (Agnus/Denise/Paula).
 *
 * This models the pieces of the chipset that Kickstart needs to get
 * going: the Paula interrupt controller (INTENA/INTREQ mapped onto the
 * 68k IPL lines), the beam counters (VPOSR/VHPOSR, plus the vertical
 * blank interrupt), and the Paula serial port wired to a chardev.
 * Display, blitter, copper, audio and disk DMA are not implemented;
 * writes to their registers are stored so later models can pick them
 * up, reads of unimplemented registers are logged.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "hw/m68k/amiga_custom.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "ui/console.h"

/* PAL timing: 227 colour clocks (~64us) per line, 312 lines per frame */
#define LINE_NS         64000
#define FRAME_LINES     312
#define FRAME_NS        (LINE_NS * FRAME_LINES)
#define LINE_CCKS       227

#define REG_BLTDDAT     0x000
#define REG_DMACONR     0x002
#define REG_VPOSR       0x004
#define REG_VHPOSR      0x006
#define REG_JOY0DAT     0x00a
#define REG_JOY1DAT     0x00c
#define REG_ADKCONR     0x010
#define REG_POTGOR      0x016
#define REG_SERDATR     0x018
#define REG_DSKBYTR     0x01a
#define REG_INTENAR     0x01c
#define REG_INTREQR     0x01e
#define REG_SERDAT      0x030
#define REG_SERPER      0x032
#define REG_POTGO       0x034
#define REG_COPCON      0x02e
#define REG_BLTCON0     0x040
#define REG_BLTCON1     0x042
#define REG_BLTAFWM     0x044
#define REG_BLTALWM     0x046
#define REG_BLTCPT      0x048
#define REG_BLTBPT      0x04c
#define REG_BLTAPT      0x050
#define REG_BLTDPT      0x054
#define REG_BLTSIZE     0x058
#define REG_BLTSIZV     0x05c
#define REG_BLTSIZH     0x05e
#define REG_BLTCMOD     0x060
#define REG_BLTBMOD     0x062
#define REG_BLTAMOD     0x064
#define REG_BLTDMOD     0x066
#define REG_BLTCDAT     0x070
#define REG_BLTBDAT     0x072
#define REG_BLTADAT     0x074
#define REG_DENISEID    0x07c
#define REG_DIWSTRT     0x08e
#define REG_DIWSTOP     0x090
#define REG_DDFSTRT     0x092
#define REG_DDFSTOP     0x094
#define REG_BPL1PT      0x0e0
#define REG_BPLCON0     0x100
#define REG_BPL1MOD     0x108
#define REG_BPL2MOD     0x10a
#define REG_COLOR00     0x180
#define REG_COP1LC      0x080
#define REG_COP2LC      0x084
#define REG_COPJMP1     0x088
#define REG_COPJMP2     0x08a
#define REG_DMACON      0x096
#define REG_INTENA      0x09a
#define REG_INTREQ      0x09c
#define REG_ADKCON      0x09e

#define SERDATR_TSRE    (1 << 12)
#define SERDATR_TBE     (1 << 13)
#define SERDATR_RBF     (1 << 14)

/*
 * Paula funnels the fifteen interrupt sources onto six 68k levels.
 */
static const uint16_t ipl_masks[7] = {
    [1] = INT_TBE | INT_DSKBLK | INT_SOFT,
    [2] = INT_PORTS,
    [3] = INT_COPER | INT_VERTB | INT_BLIT,
    [4] = INT_AUD0 | INT_AUD1 | INT_AUD2 | INT_AUD3,
    [5] = INT_RBF | INT_DSKSYN,
    [6] = INT_EXTER,
};

static void amiga_custom_update_irq(AmigaCustomState *s)
{
    uint16_t active = 0;
    int level;

    if (s->intena & INT_INTEN) {
        active = s->intreq & s->intena & 0x3fff;
    }
    for (level = 6; level > 0; level--) {
        if (active & ipl_masks[level]) {
            break;
        }
    }
    qemu_set_irq(s->ipl, level);
}

static void amiga_custom_post_int(AmigaCustomState *s, uint16_t bits)
{
    s->intreq |= bits;
    amiga_custom_update_irq(s);
}

static uint16_t setclr(uint16_t reg, uint16_t val, uint16_t mask)
{
    if (val & 0x8000) {
        return reg | (val & mask);
    }
    return reg & ~(val & mask);
}

/* --- blitter --- */

#define BLTCON0_USEA    (1 << 11)
#define BLTCON0_USEB    (1 << 10)
#define BLTCON0_USEC    (1 << 9)
#define BLTCON0_USED    (1 << 8)
#define BLTCON1_EFE     (1 << 4)
#define BLTCON1_IFE     (1 << 3)
#define BLTCON1_FCI     (1 << 2)
#define BLTCON1_DESC    (1 << 1)
#define BLTCON1_LINE    (1 << 0)

/* Agnus addresses chip RAM only */
#define CHIP_MASK       0x1ffffe

static uint16_t amiga_custom_reg(AmigaCustomState *s, unsigned reg)
{
    return s->regs[reg >> 1];
}

static uint32_t amiga_custom_ptr(AmigaCustomState *s, unsigned reg)
{
    return ((uint32_t)s->regs[reg >> 1] << 16) | s->regs[(reg >> 1) + 1];
}

static void amiga_custom_set_ptr(AmigaCustomState *s, unsigned reg,
                                 uint32_t val)
{
    s->regs[reg >> 1] = val >> 16;
    s->regs[(reg >> 1) + 1] = val;
}

static uint16_t chip_read16(uint32_t addr)
{
    uint8_t buf[2];

    dma_memory_read(&address_space_memory, addr & CHIP_MASK, buf, 2,
                    MEMTXATTRS_UNSPECIFIED);
    return (buf[0] << 8) | buf[1];
}

static void chip_write16(uint32_t addr, uint16_t val)
{
    uint8_t buf[2] = { val >> 8, val };

    dma_memory_write(&address_space_memory, addr & CHIP_MASK, buf, 2,
                     MEMTXATTRS_UNSPECIFIED);
}

static uint16_t blit_minterm(uint8_t lf, uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t d = 0;

    if (lf & 0x80) {
        d |= a & b & c;
    }
    if (lf & 0x40) {
        d |= a & b & ~c;
    }
    if (lf & 0x20) {
        d |= a & ~b & c;
    }
    if (lf & 0x10) {
        d |= a & ~b & ~c;
    }
    if (lf & 0x08) {
        d |= ~a & b & c;
    }
    if (lf & 0x04) {
        d |= ~a & b & ~c;
    }
    if (lf & 0x02) {
        d |= ~a & ~b & c;
    }
    if (lf & 0x01) {
        d |= ~a & ~b & ~c;
    }
    return d;
}

/*
 * Area fill: the carry enters each word at the right (bit 0, since
 * fill requires descending mode) and toggles at every set bit.
 */
static uint16_t blit_fill(uint16_t d, bool exclusive, bool *fc)
{
    uint16_t out = 0;
    int i;

    for (i = 0; i < 16; i++) {
        int bit = (d >> i) & 1;

        if (exclusive ? bit ^ *fc : bit | *fc) {
            out |= 1 << i;
        }
        *fc ^= bit;
    }
    return out;
}

#define BLTCON1_SIGN    (1 << 6)
#define BLTCON1_SUD     (1 << 4)
#define BLTCON1_SUL     (1 << 3)
#define BLTCON1_AUL     (1 << 2)
#define BLTCON1_SING    (1 << 1)

/*
 * Line mode: a Bresenham stepper.  BLTAPT is the error accumulator,
 * BLTAMOD/BLTBMOD the two increments, BLTCON1 carries the octant and
 * the initial sign, and C/D point at the word containing the first
 * pixel with the start bit in ASH.  One pixel is drawn per "row" of
 * the blit; with SING set only the first pixel on each raster line
 * lands, which is what area fill outlines need.
 */
static void amiga_custom_do_line(AmigaCustomState *s, int len)
{
    uint16_t con0 = amiga_custom_reg(s, REG_BLTCON0);
    uint16_t con1 = amiga_custom_reg(s, REG_BLTCON1);
    uint16_t adat = amiga_custom_reg(s, REG_BLTADAT);
    uint16_t afwm = amiga_custom_reg(s, REG_BLTAFWM);
    uint16_t bdat = amiga_custom_reg(s, REG_BLTBDAT);
    int16_t amod = amiga_custom_reg(s, REG_BLTAMOD);
    int16_t bmod = amiga_custom_reg(s, REG_BLTBMOD);
    int16_t cmod = amiga_custom_reg(s, REG_BLTCMOD);
    uint32_t cpt = amiga_custom_ptr(s, REG_BLTCPT);
    int16_t acc = amiga_custom_ptr(s, REG_BLTAPT);
    int ash = con0 >> 12, bsh = con1 >> 12;
    uint8_t lf = con0;
    bool sign = con1 & BLTCON1_SIGN;
    bool sud = con1 & BLTCON1_SUD;
    bool sul = con1 & BLTCON1_SUL;
    bool aul = con1 & BLTCON1_AUL;
    bool dot = false;
    bool zero = true;
    int i;

    for (i = 0; i < len; i++) {
        bool tex = (bdat >> (15 - ((bsh + i) & 15))) & 1;
        uint16_t a = (adat & afwm) >> ash;
        uint16_t b = tex ? 0xffff : 0;
        uint16_t c = chip_read16(cpt);
        uint16_t d = blit_minterm(lf, a, b, c);

        if (!(con1 & BLTCON1_SING) || !dot) {
            if (con0 & BLTCON0_USED) {
                chip_write16(cpt, d);
            }
            dot = true;
        }
        if (d) {
            zero = false;
        }

        /*
         * The minor ("sometimes") axis moves only while the sign is
         * clear, in the SUL direction; the major axis moves every
         * pixel in the AUL direction.  SUD selects which axis is
         * which: set means Y is the minor axis and X the major one.
         */
        acc += sign ? bmod : amod;
        if (!sign) {
            if (sud) {
                cpt += sul ? -cmod : cmod;
                dot = false;
            } else {
                if (sul) {
                    if (ash-- == 0) {
                        ash = 15;
                        cpt -= 2;
                    }
                } else {
                    if (++ash == 16) {
                        ash = 0;
                        cpt += 2;
                    }
                }
            }
        }
        if (sud) {
            if (aul) {
                if (ash-- == 0) {
                    ash = 15;
                    cpt -= 2;
                }
            } else {
                if (++ash == 16) {
                    ash = 0;
                    cpt += 2;
                }
            }
        } else {
            cpt += aul ? -cmod : cmod;
            dot = false;
        }
        sign = acc < 0;
    }

    amiga_custom_set_ptr(s, REG_BLTCPT, cpt);
    amiga_custom_set_ptr(s, REG_BLTDPT, cpt);
    s->regs[REG_BLTCON0 >> 1] = (ash << 12) | (con0 & 0x0fff);
    s->blit_zero = zero;
    amiga_custom_post_int(s, INT_BLIT);
}

static void amiga_custom_do_blit(AmigaCustomState *s, int width, int height)
{
    uint16_t con0 = amiga_custom_reg(s, REG_BLTCON0);
    uint16_t con1 = amiga_custom_reg(s, REG_BLTCON1);
    uint16_t afwm = amiga_custom_reg(s, REG_BLTAFWM);
    uint16_t alwm = amiga_custom_reg(s, REG_BLTALWM);
    uint16_t adat = amiga_custom_reg(s, REG_BLTADAT);
    uint16_t bdat = amiga_custom_reg(s, REG_BLTBDAT);
    uint16_t cdat = amiga_custom_reg(s, REG_BLTCDAT);
    int16_t amod = amiga_custom_reg(s, REG_BLTAMOD);
    int16_t bmod = amiga_custom_reg(s, REG_BLTBMOD);
    int16_t cmod = amiga_custom_reg(s, REG_BLTCMOD);
    int16_t dmod = amiga_custom_reg(s, REG_BLTDMOD);
    uint32_t apt = amiga_custom_ptr(s, REG_BLTAPT);
    uint32_t bpt = amiga_custom_ptr(s, REG_BLTBPT);
    uint32_t cpt = amiga_custom_ptr(s, REG_BLTCPT);
    uint32_t dpt = amiga_custom_ptr(s, REG_BLTDPT);
    int ash = con0 >> 12, bsh = con1 >> 12;
    uint8_t lf = con0;
    bool desc = con1 & BLTCON1_DESC;
    bool fill = desc && (con1 & (BLTCON1_EFE | BLTCON1_IFE));
    int step = desc ? -2 : 2;
    uint16_t aprev = 0, bprev = 0;
    bool zero = true;
    int x, y;

    if (con1 & BLTCON1_LINE) {
        amiga_custom_do_line(s, height);
        return;
    }

    for (y = 0; y < height; y++) {
        bool fc = con1 & BLTCON1_FCI;

        for (x = 0; x < width; x++) {
            uint16_t araw, ahold, bhold, c, d;
            uint16_t mask = 0xffff;

            if (x == 0) {
                mask &= afwm;
            }
            if (x == width - 1) {
                mask &= alwm;
            }
            if (con0 & BLTCON0_USEA) {
                adat = chip_read16(apt);
                apt += step;
            }
            araw = adat & mask;
            if (desc) {
                ahold = (((uint32_t)araw << 16) | aprev) >> (16 - ash);
            } else {
                ahold = (((uint32_t)aprev << 16) | araw) >> ash;
            }
            aprev = araw;

            if (con0 & BLTCON0_USEB) {
                bdat = chip_read16(bpt);
                bpt += step;
            }
            if (desc) {
                bhold = (((uint32_t)bdat << 16) | bprev) >> (16 - bsh);
            } else {
                bhold = (((uint32_t)bprev << 16) | bdat) >> bsh;
            }
            bprev = bdat;

            if (con0 & BLTCON0_USEC) {
                cdat = chip_read16(cpt);
                cpt += step;
            }
            c = cdat;

            d = blit_minterm(lf, ahold, bhold, c);
            if (fill) {
                d = blit_fill(d, con1 & BLTCON1_EFE, &fc);
            }
            if (d) {
                zero = false;
            }
            if (con0 & BLTCON0_USED) {
                chip_write16(dpt, d);
                dpt += step;
            }
        }
        if (con0 & BLTCON0_USEA) {
            apt += desc ? -amod : amod;
        }
        if (con0 & BLTCON0_USEB) {
            bpt += desc ? -bmod : bmod;
        }
        if (con0 & BLTCON0_USEC) {
            cpt += desc ? -cmod : cmod;
        }
        if (con0 & BLTCON0_USED) {
            dpt += desc ? -dmod : dmod;
        }
    }

    amiga_custom_set_ptr(s, REG_BLTAPT, apt);
    amiga_custom_set_ptr(s, REG_BLTBPT, bpt);
    amiga_custom_set_ptr(s, REG_BLTCPT, cpt);
    amiga_custom_set_ptr(s, REG_BLTDPT, dpt);
    s->blit_zero = zero;
    amiga_custom_post_int(s, INT_BLIT);
}

/* --- copper --- */

#define DMACON_DMAEN    (1 << 9)
#define DMACON_COPEN    (1 << 7)

static void amiga_custom_reg_write(AmigaCustomState *s, unsigned reg,
                                   uint16_t val);

/*
 * Frame-atomic copper: the whole list is executed at the vertical
 * blank with every WAIT considered satisfied, so all the MOVEs for a
 * frame are applied in one go.  That is enough for displays that only
 * use the copper to reload pointers and palettes per frame; mid-frame
 * raster effects would need a beam-synchronous model.
 */
static void amiga_custom_run_copper(AmigaCustomState *s, uint32_t pc)
{
    int budget = 20000;

    if ((s->dmacon & (DMACON_DMAEN | DMACON_COPEN)) !=
        (DMACON_DMAEN | DMACON_COPEN)) {
        return;
    }

    while (budget-- > 0) {
        uint16_t ir1 = chip_read16(pc);
        uint16_t ir2 = chip_read16(pc + 2);

        pc += 4;
        if (!(ir1 & 1)) {
            unsigned reg = ir1 & 0x1fe;

            if (reg < 0x80 && reg != REG_COPJMP1 && reg != REG_COPJMP2 &&
                !(amiga_custom_reg(s, REG_COPCON) & 2)) {
                /* illegal MOVE halts the copper */
                break;
            }
            if (reg == REG_COPJMP1) {
                pc = amiga_custom_ptr(s, REG_COP1LC);
            } else if (reg == REG_COPJMP2) {
                pc = amiga_custom_ptr(s, REG_COP2LC);
            } else {
                amiga_custom_reg_write(s, reg, ir2);
            }
        } else if (!(ir2 & 1)) {
            /* WAIT: the conventional end-of-list marker never matches */
            if ((ir1 >> 8) == 0xff && (ir1 & 0xfe) == 0xfe) {
                break;
            }
        } else {
            /* SKIP: by end of frame the position has been reached */
            pc += 4;
        }
    }
}

/* --- beam counters --- */

static void amiga_custom_beam_pos(AmigaCustomState *s, uint32_t *vpos,
                                  uint32_t *hpos)
{
    int64_t t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->frame_origin_ns;

    if (t < 0) {
        t = 0;
    }
    t %= FRAME_NS;
    *vpos = t / LINE_NS;
    *hpos = (t % LINE_NS) * LINE_CCKS / LINE_NS;
}

static void amiga_custom_vblank(void *opaque)
{
    AmigaCustomState *s = opaque;

    s->frame_origin_ns += FRAME_NS;
    timer_mod(&s->vblank_timer, s->frame_origin_ns + FRAME_NS);
    /* the copper restarts from the top of its list every frame */
    amiga_custom_run_copper(s, amiga_custom_ptr(s, REG_COP1LC));
    amiga_custom_post_int(s, INT_VERTB);
}

/* --- display --- */

#define DMACON_BPLEN    (1 << 8)

#define BPLCON0_HIRES   (1 << 15)
#define BPLCON0_HAM     (1 << 11)
#define BPLCON0_DBLPF   (1 << 10)

#define MAX_PLANES      6

static uint32_t amiga_rgb4(uint16_t c)
{
    return (((c >> 8) & 0xf) * 0x11 << 16) |
           (((c >> 4) & 0xf) * 0x11 << 8) |
           ((c & 0xf) * 0x11);
}

/*
 * Render the current frame from the bitplane pointers, which the
 * copper has just reloaded for this frame.  The geometry comes from
 * the data fetch registers; display window clipping is ignored.
 */
static bool amiga_custom_gfx_update(void *opaque)
{
    AmigaCustomState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint16_t bplcon0 = amiga_custom_reg(s, REG_BPLCON0);
    uint16_t ddfstrt = amiga_custom_reg(s, REG_DDFSTRT) & 0xfc;
    uint16_t ddfstop = amiga_custom_reg(s, REG_DDFSTOP) & 0xfc;
    uint16_t diwstrt = amiga_custom_reg(s, REG_DIWSTRT);
    uint16_t diwstop = amiga_custom_reg(s, REG_DIWSTOP);
    bool hires = bplcon0 & BPLCON0_HIRES;
    int planes = (bplcon0 >> 12) & 7;
    uint32_t palette[64];
    uint32_t bplpt[MAX_PLANES];
    uint8_t rowbuf[MAX_PLANES][1024 / 8];
    int words, width, height, vstart, vstop;
    int p, x, y;

    if (!s->con || !surface) {
        return true;
    }

    vstart = diwstrt >> 8;
    vstop = (diwstop >> 8) | ((diwstop & 0x8000) ? 0 : 0x100);
    height = vstop - vstart;
    if (hires) {
        words = ((ddfstop - ddfstrt) >> 2) + 2;
    } else {
        words = ((ddfstop - ddfstrt) >> 3) + 1;
    }
    width = words * 16;
    /* clip the fetch overrun to the display window width */
    {
        int hstart = diwstrt & 0xff;
        int hstop = (diwstop & 0xff) | 0x100;
        int diw_width = (hstop - hstart) << (hires ? 1 : 0);

        if (diw_width > 0 && diw_width < width) {
            width = diw_width;
        }
    }

    if (planes == 0 || planes > MAX_PLANES ||
        (s->dmacon & (DMACON_DMAEN | DMACON_BPLEN)) !=
            (DMACON_DMAEN | DMACON_BPLEN) ||
        width < 16 || width > 1024 || height < 16 || height > 313) {
        /* no active display: show the background colour */
        uint32_t bg = amiga_rgb4(amiga_custom_reg(s, REG_COLOR00));
        uint32_t *dst;

        for (y = 0; y < surface_height(surface); y++) {
            dst = (uint32_t *)(surface_data(surface) +
                               y * surface_stride(surface));
            for (x = 0; x < surface_width(surface); x++) {
                *dst++ = bg;
            }
        }
        qemu_console_update(s->con, 0, 0, surface_width(surface),
                            surface_height(surface));
        return true;
    }

    if (bplcon0 & (BPLCON0_HAM | BPLCON0_DBLPF)) {
        qemu_log_mask(LOG_UNIMP, "amiga-custom: HAM/dual-playfield mode\n");
    }

    if (surface_width(surface) != width ||
        surface_height(surface) != height) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
    }

    for (p = 0; p < 32; p++) {
        palette[p] = amiga_rgb4(amiga_custom_reg(s, REG_COLOR00 + p * 2));
    }
    /* extra-half-brite */
    for (p = 32; p < 64; p++) {
        palette[p] = (palette[p - 32] >> 1) & 0x7f7f7f;
    }

    for (p = 0; p < planes; p++) {
        bplpt[p] = amiga_custom_ptr(s, REG_BPL1PT + p * 4);
    }

    for (y = 0; y < height; y++) {
        uint32_t *dst = (uint32_t *)(surface_data(surface) +
                                     y * surface_stride(surface));

        for (p = 0; p < planes; p++) {
            int16_t mod = amiga_custom_reg(s, (p & 1) ? REG_BPL2MOD
                                                      : REG_BPL1MOD);

            dma_memory_read(&address_space_memory, bplpt[p] & CHIP_MASK,
                            rowbuf[p], words * 2, MEMTXATTRS_UNSPECIFIED);
            bplpt[p] += words * 2 + mod;
        }
        for (x = 0; x < width; x++) {
            int idx = 0;

            for (p = 0; p < planes; p++) {
                idx |= ((rowbuf[p][x >> 3] >> (7 - (x & 7))) & 1) << p;
            }
            *dst++ = palette[idx];
        }
    }
    qemu_console_update(s->con, 0, 0, width, height);
    return true;
}

static void amiga_custom_invalidate(void *opaque)
{
}

static const GraphicHwOps amiga_custom_gfx_ops = {
    .invalidate = amiga_custom_invalidate,
    .gfx_update = amiga_custom_gfx_update,
};

/* --- serial port --- */

static int amiga_custom_serial_can_receive(void *opaque)
{
    AmigaCustomState *s = opaque;

    return (s->intreq & INT_RBF) ? 0 : 1;
}

static void amiga_custom_serial_receive(void *opaque, const uint8_t *buf,
                                        int size)
{
    AmigaCustomState *s = opaque;

    s->serial_rx = buf[0];
    amiga_custom_post_int(s, INT_RBF);
}

/* --- register access --- */

static uint64_t amiga_custom_read(void *opaque, hwaddr addr, unsigned size)
{
    AmigaCustomState *s = opaque;
    uint32_t vpos, hpos;

    switch (addr & 0x1fe) {
    case REG_BLTDDAT:
        return 0;
    case REG_DMACONR:
        /* blits complete instantly, so the blitter is never busy */
        return s->dmacon | (s->blit_zero ? 0x2000 : 0);
    case REG_VPOSR:
        amiga_custom_beam_pos(s, &vpos, &hpos);
        return 0x8000 | (s->agnus_id << 8) | ((vpos >> 8) & 7);
    case REG_VHPOSR:
        amiga_custom_beam_pos(s, &vpos, &hpos);
        return ((vpos & 0xff) << 8) | (hpos & 0xff);
    case REG_JOY0DAT:
    case REG_JOY1DAT:
        return 0;
    case REG_ADKCONR:
        return s->adkcon;
    case REG_POTGOR:
        /* all pot lines high: no mouse buttons pressed */
        return 0xffff;
    case REG_SERDATR:
        return SERDATR_TBE | SERDATR_TSRE |
               ((s->intreq & INT_RBF) ? SERDATR_RBF | 0x100 | s->serial_rx
                                      : 0);
    case REG_DSKBYTR:
        return 0;
    case REG_INTENAR:
        return s->intena;
    case REG_INTREQR:
        return s->intreq;
    case REG_DENISEID:
        return 0xff00 | s->denise_id;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "amiga-custom: unimplemented read 0x%03" HWADDR_PRIx "\n",
                      addr & 0x1fe);
        return 0;
    }
}

static void amiga_custom_reg_write(AmigaCustomState *s, unsigned reg,
                                   uint16_t val)
{
    uint8_t ch;

    switch (reg) {
    case REG_SERDAT:
        ch = val & 0xff;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        amiga_custom_post_int(s, INT_TBE);
        break;
    case REG_SERPER:
        s->serper = val;
        break;
    case REG_POTGO:
        s->potgo = val;
        break;
    case REG_DMACON:
        s->dmacon = setclr(s->dmacon, val, 0x07ff);
        break;
    case REG_INTENA:
        s->intena = setclr(s->intena, val, 0x7fff);
        amiga_custom_update_irq(s);
        break;
    case REG_INTREQ:
        s->intreq = setclr(s->intreq, val, 0x7fff);
        /* level-sensitive sources re-latch immediately */
        if (s->ports_levels) {
            s->intreq |= INT_PORTS;
        }
        if (s->exter_levels) {
            s->intreq |= INT_EXTER;
        }
        amiga_custom_update_irq(s);
        break;
    case REG_ADKCON:
        s->adkcon = setclr(s->adkcon, val, 0x7fff);
        break;
    case REG_BLTSIZE: {
        int h = (val >> 6) & 0x3ff;
        int w = val & 0x3f;

        amiga_custom_do_blit(s, w ? w : 64, h ? h : 1024);
        break;
    }
    case REG_BLTSIZH: {
        int h = amiga_custom_reg(s, REG_BLTSIZV) & 0x7fff;
        int w = val & 0x7ff;

        amiga_custom_do_blit(s, w ? w : 2048, h ? h : 32768);
        break;
    }
    case REG_COPJMP1:
        amiga_custom_run_copper(s, amiga_custom_ptr(s, REG_COP1LC));
        break;
    case REG_COPJMP2:
        amiga_custom_run_copper(s, amiga_custom_ptr(s, REG_COP2LC));
        break;
    default:
        s->regs[reg >> 1] = val;
        break;
    }
}

static void amiga_custom_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    amiga_custom_reg_write(opaque, addr & 0x1fe, val);
}

static const MemoryRegionOps amiga_custom_ops = {
    .read = amiga_custom_read,
    .write = amiga_custom_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 2,
        .max_access_size = 2,
    },
};

static void amiga_custom_level_irq(AmigaCustomState *s, uint8_t *levels,
                                   int source, int level, uint16_t intbit)
{
    *levels = deposit32(*levels, source, 1, level);
    if (level) {
        amiga_custom_post_int(s, intbit);
    }
}

static void amiga_custom_cia_irq(void *opaque, int n, int level)
{
    AmigaCustomState *s = opaque;

    if (n == 0) {
        amiga_custom_level_irq(s, &s->ports_levels, 0, level, INT_PORTS);
    } else {
        amiga_custom_level_irq(s, &s->exter_levels, 0, level, INT_EXTER);
    }
}

static void amiga_custom_ports_irq(void *opaque, int n, int level)
{
    AmigaCustomState *s = opaque;

    amiga_custom_level_irq(s, &s->ports_levels, 1, level, INT_PORTS);
}

static void amiga_custom_exter_irq(void *opaque, int n, int level)
{
    AmigaCustomState *s = opaque;

    amiga_custom_level_irq(s, &s->exter_levels, 1, level, INT_EXTER);
}

static void amiga_custom_reset(DeviceState *dev)
{
    AmigaCustomState *s = AMIGA_CUSTOM(dev);

    s->intena = 0;
    s->intreq = 0;
    s->dmacon = 0;
    s->adkcon = 0;
    s->serper = 0;
    s->potgo = 0;
    s->serial_rx = 0;
    s->ports_levels = 0;
    s->exter_levels = 0;
    s->blit_zero = false;
    memset(s->regs, 0, sizeof(s->regs));
    s->frame_origin_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(&s->vblank_timer, s->frame_origin_ns + FRAME_NS);
    amiga_custom_update_irq(s);
}

static void amiga_custom_realize(DeviceState *dev, Error **errp)
{
    AmigaCustomState *s = AMIGA_CUSTOM(dev);

    timer_init_ns(&s->vblank_timer, QEMU_CLOCK_VIRTUAL,
                  amiga_custom_vblank, s);
    qemu_chr_fe_set_handlers(&s->chr, amiga_custom_serial_can_receive,
                             amiga_custom_serial_receive, NULL, NULL,
                             s, NULL, true);
    s->con = qemu_graphic_console_create(dev, 0, &amiga_custom_gfx_ops, s);
    qemu_console_resize(s->con, 640, 256);
}

static void amiga_custom_init(Object *obj)
{
    AmigaCustomState *s = AMIGA_CUSTOM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &amiga_custom_ops, s,
                          "amiga-custom", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->ipl);
    qdev_init_gpio_in_named(dev, amiga_custom_cia_irq, "cia-irq", 2);
    qdev_init_gpio_in_named(dev, amiga_custom_ports_irq, "ports-irq", 1);
    qdev_init_gpio_in_named(dev, amiga_custom_exter_irq, "exter-irq", 1);
}

static const VMStateDescription vmstate_amiga_custom = {
    .name = "amiga-custom",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(intena, AmigaCustomState),
        VMSTATE_UINT16(intreq, AmigaCustomState),
        VMSTATE_UINT16(dmacon, AmigaCustomState),
        VMSTATE_UINT16(adkcon, AmigaCustomState),
        VMSTATE_UINT16(serper, AmigaCustomState),
        VMSTATE_UINT16(potgo, AmigaCustomState),
        VMSTATE_UINT8(serial_rx, AmigaCustomState),
        VMSTATE_UINT8(ports_levels, AmigaCustomState),
        VMSTATE_UINT8(exter_levels, AmigaCustomState),
        VMSTATE_UINT16_ARRAY(regs, AmigaCustomState, 0x100),
        VMSTATE_BOOL(blit_zero, AmigaCustomState),
        VMSTATE_INT64(frame_origin_ns, AmigaCustomState),
        VMSTATE_TIMER(vblank_timer, AmigaCustomState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property amiga_custom_properties[] = {
    DEFINE_PROP_CHR("chardev", AmigaCustomState, chr),
    /* PAL ECS 2MB Agnus / ECS Denise by default */
    DEFINE_PROP_UINT32("agnus-id", AmigaCustomState, agnus_id, 0x22),
    DEFINE_PROP_UINT32("denise-id", AmigaCustomState, denise_id, 0xfc),
};

static void amiga_custom_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = amiga_custom_realize;
    device_class_set_legacy_reset(dc, amiga_custom_reset);
    dc->vmsd = &vmstate_amiga_custom;
    device_class_set_props(dc, amiga_custom_properties);
}

static const TypeInfo amiga_custom_info = {
    .name          = TYPE_AMIGA_CUSTOM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AmigaCustomState),
    .instance_init = amiga_custom_init,
    .class_init    = amiga_custom_class_init,
};

static void amiga_custom_register_types(void)
{
    type_register_static(&amiga_custom_info);
}

type_init(amiga_custom_register_types)
