/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Cleaned, compilable port of the SSD202D IPL (see ../ipl_decompiled.c for
 * the Ghidra reference). Built as an IPL image and validated by comparing
 * its RIU MMIO write trace against the stock IPL running in the model.
 * Ported incrementally; all logic is kept as it is reached.
 */
#include "rt.h"

/*
 * Free-running timer used for the boot delays (FUN_a0001a04): a 32-bit
 * up-counter split across two 16-bit halves at 0x1f006050/52.
 */
static unsigned int timer_read(void)            /* FUN_a0001a04 */
{
    return ((unsigned int)R16(0x1f006052) << 16) | (R16(0x1f006050) & 0xffff);
}

/*
 * Busy-delay for `ticks` of the free-running timer (FUN_a0001cd4). The
 * stock code open-codes the elapsed-time maths with the counter's
 * wrap/reload registers (0x1f006048/4c); a 32-bit unsigned subtract does
 * the same for a 32-bit counter. No RIU writes, so the write trace is
 * unaffected by this refactor.
 */
static void delay(unsigned int ticks)           /* FUN_a0001cd4 */
{
    unsigned int start = timer_read();
    while ((timer_read() - start) < ticks) {
    }
}

/*
 * Clock/PLL bring-up helpers in the 0x1f004000 bank.
 */
static int clk_check(void)                       /* FUN_a000119c */
{
    unsigned int v;

    R16(0x1f00400c) &= 0xfeff;
    v = (R16(0x1f004014) & 0x1f) >> 3;
    if (v != 0) {
        return v == 3;
    }
    return 1;
}

/* Select clock source `sel` and read back its 32-bit value. */
static unsigned int pll_read(unsigned int sel)   /* FUN_a00011c8 */
{
    volatile unsigned short *pv;

    if (sel < 8) {
        R16(0x1f00400c) &= 0xfeff;
        pv = (volatile unsigned short *)0x1f004010;
        if (sel == 0) {
            goto done;
        }
    } else {
        R16(0x1f00400c) |= 0x100;
        pv = (volatile unsigned short *)0x1f004010;
        if (sel == 8) {
            goto done;
        }
    }
    sel &= ~8u;
    pv = (volatile unsigned short *)0x1f004018;
    if (sel != 1
        && (pv = (volatile unsigned short *)0x1f004020, sel != 2)
        && (pv = (volatile unsigned short *)0x1f004028, sel != 3)
        && (pv = (volatile unsigned short *)0x1f004058, sel != 4
            && (pv = (volatile unsigned short *)0x1f004060, sel != 5))
        && (pv = (volatile unsigned short *)0x1f004068, sel != 6
            && (pv = (volatile unsigned short *)0x1f004070, sel != 7))) {
        return 0;
    }
done:
    return ((unsigned int)pv[2] << 16) | pv[0];
}

/*
 * FUN_a0001d50, the IPL's main routine. Ported through the early clock
 * bring-up; the earlier boot-timing bookkeeping (a scratch table in IMI
 * SRAM, no RIU writes) is not ported yet. Continues from here in later
 * stages.
 */
void ipl_main(void)
{
    unsigned int r;

    R16(0x1f20025c) |= 1;                                   /* a0001d84 */
    R16(0x1f203c0c) = (R16(0x1f203c0c) & 0xff8f) | 0x10;    /* a0001d98 */
    R16(0x1f203c20) = (R16(0x1f203c20) & 0xfcff) | 0x100;   /* a0001da6 */

    /* keep the short-circuit: pll_read runs only when clk_check != 0 */
    if (clk_check() == 0) {                                 /* a00011a8 */
        R16(0x1f203cf0) &= 0x3f;
    } else {
        r = pll_read(0);                                    /* a00011f0 */
        if (((r & 0x7ffff) >> 16) == 2) {
            R16(0x1f203cf0) &= 0x3f;
        }
    }

    R8(0x1f001cf4) = 0;                                     /* a0001dc4 */
    delay(0x4b0);
    R8(0x1f206005) = 0;                                     /* a0001dd0: MPLL enable */
    delay(12000);
    R8(0x1f207004) = 0x30;                                  /* a0001dde: timer clock src */
    R32(0x1f006058) = 0x23;                                 /* a0001de2 */
    R8(0x1f2070c4) = 0;                                     /* a0001de6 */
    R8(0x1f001cc0) |= 0x10;                                 /* a0001dee */
}

/* entry @ a0000010 */
void ipl_entry(void)
{
    IPL_PROGRESS = 0xa001;      /* a0000018 */
    cp15_smp_fp_init();         /* a0000028..a000004c (no RIU writes) */
    IPL_PROGRESS = 0xa002;      /* a0000058 */
    ipl_main();                 /* blx a0001d50 */
}
