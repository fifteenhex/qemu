/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ddr_init.c - SSD202D (infinity2m) DDR3 bring-up, reimplemented in C from
 * the vendor IPL's DDR init logic.
 *
 * This is a clean reverse-engineered reimplementation, NOT the IPL's compiled
 * code embedded verbatim. It is uploaded to on-chip SRAM and called via the
 * mstarpoker stub's 'G' command, so it runs bare-metal on the target: the
 * read-modify-write ZQ / drive-strength calibration executes against the real
 * DDR PHY, and the settle delays use the same PM free-running timer the IPL
 * uses - things a host-side register replay over serial cannot reproduce (see
 * ../ddr_seq_ssd202d.py and ../../../VALIDATION.md).
 *
 * Structure, mirroring the IPL (all offsets relative to the MIU at
 * 0x1f202000; see the RE notes for the source addresses in the IPL):
 *
 *   1. a table of the deterministic config writes (the HW-validated
 *      ddr_seq_ssd202d capture), with markers for where the IPL inserts a
 *      timer delay and where it runs ZQ calibration;
 *   2. zq_calibrate(): the read-compute-write impedance / drive-strength
 *      calibration the IPL runs (IPL 0x28dc-0x29f8), which a QEMU capture
 *      skips because the analog ZQ-done bits are never set in the model; and
 *   3. delay_ticks(): the IPL's timer busy-wait (IPL routine 0x1cd4).
 *
 * Entry point ddr_init() runs the whole sequence and returns (bx lr) to the
 * stub monitor, after which the host can test DRAM.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define RIU       0x1f000000u   /* register bus base; table offsets are RIU-relative */
#define TIMER     0x1f006050u   /* PM free-running counter: +0 low16, +4 high16 */
#define DRAM_BASE 0x20000000u

#define RH(a)   (*(volatile u16 *)(unsigned long)(a))
#define RB(a)   (*(volatile u8  *)(unsigned long)(a))

/* ---- timer delay: busy-wait `ticks` PM-timer ticks (IPL routine 0x1cd4) ---- */

static u32 timer_now(void)
{
    u32 lo = RH(TIMER);
    u32 hi = RH(TIMER + 4);
    return lo | (hi << 16);
}

/*
 * Wait `ticks` timer ticks. A large fallback spin cap makes this safe even if
 * the PM timer is not running (it must not hang the target): if the counter
 * never advances we still return after the cap.
 */
static void delay_ticks(u32 ticks)
{
    u32 start = timer_now();
    volatile u32 guard = 0;

    while ((timer_now() - start) < ticks) {
        if (++guard > 0x08000000u)
            break;
    }
}

#define DDR_DELAY 0x2ee0u       /* 12000 ticks - the IPL's DDR settle delay */

/* ---- watchdog: auto-reset the SoC if we bus-hang ----
 *
 * The DRAM self-test (and any bad register access) can bus-hang the CPU with
 * no recoverable abort - normally that wedges the target until a manual power
 * cycle. Arm the watchdog around the whole sequence so a hang instead resets
 * the SoC: the mask ROM then reloads this stub from flash and the host just
 * reconnects. On a clean run we disarm it before returning.
 *
 * WDT at 0x1f006000, 12 MHz crystal: MAX_L/H = timeout in ticks, CLR feeds it,
 * MAX = 0 disarms (see hw/watchdog/mstar_wdt.c / the mainline msc313e driver).
 */
#define WDT       0x1f006000u
#define WDT_CLR   0x00u
#define WDT_MAX_L 0x10u
#define WDT_MAX_H 0x14u
#define WDT_TIMEOUT 0x02000000u  /* ~2.8 s at 12 MHz (a clean run takes <1 s) */

static void wdt_arm(u32 ticks)
{
    RH(WDT + WDT_MAX_L) = (u16)(ticks & 0xffff);
    RH(WDT + WDT_MAX_H) = (u16)(ticks >> 16);
    RH(WDT + WDT_CLR) = 1;        /* feed / start the counter */
}

static void wdt_disarm(void)
{
    RH(WDT + WDT_MAX_L) = 0;
    RH(WDT + WDT_MAX_H) = 0;
}

/* ---- config-write table ----
 *
 * The full ordered write sequence the vendor IPL makes during DDR bring-up,
 * captured authoritatively (every RIU write, all blocks) by running the vendor
 * IPL under QEMU with the register bus logged - see the RE notes. Offsets are
 * relative to the RIU base (0x1f000000), so unlike the earlier MIU-only capture
 * this now includes the essential non-MIU setup the DDR needs:
 *
 *   - the MIU clock PLL (miupll, 0x206205-0x206211) - without it the controller
 *     runs against a dead clock;
 *   - the MIU select / reset (0x20025c) and cpupll poke (0x206005); and
 *   - the ZQ analog latch trigger (0x00400c toggling 0->0x100->0).
 *
 * The console UART writes (0x221xxx) the IPL makes here are deliberately
 * excluded - replicating them would reconfigure the UART and kill our link.
 */
#define F_B   1                 /* byte write (else 16-bit) */
#define F_DLY 2                 /* delay after this write */
#define F_ZQ  4                 /* run ZQ read/adjust after this write */

struct rw { u32 off; u16 val; u8 flags; };

static const struct rw seq[] = {
#include "ddr_table.inc"
};

/* ---- ZQ / drive-strength calibration (IPL 0x28dc-0x29f8) ---- */

/* analog ZQ result registers (chiptop analog domain) */
#define ZQ_400C   0x1f00400cu   /* +bit8 latch enable (cleared before reading) */
#define ZQ_402C   0x1f00402cu   /* bit11 zq valid, [10:5] zq; bit15 drvN valid, [14:12] drvN */
#define ZQ_4024   0x1f004024u   /* bit15 drvp valid, [14:12] drvp */
#define PHY_2160  0x1f202160u   /* zq code lands in bits [14:9] */
#define SBITS_21BC 0x1f2021bcu  /* holds the high (bit4) of each drive-strength field */

/* IPL table @0xa0004920: drv code 1..7 -> signed delta */
static const signed char drv_delta[7] = { -1, -2, -3, 0, 1, 2, 3 };

/*
 * Saturating 5-bit drive-strength field adjust (IPL helper 0x1128). The field
 * is split: its top bit lives at SBITS_21BC[bitpos], its low 4 bits at
 * reg[shift+3:shift]. Add `delta`, clamp at 0, write both halves back.
 */
static void field_adjust(u32 bitpos, u32 reg, u32 shift, int delta)
{
    u16 sbits = RH(SBITS_21BC);
    u16 cur   = RH(reg);
    int v = ((cur >> shift) & 0xf) | (((sbits >> bitpos) & 1) << 4);

    v += delta;
    if (v < 0)
        v = 0;

    sbits = (u16)((sbits & ~(1u << bitpos)) | (((u32)(v >> 4) & 1u) << bitpos));
    RH(SBITS_21BC) = sbits;
    cur = (u16)((cur & ~(0xfu << shift)) | (((u32)v & 0xfu) << shift));
    RH(reg) = cur;
}

static void zq_calibrate(void)
{
    u16 s402c, s4024;

    /* clear the latch-enable bit, as the IPL does before sampling */
    RH(ZQ_400C) = (u16)(RH(ZQ_400C) & ~0x100u);

    s402c = RH(ZQ_402C);

    /* impedance (ZQ) code -> PHY reg 0x1f202160 [14:9] */
    if (s402c & 0x800u) {
        u32 zq = (RH(ZQ_402C) >> 5) & 0x3fu;
        u16 r = RH(PHY_2160);
        RH(PHY_2160) = (u16)((r & ~0x7e00u) | (zq << 9));
    }

    /* drive-strength P from 0x1f004024[14:12] */
    s4024 = RH(ZQ_4024);
    if (s4024 & 0x8000u) {
        u32 drvp = (RH(ZQ_4024) >> 12) & 7u;
        int d = drvp ? drv_delta[drvp - 1] : 0;
        field_adjust(0xe, 0x1f2020b0u, 8,  d);
        field_adjust(0x8, 0x1f2020bcu, 0,  d);
        field_adjust(0xa, 0x1f2020bcu, 4,  d);
        field_adjust(0xc, 0x1f2020bcu, 8,  d);
        field_adjust(0xd, 0x1f2020bcu, 12, d);
    }

    /* drive-strength N from 0x1f00402c[14:12] */
    if (s402c & 0x8000u) {
        u32 drvn = (RH(ZQ_402C) >> 12) & 7u;
        int d = drvn ? drv_delta[drvn - 1] : 0;
        field_adjust(0x6, 0x1f2020b0u, 0,  d);
        field_adjust(0x0, 0x1f2020b8u, 0,  d);
        field_adjust(0x2, 0x1f2020b8u, 4,  d);
        field_adjust(0x4, 0x1f2020b8u, 8,  d);
        field_adjust(0x5, 0x1f2020b8u, 12, d);
    }
}

/* ---- on-target DRAM self-test ----
 *
 * The MIU init-done bit is faked by the QEMU model and, on real silicon, the
 * IPL never polls it - so it is not a reliable "DRAM trained" signal. Instead
 * the blob writes/reads a few words of DRAM itself and reports the verdict in
 * an SRAM word the host can read safely (DRAM that is not trained bus-hangs on
 * the read; the RESULT_PROBING marker tells the host that is where it stopped).
 */
#define RESULT_ADDR    0xa0009000u
#define RESULT_PROBING 0xd1900001u  /* about to touch DRAM */
#define RESULT_PASS    0xd1900a00u  /* DRAM read back correctly */
#define RESULT_FAIL    0xd1900badu  /* DRAM responded but data wrong */

static void dram_selftest(void)
{
    volatile u32 *mark = (volatile u32 *)(unsigned long)RESULT_ADDR;
    volatile u32 *dram = (volatile u32 *)(unsigned long)DRAM_BASE;
    static const u32 pat[4] = { 0xa5a5a5a5u, 0x5a5a5a5au,
                                0x00000000u, 0xffffffffu };
    unsigned i;
    u32 ok = RESULT_PASS;

    *mark = RESULT_PROBING;
    for (i = 0; i < 4; i++)
        dram[i] = pat[i];
    for (i = 0; i < 4; i++)
        if (dram[i] != pat[i])
            ok = RESULT_FAIL;
    *mark = ok;
}

/* ---- entry: bring DDR up, self-test it, then return to the stub monitor ---- */

__attribute__((section(".text.start"), used))
void ddr_init(void)
{
    unsigned i;

    wdt_arm(WDT_TIMEOUT);       /* a hang from here on auto-resets the SoC */

    for (i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        u32 a = RIU + seq[i].off;

        if (seq[i].flags & F_B)
            RB(a) = (u8)seq[i].val;
        else
            RH(a) = seq[i].val;

        if (seq[i].flags & F_DLY)
            delay_ticks(DDR_DELAY);
        if (seq[i].flags & F_ZQ)
            zq_calibrate();
    }

    dram_selftest();            /* bus-hangs here if DRAM is not trained */

    wdt_disarm();               /* clean run - cancel the auto-reset */
}
