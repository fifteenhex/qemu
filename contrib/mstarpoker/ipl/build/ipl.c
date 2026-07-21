/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Cleaned, compilable port of the SSD202D IPL (see ../ipl_decompiled.c for
 * the Ghidra reference). Built as an IPL image and validated by comparing
 * its RIU MMIO write trace against the stock IPL running in the model.
 * Ported incrementally; all logic is kept as it is reached.
 */
#include "rt.h"

/* ---- UART0 boot-message output (a0001678 / a00016ee / a0001690) ------- */
static void uart_putc(char c)                    /* FUN_a0001678 */
{
    while ((R8(0x1f221028) & 0x20) == 0) {       /* wait for THR empty */
    }
    R8(0x1f221000) = c;
}

static void uart_puts(const char *s)             /* FUN_a00016ee */
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

static void uart_put_hex8(unsigned int byte)     /* FUN_a0001690 */
{
    unsigned int hi = (byte >> 4) & 0xf;
    unsigned int lo = byte & 0xf;
    uart_putc(hi < 10 ? hi + '0' : hi + ('a' - 10));
    uart_putc(lo < 10 ? lo + '0' : lo + ('a' - 10));
}

/* ---- free-running timer + delay (a0001a04 / a0001cd4) ----------------- */
static unsigned int timer_read(void)             /* FUN_a0001a04 */
{
    return ((unsigned int)R16(0x1f006052) << 16) | (R16(0x1f006050) & 0xffff);
}

static void delay(unsigned int ticks)            /* FUN_a0001cd4 */
{
    unsigned int start = timer_read();
    while ((timer_read() - start) < ticks) {
    }
}

/* ---- boot-timing record (a00015fc) ------------------------------------
 * Logs (timestamp, step id, 7-char label) for each boot step into a small
 * table. The stock IPL keeps this in IMI SRAM; a static array is
 * equivalent and emits no RIU writes, so it does not affect the trace.
 */
struct boot_step {
    unsigned int ts;
    unsigned int id;
    char label[8];
};
static struct boot_step boot_log[25];
static unsigned int boot_log_n;

static void boot_record(unsigned int id, const char *label)   /* FUN_a00015fc */
{
    if (boot_log_n < 25) {
        unsigned int lo, hi;
        int i;
        __asm__ volatile("isb");
        __asm__ volatile("mrrc p15,0,%0,%1,c14" : "=r"(lo), "=r"(hi));  /* CNTPCT */
        unsigned long long t = ((unsigned long long)hi << 32) | lo;
        boot_log[boot_log_n].ts = (unsigned int)((t * 0xaaaaaaabULL) >> 0x22);
        boot_log[boot_log_n].id = id;
        for (i = 0; i < 7; i++) {
            boot_log[boot_log_n].label[i] = label[i];
        }
        boot_log[boot_log_n].label[7] = 0;
        boot_log_n++;
    }
}

/* ---- clock/PLL helpers in the 0x1f004000 bank ------------------------- */
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
 * FUN_a0001d50, the IPL's main routine. Ported through the print preamble
 * (clock bring-up, UART bring-up and the boot banner); the DDR/MIU init
 * that follows the SSD201/SSD202D split is not ported yet.
 */
void ipl_main(void)
{
    unsigned char bond;
    unsigned int r, i, u34;
    unsigned char b2;
    const char *reset_msg;

    bond = R8(0x1f203d20);              /* chip bond id (0x1e = SSD202D) */
    boot_log_n = 0;
    boot_record(timer_read() / 12u, "IPL+");

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

    /* UART0 bring-up (a0001de8..a0001e98) */
    for (i = 0; i != 0x100; i++) {         /* wait for LSR bit6, else 256 tries */
        u34 = i & 0xff;
        if ((R16(0x1f221028) & 0x40) != 0) {
            goto lsr_ready;
        }
    }
    u34 = 0xff;
lsr_ready:
    R8(0x1f2070c4) = 0;                                     /* a0001e10 */
    R16(0x1f001c24) &= 0xf7ff;                              /* a0001e24 */
    R16(0x1f203d4c) = 0x3210;                               /* a0001e2a */
    R8(0x1f221070) &= 0xfe;                                 /* a0001e34 */
    R8(0x1f221070) |= 1;                                    /* a0001e40 */
    R8(0x1f221008) = 0;                                     /* a0001e44 */
    R8(0x1f221020) |= 0x10;                                 /* a0001e52 */
    while ((b2 = R8(0x1f221038)) & 1) {                     /* wait for UART idle */
        if (u34 == 0xff) {
            goto uart_err;
        }
        u34 = (u34 + 1) & 0xff;
    }
    if (u34 == 0xff) {
uart_err:
        R32(0x1f200800) = 0xbf1;
    } else {
        R8(0x1f221018) |= 0x80;                             /* a0001f2e */
        R8(0x1f221000) = 0x5e;                              /* a0001f32 */
        R8(0x1f221008) = b2 & 1;                            /* a0001f36 */
        R8(0x1f221018) &= 0x7f;                             /* a0001f40 */
    }
    R8(0x1f221018) = 3;                                     /* a0001e76 */
    R8(0x1f221020) &= 0xef;                                 /* a0001e84 */
    R8(0x1f221010) = 7;                                     /* a0001e8a */
    R16(0x1f203d4c) = 0x3012;                               /* a0001e90 */
    R16(0x1f001c24) |= 0x800;                               /* a0001e98 */

    /* boot banner (a0001f4c..) */
    uart_puts("\r\nIPL ");
    uart_puts("g5da0ceb");                 /* version, from the image config struct */
    uart_puts("\r\n");
    uart_puts("D-");
    uart_put_hex8(bond);
    uart_puts("\n\r");

    /* reset-reason detect: WDT / SW / HW (0x1f002400, 0x1f006008) */
    reset_msg = "HW Reset\r\n";
    if ((R16(0x1f002400) & 2) != 0) {
        reset_msg = "SW Reset\r\n";
        if ((R16(0x1f006008) & 1) != 0) {
            uart_puts("WDT Reset\r\n");
            R16(0x1f006008) |= 1;
            goto reset_done;
        }
    }
    uart_puts(reset_msg);
reset_done:
    boot_record(0x29e, "MIU+");

    /* TODO: DDR/MIU init (the bond==0x1d SSD201 / else SSD202D split) */
}

/* entry @ a0000010 */
void ipl_entry(void)
{
    IPL_PROGRESS = 0xa001;      /* a0000018 */
    cp15_smp_fp_init();         /* a0000028..a000004c (no RIU writes) */
    IPL_PROGRESS = 0xa002;      /* a0000058 */
    ipl_main();                 /* blx a0001d50 */
}
