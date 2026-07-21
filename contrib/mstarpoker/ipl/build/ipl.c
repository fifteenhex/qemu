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
    while ((rd8(0x1f221028) & 0x20) == 0) {       /* wait for THR empty */
    }
    wr8(0x1f221000, c);
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

static void uart_put_hex16(unsigned int val)     /* FUN_a00016da */
{
    uart_put_hex8((val >> 8) & 0xff);
    uart_put_hex8(val & 0xff);
}

/* ---- free-running timer + delay (a0001a04 / a0001cd4) ----------------- */
static unsigned int timer_read(void)             /* FUN_a0001a04 */
{
    return ((unsigned int)rd16(0x1f006052) << 16) | (rd16(0x1f006050) & 0xffff);
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

    wr16(0x1f00400c, rd16(0x1f00400c) & 0xfeff);
    v = (rd16(0x1f004014) & 0x1f) >> 3;
    if (v != 0) {
        return v == 3;
    }
    return 1;
}

static unsigned int pll_read(unsigned int sel)   /* FUN_a00011c8 */
{
    unsigned int pv;

    if (sel < 8) {
        wr16(0x1f00400c, rd16(0x1f00400c) & 0xfeff);
        pv = 0x1f004010;
        if (sel == 0) {
            goto done;
        }
    } else {
        wr16(0x1f00400c, rd16(0x1f00400c) | 0x100);
        pv = 0x1f004010;
        if (sel == 8) {
            goto done;
        }
    }
    sel &= ~8u;
    pv = 0x1f004018;
    if (sel != 1
        && (pv = 0x1f004020, sel != 2)
        && (pv = 0x1f004028, sel != 3)
        && (pv = 0x1f004058, sel != 4
            && (pv = 0x1f004060, sel != 5))
        && (pv = 0x1f004068, sel != 6
            && (pv = 0x1f004070, sel != 7))) {
        return 0;
    }
done:
    return (rd16(pv + 4) << 16) | rd16(pv);
}

static void FUN_a0001128(unsigned int p1, unsigned int p2,
                         unsigned int p3, int p4)   /* FUN_a0001128 */
{
    unsigned int shared = 0x1f2021bc;
    unsigned short v1;
    int i2;
    unsigned int u3 = 0xfu << (p3 & 0xff);
    unsigned int u4 = 1u << (p1 & 0xff);

    i2 = p4 + (((int)(rd16(p2) & u3) >> (p3 & 0xff)
                | ((int)(u4 & rd16(shared)) >> (p1 & 0xff)) << 4) & 0xffff);
    v1 = (p4 < 0 && i2 < 0) ? 0 : (unsigned short)i2;
    wr16(shared, (unsigned short)(((v1 & 0x1f) >> 4) << (p1 & 0xff))
                 | (rd16(shared) & ~(unsigned short)u4));
    wr16(p2, (rd16(p2) & ~(unsigned short)u3) | (unsigned short)((v1 & 0xf) << (p3 & 0xff)));
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
    unsigned int puVar8, puVar12, puVar26, puVar35, puVar39, puVar40, puVar41;
    unsigned int puVar13, puVar36;
    unsigned int puVar11, puVar14, puVar46;
    unsigned int uVar24;
    int iVar25;

    bond = rd8(0x1f203d20);              /* chip bond id (0x1e = SSD202D) */
    boot_log_n = 0;
    boot_record(timer_read() / 12u, "IPL+");

    wr16(0x1f20025c, rd16(0x1f20025c) | 1);                                   /* a0001d84 */
    wr16(0x1f203c0c, (rd16(0x1f203c0c) & 0xff8f) | 0x10);    /* a0001d98 */
    wr16(0x1f203c20, (rd16(0x1f203c20) & 0xfcff) | 0x100);   /* a0001da6 */

    /* keep the short-circuit: pll_read runs only when clk_check != 0 */
    if (clk_check() == 0) {                                 /* a00011a8 */
        wr16(0x1f203cf0, rd16(0x1f203cf0) & 0x3f);
    } else {
        r = pll_read(0);                                    /* a00011f0 */
        if (((r & 0x7ffff) >> 16) == 2) {
            wr16(0x1f203cf0, rd16(0x1f203cf0) & 0x3f);
        }
    }

    wr8(0x1f001cf4, 0);                                     /* a0001dc4 */
    delay(0x4b0);
    wr8(0x1f206005, 0);                                     /* a0001dd0: MPLL enable */
    delay(12000);
    wr8(0x1f207004, 0x30);                                  /* a0001dde: timer clock src */
    wr32(0x1f006058, 0x23);                                 /* a0001de2 */
    wr8(0x1f2070c4, 0);                                     /* a0001de6 */
    wr8(0x1f001cc0, rd8(0x1f001cc0) | 0x10);                                 /* a0001dee */

    /* UART0 bring-up (a0001de8..a0001e98) */
    for (i = 0; i != 0x100; i++) {         /* wait for LSR bit6, else 256 tries */
        u34 = i & 0xff;
        if ((rd16(0x1f221028) & 0x40) != 0) {
            goto lsr_ready;
        }
    }
    u34 = 0xff;
lsr_ready:
    wr8(0x1f2070c4, 0);                                     /* a0001e10 */
    wr16(0x1f001c24, rd16(0x1f001c24) & 0xf7ff);                              /* a0001e24 */
    wr16(0x1f203d4c, 0x3210);                               /* a0001e2a */
    wr8(0x1f221070, rd8(0x1f221070) & 0xfe);                                 /* a0001e34 */
    wr8(0x1f221070, rd8(0x1f221070) | 1);                                    /* a0001e40 */
    wr8(0x1f221008, 0);                                     /* a0001e44 */
    wr8(0x1f221020, rd8(0x1f221020) | 0x10);                                 /* a0001e52 */
    while ((b2 = rd8(0x1f221038)) & 1) {                     /* wait for UART idle */
        if (u34 == 0xff) {
            goto uart_err;
        }
        u34 = (u34 + 1) & 0xff;
    }
    if (u34 == 0xff) {
uart_err:
        wr32(0x1f200800, 0xbf1);
    } else {
        wr8(0x1f221018, rd8(0x1f221018) | 0x80);                             /* a0001f2e */
        wr8(0x1f221000, 0x5e);                              /* a0001f32 */
        wr8(0x1f221008, b2 & 1);                            /* a0001f36 */
        wr8(0x1f221018, rd8(0x1f221018) & 0x7f);                             /* a0001f40 */
    }
    wr8(0x1f221018, 3);                                     /* a0001e76 */
    wr8(0x1f221020, rd8(0x1f221020) & 0xef);                                 /* a0001e84 */
    wr8(0x1f221010, 7);                                     /* a0001e8a */
    wr16(0x1f203d4c, 0x3012);                               /* a0001e90 */
    wr16(0x1f001c24, rd16(0x1f001c24) | 0x800);                               /* a0001e98 */

    /* boot banner (a0001f4c..) */
    uart_puts("\r\nIPL ");
    uart_puts("g5da0ceb");                 /* version, from the image config struct */
    uart_puts("\r\n");
    uart_puts("D-");
    uart_put_hex8(bond);
    uart_puts("\n\r");

    /* reset-reason detect: WDT / SW / HW (0x1f002400, 0x1f006008) */
    reset_msg = "HW Reset\r\n";
    if ((rd16(0x1f002400) & 2) != 0) {
        reset_msg = "SW Reset\r\n";
        if ((rd16(0x1f006008) & 1) != 0) {
            uart_puts("WDT Reset\r\n");
            wr16(0x1f006008, rd16(0x1f006008) | 1);
            goto reset_done;
        }
    }
    uart_puts(reset_msg);
reset_done:
    boot_record(0x29e, "MIU+");

    /* DDR/MIU init, split by chip bond id (0x1e = SSD202D, 128MB) */
    if (bond == 0x1d) {
        /* SSD201 / 64MB DDR-MIU init */
        uart_puts("miupll_166MHz\r\n");
        puVar12 = 0x1f20248c;
        puVar8 = 0x1f20243c;
        puVar11 = 0x1f206205;
        wr8(puVar11, 0);
        wr8(puVar11 + 0x3, 0);
        wr8(puVar11 + 0x4, 0);
        wr8(puVar11 + 0x7, 0x1c);
        wr8(puVar11 + 0x8, 2);
        wr8(puVar11 + 0xb, 0x10);
        puVar35 = 0x1f2024cc;
        wr8(puVar11 + 0xc, 0);
        wr16(puVar8, 0xc00);
        wr16(puVar8, 0xc00);
        wr16(puVar8, 0xc00);
        wr16(puVar8, 0xc01);
        wr16(puVar12, 0xfffe);
        wr16(puVar35, 0xffff);
        wr16(puVar35 + 0x40, 0xffff);
        wr16(puVar35 + 0x80, 0xffff);
        wr16(puVar35 - 0x2c0, 0xffff);
        wr16(puVar35 - 0x280, 0xffff);
        wr16(puVar35 - 0x100, 0xfffe);
        puVar40 = 0x1f202114;
        puVar35 = 0x1f202048;
        wr16(0x1f2020f0, 1);
        delay(12000);
        wr16(puVar35, 0x1000);
        delay(12000);
        wr16(puVar35, 0);
        delay(12000);
        puVar35 = 0x1f20206c;
        wr16(puVar35, 0x400);
        wr16(puVar35 - 0x4, 0x2004);
        wr16(puVar40, 1);
        wr16(puVar35 - 0xc, 0x8000);
        wr16(puVar35 - 0x8, 0x29);
        delay(12000);
        puVar39 = 0x1f20207c;
        puVar35 = 0x1f202044;
        wr16(puVar35, 4);
        wr16(puVar35 + 0x14, 0x114);
        wr16(puVar35 + 0xcc, 0x11);
        wr16(puVar35 + 0x3c0, 0x292);
        wr16(puVar35 + 0x3c4, 0x52);
        wr16(puVar35 + 0x3c8, 0x1b50);
        wr16(puVar35 + 0x3cc, 0x1e99);
        wr16(puVar35 + 0x3d0, 0x2777);
        wr16(puVar35 + 0x3d4, 0x95a8);
        wr16(puVar35 + 0x3d8, 0x404c);
        wr16(puVar35 + 0x3dc, 0x203);
        wr16(puVar35 + 0x3e0, 0x4004);
        wr16(puVar35 + 0x3e4, 0x8000);
        wr16(puVar35 + 0x3e8, 0xc000);
        wr16(puVar35 + 0x40c, 0x70);
        wr16(puVar35 + 0x560, 0x6000);
        wr16(puVar35 + 0x600, 3);
        wr16(puVar35 + 0x638, 0);
        wr16(puVar35 + 0x63c, 0x909);
        wr16(puVar35 + 0x640, 0x71e);
        wr16(puVar35 + 0x644, 0x2707);
        wr16(puVar35 + 0x648, 0x908);
        wr16(puVar35 + 0x64c, 0x905);
        wr16(puVar35 + 0x650, 0x304);
        wr16(puVar35 + 0x654, 0x528);
        wr16(puVar35 + 0x658, 0x46);
        wr16(puVar35 + 0x65c, 0xe000);
        wr16(puVar35 + 0x660, 0);
        wr16(puVar35 + 0x664, 0x900);
        wr16(puVar35 + 0x6bc, 0);
        wr16(puVar35 + 0x6c8, 0);
        wr16(puVar35 + 0x7b8, 0);
        wr16(puVar35 + 0x27c, 0);
        wr16(puVar35 + 0x280, 0);
        wr16(puVar35 + 0x284, 0);
        wr16(puVar35 + 0x288, 0x30);
        wr16(puVar35 + 0x28c, 0x5000);
        wr16(puVar35 - 0x40, 0xaaaa);
        wr16(puVar35 - 0x3c, 0);
        wr16(puVar35 - 0x30, 0x1100);
        wr16(puVar35 - 0x28, 0x8f);
        wr16(puVar35 + 0x18, 0x1122);
        wr16(puVar35 + 0x2c, 0x77);
        wr16(puVar35 + 0x30, 0x5050);
        wr16(puVar35 + 0x34, 0x9111);
        wr16(puVar39, 0x1111);
        wr16(puVar39 + 0x14, 0x77);
        wr16(puVar39 + 0x18, 0);
        wr16(puVar39 + 0x1c, 0x11);
        wr16(puVar39 + 0x20, 0x11);
        puVar35 = 0x1f2020a0;
        wr16(puVar35, 0x1111);
        wr16(puVar35 + 0x4, 0);
        wr16(puVar39 + 0x5c, 0x808);
        wr16(puVar39 + 0x60, 0x808);
        wr16(puVar39 + 0x6c, 0x404);
        wr16(puVar39 + 0x70, 0x404);
        puVar35 = 0x1f202128;
        puVar26 = 0x1f202128 + 0x1c;
        wr16(puVar35, 0x1317);
        wr16(puVar35 + 0x18, 0x6466);
        wr16(puVar26, 0x6666);
        wr16(puVar35 + 0x20, 0x1112);
        wr16(puVar35 + 0x24, 0x4112);
        wr16(puVar35 + 0x28, 0x1111);
        wr16(puVar35 + 0x2c, 0x1111);
        wr16(puVar35 + 0x30, 0x1111);
        wr16(puVar35 + 0x34, 0x1111);
        wr16(puVar35 + 0x44, 0);
        wr16(puVar35 + 0x48, 0x1111);
        wr16(puVar35 + 0x4c, 0x111);
        wr16(puVar35 + 0x50, 0x111);
        wr16(puVar35 + 0x54, 0x111);
        wr16(puVar35 + 0x78, 0x4444);
        wr16(puVar35 + 0x7c, 0x4444);
        wr16(puVar35 + 0x80, 0x4444);
        wr16(puVar35 + 0x84, 0x4444);
        wr16(0x1f2021b0, 0x44);
        puVar35 = 0x1f2021c0;
        wr16(puVar35, 0x5555);
        wr16(puVar35 + 0x4, 0x5555);
        wr16(puVar35 + 0x8, 0x5555);
        wr16(puVar35 + 0xc, 0x5555);
        puVar35 = 0x1f2021d0;
        puVar26 = 0x1f2021d0 - 0x10c;
        wr16(puVar35, 0x55);
        puVar41 = puVar35 - 0x110;
        wr16(puVar26, 0x7f);
        wr16(puVar35 - 0x108, 0xf000);
        wr16(puVar41, 0xcb);
        wr16(puVar41, 0xcf);
        wr16(puVar41, 0xcb);
        wr16(puVar41, 0xc3);
        wr16(puVar41, 0xcb);
        wr16(puVar41, 0xc3);
        wr16(puVar41, 0xcb);
        wr16(puVar41, 0xc2);
        wr16(puVar41, 0xc0);
        wr16(puVar41, 0x33c8);
        puVar26 = 0x1f202130;
        wr16(puVar35 - 0xf0, 0);
        wr16(puVar26, 0);
        wr16(puVar26 + 0x4, 0);
        wr16(puVar26 - 0x10, 0xf0f1);
        wr16(puVar35 - 0xf0, 0x800);
        wr16(puVar39 + 0x3dc, 0x8021);
        wr16(puVar39 + 0x57c, 0x951a);
        wr16(puVar39 + 0x428, 0xffff);
        wr16(puVar39 + 0x468, 0xffff);
        wr16(puVar39 + 0x4a8, 0xffff);
        wr16(puVar39 + 0x4e8, 0xffff);
        wr16(puVar39 + 0x1a8, 0xffff);
        wr16(puVar39 + 0x1e8, 0xffff);
        wr16(puVar39 + 0x404, 0x8015);
        wr16(puVar39 + 0x444, 0x8015);
        wr16(puVar39 + 0x484, 0x8015);
        wr16(puVar39 + 0x4c4, 0x8015);
        wr16(puVar39 + 0x184, 0x8015);
        wr16(puVar39 + 0x1c4, 0x8015);
        puVar39 = 0x1f20203c;
        wr16(puVar40, 1);
        wr16(puVar35 - 0xf0, 0x800);
        wr16(puVar35 - 0x120, 0xa0a);
        wr16(puVar35 - 0x11c, 0xaaaa);
        wr16(puVar35 - 0x118, 0xaaaa);
        wr16(puVar35 - 0x114, 0xaaaa);
        wr16(puVar35 - 0x19c, 0x8000);
        wr16(puVar35 - 0x198, 0x20);
        wr16(puVar35 - 0x1c0, 0x3f);
        wr16(puVar39, 5);
        wr16(puVar39, 0xf);
        wr16(puVar39, 5);
        wr16(puVar8, 0x8c01);
        wr16(puVar8, 0x8c00);
        puVar35 = 0x1f202000;
        wr16(puVar35, 0x2010);
        wr16(puVar35, 0);
        wr16(0x1f202030, 0);
        wr16(0x1f2020f8, 0);
        wr16(0x1f2020a8, 0x4000);
        wr16(puVar39, 5);
        wr16(puVar39, 0xf);
        wr16(puVar39, 5);
        wr16(puVar35, 1);
        wr16(puVar8 - 0x3c, 0);
        delay(12000);
        wr16(puVar8 - 0x3c, 8);
        wr16(puVar8 - 0x3c, 0xc);
        delay(12000);
        wr16(puVar8 - 0x3c, 0xe);
        delay(12000);
        wr16(puVar8 - 0x3c, 0xf);
        delay(12000);
        delay(12000);
        delay(12000);
        wr16(puVar39, 5);
        wr16(puVar39, 0xf);
        wr16(puVar39, 5);
        wr16(puVar12, 0x7ffe);
        /* LAB_a00028c0: shared tail (both DDR paths converge here) */
        puVar8 = 0x1f2023cc;
        wr16(puVar8, 0xfffa);
        wr16(puVar8 + 0x230, 0xa0e1);
        wr16(puVar8 + 0x230, 0x80e1);
        wr16(0x1f2025e0, 0);
    } else if ((0x1c < bond) && (bond < 0x20)) {
        uart_puts("miupll_233MHz\r\n");
        puVar8 = 0x1f20243c;
        puVar11 = 0x1f206205;
        wr8(0x1f206205, 0);
        wr8(puVar11 + 0x3, 0);
        wr8(puVar11 + 0x4, 0);
        puVar26 = 0x1f20248c;
        wr8(puVar11 + 0x7, 0x1e);
        wr8(puVar11 + 0x8, 1);
        wr8(puVar11 + 0xb, 0x10);
        puVar35 = 0x1f2024cc;
        wr8(puVar11 + 0xc, 0);
        wr16(puVar8, 0xc00);
        wr16(puVar8, 0xc00);
        wr16(puVar8, 0xc00);
        wr16(puVar8, 0xc01);
        wr16(puVar26, 0xfffe);
        wr16(puVar35, 0xffff);
        wr16(puVar35 + 0x40, 0xffff);
        wr16(puVar35 + 0x80, 0xffff);
        wr16(puVar35 - 0x2c0, 0xffff);
        wr16(puVar35 - 0x280, 0xffff);
        wr16(puVar35 - 0x100, 0xfffe);
        puVar12 = 0x1f202048;
        wr16(0x1f2020f0, 1);
        delay(12000);
        wr16(puVar12, 0x1000);
        delay(12000);
        wr16(puVar12, 0);
        delay(12000);
        puVar35 = 0x1f20206c;
        wr16(0x1f20206c, 0x400);
        wr16(puVar35 - 0x4, 0x2004);
        wr16(puVar12 + 0xcc, 1);
        wr16(puVar35 - 0xc, 0x8f5c);
        wr16(puVar35 - 0x8, 0x1e);
        delay(12000);
        puVar35 = 0x1f202044;
        wr16(0x1f202044, 4);
        wr16(puVar35 + 0x14, 0x114);
        wr16(puVar35 + 0xcc, 0x11);
        wr16(puVar35 + 0x3c0, 0x2a3);
        wr16(puVar35 + 0x3c4, 0x54);
        wr16(puVar35 + 0x3c8, 0x1570);
        wr16(puVar35 + 0x3cc, 0x20dd);
        wr16(puVar35 + 0x3d0, 0x2d76);
        wr16(puVar35 + 0x3d4, 0xe7e9);
        wr16(puVar35 + 0x3d8, 0x4096);
        wr16(puVar35 + 0x3dc, 0x1f14);
        wr16(puVar35 + 0x3e0, 0x4004);
        wr16(puVar35 + 0x3e4, 0x8020);
        wr16(puVar35 + 0x3e8, 0xc000);
        wr16(puVar35 + 0x40c, 0xb0);
        wr16(puVar35 + 0x600, 3);
        wr16(puVar35 + 0x63c, 0xd0d);
        wr16(puVar35 + 0x640, 0x620);
        wr16(puVar35 + 0x644, 0x2d07);
        wr16(puVar35 + 0x648, 0xe09);
        wr16(puVar35 + 0x64c, 0xe07);
        wr16(puVar35 + 0x650, 0x504);
        wr16(puVar35 + 0x654, 0x528);
        wr16(puVar35 + 0x658, 0x96);
        wr16(puVar35 + 0x65c, 0xe000);
        wr16(puVar35 + 0x660, 0);
        wr16(puVar35 + 0x664, 0xd00);
        wr16(puVar35 + 0x6bc, 0);
        wr16(puVar35 + 0x6c8, 0);
        wr16(puVar35 + 0x7b8, 0);
        wr16(puVar35 + 0x27c, 0);
        wr16(puVar35 + 0x280, 0);
        wr16(puVar35 + 0x284, 0);
        wr16(puVar35 + 0x288, 0x30);
        wr16(puVar35 + 0x28c, 0x5000);
        wr16(puVar35 - 0x40, 0xaaaa);
        wr16(puVar35 - 0x3c, 0x80);
        wr16(puVar35 - 0x30, 0x2200);
        wr16(puVar35 - 0x28, 0x97);
        wr16(puVar35 + 0x18, 0x1122);
        puVar39 = 0x1f2020d8;
        wr16(0x1f202070, 0x77);
        wr16(0x1f202074, 0x6066);
        wr16(0x1f202078, 0x9422);
        wr16(0x1f20207c, 0xa044);
        wr16(0x1f202090, 0x77);
        wr16(0x1f202094, 0x6060);
        puVar35 = 0x1f202098;
        wr16(0x1f202098, 0x44);
        wr16(puVar35 + 0x4, 0x44);
        wr16(puVar35 + 0x8, 0x1111);
        wr16(puVar35 + 0xc, 0xc);
        wr16(puVar39, 0x808);
        wr16(puVar39 + 0x4, 0x808);
        wr16(puVar39 + 0x10, 0x404);
        wr16(puVar39 + 0x14, 0x404);
        puVar35 = 0x1f202128;
        wr16(0x1f202128, 0x1313);
        wr16(puVar35 + 0x18, 0x4045);
        wr16(puVar35 + 0x1c, 0x5453);
        wr16(puVar35 + 0x20, 0x6555);
        wr16(puVar35 + 0x24, 0x6666);
        wr16(puVar35 + 0x28, 0x1111);
        puVar39 = 0x1f202174;
        wr16(puVar35 + 0x2c, 0x1111);
        wr16(puVar35 + 0x30, 0x1111);
        wr16(puVar35 + 0x34, 0x1111);
        wr16(0x1f20216c, 0);
        wr16(puVar35 + 0x48, 0x4444);
        wr16(puVar39, 0x444);
        wr16(puVar39 + 0x4, 0x444);
        wr16(puVar39 + 0x8, 0x444);
        puVar35 = 0x1f2021a0;
        wr16(0x1f2021a0, 0x4444);
        wr16(puVar35 + 0x4, 0x4444);
        wr16(puVar35 + 0x8, 0x5555);
        wr16(puVar35 + 0xc, 0x5555);
        wr16(puVar35 + 0x10, 0x54);
        wr16(puVar35 + 0x20, 0x5555);
        wr16(puVar35 + 0x24, 0x5555);
        wr16(puVar35 + 0x28, 0x5555);
        wr16(puVar35 + 0x2c, 0x5555);
        puVar35 = 0x1f2021d0;
        puVar39 = 0x1f2021d0 - 0x10c;
        wr16(0x1f2021d0, 0x55);
        puVar40 = puVar35 - 0x110;
        wr16(puVar39, 0x7f);
        wr16(puVar35 - 0x108, 0xf000);
        wr16(puVar40, 0xcb);
        wr16(puVar40, 0xcf);
        wr16(puVar40, 0xcb);
        wr16(puVar40, 0xc3);
        wr16(puVar40, 0xcb);
        wr16(puVar40, 0xc3);
        wr16(puVar40, 0xcb);
        wr16(puVar40, 0xc2);
        wr16(puVar40, 0xc0);
        wr16(puVar40, 0x33c8);
        puVar39 = 0x1f202130;
        wr16(puVar35 - 0xf0, 0);
        wr16(puVar39, 0);
        wr16(puVar39 + 0x4, 0);
        wr16(puVar39 - 0x10, 0xf0f1);
        puVar39 = 0x1f202458;
        wr16(puVar35 - 0xf0, 0x800);
        wr16(puVar39, 0x8021);
        wr16(puVar39 + 0x1a0, 0x951a);
        wr16(puVar39 + 0x4c, 0xffff);
        wr16(puVar39 + 0x8c, 0xffff);
        wr16(puVar39 + 0xcc, 0xffff);
        wr16(puVar39 + 0x10c, 0xffff);
        wr16(puVar39 - 0x234, 0xffff);
        wr16(puVar39 - 0x1f4, 0xffff);
        wr16(puVar39 + 0x28, 0x8015);
        wr16(puVar39 + 0x68, 0x8015);
        wr16(puVar39 + 0xa8, 0x8015);
        wr16(puVar39 + 0xe8, 0x8015);
        wr16(puVar39 - 0x258, 0x8015);
        wr16(puVar39 - 0x218, 0x8015);
        puVar39 = 0x1f20203c;
        wr16(puVar12 + 0xcc, 1);
        wr16(puVar35 - 0xf0, 0x800);
        wr16(puVar35 - 0x120, 0xa0a);
        wr16(puVar35 - 0x11c, 0xaaaa);
        wr16(puVar35 - 0x118, 0xaaaa);
        wr16(puVar35 - 0x114, 0xaaaa);
        wr16(puVar35 - 0x19c, 0x8000);
        wr16(puVar35 - 0x198, 0x20);
        wr16(puVar35 - 0x1c0, 0x3f);
        wr16(puVar39, 5);
        wr16(puVar39, 0xf);
        wr16(puVar39, 5);
        wr16(puVar8, 0x8c01);
        wr16(puVar8, 0x8c00);
        puVar35 = 0x1f202000;
        wr16(0x1f202000, 0x2010);
        wr16(puVar35, 0);
        wr16(0x1f202030, 0);
        wr16(0x1f2020f8, 0);
        wr16(0x1f2020a8, 0xc000);
        wr16(puVar39, 5);
        wr16(puVar39, 0xf);
        wr16(puVar39, 5);
        wr16(puVar35, 2);
        wr16(puVar8 - 0x3c, 0);
        delay(12000);
        wr16(puVar8 - 0x3c, 8);
        wr16(puVar8 - 0x3c, 0xc);
        delay(12000);
        wr16(puVar8 - 0x3c, 0xe);
        delay(12000);
        wr16(puVar8 - 0x3c, 0xf);
        delay(12000);
        delay(12000);
        delay(12000);
        wr16(puVar39, 5);
        wr16(puVar39, 0xf);
        wr16(puVar39, 5);
        wr16(puVar26, 0x7ffe);
        /* LAB_a00028c0: shared tail (both DDR paths converge here) */
        puVar8 = 0x1f2023cc;
        wr16(puVar8, 0xfffa);
        wr16(puVar8 + 0x230, 0xa0e1);
        wr16(puVar8 + 0x230, 0x80e1);
        wr16(0x1f2025e0, 0);
    } else {
        uart_puts("unknown miupll\\r\\n");
    }

    /* --- DDR calibration report + MIU byte config (a00028c0..) --- */
    puVar13 = 0x1f00402c;
    puVar36 = 0x1f00400c;
    wr16(puVar36, rd16(0x1f00400c) & 0xfeff);
    if ((rd16(puVar13) & 0x800) != 0) {
      uVar24 = (rd16(puVar13) & 0x7ff) >> 5;
      uart_puts("MIU0 zq=0x");
      uart_put_hex16(uVar24);
      uart_puts("\n\r");
      wr16(0x1f202160, rd16(0x1f202160) & 0x81ff | (ushort)(uVar24 << 9));
    }
    if ((int)((uint)rd16(0x1f004024) << 0x10) < 0) {
      uVar24 = (rd16(0x1f004024) & 0x7fff) >> 0xc;
      uart_puts("MIU0 drvp=0x");
      uart_put_hex16(uVar24);
      uart_puts("\n\r");
      uVar24 = uVar24 - 1 & 0xffff;
      if (uVar24 < 7) {
        iVar25 = (int)*(char *)(0xa0004920 + uVar24);
      }
      else {
        iVar25 = 0;
      }
      FUN_a0001128(0xe,0x1f2020b0,8,iVar25);
      FUN_a0001128(8,0x1f2020bc,0,iVar25);
      FUN_a0001128(10,0x1f2020bc,4,iVar25);
      FUN_a0001128(0xc,0x1f2020bc,8,iVar25);
      FUN_a0001128(0xd,0x1f2020bc,0xc,iVar25);
    }
    if ((int)((uint)rd16(puVar13) << 0x10) < 0) {
      uVar24 = (rd16(puVar13) & 0x7fff) >> 0xc;
      uart_puts("MIU0 drvN=0x");
      uart_put_hex16(uVar24);
      uart_puts("\n\r");
      uVar24 = uVar24 - 1 & 0xffff;
      if (uVar24 < 7) {
        iVar25 = (int)*(char *)(0xa0004920 + uVar24);
      }
      else {
        iVar25 = 0;
      }
      FUN_a0001128(6,0x1f2020b0,0,iVar25);
      FUN_a0001128(0,0x1f2020b8,0,iVar25);
      FUN_a0001128(2,0x1f2020b8,4,iVar25);
      FUN_a0001128(4,0x1f2020b8,8,iVar25);
      FUN_a0001128(5,0x1f2020b8,0xc,iVar25);
    }
    boot_record(0x2a0, "MIU-");
    puVar46 = 0x1f2024a1;
    puVar14 = 0x1f202480;
    puVar11 = 0x1f202490;
    wr8(puVar14, 0x15);
    wr8(puVar14 + 0x1, 0x80);
    wr8(puVar14 + 0x4, 8);
    wr8(puVar14 + 0x5, 0x20);
    wr8(puVar14 + 0x8, 0);
    wr8(puVar14 + 0x9, 4);
    wr8(puVar11, 0xff);
    wr8(puVar11 + 0x1, 0xff);
    wr8(puVar11 + 0x4, 0x10);
    wr8(puVar11 + 0x5, 0x32);
    wr8(puVar11 + 0x8, 0x54);
    wr8(puVar11 + 0x9, 0x76);
    wr8(puVar11 + 0xc, 0x98);
    wr8(puVar11 + 0xd, 0xba);
    wr8(puVar11 + 0x10, 0xdc);
    wr8(puVar46, 0xfe);
    wr8(puVar46 + 0x17, 0);
    wr8(puVar46 + 0x18, 0);
    wr8(puVar46 + 0x1f, 0x15);
    wr8(puVar46 + 0x20, 0x80);
    wr8(puVar46 + 0x23, 8);
    wr8(puVar46 + 0x24, 0x20);
    wr8(puVar46 + 0x27, 0);
    wr8(puVar46 + 0x28, 4);
    wr8(puVar46 + 0x2f, 0xff);
    wr8(puVar46 + 0x30, 0xff);
    wr8(puVar46 + 0x33, 0x10);
    wr8(puVar46 + 0x34, 0x32);
    wr8(puVar46 + 0x37, 0x54);
    wr8(puVar46 + 0x38, 0x76);
    wr8(puVar46 + 0x3b, 0x98);
    wr8(puVar46 + 0x3c, 0xba);
    wr8(puVar46 + 0x3f, 0xdc);
    wr8(puVar46 + 0x40, 0xfe);
    wr8(puVar46 + 0x5f, 0x15);
    wr8(puVar46 + 0x60, 0x80);
    wr8(puVar46 + 0x63, 8);
    wr8(puVar46 + 0x64, 0x20);
    wr8(puVar46 + 0x67, 0);
    wr8(puVar46 + 0x68, 4);
    wr8(puVar46 + 0x6f, 0xff);
    wr8(puVar46 + 0x70, 0xff);
    wr8(puVar46 + 0x73, 0x10);
    wr8(puVar46 + 0x74, 0x32);
    wr8(puVar46 + 0x77, 0x54);
    wr8(puVar46 + 0x78, 0x76);
    wr8(puVar46 + 0x7b, 0x98);
    wr8(puVar46 + 0x7c, 0xba);
    wr8(puVar46 + 0x7f, 0xdc);
    wr8(puVar46 + 0x80, 0xfe);
    wr8(puVar46 + 0x9f, 0x15);
    puVar11 = 0x1f202541;
    wr8(puVar11, 0x80);
    wr8(puVar11 + 0x3, 8);
    wr8(puVar11 + 0x4, 0x20);
    puVar11 = 0x1f202548;
    wr8(puVar11, 0);
    wr8(puVar11 + 0x1, 4);
    puVar11 = 0x1f202550;
    wr8(puVar11, 0xff);
    wr8(puVar11 + 0x1, 0xff);
    wr8(puVar11 + 0x4, 0x10);
    wr8(puVar11 + 0x5, 0x32);
    wr8(puVar11 + 0x8, 0x54);
    wr8(puVar11 + 0x9, 0x76);
    wr8(puVar11 + 0xc, 0x98);
    wr16(0x1f20255d, 0xba);
    wr16(0x1f202560, 0xdc);
    puVar11 = 0x1f202561;
    wr8(puVar11, 0xfe);
    wr8(puVar11 + 0x9b, 0xe1);
    wr8(puVar11 + 0x9c, 0x80);
    wr8(puVar11 - 0x1a1, 2);
    wr8(puVar11 - 0x1a0, 0);
    wr8(puVar11 - 0x19d, 0x1e);
    wr8(puVar11 - 0x19c, 0);
    wr8(puVar11 - 0x191, 0x18);
    wr8(puVar11 - 0x190, 0);
    wr8(puVar11 - 0x18d, 8);
    puVar11 = 0x1f2023d5;
    wr8(puVar11, 0x40);
    wr8(puVar11 + 0x3, 2);
    wr8(puVar11 + 0x4, 2);
    wr8(puVar11 + 0x1b, 0xe1);
    wr8(puVar11 + 0x1c, 0xff);
    uart_puts("miu_bw_set\r\n");
}

/* entry @ a0000010 */
void ipl_entry(void)
{
    wr32(IPL_PROGRESS, 0xa001);      /* a0000018 */
    cp15_smp_fp_init();         /* a0000028..a000004c (no RIU writes) */
    wr32(IPL_PROGRESS, 0xa002);      /* a0000058 */
    ipl_main();                 /* blx a0001d50 */
}
