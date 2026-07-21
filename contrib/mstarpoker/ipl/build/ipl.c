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

static void uart_put_hex16(unsigned int val)     /* FUN_a00016da */
{
    uart_put_hex8((val >> 8) & 0xff);
    uart_put_hex8(val & 0xff);
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

static void FUN_a0001128(unsigned int p1, volatile unsigned short *p2,
                         unsigned int p3, int p4)   /* FUN_a0001128 */
{
    volatile unsigned short *shared = (volatile unsigned short *)0x1f2021bc;
    unsigned short v1;
    int i2;
    unsigned int u3 = 0xfu << (p3 & 0xff);
    unsigned int u4 = 1u << (p1 & 0xff);

    i2 = p4 + (((int)(*p2 & u3) >> (p3 & 0xff)
                | ((int)(u4 & *shared) >> (p1 & 0xff)) << 4) & 0xffff);
    v1 = (p4 < 0 && i2 < 0) ? 0 : (unsigned short)i2;
    *shared = (unsigned short)(((v1 & 0x1f) >> 4) << (p1 & 0xff))
              | (*shared & ~(unsigned short)u4);
    *p2 = (*p2 & ~(unsigned short)u3) | (unsigned short)((v1 & 0xf) << (p3 & 0xff));
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
    volatile unsigned short *puVar8, *puVar12, *puVar26, *puVar35, *puVar39, *puVar40;
    volatile unsigned short *puVar13, *puVar36;
    volatile unsigned char *puVar11, *puVar14, *puVar46;
    unsigned int uVar24;
    int iVar25;

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

    /* DDR/MIU init, split by chip bond id (0x1e = SSD202D, 128MB) */
    if (bond == 0x1d) {
        /* SSD201 / 64MB path - not ported yet (bond is 0x1e on this board) */
    } else if ((0x1c < bond) && (bond < 0x20)) {
        uart_puts("miupll_233MHz\r\n");
        puVar8 = (volatile unsigned short *)0x1f20243c;
        puVar11 = (volatile unsigned char *)0x1f206205;
        *(volatile unsigned char *)0x1f206205 = 0;
        puVar11[3] = 0;
        puVar11[4] = 0;
        puVar26 = (volatile unsigned short *)0x1f20248c;
        puVar11[7] = 0x1e;
        puVar11[8] = 1;
        puVar11[0xb] = 0x10;
        puVar35 = (volatile unsigned short *)0x1f2024cc;
        puVar11[0xc] = 0;
        puVar8[0] = 0xc00;
        puVar8[0] = 0xc00;
        puVar8[0] = 0xc00;
        puVar8[0] = 0xc01;
        puVar26[0] = 0xfffe;
        puVar35[0] = 0xffff;
        puVar35[0x20] = 0xffff;
        puVar35[0x40] = 0xffff;
        puVar35[-0x160] = 0xffff;
        puVar35[-0x140] = 0xffff;
        puVar35[-0x80] = 0xfffe;
        puVar12 = (volatile unsigned short *)0x1f202048;
        *(volatile unsigned short *)0x1f2020f0 = 1;
        delay(12000);
        puVar12[0] = 0x1000;
        delay(12000);
        puVar12[0] = 0;
        delay(12000);
        puVar35 = (volatile unsigned short *)0x1f20206c;
        *(volatile unsigned short *)0x1f20206c = 0x400;
        puVar35[-2] = 0x2004;
        puVar12[0x66] = 1;
        puVar35[-6] = 0x8f5c;
        puVar35[-4] = 0x1e;
        delay(12000);
        puVar35 = (volatile unsigned short *)0x1f202044;
        *(volatile unsigned short *)0x1f202044 = 4;
        puVar35[10] = 0x114;
        puVar35[0x66] = 0x11;
        puVar35[0x1e0] = 0x2a3;
        puVar35[0x1e2] = 0x54;
        puVar35[0x1e4] = 0x1570;
        puVar35[0x1e6] = 0x20dd;
        puVar35[0x1e8] = 0x2d76;
        puVar35[0x1ea] = 0xe7e9;
        puVar35[0x1ec] = 0x4096;
        puVar35[0x1ee] = 0x1f14;
        puVar35[0x1f0] = 0x4004;
        puVar35[0x1f2] = 0x8020;
        puVar35[500] = 0xc000;
        puVar35[0x206] = 0xb0;
        puVar35[0x300] = 3;
        puVar35[0x31e] = 0xd0d;
        puVar35[800] = 0x620;
        puVar35[0x322] = 0x2d07;
        puVar35[0x324] = 0xe09;
        puVar35[0x326] = 0xe07;
        puVar35[0x328] = 0x504;
        puVar35[0x32a] = 0x528;
        puVar35[0x32c] = 0x96;
        puVar35[0x32e] = 0xe000;
        puVar35[0x330] = 0;
        puVar35[0x332] = 0xd00;
        puVar35[0x35e] = 0;
        puVar35[0x364] = 0;
        puVar35[0x3dc] = 0;
        puVar35[0x13e] = 0;
        puVar35[0x140] = 0;
        puVar35[0x142] = 0;
        puVar35[0x144] = 0x30;
        puVar35[0x146] = 0x5000;
        puVar35[-0x20] = 0xaaaa;
        puVar35[-0x1e] = 0x80;
        puVar35[-0x18] = 0x2200;
        puVar35[-0x14] = 0x97;
        puVar35[0xc] = 0x1122;
        puVar39 = (volatile unsigned short *)0x1f2020d8;
        R16(0x1f202070) = 0x77;
        R16(0x1f202074) = 0x6066;
        R16(0x1f202078) = 0x9422;
        R16(0x1f20207c) = 0xa044;
        R16(0x1f202090) = 0x77;
        *(volatile unsigned short *)0x1f202094 = 0x6060;
        puVar35 = (volatile unsigned short *)0x1f202098;
        *(volatile unsigned short *)0x1f202098 = 0x44;
        puVar35[2] = 0x44;
        puVar35[4] = 0x1111;
        puVar35[6] = 0xc;
        puVar39[0] = 0x808;
        puVar39[2] = 0x808;
        puVar39[8] = 0x404;
        puVar39[10] = 0x404;
        puVar35 = (volatile unsigned short *)0x1f202128;
        *(volatile unsigned short *)0x1f202128 = 0x1313;
        puVar35[0xc] = 0x4045;
        puVar35[0xe] = 0x5453;
        puVar35[0x10] = 0x6555;
        puVar35[0x12] = 0x6666;
        puVar35[0x14] = 0x1111;
        puVar39 = (volatile unsigned short *)0x1f202174;
        puVar35[0x16] = 0x1111;
        puVar35[0x18] = 0x1111;
        puVar35[0x1a] = 0x1111;
        *(volatile unsigned short *)0x1f20216c = 0;
        puVar35[0x24] = 0x4444;
        puVar39[0] = 0x444;
        puVar39[2] = 0x444;
        puVar39[4] = 0x444;
        puVar35 = (volatile unsigned short *)0x1f2021a0;
        *(volatile unsigned short *)0x1f2021a0 = 0x4444;
        puVar35[2] = 0x4444;
        puVar35[4] = 0x5555;
        puVar35[6] = 0x5555;
        puVar35[8] = 0x54;
        puVar35[0x10] = 0x5555;
        puVar35[0x12] = 0x5555;
        puVar35[0x14] = 0x5555;
        puVar35[0x16] = 0x5555;
        puVar35 = (volatile unsigned short *)0x1f2021d0;
        puVar39 = (volatile unsigned short *)0x1f2021d0 - 0x86;
        *(volatile unsigned short *)0x1f2021d0 = 0x55;
        puVar40 = puVar35 - 0x88;
        puVar39[0] = 0x7f;
        puVar35[-0x84] = 0xf000;
        puVar40[0] = 0xcb;
        puVar40[0] = 0xcf;
        puVar40[0] = 0xcb;
        puVar40[0] = 0xc3;
        puVar40[0] = 0xcb;
        puVar40[0] = 0xc3;
        puVar40[0] = 0xcb;
        puVar40[0] = 0xc2;
        puVar40[0] = 0xc0;
        puVar40[0] = 0x33c8;
        puVar39 = (volatile unsigned short *)0x1f202130;
        puVar35[-0x78] = 0;
        puVar39[0] = 0;
        puVar39[2] = 0;
        puVar39[-8] = 0xf0f1;
        puVar39 = (volatile unsigned short *)0x1f202458;
        puVar35[-0x78] = 0x800;
        puVar39[0] = 0x8021;
        puVar39[0xd0] = 0x951a;
        puVar39[0x26] = 0xffff;
        puVar39[0x46] = 0xffff;
        puVar39[0x66] = 0xffff;
        puVar39[0x86] = 0xffff;
        puVar39[-0x11a] = 0xffff;
        puVar39[-0xfa] = 0xffff;
        puVar39[0x14] = 0x8015;
        puVar39[0x34] = 0x8015;
        puVar39[0x54] = 0x8015;
        puVar39[0x74] = 0x8015;
        puVar39[-300] = 0x8015;
        puVar39[-0x10c] = 0x8015;
        puVar39 = (volatile unsigned short *)0x1f20203c;
        puVar12[0x66] = 1;
        puVar35[-0x78] = 0x800;
        puVar35[-0x90] = 0xa0a;
        puVar35[-0x8e] = 0xaaaa;
        puVar35[-0x8c] = 0xaaaa;
        puVar35[-0x8a] = 0xaaaa;
        puVar35[-0xce] = 0x8000;
        puVar35[-0xcc] = 0x20;
        puVar35[-0xe0] = 0x3f;
        puVar39[0] = 5;
        puVar39[0] = 0xf;
        puVar39[0] = 5;
        puVar8[0] = 0x8c01;
        puVar8[0] = 0x8c00;
        puVar35 = (volatile unsigned short *)0x1f202000;
        *(volatile unsigned short *)0x1f202000 = 0x2010;
        puVar35[0] = 0;
        R16(0x1f202030) = 0;
        R16(0x1f2020f8) = 0;
        R16(0x1f2020a8) = 0xc000;
        puVar39[0] = 5;
        puVar39[0] = 0xf;
        puVar39[0] = 5;
        puVar35[0] = 2;
        puVar8[-0x1e] = 0;
        delay(12000);
        puVar8[-0x1e] = 8;
        puVar8[-0x1e] = 0xc;
        delay(12000);
        puVar8[-0x1e] = 0xe;
        delay(12000);
        puVar8[-0x1e] = 0xf;
        delay(12000);
        delay(12000);
        delay(12000);
        puVar39[0] = 5;
        puVar39[0] = 0xf;
        puVar39[0] = 5;
        puVar26[0] = 0x7ffe;
        /* LAB_a00028c0: shared tail (both DDR paths converge here) */
        puVar8 = (volatile unsigned short *)0x1f2023cc;
        puVar8[0] = 0xfffa;
        puVar8[0x118] = 0xa0e1;
        puVar8[0x118] = 0x80e1;
        *(volatile unsigned short *)0x1f2025e0 = 0;
    } else {
        uart_puts("unknown miupll\\r\\n");
    }

    /* --- DDR calibration report + MIU byte config (a00028c0..) --- */
    puVar13 = (volatile unsigned short *)0x1f00402c;
    puVar36 = (volatile unsigned short *)0x1f00400c;
    puVar36[0] = *(volatile unsigned short *)0x1f00400c & 0xfeff;
    if ((*puVar13 & 0x800) != 0) {
      uVar24 = (*puVar13 & 0x7ff) >> 5;
      uart_puts("MIU0 zq=0x");
      uart_put_hex16(uVar24);
      uart_puts("\n\r");
      *(volatile unsigned short *)0x1f202160 = *(volatile unsigned short *)0x1f202160 & 0x81ff | (ushort)(uVar24 << 9);
    }
    if ((int)((uint)*(volatile unsigned short *)0x1f004024 << 0x10) < 0) {
      uVar24 = (*(volatile unsigned short *)0x1f004024 & 0x7fff) >> 0xc;
      uart_puts("MIU0 drvp=0x");
      uart_put_hex16(uVar24);
      uart_puts("\n\r");
      uVar24 = uVar24 - 1 & 0xffff;
      if (uVar24 < 7) {
        iVar25 = (int)*(char *)((volatile unsigned short *)0xa0004920 + uVar24);
      }
      else {
        iVar25 = 0;
      }
      FUN_a0001128(0xe,(volatile unsigned short *)0x1f2020b0,8,iVar25);
      FUN_a0001128(8,(volatile unsigned short *)0x1f2020bc,0,iVar25);
      FUN_a0001128(10,(volatile unsigned short *)0x1f2020bc,4,iVar25);
      FUN_a0001128(0xc,(volatile unsigned short *)0x1f2020bc,8,iVar25);
      FUN_a0001128(0xd,(volatile unsigned short *)0x1f2020bc,0xc,iVar25);
    }
    if ((int)((uint)*puVar13 << 0x10) < 0) {
      uVar24 = (*puVar13 & 0x7fff) >> 0xc;
      uart_puts("MIU0 drvN=0x");
      uart_put_hex16(uVar24);
      uart_puts("\n\r");
      uVar24 = uVar24 - 1 & 0xffff;
      if (uVar24 < 7) {
        iVar25 = (int)*(char *)((volatile unsigned short *)0xa0004920 + uVar24);
      }
      else {
        iVar25 = 0;
      }
      FUN_a0001128(6,(volatile unsigned short *)0x1f2020b0,0,iVar25);
      FUN_a0001128(0,(volatile unsigned short *)0x1f2020b8,0,iVar25);
      FUN_a0001128(2,(volatile unsigned short *)0x1f2020b8,4,iVar25);
      FUN_a0001128(4,(volatile unsigned short *)0x1f2020b8,8,iVar25);
      FUN_a0001128(5,(volatile unsigned short *)0x1f2020b8,0xc,iVar25);
    }
    boot_record(0x2a0, "MIU-");
    puVar46 = (volatile unsigned char *)0x1f2024a1;
    puVar14 = (volatile unsigned char *)0x1f202480;
    puVar11 = (volatile unsigned char *)0x1f202490;
    puVar14[0] = 0x15;
    puVar14[1] = 0x80;
    puVar14[4] = 8;
    puVar14[5] = 0x20;
    puVar14[8] = 0;
    puVar14[9] = 4;
    puVar11[0] = 0xff;
    puVar11[1] = 0xff;
    puVar11[4] = 0x10;
    puVar11[5] = 0x32;
    puVar11[8] = 0x54;
    puVar11[9] = 0x76;
    puVar11[0xc] = 0x98;
    puVar11[0xd] = 0xba;
    puVar11[0x10] = 0xdc;
    puVar46[0] = 0xfe;
    puVar46[0x17] = 0;
    puVar46[0x18] = 0;
    puVar46[0x1f] = 0x15;
    puVar46[0x20] = 0x80;
    puVar46[0x23] = 8;
    puVar46[0x24] = 0x20;
    puVar46[0x27] = 0;
    puVar46[0x28] = 4;
    puVar46[0x2f] = 0xff;
    puVar46[0x30] = 0xff;
    puVar46[0x33] = 0x10;
    puVar46[0x34] = 0x32;
    puVar46[0x37] = 0x54;
    puVar46[0x38] = 0x76;
    puVar46[0x3b] = 0x98;
    puVar46[0x3c] = 0xba;
    puVar46[0x3f] = 0xdc;
    puVar46[0x40] = 0xfe;
    puVar46[0x5f] = 0x15;
    puVar46[0x60] = 0x80;
    puVar46[99] = 8;
    puVar46[100] = 0x20;
    puVar46[0x67] = 0;
    puVar46[0x68] = 4;
    puVar46[0x6f] = 0xff;
    puVar46[0x70] = 0xff;
    puVar46[0x73] = 0x10;
    puVar46[0x74] = 0x32;
    puVar46[0x77] = 0x54;
    puVar46[0x78] = 0x76;
    puVar46[0x7b] = 0x98;
    puVar46[0x7c] = 0xba;
    puVar46[0x7f] = 0xdc;
    puVar46[0x80] = 0xfe;
    puVar46[0x9f] = 0x15;
    puVar11 = (volatile unsigned char *)0x1f202541;
    puVar11[0] = 0x80;
    puVar11[3] = 8;
    puVar11[4] = 0x20;
    puVar11 = (volatile unsigned char *)0x1f202548;
    puVar11[0] = 0;
    puVar11[1] = 4;
    puVar11 = (volatile unsigned char *)0x1f202550;
    puVar11[0] = 0xff;
    puVar11[1] = 0xff;
    puVar11[4] = 0x10;
    puVar11[5] = 0x32;
    puVar11[8] = 0x54;
    puVar11[9] = 0x76;
    puVar11[0xc] = 0x98;
    *(volatile unsigned short *)0x1f20255d = 0xba;
    *(volatile unsigned short *)0x1f202560 = 0xdc;
    puVar11 = (volatile unsigned char *)0x1f202561;
    puVar11[0] = 0xfe;
    puVar11[0x9b] = 0xe1;
    puVar11[0x9c] = 0x80;
    puVar11[-0x1a1] = 2;
    puVar11[-0x1a0] = 0;
    puVar11[-0x19d] = 0x1e;
    puVar11[-0x19c] = 0;
    puVar11[-0x191] = 0x18;
    puVar11[-400] = 0;
    puVar11[-0x18d] = 8;
    puVar11 = (volatile unsigned char *)0x1f2023d5;
    puVar11[0] = 0x40;
    puVar11[3] = 2;
    puVar11[4] = 2;
    puVar11[0x1b] = 0xe1;
    puVar11[0x1c] = 0xff;
    uart_puts("miu_bw_set\r\n");
}

/* entry @ a0000010 */
void ipl_entry(void)
{
    IPL_PROGRESS = 0xa001;      /* a0000018 */
    cp15_smp_fp_init();         /* a0000028..a000004c (no RIU writes) */
    IPL_PROGRESS = 0xa002;      /* a0000058 */
    ipl_main();                 /* blx a0001d50 */
}
