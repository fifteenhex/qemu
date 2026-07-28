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

static void uart_puts((const char *)(const char *s))             /* FUN_a00016ee */
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

static void boot_record(unsigned int id, (const char *)(const char *label))   /* FUN_a00015fc */
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

/* ---- generated helper functions (from ipl_decompiled.c) ---- */
static void FUN_a000014c();
static unsigned int FUN_a00002ec(unsigned int x);
static unsigned int FUN_a0000300(int a, int b, unsigned int c, unsigned int d);
static void FUN_a000036c(int a, int b, unsigned int c, unsigned int d);
static unsigned int * FUN_a00008f0(unsigned int param_1, unsigned char param_2, unsigned int param_3);
static unsigned short FUN_a00009dc(unsigned short param_1, unsigned char param_2);
static void FUN_a0000a28();
static void FUN_a0000a78(int param_1);
static void FUN_a0000ac4(int param_1);
static void FUN_a0000bac();
static void FUN_a0000c36(int param_1);
static void FUN_a0000d1c(short param_1);
static void FUN_a0000d34(int param_1, int param_2, unsigned int param_3, unsigned int param_4);
static void FUN_a0000d84();
static unsigned short FUN_a0000d94();
static void FUN_a0000da0();
static void FUN_a0000db8();
static void FUN_a0000dd4(short param_1);
static unsigned int FUN_a0000de8();
static void FUN_a0000e2c();
static void FUN_a0000e54(unsigned int param_1);
static void FUN_a0000e90(unsigned int param_1);
static void FUN_a0000ebc(int param_1);
static unsigned short FUN_a0000ee4();
static void FUN_a0000ef0();
static void FUN_a0000f0c(int param_1);
static void FUN_a0000f30(int param_1);
static void FUN_a0000f7e(unsigned int param_1, unsigned int param_2, int param_3, unsigned int param_4);
static void FUN_a0000fe8(unsigned int param_1);
static unsigned int FUN_a00010c2(unsigned int a, unsigned int b, unsigned int c, int *d);
static void FUN_a000128c(unsigned short param_1, int param_2, unsigned int param_3, int param_4);
static void FUN_a000134c(int param_1, unsigned int param_2);
static void FUN_a000137c();
static unsigned int FUN_a0001398(unsigned int param_1);
static unsigned int FUN_a00013f8(unsigned int param_1);
static unsigned int FUN_a0001440(unsigned int param_1, unsigned int param_2);
static unsigned int FUN_a00014a8(unsigned int param_1, short param_2);
static unsigned int FUN_a00014f0(unsigned int param_1, short param_2);
static void FUN_a000153c(int param_1, unsigned int param_2, unsigned int param_3, unsigned int param_4);
static void FUN_a0001574(int param_1, unsigned int param_2, unsigned int param_3, unsigned int param_4);
static unsigned char FUN_a00015ac(unsigned int param_1, unsigned int param_2);
static char FUN_a00015e8(unsigned int param_1);
static void FUN_a000165c();
static void FUN_a0001700(unsigned int param_1, short *param_2, unsigned int param_3);
static void FUN_a00017f8();
static void FUN_a0001824(unsigned int param_1, unsigned int param_2, unsigned int param_3, unsigned short param_4);
static unsigned int FUN_a0001870(unsigned int param_1);
static unsigned int FUN_a00018ec(unsigned int param_1, unsigned int param_2, unsigned int param_3);
static unsigned int FUN_a000199c(unsigned short param_1);
static unsigned int FUN_a0001a1c();
static unsigned int FUN_a0001a58(unsigned char param_1);
static unsigned int FUN_a0001b24(unsigned short param_1, unsigned int param_2);
static unsigned int FUN_a0001bcc(unsigned short param_1);
static void FUN_a0001d10();
static unsigned long long FUN_a00046cc(unsigned int param_1, unsigned int param_2);

static void FUN_a000014c()
{

}

static unsigned int FUN_a00002ec(unsigned int x)
{
  return x;
}

static unsigned int FUN_a0000300(int a, int b, unsigned int c, unsigned int d)
{
  (void)a;(void)b;(void)c;(void)d;return 0;
}

static void FUN_a000036c(int a, int b, unsigned int c, unsigned int d)
{
  (void)a;(void)b;(void)c;(void)d;
}

static unsigned int * FUN_a00008f0(unsigned int param_1, unsigned char param_2, unsigned int param_3)
{
  unsigned int puVar1;
  unsigned int puVar2;
  unsigned int uVar3;
  unsigned int uVar4;
  unsigned int uVar5;
  unsigned int uVar6;
  int bVar7;
  uVar6 = -(int)param_1 & 3U;
  if (param_3 < (-(int)param_1 & 3U)) {
  uVar6 = param_3;
  }
  uVar3 = (unsigned int)param_2 << 0x18 | (unsigned int)param_2 << 0x10;
  uVar4 = uVar3 | uVar3 >> 0x10;
  puVar1 = param_1;
  if ((bool)((unsigned char)(uVar6 >> 1) & 1)) {
  *(unsigned char *)param_1 = param_2;
  puVar1 = ((int)param_1 + 2);
  *(unsigned char *)((int)param_1 + 1) = param_2;
  }
  puVar2 = puVar1;
  if ((int)(uVar6 << 0x1f) < 0) {
  puVar2 = ((int)puVar1 + 1);
  *(unsigned char *)puVar1 = param_2;
  }
  uVar5 = param_3 - uVar6;
  if (uVar6 <= param_3 && uVar5 != 0) {
  uVar6 = -(int)puVar2 & 0x1c;
  puVar1 = puVar2;
  if (uVar6 != 0) {
  if (uVar5 < uVar6) {
  uVar6 = uVar5 & 0x1c;
  }
  uVar5 = uVar5 - uVar6;
  if (SUB41(uVar6 >> 4,0)) {
  wr32(puVar2, uVar4);
  wr32(puVar2 + 0x4, uVar4);
  wr32(puVar2 + 0x8, uVar4);
  wr32(puVar2 + 0xc, uVar4);
  puVar2 = puVar2 + 0x10;
  }
  if ((int)(uVar6 << 0x1c) < 0) {
  wr32(puVar2, uVar4);
  wr32(puVar2 + 0x4, uVar4);
  puVar2 = puVar2 + 0x8;
  }
  puVar1 = puVar2;
  if ((bool)((unsigned char)((uVar6 << 0x1c) >> 0x1e) & 1)) {
  puVar1 = puVar2 + 0x4;
  wr32(puVar2, uVar4);
  }
  }
  uVar5 = uVar5 - 0x20;
  if (-1 < (int)uVar5) {
  do {
  bVar7 = 0x1f < uVar5;
  uVar5 = uVar5 - 0x20;
  wr32(puVar1, uVar4);
  wr32(puVar1 + 0x4, uVar4);
  wr32(puVar1 + 0x8, uVar4);
  wr32(puVar1 + 0xc, uVar4);
  wr32(puVar1 + 0x10, uVar4);
  wr32(puVar1 + 0x14, uVar4);
  wr32(puVar1 + 0x18, uVar4);
  wr32(puVar1 + 0x1c, uVar4);
  puVar1 = puVar1 + 0x20;
  } while (bVar7);
  }
  if ((bool)((unsigned char)(uVar5 + 0x20 >> 4) & 1)) {
  wr32(puVar1, uVar4);
  wr32(puVar1 + 0x4, uVar4);
  wr32(puVar1 + 0x8, uVar4);
  wr32(puVar1 + 0xc, uVar4);
  puVar1 = puVar1 + 0x10;
  }
  if ((int)(uVar5 << 0x1c) < 0) {
  wr32(puVar1, uVar4);
  wr32(puVar1 + 0x4, uVar4);
  puVar1 = puVar1 + 0x8;
  }
  puVar2 = puVar1;
  if ((bool)((unsigned char)((uVar5 << 0x1c) >> 0x1e) & 1)) {
  puVar2 = puVar1 + 0x4;
  wr32(puVar1, uVar4);
  }
  puVar1 = puVar2;
  if ((int)(uVar5 << 0x1e) < 0) {
  puVar1 = ((int)puVar2 + 2);
  *(short *)puVar2 = (short)(uVar3 >> 0x10);
  }
  if ((bool)((unsigned char)((uVar5 << 0x1e) >> 0x1e) & 1)) {
  *(unsigned char *)puVar1 = param_2;
  }
  return param_1;
  }
  return param_1;
}

static unsigned short FUN_a00009dc(unsigned short param_1, unsigned char param_2)
{
  unsigned short local_c;
  unsigned char local_9;
  local_c = 0;
  for (local_9 = 0; local_9 < param_2; local_9 = local_9 + 1) {
  local_c = *(unsigned short *)(((unsigned int)param_1 * 2 + 0x112200) * 2 + 0x1f000000);
  }
  return local_c;
}

static void FUN_a0000a28()
{
  wr16(0x1f2244a0, rd16(0x1f2244a0) | 1);
  FUN_a00009dc(0x28, 1);
  wr16(0x1f2244a0, rd16(0x1f2244a0) & 0xfffe);
  FUN_a00009dc(0x28, 1);
  return;
}

static void FUN_a0000a78(int param_1)
{
  unsigned int puVar1;
  unsigned short uVar2;
  puVar1 = 0x1f224484;
  if (param_1 == 1) {
  uVar2 = rd16(0x1f224484) | 2;
  }
  else {
  uVar2 = rd16(0x1f224484) & 0xfffd;
  }
  wr16(puVar1, uVar2);
  FUN_a00009dc(0x21, 1);
  wr16(puVar1, rd16(puVar1) | 4);
  FUN_a00009dc(0x21, 1);
  wr16(puVar1, rd16(puVar1) | 8);
  FUN_a00009dc(0x21, 1);
  return;
}

static void FUN_a0000ac4(int param_1)
{
  unsigned int local_c;
  wr16(0x1f224488, 0x80);
  wr16(0x1f224480, rd16(0x1f224480) | 1);
  for (local_c = 0x7f; -1 < local_c; local_c = local_c + -2) {
  rd16(0x1f22448c) = *(unsigned short *)(local_c * 2 + param_1) >> 8 | *(short *)(local_c * 2 + param_1) << 8
  ;
  FUN_a00009dc(0x23, 1);
  rd16(0x1f224490) =
  *(unsigned short *)((local_c + 0x7fffffff) * 2 + param_1) >> 8 |
  *(short *)((local_c + 0x7fffffff) * 2 + param_1) << 8;
  FUN_a00009dc(0x24, 1);
  }
  wr16(0x1f224480, rd16(0x1f224480) & 0xfffe);
  return;
}

static void FUN_a0000bac()
{
  wr16(0x1f224488, 0);
  FUN_a00009dc(0x22, 1);
  wr16(0x1f224480, rd16(0x1f224480) | 1);
  FUN_a00009dc(0x20, 1);
  wr16(0x1f22448c, 1);
  FUN_a00009dc(0x23, 1);
  wr16(0x1f224490, 1);
  FUN_a00009dc(0x24, 1);
  wr16(0x1f224480, rd16(0x1f224480) & 0xfffe);
  FUN_a00009dc(0x20, 1);
  return;
}

static void FUN_a0000c36(int param_1)
{
  unsigned int local_c;
  wr16(0x1f224488, 0x40);
  wr16(0x1f224480, rd16(0x1f224480) | 1);
  for (local_c = 0; local_c < 0x40; local_c = local_c + 1) {
  rd16(0x1f22448c) =
  (unsigned short)(unsigned char)((unsigned int)*(unsigned int *)(local_c * -4 + 0xfc + param_1) >> 0x18) |
  (unsigned short)((unsigned int)*(unsigned int *)(local_c * -4 + 0xfc + param_1) >> 8) & 0xff00;
  FUN_a00009dc(0x23, 1);
  rd16(0x1f224490) =
  (unsigned short)((*(unsigned int *)(local_c * -4 + 0xfc + param_1) & 0xff) << 8) |
  (unsigned short)((unsigned int)*(unsigned int *)(local_c * -4 + 0xfc + param_1) >> 8) & 0xff;
  FUN_a00009dc(0x24, 1);
  }
  wr16(0x1f224480, rd16(0x1f224480) & 0xfffe);
  return;
}

static void FUN_a0000d1c(short param_1)
{
  wr16(0x1f2244a0, rd16(0x1f2244a0) | param_1 << 8);
  FUN_a00009dc(0x28, 1);
  return;
}

static void FUN_a0000d34(int param_1, int param_2, unsigned int param_3, unsigned int param_4)
{
  unsigned int puVar1;
  unsigned int extraout_r2;
  unsigned short uVar2;
  puVar1 = 0x1f2244a0;
  if (param_1 == 1) {
  uVar2 = rd16(0x1f2244a0) | 2;
  }
  else {
  uVar2 = rd16(0x1f2244a0) & 0xfffd;
  }
  wr16(puVar1, uVar2);
  FUN_a00009dc(0x28, 1);
  uVar2 = rd16(puVar1);
  if (param_2 == 1) {
  uVar2 = uVar2 | 4;
  }
  else {
  uVar2 = uVar2 & 0xfffb;
  }
  wr16(puVar1, uVar2);
  FUN_a00009dc(0x28, 1);
  return;
}

static void FUN_a0000d84()
{
  wr16(0x1f22449c, rd16(0x1f22449c) | 1);
  return;
}

static unsigned short FUN_a0000d94()
{
  return rd16(0x1f2244a4);
}

static void FUN_a0000da0()
{
  wr16(0x1f224480, rd16(0x1f224480) | 1);
  FUN_a00009dc(0x20, 1);
  return;
}

static void FUN_a0000db8()
{
  wr16(0x1f224480, rd16(0x1f224480) & 0xfffe);
  FUN_a00009dc(0x20, 1);
  return;
}

static void FUN_a0000dd4(short param_1)
{
  wr16(0x1f224488, param_1 + 0xc0);
  FUN_a00009dc(0x22, 1);
  return;
}

static unsigned int FUN_a0000de8()
{
  unsigned short uVar1;
  FUN_a00009dc(0x25, 2);
  uVar1 = rd16(0x1f224494);
  FUN_a00009dc(0x26, 2);
  return CONCAT22(rd16(0x1f224498),uVar1);
}

static void FUN_a0000e2c()
{
  unsigned int puVar1;
  puVar1 = 0x1f224420;
  wr16(puVar1, rd16(0x1f224420) | 0x80);
  wr16(puVar1, rd16(puVar1) & 0xff7f);
  wr16(0x1f224574, 0);
  return;
}

static void FUN_a0000e54(unsigned int param_1)
{
  wr16(0x1f224428, (short)(param_1 & 0xfffffff));
  FUN_a00009dc(10, 1);
  wr16(0x1f22442c, (short)((param_1 & 0xfffffff) >> 0x10));
  FUN_a00009dc(0xb, 1);
  wr16(0x1f224420, rd16(0x1f224420) | 0x800);
  return;
}

static void FUN_a0000e90(unsigned int param_1)
{
  wr16(0x1f224430, (short)param_1);
  FUN_a00009dc(0xc, 1);
  wr16(0x1f224434, (short)((unsigned int)param_1 >> 0x10));
  FUN_a00009dc(0xd, 1);
  return;
}

static void FUN_a0000ebc(int param_1)
{
  unsigned short uVar1;
  if (param_1 == 1) {
  uVar1 = rd16(0x1f224420) | 0x200;
  }
  else {
  uVar1 = rd16(0x1f224420) & 0xfdff;
  }
  wr16(0x1f224420, uVar1);
  FUN_a00009dc(8, 1);
  return;
}

static unsigned short FUN_a0000ee4()
{
  return rd16(0x1f22443c);
}

static void FUN_a0000ef0()
{
  unsigned int puVar1;
  puVar1 = 0x1f224420;
  wr16(puVar1, rd16(0x1f224420) & 0xffbf);
  wr16(puVar1, rd16(puVar1) | 0x40);
  return;
}

static void FUN_a0000f0c(int param_1)
{
  unsigned short uVar1;
  if (param_1 == 0) {
  uVar1 = rd16(0x1f224420) & 0xfffe;
  }
  else {
  uVar1 = rd16(0x1f224420) | 1;
  }
  wr16(0x1f224420, uVar1);
  FUN_a00009dc(8, 1);
  return;
}

static void FUN_a0000f30(int param_1)
{
  unsigned int local_c;
  for (local_c = 0; local_c < 0x10; local_c = local_c + 1) {
  FUN_a00009dc((local_c & 0xffff) + 0x10 & 0xffff, 2);
  wr16((param_1 + local_c * 2), *(unsigned short *)((local_c + 0x89110) * 4 + 0x1f000000));
  }
  return;
}

static void FUN_a0000f7e(unsigned int param_1, unsigned int param_2, int param_3, unsigned int param_4)
{
  unsigned int uVar1;
  FUN_a0000e2c();
  FUN_a0000e54(param_1);
  FUN_a0000e90(param_2);
  if (param_3 == 0) {
  FUN_a0000ebc(0);
  }
  else {
  if (param_3 != 1) {
  return;
  }
  FUN_a0000ebc(1);
  }
  FUN_a0000f0c(1);
  do {
  uVar1 = FUN_a0000ee4();
  } while ((uVar1 & 1) != 1);
  FUN_a0000f30(param_4);
  FUN_a0000f0c(0);
  FUN_a0000ef0();
  FUN_a0000e2c();
  return;
}

static void FUN_a0000fe8(unsigned int param_1)
{
  unsigned int uVar1;
  unsigned int uVar2;
  int iVar3;
  int local_18;
  int local_14;
  FUN_a0000a28();
  FUN_a0000d1c((rd32(param_1 + 0x14) & 0xffff) - 1 & 0x3f);
  FUN_a0000d34(*(unsigned char *)(param_1 + 4), *(unsigned char *)((int)param_1 + 0x11), 0, 0);
  FUN_a0000a78(0);
  FUN_a0000a78(1);
  FUN_a0000ac4(rd32(param_1));
  if (*(char *)(param_1 + 4) == '\0') {
  if (rd32(param_1 + 0x4) != 0) {
  FUN_a0000c36(rd32(param_1 + 0x4));
  }
  FUN_a0000bac();
  }
  FUN_a0000d84();
  do {
  uVar1 = FUN_a0000d94();
  } while ((uVar1 & 2) != 2);
  if ((*(char *)(param_1 + 4) == '\0') && (rd32(param_1 + 0x14) != 0x800)) {
  local_14 = 0x20;
  }
  else {
  local_14 = 0x40;
  }
  FUN_a0000a78(0);
  local_18 = 0;
  while( true ) {
  if (local_14 <= local_18) break;
  FUN_a0000dd4(local_18);
  FUN_a0000da0();
  iVar3 = rd32(param_1 + 0xc);
  uVar2 = FUN_a0000de8();
  wr32((iVar3 + local_18 * 4), uVar2);
  local_18 = local_18 + 1;
  }
  FUN_a0000db8();
  FUN_a0000a28();
  return;
}

static unsigned int FUN_a00010c2(unsigned int a, unsigned int b, unsigned int c, int *d)
{
  (void)a;(void)b;(void)c;(void)d;return 0;
}

static void FUN_a000128c(unsigned short param_1, int param_2, unsigned int param_3, int param_4)
{
  unsigned int puVar1;
  unsigned int uVar2;
  int iVar3;
  unsigned int uVar4;
  unsigned int puVar5;
  unsigned int in_cr6;
  unsigned int in_cr7;
  iVar3 = rd16(0xa000c010);
  if ((iVar3 != 0) && (*(code **)(iVar3 + 0xc) != 0x0)) {
  (**(code **)(iVar3 + 0xc))(*(unsigned char *)(iVar3 + 4));
  }
  puVar1 = 0x1f200400;
  uVar2 = (param_3 + param_2 & 0xffffffc0) - (param_3 & 0xffffffc0) >> 6;
  uVar4 = param_3;
  if ((param_3 + param_2 & 0x3f) != 0) {
  uVar2 = uVar2 + 1;
  }
  while (uVar2 != 0) {
  uVar2 = uVar2 - 1;
  coprocessor_moveto(0xf,0,1,uVar4,in_cr7,in_cr6);
  uVar4 = uVar4 + 0x40;
  }
  wr16(puVar1, 0);
  wr16(puVar1 + 0x8, 0x4035);
  if (param_4 == 0) {
  wr16(0x1f20040c, 0x2000);
  }
  else {
  wr16(0x1f20040c, 0);
  }
  puVar1 = 0x1f200414;
  wr16(0x1f200410, param_1);
  wr16(puVar1, 0);
  wr16(puVar1 + 0x4, (short)param_3);
  wr16(puVar1 + 0x8, (unsigned short)((param_3 << 4) >> 0x14));
  wr16(puVar1 + 0xc, (short)param_2);
  wr16(puVar1 + 0x10, 0);
  puVar5 = 0x1f200400 + 0x4;
  wr16(puVar1, 1);
  do {
  } while (-1 < (int)((unsigned int)rd16(puVar5) << 0x1c));
  wr8(puVar5, 8);
  puVar1 = 0x1f002ff4;
  wr16(puVar1, 0x3800);
  wr16(puVar1 - 0x34, 0x17);
  wr16(puVar1 - 0x2c, 0);
  return;
}

static void FUN_a000134c(int param_1, unsigned int param_2)
{
  unsigned int uVar1;
  unsigned int uVar2;
  unsigned int puVar3;
  int iVar4;
  uVar1 = 0;
  while (uVar2 = (uVar1 & 0x7f) * 2, uVar2 < param_2) {
  puVar3 = (0x1f002d94u + uVar1 * 4);
  uVar1 = uVar1 + 1 & 0xff;
  *(char *)(param_1 + uVar2) = (char)rd16(puVar3);
  iVar4 = uVar2 + 1;
  if (iVar4 < (int)param_2) {
  uVar2 = uVar2 + param_1;
  puVar3 = (unsigned int)(rd16(puVar3) >> 8);
  }
  if (iVar4 < (int)param_2) {
  *(char *)(uVar2 + 1) = (char)puVar3;
  }
  }
  return;
}

static void FUN_a000137c()
{
  short sVar1;
  sVar1 = 100;
  do {
  if ((rd16(0x1f002db8) & 1) != 0) {
  return;
  }
  sVar1 = sVar1 + -1;
  } while (sVar1 != 0);
  return;
}

static unsigned int FUN_a0001398(unsigned int param_1)
{
  unsigned int puVar1;
  unsigned int puVar2;
  unsigned int puVar3;
  unsigned int uVar4;
  unsigned long long uVar5;
  puVar2 = 0x1f002dbc;
  puVar1 = 0x1f002db0;
  wr16(puVar2, 1);
  wr16(puVar1, 0);
  wr16(puVar1, 0x8007);
  wr16(puVar1 + 0x24, 0);
  wr16(puVar1 + 0x28, 0);
  wr16(puVar1 - 0x30, 0x1306);
  wr16(0x1f002d84, (unsigned short)param_1 & 0xff00 | (unsigned short)(unsigned char)((unsigned int)param_1 >> 0x10));
  puVar3 = 0x1f002d88;
  wr16(puVar3, (unsigned short)param_1 & 0xff);
  wr16(puVar3 + 0x20, 0x41);
  wr16(puVar3 + 0x24, 0);
  wr16(0x1f002db4, 1);
  uVar5 = FUN_a000137c();
  uVar4 = 0;
  if ((int)uVar5 != 0) {
  wr16(puVar2, (short)((unsigned long long)uVar5 >> 0x20));
  uVar4 = (int)((unsigned long long)uVar5 >> 0x20);
  }
  return uVar4;
}

static unsigned int FUN_a00013f8(unsigned int param_1)
{
  unsigned int puVar1;
  unsigned int uVar2;
  unsigned long long uVar3;
  puVar1 = 0x1f002db0;
  wr16(puVar1, 7);
  wr16(puVar1 + 0x24, 0);
  uVar2 = 2;
  wr16(puVar1 + 0x28, 0);
  wr16(puVar1 - 0x30, 0xc00f);
  wr16(puVar1 - 0x8, 2);
  wr16(puVar1 - 0x4, 1);
  wr16(puVar1 + 0x4, 1);
  uVar3 = FUN_a000137c();
  if ((int)uVar3 != 0) {
  wr16(0x1f002dbc, (short)((unsigned long long)uVar3 >> 0x20));
  FUN_a000134c(param_1, 0);
  uVar2 = 0;
  }
  return uVar2;
}

static unsigned int FUN_a0001440(unsigned int param_1, unsigned int param_2)
{
  unsigned int puVar1;
  char cVar2;
  unsigned int uVar3;
  short sVar4;
  unsigned long long uVar5;
  unsigned int uStack_c;
  puVar1 = 0x1f002db0;
  uStack_c = (unsigned int)param_2;
  wr16(puVar1, 7);
  wr16(puVar1 + 0x24, 0);
  wr16(puVar1 + 0x28, 0);
  wr16(puVar1 - 0x30, 0xff);
  wr16(puVar1 - 0x8, 1);
  wr16(puVar1 - 0x4, 0);
  wr16(0x1f002db4, 1);
  uVar5 = FUN_a000137c();
  if ((int)uVar5 == 0) {
  LAB_a0001498:
  uVar3 = 0;
  }
  else {
  sVar4 = 0x2711;
  wr16(0x1f002dbc, (short)((unsigned long long)uVar5 >> 0x20));
  do {
  sVar4 = sVar4 + -1;
  if (sVar4 == 0) goto LAB_a0001498;
  cVar2 = FUN_a00013f8((int)&uStack_c + 3);
  } while (cVar2 != '\0' || (uStack_c & 0x1000000) != 0);
  uVar3 = 1;
  }
  return uVar3;
}

static unsigned int FUN_a00014a8(unsigned int param_1, short param_2)
{
  unsigned int puVar1;
  unsigned int uVar2;
  unsigned long long uVar3;
  puVar1 = 0x1f002db0;
  wr16(puVar1, 7);
  wr16(puVar1 + 0x24, 0);
  wr16(puVar1 + 0x28, 0);
  wr16(puVar1 - 0x30, param_2 << 8 | 0x1f);
  wr16(puVar1 - 0x2c, (unsigned short)rd8(param_1));
  wr16(puVar1 - 0x8, 3);
  wr16(puVar1 - 0x4, 0);
  wr16(puVar1 + 0x4, 1);
  uVar3 = FUN_a000137c();
  if ((int)uVar3 == 0) {
  uVar2 = 2;
  }
  else {
  uVar2 = 0;
  wr16(0x1f002dbc, (short)((unsigned long long)uVar3 >> 0x20));
  }
  return uVar2;
}

static unsigned int FUN_a00014f0(unsigned int param_1, short param_2)
{
  unsigned int puVar1;
  unsigned int uVar2;
  unsigned long long uVar3;
  puVar1 = 0x1f002db0;
  wr16(puVar1, 7);
  wr16(puVar1 + 0x24, 0);
  wr16(puVar1 + 0x28, 0);
  wr16(puVar1 - 0x30, param_2 << 8 | 0xf);
  wr16(puVar1 - 0x8, 2);
  wr16(puVar1 - 0x4, 1);
  wr16(puVar1 + 0x4, 1);
  uVar3 = FUN_a000137c();
  uVar2 = (unsigned int)((unsigned long long)uVar3 >> 0x20);
  if ((int)uVar3 != 0) {
  FUN_a000134c(param_1, 1);
  uVar2 = 0;
  wr16(0x1f002dbc, 1);
  }
  return uVar2;
}

static void FUN_a000153c(int param_1, unsigned int param_2, unsigned int param_3, unsigned int param_4)
{
  unsigned char bVar1;
  unsigned int uStack_c;
  uStack_c = param_2;
  FUN_a00014f0((int)&uStack_c + 3, 0xb0);
  bVar1 = (unsigned char)(uStack_c >> 0x18);
  if (param_1 == 0) {
  if (-1 < (int)((uStack_c >> 0x18) << 0x1f)) {
  return;
  }
  bVar1 = bVar1 & 0xfe;
  }
  else {
  if ((int)((uStack_c >> 0x18) << 0x1f) < 0) {
  return;
  }
  bVar1 = bVar1 | 1;
  }
  uStack_c = CONCAT13(bVar1,(unsigned int)uStack_c);
  FUN_a00014a8((int)&uStack_c + 3, 0xb0);
  return;
}

static void FUN_a0001574(int param_1, unsigned int param_2, unsigned int param_3, unsigned int param_4)
{
  unsigned char bVar1;
  unsigned int uStack_c;
  uStack_c = param_2;
  FUN_a00014f0((int)&uStack_c + 3, 0xb0);
  bVar1 = (unsigned char)(uStack_c >> 0x18);
  if (param_1 == 0) {
  if ((int)((uStack_c >> 0x18) << 0x1c) < 0) {
  return;
  }
  bVar1 = bVar1 | 8;
  }
  else {
  if (-1 < (int)((uStack_c >> 0x18) << 0x1c)) {
  return;
  }
  bVar1 = bVar1 & 0xf7;
  }
  uStack_c = CONCAT13(bVar1,(unsigned int)uStack_c);
  FUN_a00014a8((int)&uStack_c + 3, 0xb0);
  return;
}

static unsigned char FUN_a00015ac(unsigned int param_1, unsigned int param_2)
{
  char cVar1;
  short sVar2;
  unsigned int uStack_c;
  sVar2 = 10000;
  uStack_c = (unsigned int)param_2;
  do {
  cVar1 = FUN_a00013f8((int)&uStack_c + 3);
  if (cVar1 == '\0' && (uStack_c & 0x1000000) == 0) {
  if ((uStack_c & 0x20000000) == 0) {
  return ((unsigned char)((uStack_c) >> 24)) & 0x20;
  }
  return 0xb;
  }
  sVar2 = sVar2 + -1;
  } while (sVar2 != 0);
  return 2;
}

static char FUN_a00015e8(unsigned int param_1)
{
  char cVar1;
  cVar1 = '\0';
  while( true ) {
  if (param_1 == 0) break;
  param_1 = param_1 >> 1;
  cVar1 = cVar1 + '\x01';
  }
  return cVar1 + -1;
}

static void FUN_a000165c()
{
  unsigned int puVar1;
  puVar1 = 0x1f204414;
  wr16(puVar1, 0);
  wr16(puVar1, 1);
  do {
  } while (-1 < (int)((unsigned int)rd16(0x1f204440) << 0x13));
  return;
}

static void FUN_a0001700(unsigned int param_1, short *param_2, unsigned int param_3)
{
  unsigned int uVar1;
  int iVar2;
  unsigned char uVar3;
  short sVar4;
  unsigned int uVar5;
  unsigned int pbVar6;
  unsigned int pbVar7;
  if (*(unsigned short *)(0xa000aa00u + 3) < 0x3f) {
  for (uVar5 = 0; uVar5 < *(unsigned short *)(0xa000aa00u + 3); uVar5 = uVar5 + 1) {
  if (*(short *)(0xa000aa10u + uVar5 * 8 + 4) == 1) goto LAB_a0001734;
  }
  }
  uart_puts("unable to find IDX for part type:");
  uart_put_hex16(1);
  uart_puts("\r\n");
  uart_puts("[I]m7\r\n");
  uVar5 = 1;
  LAB_a0001734:
  /* uVar1 = "Can't find PniPart\r\n" */
  iVar2 = 0xa000a010u;
  if (*(unsigned char *)(0xa000a010u + 0x20) == 0) {
  iVar2 = 0;
  pbVar7 = 0xa000aa04;
  do {
  pbVar6 = pbVar7 + 0x1;
  iVar2 = iVar2 + (unsigned int)rd8(pbVar7);
  pbVar7 = pbVar6;
  } while (pbVar6 != 0xa000ac00u);
  if (rd16(0xa000aa00) == iVar2) {
  if ((unsigned int)*(unsigned short *)(0xa000aa00u + 3) < (uVar5 & 0xffff)) {
  wr16(param_1, 0);
  uart_puts("Can't find PniPart\r\n");
  uart_puts("[I]de\n");
  uart_puts("unable to find PBA for part type:");
  uart_put_hex16(1);
  uart_puts("\r\n");
  uart_puts("[I]m8\r\n");
  return;
  }
  iVar2 = uVar5 * 8;
  wr16(param_1, *(unsigned short *)(iVar2 + -0x5fff55f0));
  wr16(param_2, *(short *)(0xa000aa18 + iVar2));
  uVar3 = (unsigned char)*(unsigned short *)(iVar2 + -0x5fff55ea);
  goto LAB_a000178a;
  }
  wr16(param_1, 0x10);
  sVar4 = 0x13;
  }
  else {
  wr16(param_1, (unsigned short)*(unsigned char *)(0xa000a010u + 0x20));
  sVar4 = *(unsigned char *)(iVar2 + 0x20) + 2;
  }
  wr16(param_2, sVar4);
  uVar3 = 1;
  LAB_a000178a:
  wr8(param_3, uVar3);
  return;
}

static void FUN_a00017f8()
{
  FUN_a00002ec(0);
  uart_puts("Disable MMU and D-cache\r\n");
  uart_puts("[HALT]\r\n");
  do {
  /* WARNING: Do nothing block with infinite loop */
  } while( true );
}

static void FUN_a0001824(unsigned int param_1, unsigned int param_2, unsigned int param_3, unsigned short param_4)
{
  int bVar1;
  unsigned int uVar2;
  int iVar3;
  unsigned int uVar4;
  int iVar5;
  unsigned int uStack_28;
  unsigned int uStack_24;
  unsigned int uStack_20;
  unsigned short uStack_1c;
  unsigned short local_1a;
  _uStack_1c = CONCAT22(0x30,param_4);
  uVar2 = param_1;
  iVar3 = 0xb;
  uStack_24 = param_2;
  uStack_20 = param_3;
  do {
  iVar5 = iVar3;
  /* iVar3 = " n/a " */
  if (iVar5 == 0) goto LAB_a000185e;
  uVar4 = (unsigned int)((unsigned long long)uVar2 * (unsigned long long)0xcccccccdu >> 0x23);
  *(char *)((int)&uStack_28 + iVar5 + 3) = (char)uVar2 + '0' + (char)uVar4 * -10;
  bVar1 = 9 < uVar2;
  uVar2 = uVar4;
  iVar3 = iVar5 + -1;
  } while (bVar1);
  iVar3 = (int)&uStack_28 + iVar5 + 3;
  LAB_a000185e:
  uStack_28 = param_1;
  uart_puts(" n/a ");
  return;
}

static unsigned int FUN_a0001870(unsigned int param_1)
{
  unsigned int puVar1;
  unsigned int puVar2;
  unsigned int uVar3;
  unsigned long long uVar4;
  puVar1 = 0x1f002db0;
  wr16(puVar1, 7);
  puVar2 = 0x1f002d80;
  wr16(puVar1 + 0x24, 0);
  wr16(puVar1 + 0x28, 0);
  wr16(puVar2, (unsigned char)(param_1 >> 0x10) | 0x13);
  puVar2 = 0x1f002d84;
  wr16(puVar2, (unsigned short)((param_1 & 0xff) << 8) | (unsigned short)(param_1 >> 8) & 0xff);
  wr16(puVar2 + 0x24, 4);
  wr16(puVar2 + 0x28, 0);
  wr16(puVar2 + 0x30, 1);
  uVar4 = FUN_a000137c();
  uVar3 = (unsigned int)((unsigned long long)uVar4 >> 0x20);
  if ((int)uVar4 == 0) {
  uart_puts("ERR_SPINAND_TIMEOUT\n");
  uart_puts("source/driver/spinand/halSPINAND.c");
  FUN_a0001824(0x188, 0, 0, 0);
  uart_puts("\r\n");
  uart_puts("[I]e3\n");
  uVar3 = 2;
  }
  else {
  wr16(0x1f002dbc, 1);
  }
  return uVar3;
}

static unsigned int FUN_a00018ec(unsigned int param_1, unsigned int param_2, unsigned int param_3)
{
  unsigned int puVar1;
  unsigned int puVar2;
  unsigned int puVar3;
  unsigned int puVar4;
  short sVar5;
  unsigned long long uVar6;
  unsigned char local_21;
  unsigned int uStack_20;
  puVar4 = 0x1f002dd8;
  puVar3 = 0x1f002dd4;
  puVar2 = 0x1f002db0;
  sVar5 = 0x65;
  local_21 = 1;
  uStack_20 = param_3;
  while (((int)((unsigned int)local_21 << 0x1f) < 0 && (sVar5 = sVar5 + -1, sVar5 != 0))) {
  wr16(puVar2, 7);
  wr16(puVar3, 0);
  wr16(puVar4, 0);
  puVar1 = 0x1f002d80;
  wr16(puVar1, 0xc00f);
  wr16(puVar1 + 0x28, 2);
  wr16(puVar1 + 0x2c, 1);
  wr16(puVar1 + 0x34, 1);
  uVar6 = FUN_a000137c();
  if ((int)uVar6 == 0) {
  uart_puts("CD Wait FSP Done Time Out !!!!\r\n");
  uart_puts("source/driver/spinand/halSPINAND.c");
  FUN_a0001824(0x149, 0, 0, 0);
  uart_puts("\r\n");
  uart_puts("[I]e1\n");
  return 2;
  }
  wr16(0x1f002dbc, (short)((unsigned long long)uVar6 >> 0x20));
  FUN_a000134c(&local_21, 0);
  }
  wr8(param_1, local_21);
  return 0;
}

static unsigned int FUN_a000199c(unsigned short param_1)
{
  unsigned int puVar1;
  unsigned int uVar2;
  unsigned long long uVar3;
  puVar1 = 0x1f002db0;
  wr16(puVar1, 7);
  wr16(puVar1 + 0x24, 0);
  wr16(puVar1 + 0x28, 0);
  wr16(puVar1 - 0x30, param_1);
  wr16(puVar1 - 0x8, 1);
  wr16(puVar1 - 0x4, 0);
  wr16(puVar1 + 0x4, 1);
  uVar3 = FUN_a000137c();
  uVar2 = (unsigned int)((unsigned long long)uVar3 >> 0x20);
  if ((int)uVar3 == 0) {
  uart_puts("ERR_SPINAND_TIMEOUT\n");
  uart_puts("source/driver/spinand/halSPINAND.c");
  FUN_a0001824(0xdc, 0, 0, 0);
  uart_puts("\r\n");
  uart_puts("[I]e2\n");
  uVar2 = 2;
  }
  else {
  wr16(0x1f002dbc, 1);
  }
  return uVar2;
}

static unsigned int FUN_a0001a1c()
{
  unsigned int puVar1;
  int iVar2;
  unsigned int uVar3;
  unsigned int uVar4;
  unsigned int extraout_r2;
  unsigned long long uVar5;
  iVar2 = timer_read();
  puVar1 = 0x1f002db8;
  uVar4 = 0x022550ffu;
  do {
  if ((rd16(puVar1) & 1) != 0) {
  uVar3 = 1;
  break;
  }
  uVar5 = timer_read(rd16(puVar1),uVar4);
  uVar4 = (unsigned int)((unsigned long long)uVar5 >> 0x20);
  uVar3 = extraout_r2;
  } while ((unsigned int)((int)uVar5 - iVar2) <= uVar4);
  wr16(0x1f002dbc, rd16(0x1f002dbc) | 1);
  return uVar3;
}

static unsigned int FUN_a0001a58(unsigned char param_1)
{
  unsigned int puVar1;
  unsigned int uVar2;
  unsigned int puVar3;
  wr16(0x1f002fc8, 1);
  puVar1 = 0x1f002d80;
  puVar3 = 0x1f002d80 + 0x4;
  wr16(puVar1, rd16(0x1f002d80) & 0xff00 | 6);
  wr16(puVar1, rd16(puVar1) & 0xff | 0x3100);
  wr16(puVar3, rd16(puVar3) & 0xff00 | (unsigned short)param_1);
  wr16(puVar3, rd16(puVar3) & 0xff | 0x500);
  wr16(puVar1 + 0x28, rd16(puVar1 + 0x28) & 0xfff0 | 1);
  wr16(puVar1 + 0x28, rd16(puVar1 + 0x28) & 0xff0f | 0x20);
  wr16(puVar1 + 0x28, rd16(puVar1 + 0x28) & 0xf0ff | 0x100);
  wr16(puVar1 + 0x2c, rd16(puVar1 + 0x2c) & 0xfff0);
  wr16(puVar1 + 0x2c, rd16(puVar1 + 0x2c) & 0xff0f);
  wr16(puVar1 + 0x2c, rd16(puVar1 + 0x2c) & 0xf0ff | 0x100);
  wr16(0x1f002db0, 0xf007);
  wr16(0x1f002db4, rd16(0x1f002db4) | 1);
  uVar2 = FUN_a0001a1c();
  if ((uVar2 & 1) == 0) {
  uart_puts("FSP FAIL Timeout !!!!\r\n");
  }
  return uVar2 & 1;
}

static unsigned int FUN_a0001b24(unsigned short param_1, unsigned int param_2)
{
  unsigned int puVar1;
  unsigned int uVar2;
  wr16(0x1f002fc8, 1);
  puVar1 = 0x1f002d80;
  wr16(puVar1, param_1 | rd16(0x1f002d80) & 0xff00);
  wr16(puVar1 + 0x28, rd16(puVar1 + 0x28) & 0xfff0 | 1);
  wr16(puVar1 + 0x28, rd16(puVar1 + 0x28) & 0xff0f);
  wr16(puVar1 + 0x28, rd16(puVar1 + 0x28) & 0xf0ff);
  wr16(puVar1 + 0x2c, rd16(puVar1 + 0x2c) & 0xfff0 | 1);
  wr16(puVar1 + 0x2c, rd16(puVar1 + 0x2c) & 0xff0f);
  wr16(puVar1 + 0x2c, rd16(puVar1 + 0x2c) & 0xf0ff);
  wr16(0x1f002db0, 7);
  wr16(0x1f002db4, rd16(0x1f002db4) | 1);
  uVar2 = FUN_a0001a1c();
  if ((uVar2 & 1) == 0) {
  uart_puts("FSP FAIL Timeout !!!!\r\n");
  }
  wr16(param_2, rd16(0x1f002d94) & 0xff);
  return uVar2 & 1;
}

static unsigned int FUN_a0001bcc(unsigned short param_1)
{
  unsigned int puVar1;
  unsigned int uVar2;
  unsigned int puVar3;
  wr16(0x1f002fc8, 1);
  puVar1 = 0x1f002d80;
  puVar3 = 0x1f002d80 + 0x4;
  wr16(puVar1, rd16(0x1f002d80) & 0xff00 | 6);
  wr16(puVar1, rd16(puVar1) & 0xff | 0x100);
  wr16(puVar3, param_1 | rd16(puVar3) & 0xff00);
  wr16(puVar3, rd16(puVar3) & 0xff);
  wr16(puVar1 + 0x8, rd16(puVar1 + 0x8) & 0xff00 | 5);
  wr16(puVar1 + 0x28, rd16(puVar1 + 0x28) & 0xfff0 | 1);
  wr16(puVar1 + 0x28, rd16(puVar1 + 0x28) & 0xff0f | 0x30);
  wr16(puVar1 + 0x28, rd16(puVar1 + 0x28) & 0xf0ff | 0x100);
  wr16(puVar1 + 0x2c, rd16(puVar1 + 0x2c) & 0xfff0);
  wr16(puVar1 + 0x2c, rd16(puVar1 + 0x2c) & 0xff0f);
  wr16(puVar1 + 0x2c, rd16(puVar1 + 0x2c) & 0xf0ff | 0x100);
  puVar1 = 0x1f002db0;
  wr16(puVar1, rd16(0x1f002db0) | 1);
  wr16(puVar1, rd16(puVar1) | 2);
  wr16(puVar1, rd16(puVar1) | 4);
  wr16(puVar1, rd16(puVar1) | 0x8000);
  wr16(puVar1, rd16(puVar1) | 0x4000);
  wr16(puVar1, rd16(puVar1) & 0xe7ff | 0x1000);
  wr16(puVar1, rd16(puVar1) | 0x2000);
  wr16(0x1f002db4, rd16(0x1f002db4) | 1);
  uVar2 = FUN_a0001a1c();
  if ((uVar2 & 1) == 0) {
  uart_puts("FSP FAIL Timeout !!!!\r\n");
  }
  return uVar2 & 1;
}

static void FUN_a0001d10()
{
  unsigned int pbVar1;
  unsigned int uVar2;
  uart_puts("SPI ");
  /* uVar2 = "54M" */
  pbVar1 = 0x1f001c81;
  wr8(pbVar1, 0x10);
  wr8(pbVar1, rd8(pbVar1) | 0x40);
  uart_puts("54M");
  uart_puts("\r\n");
  wr16(0x1f001e04, 0x1011);
  return;
}

static unsigned long long FUN_a00046cc(unsigned int param_1, unsigned int param_2)
{
  unsigned int uVar1;
  int iVar2;
  int bVar3;
  int bVar4;
  int bVar5;
  int bVar6;
  int bVar7;
  int bVar8;
  int bVar9;
  int bVar10;
  int bVar11;
  int bVar12;
  int bVar13;
  int bVar14;
  int bVar15;
  int bVar16;
  int bVar17;
  int bVar18;
  int bVar19;
  int bVar20;
  int bVar21;
  int bVar22;
  int bVar23;
  int bVar24;
  int bVar25;
  int bVar26;
  int bVar27;
  int bVar28;
  int bVar29;
  int bVar30;
  int bVar31;
  int bVar32;
  int bVar33;
  unsigned long long uVar34;
  if (param_2 - 1 == 0) {
  return CONCAT44(param_2,param_1);
  }
  if (param_2 == 0) {
  uVar1 = func_0xa00055b4(8);
  return (unsigned long long)uVar1;
  }
  if (param_1 <= param_2) {
  return CONCAT44(param_2,(unsigned int)(param_1 == param_2));
  }
  if ((param_2 & param_2 - 1) == 0) {
  return CONCAT44(param_2,param_1 >> (0x1fU - LZCOUNT(param_2) & 0xff));
  }
  iVar2 = 0x1f - (LZCOUNT(param_2) - LZCOUNT(param_1));
  if (iVar2 == 0) {
  bVar3 = param_2 << 0x1f <= param_1;
  if (bVar3) {
  param_1 = param_1 + param_2 * -0x80000000;
  }
  bVar4 = param_2 << 0x1e <= param_1;
  if (bVar4) {
  param_1 = param_1 + param_2 * -0x40000000;
  }
  bVar5 = param_2 << 0x1d <= param_1;
  if (bVar5) {
  param_1 = param_1 + param_2 * -0x20000000;
  }
  bVar6 = param_2 << 0x1c <= param_1;
  if (bVar6) {
  param_1 = param_1 + param_2 * -0x10000000;
  }
  bVar7 = param_2 << 0x1b <= param_1;
  if (bVar7) {
  param_1 = param_1 + param_2 * -0x8000000;
  }
  bVar8 = param_2 << 0x1a <= param_1;
  if (bVar8) {
  param_1 = param_1 + param_2 * -0x4000000;
  }
  bVar9 = param_2 << 0x19 <= param_1;
  if (bVar9) {
  param_1 = param_1 + param_2 * -0x2000000;
  }
  bVar10 = param_2 << 0x18 <= param_1;
  if (bVar10) {
  param_1 = param_1 + param_2 * -0x1000000;
  }
  bVar11 = param_2 << 0x17 <= param_1;
  if (bVar11) {
  param_1 = param_1 + param_2 * -0x800000;
  }
  bVar12 = param_2 << 0x16 <= param_1;
  if (bVar12) {
  param_1 = param_1 + param_2 * -0x400000;
  }
  bVar13 = param_2 << 0x15 <= param_1;
  if (bVar13) {
  param_1 = param_1 + param_2 * -0x200000;
  }
  bVar14 = param_2 << 0x14 <= param_1;
  if (bVar14) {
  param_1 = param_1 + param_2 * -0x100000;
  }
  bVar15 = param_2 << 0x13 <= param_1;
  if (bVar15) {
  param_1 = param_1 + param_2 * -0x80000;
  }
  bVar16 = param_2 << 0x12 <= param_1;
  if (bVar16) {
  param_1 = param_1 + param_2 * -0x40000;
  }
  bVar17 = param_2 << 0x11 <= param_1;
  if (bVar17) {
  param_1 = param_1 + param_2 * -0x20000;
  }
  bVar18 = param_2 << 0x10 <= param_1;
  if (bVar18) {
  param_1 = param_1 + param_2 * -0x10000;
  }
  bVar19 = param_2 << 0xf <= param_1;
  if (bVar19) {
  param_1 = param_1 + param_2 * -0x8000;
  }
  bVar20 = param_2 << 0xe <= param_1;
  if (bVar20) {
  param_1 = param_1 + param_2 * -0x4000;
  }
  bVar21 = param_2 << 0xd <= param_1;
  if (bVar21) {
  param_1 = param_1 + param_2 * -0x2000;
  }
  bVar22 = param_2 << 0xc <= param_1;
  if (bVar22) {
  param_1 = param_1 + param_2 * -0x1000;
  }
  bVar23 = param_2 << 0xb <= param_1;
  if (bVar23) {
  param_1 = param_1 + param_2 * -0x800;
  }
  bVar24 = param_2 << 10 <= param_1;
  if (bVar24) {
  param_1 = param_1 + param_2 * -0x400;
  }
  bVar25 = param_2 << 9 <= param_1;
  if (bVar25) {
  param_1 = param_1 + param_2 * -0x200;
  }
  bVar26 = param_2 << 8 <= param_1;
  if (bVar26) {
  param_1 = param_1 + param_2 * -0x100;
  }
  bVar27 = param_2 << 7 <= param_1;
  if (bVar27) {
  param_1 = param_1 + param_2 * -0x80;
  }
  bVar28 = param_2 << 6 <= param_1;
  if (bVar28) {
  param_1 = param_1 + param_2 * -0x40;
  }
  bVar29 = param_2 << 5 <= param_1;
  if (bVar29) {
  param_1 = param_1 + param_2 * -0x20;
  }
  bVar30 = param_2 << 4 <= param_1;
  if (bVar30) {
  param_1 = param_1 + param_2 * -0x10;
  }
  bVar31 = param_2 << 3 <= param_1;
  if (bVar31) {
  param_1 = param_1 + param_2 * -8;
  }
  bVar32 = param_2 << 2 <= param_1;
  if (bVar32) {
  param_1 = param_1 + param_2 * -4;
  }
  bVar33 = param_2 << 1 <= param_1;
  if (bVar33) {
  param_1 = param_1 + param_2 * -2;
  }
  return CONCAT44(param_2,(((((((((((((((((((((((((((((((unsigned int)bVar3 * 2 + (unsigned int)bVar4) * 2 +
  (unsigned int)bVar5) * 2 + (unsigned int)bVar6) * 2 +
  (unsigned int)bVar7) * 2 + (unsigned int)bVar8) * 2 +
  (unsigned int)bVar9) * 2 + (unsigned int)bVar10) * 2 +
  (unsigned int)bVar11) * 2 + (unsigned int)bVar12) * 2 +
  (unsigned int)bVar13) * 2 + (unsigned int)bVar14) * 2 + (unsigned int)bVar15
  ) * 2 + (unsigned int)bVar16) * 2 + (unsigned int)bVar17) * 2 +
  (unsigned int)bVar18) * 2 + (unsigned int)bVar19) * 2 + (unsigned int)bVar20) * 2
  + (unsigned int)bVar21) * 2 + (unsigned int)bVar22) * 2 + (unsigned int)bVar23) * 2
  + (unsigned int)bVar24) * 2 + (unsigned int)bVar25) * 2 + (unsigned int)bVar26) * 2 +
  (unsigned int)bVar27) * 2 + (unsigned int)bVar28) * 2 + (unsigned int)bVar29) * 2 +
  (unsigned int)bVar30) * 2 + (unsigned int)bVar31) * 2 + (unsigned int)bVar32) * 2 +
  (unsigned int)bVar33) * 2 + (unsigned int)(param_2 <= param_1));
  }
  /* WARNING: Could not recover jumptable at 0xa0004700. Too many branches */
  /* WARNING: Treating indirect jump as call */
  uVar34 = (*(iVar2 * 0xc + -0x5fffb8f8))();
  return uVar34;
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
    unsigned char bVar1;
    unsigned char bVar2;
    unsigned short uVar3;
    unsigned long long uVar4;
    char cVar5;
    char cVar6;
    unsigned int puVar7;
    unsigned int puVar8;
    unsigned int pbVar9;
    unsigned int pbVar10;
    unsigned int puVar11;
    unsigned int puVar12;
    unsigned int puVar13;
    unsigned int puVar14;
    unsigned int puVar15;
    unsigned int puVar16;
    unsigned int uVar17;
    unsigned int puVar18;
    unsigned int puVar19;
    unsigned int psVar20;
    unsigned int piVar21;
    unsigned char uVar22;
    unsigned short uVar23;
    unsigned int uVar24;
    int iVar25;
    unsigned int puVar26;
    unsigned int uVar27;
    unsigned int uVar28;
    unsigned int uVar29;
    int iVar30;
    unsigned int uVar31;
    unsigned int uVar32;
    int extraout_r1;
    int extraout_r1_00;
    unsigned short uVar33;
    unsigned int uVar34;
    unsigned int puVar35;
    unsigned int puVar36;
    unsigned short uVar37;
    unsigned int pbVar38;
    unsigned int puVar39;
    unsigned int puVar40;
    unsigned int puVar41;
    unsigned int extraout_r3;
    unsigned int pcVar42;
    unsigned short uVar43;
    unsigned int puVar44;
    unsigned int pbVar45;
    unsigned int puVar46;
    unsigned int uVar47;
    unsigned int puVar48;
    int iVar49;
    unsigned int in_cr14;
    unsigned long long uVar50;
    unsigned long long uVar51;
    unsigned int local_84;
    unsigned int local_7c;
    unsigned char local_68;
    unsigned char local_67;
    unsigned short local_66;

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
    uart_puts((const char *)(reset_msg));
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

    /* ---- FUN_a0001d50 tail (a0002e00..) ---- */
    bVar1 = bond;
    if ((rd16(0x1f283e00) & 0x18) != 0) {
    wr16(0x1f283e00, 0xc0);
    puVar14 = 0x1f2842a4;
    puVar8 = 0x1f284200;
    puVar11 = 0x1f283e1c;
    wr8(puVar11, 0x11);
    wr8(puVar11 + 0x1e4, 0xb0);
    delay(0x4b0);
    puVar35 = 0x1f284210;
    puVar11 = 0x1f283e08;
    wr8(puVar11, 0x10);
    wr8(puVar11 + 0x1, 1);
    wr8(puVar35, 0x2f);
    wr8(puVar11 + 0x409, 0xc);
    wr16(puVar35, 0x40f);
    puVar11 = 0x1f284600;
    wr16(puVar8, 0x7f05);
    wr8(puVar14, 0);
    wr8(puVar11, 10);
    wr8(puVar11, 0x28);
    wr16((puVar11 + -0x3bc), 0x2088);
    wr16((puVar11 + -0x3c0), 0x8051);
    wr16((puVar11 + -0x3fc), 0x2084);
    wr16(puVar35, 0x426);
    wr16(puVar8, 0x6bc3);
    wr8(puVar14, 0x3f);
    delay(0x4b0);
    wr16(puVar8, 0x69c3);
    wr8(puVar14, 0x3f);
    delay(0x4b0);
    wr16(puVar8, 1);
    /* uVar32 = "utmi_1_init done\r\n" */
    wr8(puVar14, 0);
    puVar46 = puVar14 + 0x800;
    wr16(puVar8, 0x7f03);
    puVar39 = puVar8 + 0x800;
    uart_puts("utmi_1_init done\r\n");
    puVar11 = 0x1f284a11;
    puVar35 = 0x1f284a10;
    wr8(0x1f284a10u, 0x2f);
    wr8(puVar11, 0xc);
    wr16(puVar35, 0x40f);
    wr16(puVar39, 0x7f05);
    wr8(puVar46, 0);
    wr8(puVar11 + 0x3ef, 10);
    wr8(puVar11 + 0x3ef, 0x28);
    wr16((puVar11 + 0x33), 0x2088);
    wr16((puVar11 + 0x2f), 0x8051);
    wr16((puVar11 + -0xd), 0x2084);
    wr16(puVar35, 0x426);
    wr16(puVar39, 0x6bc3);
    wr8(puVar46, 0x3f);
    delay(0x4b0);
    wr16(puVar39, 0x69c3);
    wr8(puVar46, 0x3f);
    delay(0x4b0);
    wr16(puVar39, 1);
    /* uVar32 = "utmi_2_init done\r\n" */
    wr8(puVar46, 0);
    wr16(puVar39, 0x7f03);
    uart_puts("utmi_2_init done\r\n");
    puVar35 = 0x1f285210;
    wr8(0x1f285210u, 0x2f);
    wr16(0x1f285211, 0xc);
    wr16(puVar35, 0x40f);
    puVar11 = 0x1f286200;
    wr16(puVar8 + 0x1000, 0x7f05);
    wr8(puVar14 + 0x1000, 0);
    wr8(puVar11, 10);
    wr8(puVar11, 0x28);
    wr16((puVar11 + -0xfbc), 0x2088);
    wr16((puVar11 + -0xfc0), 0x8051);
    wr16((puVar11 + -0xffc), 0x2084);
    wr16(puVar35, 0x426);
    wr16(puVar8 + 0x1000, 0x6bc3);
    wr8(puVar14 + 0x1000, 0x3f);
    delay(0x4b0);
    wr16(puVar8 + 0x1000, 0x69c3);
    wr8(puVar14 + 0x1000, 0x3f);
    delay(0x4b0);
    wr16(puVar8 + 0x1000, 1);
    /* uVar32 = "utmi_3_init done\r\n" */
    wr8(puVar14 + 0x1000, 0);
    wr16(puVar8 + 0x1000, 0x7f03);
    uart_puts("utmi_3_init done\r\n");
    /* uVar32 = "usbpll init done......\r\n" */
    }
    uart_puts("usbpll init done......\r\n");
    puVar14 = 0x1f206448;
    puVar11 = 0x1f206584;
    wr8(puVar14, 0x88);
    wr8(puVar14 + 0x1, 0);
    wr8(puVar14 - 0x3, 1);
    wr8(puVar11, 0x37);
    wr8(puVar11 - 0x3, 0x4b);
    wr8(puVar11 - 0x4, 199);
    wr16((puVar11 + -0x44), 0x4bc7);
    wr16((puVar11 + -0x40), 0x37);
    puVar46 = 0x1f207184;
    local_7c = 0x1f002fc8;
    wr16(0x1f206588, 1);
    wr8(puVar14 - 0x3, 0);
    delay(0x4b0);
    uart_puts("cpupll init done\r\n");
    puVar11 = 0x1f20705c;
    wr8(puVar11, 0x18);
    wr8(puVar11 + 0x24, 4);
    wr8(puVar44, 0x30);
    puVar44 = 0x1f2041f0;
    wr8(puVar44, 1);
    wr8(puVar44 + 0x214, 0x84);
    wr8(puVar7, 0);
    wr8(puVar46, 4);
    wr8(puVar46, 0x15);
    puVar44 = 0x1f207180;
    wr8(puVar44, 0x10);
    wr8(puVar44 - 0xb8, 0x10);
    wr8(puVar44 - 0xb8, rd8(puVar44 - 0xb8) | 0x20);
    FUN_a0001d10();
    puVar44 = 0x1f001c80;
    wr8(local_7c, 1);
    /* uVar32 = "clk_init done \r\n" */
    wr8(puVar44, 0x80);
    puVar44 = 0x1f2071c4;
    wr8(puVar44, 3);
    wr8(puVar44 + 0x1, 0);
    wr8(puVar46, 4);
    wr8(puVar46, 0x14);
    uart_puts("clk_init done \r\n");
    uVar24 = pll_read(2);
    uVar34 = pll_read(3);
    uVar27 = pll_read(0xd);
    uVar28 = pll_read(0xe);
    uVar29 = pll_read(0xf);
    /* uVar32 = "P1 USB_rterm trim=0x" */
    if ((uVar24 & 0x10) != 0) {
    wr16(0x1f284250, rd16(0x1f284250) & 0xfe1f | (unsigned short)((uVar24 & 0xf) << 5));
    uart_puts("P1 USB_rterm trim=0x");
    uart_put_hex16(uVar24 & 0xf);
    uart_puts("\n\r");
    /* uVar32 = "P1 USB_HS_TX_CURRENT trim=0x" */
    puVar13 = 0x1f28425c;
    uVar47 = uVar28 & 0xf;
    wr16(puVar13, rd16(0x1f28425c) & 0xfff8 | (unsigned short)(uVar47 >> 1));
    wr16(puVar13 - 0x4, rd16(puVar13 - 0x4) & 0x7fff | (unsigned short)(uVar47 << 0xf));
    uart_puts("P1 USB_HS_TX_CURRENT trim=0x");
    uart_put_hex16(uVar47);
    uart_puts("\n\r");
    }
    /* uVar32 = "P2 USB_rterm trim=0x" */
    if ((uVar28 & 0x1000) != 0) {
    uVar47 = (uVar28 & 0xff) >> 4;
    wr16(0x1f284a50, rd16(0x1f284a50) & 0xfe1f | (unsigned short)(uVar47 << 5));
    uart_puts("P2 USB_rterm trim=0x");
    uart_put_hex16(uVar47);
    uart_puts("\n\r");
    /* uVar32 = "P2 USB_HS_TX_CURRENT trim=0x" */
    puVar13 = 0x1f284a5c;
    uVar47 = (uVar28 & 0xfff) >> 8;
    wr16(puVar13, rd16(0x1f284a5c) & 0xfff8 | (unsigned short)((uVar28 << 0x14) >> 0x1d));
    wr16(puVar13 - 0x4, rd16(puVar13 - 0x4) & 0x7fff | (unsigned short)(uVar47 << 0xf));
    uart_puts("P2 USB_HS_TX_CURRENT trim=0x");
    uart_put_hex16(uVar47);
    uart_puts("\n\r");
    }
    /* uVar17 = "P3 USB_rterm trim=0x" */
    /* uVar32 = "P3 USB_rterm trim=0x" */
    if ((int)(uVar27 << 0x12) < 0) {
    uVar47 = (uVar27 & 0x1ff) >> 5;
    wr16(0x1f285250, rd16(0x1f285250) & 0xfe1f | (unsigned short)(uVar47 << 5));
    uart_puts("P3 USB_rterm trim=0x");
    uart_put_hex16(uVar47);
    uart_puts("\n\r");
    uVar47 = uVar27 << 0x13;
    LAB_a0002e12:
    /* uVar32 = "P3 USB_HS_TX_CURRENT trim=0x" */
    puVar13 = 0x1f28525c;
    wr16(puVar13, rd16(0x1f28525c) & 0xfff8 | (unsigned short)(uVar47 >> 0x1d));
    wr16(puVar13 - 0x4, rd16(puVar13 - 0x4) & 0x7fff | (unsigned short)((uVar47 >> 0x1c) << 0xf));
    uart_puts("P3 USB_HS_TX_CURRENT trim=0x");
    uart_put_hex16(uVar47 >> 0x1c);
    uart_puts("\n\r");
    }
    else if ((uVar28 & 0x1000) != 0) {
    uVar47 = (uVar28 & 0xff) >> 4;
    wr16(0x1f285250, rd16(0x1f285250) & 0xfe1f | (unsigned short)(uVar47 << 5));
    uart_puts("P3 USB_rterm trim=0x");
    uart_put_hex16(uVar47);
    uart_puts("\n\r");
    uVar47 = uVar28 << 0x14;
    goto LAB_a0002e12;
    }
    /* uVar32 = "PM_vol_bgap trim=0x" */
    if ((uVar24 & 0x4000000) != 0) {
    uVar47 = (uVar24 & 0xffff) >> 10;
    wr16(0x1f001d90, rd16(0x1f001d90) & 0xff80 | (unsigned short)(uVar47 << 1) | 1);
    uart_puts("PM_vol_bgap trim=0x");
    uart_put_hex16(uVar47);
    uart_puts("\n\r");
    uart_puts("GCR_SAR_DATA trim=0x");
    uart_put_hex16((uVar24 & 0x3ffffff) >> 0x10);
    uart_puts("\n\r");
    }
    puVar16 = 0x1f006708;
    /* uVar32 = "ETH 10T output swing trim=0x" */
    puVar15 = 0x1f0066d0;
    puVar13 = 0x1f0066c0;
    if ((uVar34 & 0x100000) != 0) {
    wr16(puVar13, rd16(0x1f0066c0) | 4);
    wr16(puVar15, rd16(puVar15) | 0x8000);
    wr16(puVar16, rd16(puVar16) & 0xffe0 | (unsigned short)(uVar34 & 0x1f));
    uart_puts("ETH 10T output swing trim=0x");
    uart_put_hex16(uVar34 & 0x1f);
    uart_puts("\n\r");
    /* uVar32 = "ETH 100T output swing trim=0x" */
    uVar24 = (uVar34 & 0x3ff) >> 5;
    wr16(puVar16, rd16(puVar16) & 0xe0ff | (unsigned short)(uVar24 << 8));
    uart_puts("ETH 100T output swing trim=0x");
    uart_put_hex16(uVar24);
    uart_puts("\n\r");
    /* uVar32 = "ETH RX input impedance trim=0x" */
    uVar24 = (uVar34 & 0x3fff) >> 10;
    wr16(puVar13, rd16(puVar13) & 0xf87f | (unsigned short)(uVar24 << 7));
    uart_puts("ETH RX input impedance trim=0x");
    uart_put_hex16(uVar24);
    uart_puts("\n\r");
    /* uVar32 = "ETH TX output impedance trim=0x" */
    wr16(0x1f006700, rd16(0x1f006700) & 0xfff0 | (unsigned short)((uVar34 & 0xfffff) >> 0x10));
    uart_puts("ETH TX output impedance trim=0x");
    uart_put_hex16((uVar34 & 0xfffff) >> 0x10);
    uart_puts("\n\r");
    wr16(0x1f0062b8, rd16(0x1f0062b8) | 1);
    }
    /* uVar32 = "MIPI_HS_RTERM trim=0x" */
    puVar13 = 0x1f2a5114;
    iVar25 = uVar28 << 0x10;
    if (iVar25 < 0) {
    uVar37 = (unsigned short)uVar34 >> 0xe;
    wr16(puVar13, rd16(0x1f2a5114) & 0xfffc | uVar37);
    wr16(puVar13 + 0x4, rd16(puVar13 + 0x4) & 0xfffc | uVar37);
    wr16(puVar13 + 0x8, rd16(puVar13 + 0x8) & 0xfffc | uVar37);
    wr16(puVar13 + 0xc, rd16(puVar13 + 0xc) & 0xfffc | uVar37);
    wr16(puVar13 + 0x10, uVar37 | rd16(puVar13 + 0x10) & 0xfffc);
    uart_puts("MIPI_HS_RTERM trim=0x");
    uart_put_hex16((uVar34 & 0xffff) >> 0xe);
    uart_puts("\n\r");
    /* uVar32 = "MIPI_LP_RTERM trim=0x" */
    uVar24 = (uVar28 & 0x7fff) >> 0xd;
    wr16(0x1f2a5050, rd16(0x1f2a5050) & 0xe7ff | (unsigned short)(uVar24 << 0xb));
    uart_puts("MIPI_LP_RTERM trim=0x");
    uart_put_hex16(uVar24);
    iVar25 = uart_puts((const char *)(0xa0004da2u));
    }
    /* uVar32 = "TX_current trimming trim[0x1A]=0x" */
    if ((uVar29 & 0x80) != 0) {
    wr16(0x1f224e68, rd16(0x1f224e68) & 0xff80 | (unsigned short)(uVar29 & 0x7f));
    uart_puts("TX_current trimming trim[0x1A]=0x");
    uart_put_hex16(uVar29 & 0x7f);
    iVar25 = uart_puts((const char *)(0xa0004da2u));
    }
    /* uVar32 = "TX_current trimming trim[0x1B]=0x" */
    if ((uVar29 & 0x8000) != 0) {
    wr16(0x1f224e6c, rd16(0x1f224e6c) & 0xff80 | (unsigned short)((uVar29 & 0x7fff) >> 8));
    uart_puts("TX_current trimming trim[0x1B]=0x");
    uart_put_hex16((uVar29 & 0x7fff) >> 8);
    iVar25 = uart_puts((const char *)(0xa0004da2u));
    }
    /* uVar32 = "TX_current trimming trim[0x1C]=0x" */
    if ((uVar29 & 0x800000) != 0) {
    wr16(0x1f224e70, rd16(0x1f224e70) & 0xff80 | (unsigned short)((uVar29 & 0x7fffff) >> 0x10));
    uart_puts("TX_current trimming trim[0x1C]=0x");
    uart_put_hex16((uVar29 & 0x7fffff) >> 0x10);
    iVar25 = uart_puts((const char *)(0xa0004da2u));
    }
    /* uVar32 = "HDMI2TX Rterm trim=0x" */
    puVar13 = 0x1f224c64;
    if ((int)(uVar28 << 5) < 0) {
    wr16(0x1f224ce0, rd16(0x1f224ce0) & 0xffe0 | (unsigned short)((uVar28 & 0x1fffff) >> 0x10));
    uart_puts("HDMI2TX Rterm trim=0x");
    uart_put_hex16((uVar28 & 0x1fffff) >> 0x10);
    uart_puts("\n\r");
    /* uVar32 = "HDMI2TX_Ibias_CH0 trim=0x" */
    wr16(puVar13, rd16(puVar13) & 0xffe0 | (unsigned short)((uVar28 & 0x3ffffff) >> 0x15));
    uart_puts("HDMI2TX_Ibias_CH0 trim=0x");
    uart_put_hex16((uVar28 & 0x3ffffff) >> 0x15);
    uart_puts("\n\r");
    /* uVar32 = "HDMI2TX_Ibias_CH1 trim=0x" */
    uVar24 = (uVar27 & 0x1fffff) >> 0x10;
    wr16(puVar13, rd16(puVar13) & 0xe0ff | (unsigned short)((uVar27 & 0x1f) << 8));
    uart_puts("HDMI2TX_Ibias_CH1 trim=0x");
    puVar13 = 0x1f224c68;
    uart_put_hex16(uVar27 & 0x1f);
    uart_puts("\n\r");
    /* uVar32 = "HDMI2TX_Ibias_CH2 trim=0x" */
    wr16(puVar13, rd16(puVar13) & 0xffe0 | (unsigned short)((uVar27 & 0x1fff) >> 8));
    uart_puts("HDMI2TX_Ibias_CH2 trim=0x");
    uart_put_hex16((uVar27 & 0x1fff) >> 8);
    uart_puts("\n\r");
    /* uVar32 = "HDMI2TX_Ibias_CH3 trim=0x" */
    wr16(puVar13, rd16(puVar13) & 0xe0ff | (unsigned short)(uVar24 << 8));
    uart_puts("HDMI2TX_Ibias_CH3 trim=0x");
    uart_put_hex16(uVar24);
    iVar25 = uart_puts((const char *)(0xa0004da2u));
    }
    puVar13 = 0x1f004014;
    wr16(puVar36, rd16(puVar36) & 0xfeff);
    uVar51 = clk_check(iVar25,rd16(puVar13) & 7);
    iVar25 = (int)((unsigned long long)uVar51 >> 0x20);
    if ((int)uVar51 == 0) {
    if ((iVar25 == 2) || (iVar25 - 5U < 2)) {
    do {
    /* WARNING: Do nothing block with infinite loop */
    } while( true );
    }
    }
    else if ((iVar25 == 3) || (iVar25 - 5U < 2)) {
    do {
    /* WARNING: Do nothing block with infinite loop */
    } while( true );
    }
    wr16(puVar36, rd16(puVar36) & 0xfeff);
    if ((rd16(puVar13) & 0x7f) >> 5 == 1) {
    uart_puts("Not China \n\r");
    do {
    /* WARNING: Do nothing block with infinite loop */
    } while( true );
    }
    wr16(puVar36, rd16(puVar36) & 0xfeff);
    uVar24 = (rd16(puVar13) & 0x7f) >> 5;
    if ((uVar24 != 0) && (uVar24 != 3)) goto LAB_a00034fe;
    uVar51 = clk_check();
    puVar44 = 0x1f226640;
    uVar24 = (unsigned int)((unsigned long long)uVar51 >> 0x20) & 7;
    if ((int)uVar51 == 0) {
    if (uVar24 != 4) goto LAB_a00034fe;
    uVar37 = rd16(0x1f226e40) | 3;
    puVar36 = 0x1f226e40;
    }
    else {
    if (uVar24 == 1) {
    wr16(0x1f226640, 0);
    wr16(0x1f226644, rd16(0x1f226644) | 8);
    goto LAB_a00034fe;
    }
    if (uVar24 == 2) {
    wr16(0x1f2a2a94, 0xffff);
    goto LAB_a00034fe;
    }
    if (uVar24 != 4) goto LAB_a00034fe;
    wr8(puVar44, 0);
    wr16((puVar44 + 0x7c454), 0xffff);
    puVar36 = 0x1f2a4a00;
    wr16(puVar36, rd16(0x1f2a4a00) & 0xefff);
    wr16(puVar36 + 0x540, rd16(puVar36 + 0x540) | 1);
    uVar37 = rd16(puVar36 + 0x230) | 0x100;
    puVar36 = puVar36 + 0x230;
    }
    wr16(puVar36, uVar37);
    LAB_a00034fe:
    puVar8 = 0x1f20248c;
    puVar35 = 0x1f20248c + 0x-240;
    wr16(puVar8, 0);
    wr16(puVar8 + 0x40, 0);
    wr16(puVar8 + 0x80, 0);
    wr16(puVar8 + 0xc0, 0);
    wr16(puVar35, 0);
    wr16(puVar8 - 0xc0, 0);
    wr16(0x1f20243c, 0x8c08);
    wr16(0x20000000, 0x11111111);
    wr16(0x22000000, 0x33333333);
    wr16(0x24000000, 0x22222222);
    wr16(0x28000000, 0x44444444);
    wr16(0x30000000, 0x88888888);
    uart_puts("512MB\n\r");
    wr16(0x1f2025a4, (unsigned short)(((unsigned int)rd16(0x1f2025a4) << 0x14) >> 0x14) | 0x9000);
    /* uVar32 = "BIST0_" */
    puVar15 = 0x1f2025e0;
    puVar13 = 0x1f2025c0;
    puVar36 = 0x1f2025bc;
    wr16(puVar36, rd16(0x1f2025bc) | 1);
    wr16(puVar36, rd16(puVar36) & 0xfffe);
    puVar8 = 0x1f2025c4;
    wr16(puVar13, 0);
    wr16(puVar8, 0);
    wr16(puVar8 + 0x4, 0xffff);
    wr16(puVar8 + 0x8, 0x1fe);
    wr16(0x1f2025d0, 0x5aa5);
    uart_puts("BIST0_");
    wr16(puVar15, 0);
    uart_put_hex16(1);
    wr16(puVar13, 1);
    do {
    uVar37 = rd16(puVar13);
    } while (-1 < (int)((unsigned int)uVar37 << 0x10));
    local_84 = 0;
    /* uVar32 = "-FAIL\n\r" */
    if ((uVar37 & 0x6000) != 0) goto LAB_a0003b2a;
    uart_puts("-OK\n\r");
    wr16(puVar15, uVar37 & 0x6000);
    wr16(puVar13, uVar37 & 0x6000);
    FUN_a000014c();
    uart_puts("Enable MMU and CACHE\r\n");
    boot_record(499, (const char *)(0xa0004e04u));
    uVar24 = (unsigned int)rd16(0x1f0071c0);
    if (rd16(0xa000000b) == -6) goto LAB_a0003b32;
    uVar37 = rd16(0x1f0071c0) & 1;
    LAB_a0003616:
    pcVar42 = 0xa0005565;
    if ((uVar24 & 0x24) == 0x20) {
    uart_puts("Load IPL_CUST from NOR\r\n");
    puVar15 = 0x1f002db4;
    psVar20 = 0xa000c000;
    pcVar42 = 0xa000c00c;
    puVar13 = 0x1f002dac;
    puVar36 = 0x1f002da8;
    if (*(int *)(rd16(0x14000008) + 0x14000104) == 0x4e4c5049u) {
    iVar25 = rd16(0x14000008) + 0x100;
    }
    else {
    iVar25 = 0x10000;
    }
    uart_puts("offset:");
    uart_put_hex32(iVar25);
    uart_puts("\n\r");
    puVar16 = 0x1f002d80;
    local_84 = iVar25 + 0x14000000;
    uVar43 = *(unsigned short *)(0x14000008 + iVar25);
    wr8(pcVar42, '\0');
    puVar19 = 0x1f002db0;
    rd16(psVar20) = uVar50;
    *(psVar20 + 2) = uVar50;
    wr8(local_7c, 1);
    wr16(puVar16, rd16(puVar16) & 0xff00 | 0x9f);
    wr16(puVar36, rd16(puVar36) & 0xfff0 | 1);
    wr16(puVar36, rd16(puVar36) & 0xff0f);
    wr16(puVar36, rd16(puVar36) & 0xf0ff);
    wr16(puVar13, rd16(puVar13) & 0xfff0 | 3);
    wr16(puVar13, rd16(puVar13) & 0xff0f);
    wr16(puVar13, rd16(puVar13) & 0xf0ff);
    wr16(puVar19, rd16(puVar19) | 1);
    wr16(puVar19, rd16(puVar19) | 2);
    wr16(puVar19, rd16(puVar19) | 4);
    wr16(puVar19, (unsigned short)(((unsigned int)rd16(puVar19) << 0x11) >> 0x11));
    wr16(puVar19, rd16(puVar19) & 0xbfff);
    wr16(puVar19, rd16(puVar19) & 0xdfff);
    wr16(puVar15, rd16(puVar15) | 1);
    uVar24 = FUN_a0001a1c();
    if ((uVar24 & 1) == 0) {
    uart_puts("FSP FAIL Timeout !!!!\r\n");
    }
    puVar16 = 0x1f002d94;
    if ((uVar24 & 1) == 0) {
    LAB_a000399a:
    if (rd8(pcVar42) != '\0') goto LAB_a0003b66;
    uart_puts("Unable to detect NOR\r\n");
    }
    else {
    cVar5 = (char)rd16(0x1f002d94);
    cVar6 = (char)rd16(0x1f002d98);
    if (((cVar5 != -0x38) || ((int)(unsigned int)rd16(0x1f002d94) >> 8 != 0x40)) || (cVar6 != '\x18')) {
    uVar33 = rd16(0x1f002d94) >> 8;
    iVar49 = 0;
    for (iVar25 = 0xa0005248u; *(char *)(iVar25 + 2) != '\0'; iVar25 = iVar25 + 0xc) {
    if (((*(char *)(iVar25 + 2) == cVar5) && (*(unsigned char *)(iVar25 + 3) == uVar33)) &&
    (*(char *)(iVar25 + 4) == cVar6)) {
    puVar48 = (iVar49 * 0xc + 0xa0005248u);
    wr32(psVar20, rd32(puVar48));
    wr32((psVar20 + 2), rd32(puVar48 + 0x4));
    wr32((psVar20 + 4), rd32(puVar48 + 0x8));
    uart_puts("Flash:");
    uart_put_hex8(cVar5);
    uart_put_hex8(uVar33);
    uart_put_hex8(cVar6);
    uart_puts("\r\n");
    puVar18 = 0x1f002d80;
    if ((rd16(psVar20) == 0x506) || (rd16(psVar20) == 0x508)) {
    wr8(local_7c, 1);
    wr16(puVar18, rd16(puVar18) & 0xff00 | 0x90);
    wr16(puVar18, rd16(puVar18) & 0xff);
    puVar18 = 0x1f002d84;
    wr16(puVar18, rd16(0x1f002d84) & 0xff00);
    wr16(puVar18, rd16(puVar18) & 0xff);
    wr16(puVar36, rd16(puVar36) & 0xfff0 | 4);
    wr16(puVar36, rd16(puVar36) & 0xff0f);
    wr16(puVar36, rd16(puVar36) & 0xf0ff);
    wr16(puVar13, rd16(puVar13) & 0xfff0 | 2);
    wr16(puVar13, rd16(puVar13) & 0xff0f);
    wr16(puVar13, rd16(puVar13) & 0xf0ff);
    wr16(puVar19, rd16(puVar19) | 1);
    wr16(puVar19, rd16(puVar19) | 2);
    wr16(puVar19, rd16(puVar19) | 4);
    wr16(puVar19, (unsigned short)(((unsigned int)rd16(puVar19) << 0x11) >> 0x11));
    wr16(puVar19, rd16(puVar19) & 0xbfff);
    wr16(puVar19, rd16(puVar19) & 0xdfff);
    wr16(puVar15, rd16(puVar15) | 1);
    iVar25 = FUN_a0001a1c();
    if (iVar25 == 0) {
    uart_puts("FSP FAIL Timeout !!!!\r\n");
    }
    uVar33 = rd16(puVar16);
    uVar3 = rd16(puVar16);
    uart_puts("MXIC:");
    cVar5 = (char)uVar33;
    uVar24 = (unsigned int)(uVar3 >> 8);
    uart_put_hex8(cVar5);
    uart_put_hex8(uVar24);
    uart_puts("\r\n");
    if (cVar5 == -0x3e) {
    if (uVar24 - 0x16 < 2) {
    wr16(psVar20 + 0x6, 6);
    wr16(psVar20 + 0x8, 4);
    }
    }
    }
    wr8(pcVar42, '\x01');
    break;
    }
    iVar49 = iVar49 + 1;
    }
    goto LAB_a000399a;
    }
    wr8((psVar20 + 2), 0x18);
    wr16(psVar20, 0xb05);
    wr16(psVar20 + 0x2, 0x40c8);
    wr16(psVar20 + 0x6, 3);
    wr16(psVar20 + 0xa, 0x101);
    wr8(pcVar42, '\x01');
    LAB_a0003b66:
    cVar5 = (char)rd16(psVar20 + 0x2);
    if (cVar5 == 'h') {
    if (rd8(pcVar42) != '\0') {
    FUN_a0001bcc(0);
    uart_puts("QE=0\r\n");
    }
    uVar22 = 0;
    }
    else {
    if (rd8(pcVar42) != '\0') {
    if ((cVar5 == -0x38) || (cVar5 == -0x11)) {
    FUN_a0001b24(0x35, local_64);
    local_64[0] = local_64[0] | 2;
    FUN_a0001a58(0);
    /* uVar32 = "GD QE=1\r\n" */
    }
    else if (cVar5 == '\v') {
    FUN_a0001b24(0x35, local_64);
    bVar1 = local_64[0];
    puVar16 = 0x1f002d80;
    local_64[0] = local_64[0] | 2;
    wr8(local_7c, 1);
    wr16(puVar16, rd16(puVar16) & 0xff00 | 6);
    wr16(puVar16, rd16(puVar16) & 0xff | 0x100);
    wr16(puVar16 + 0x4, rd16(puVar16 + 0x4) & 0xff00 | 0x40);
    puVar18 = 0x1f002d88;
    wr16(puVar16 + 0x4, CONCAT11(bVar1,(char)rd16(puVar16 + 0x4)) | 0x200);
    wr16(puVar18, rd16(puVar18) & 0xff00 | 5);
    wr16(puVar36, rd16(puVar36) & 0xfff0 | 1);
    wr16(puVar36, rd16(puVar36) & 0xff0f | 0x30);
    wr16(puVar36, rd16(puVar36) & 0xf0ff | 0x100);
    wr16(puVar13, rd16(puVar13) & 0xfff0);
    wr16(puVar13, rd16(puVar13) & 0xff0f);
    wr16(puVar13, rd16(puVar13) & 0xf0ff | 0x100);
    wr16(puVar19, rd16(puVar19) | 1);
    wr16(puVar19, rd16(puVar19) | 2);
    wr16(puVar19, rd16(puVar19) | 4);
    wr16(puVar19, rd16(puVar19) | 0x8000);
    wr16(puVar19, rd16(puVar19) | 0x4000);
    wr16(puVar19, rd16(puVar19) & 0xe7ff | 0x1000);
    wr16(puVar19, rd16(puVar19) | 0x2000);
    wr16(puVar15, rd16(puVar15) | 1);
    iVar25 = FUN_a0001a1c();
    /* uVar32 = "XT QE=1\r\n" */
    if (-1 < iVar25 << 0x1f) {
    uart_puts("FSP FAIL Timeout !!!!\r\n");
    /* uVar32 = "XT QE=1\r\n" */
    }
    }
    else if (cVar5 == ' ') {
    if (*(char *)((int)psVar20 + 3) == 'p') {
    FUN_a0001b24(0x3a, local_64);
    FUN_a0001bcc(0x40);
    FUN_a0001b24(4, local_64);
    /* uVar32 = "XMC OTP QE = 1\r\n" */
    }
    else {
    FUN_a0001bcc(0x40);
    /* uVar32 = "XMC QE=1\r\n" */
    }
    }
    else if (cVar5 == '^') {
    FUN_a0001b24(0x35, local_64);
    local_64[0] = local_64[0] | 2;
    FUN_a0001a58(0);
    /* uVar32 = "ZB QE=1\r\n" */
    }
    else {
    FUN_a0001bcc(0x40);
    /* uVar32 = "QE=1\r\n" */
    }
    uart_puts("QE=1\r\n");
    }
    uVar22 = 10;
    }
    wr8(local_7c, uVar22);
    }
    puVar36 = 0x1f200404;
    uVar24 = uVar43 + 0x20f & 0xfffffff0;
    iVar25 = coprocessor_movefromRt(0xf,0,in_cr14);
    coprocessor_movefromRt2(0xf,0,in_cr14);
    /* uVar32 = "MDrv_BDMA_FlashToMem error!! address or length should be 16 bytes aligned!!\r\n" */
    if ((local_84 & 0xf) == 0) {
    iVar49 = 5000;
    do {
    uVar43 = rd16(puVar36);
    uVar33 = uVar43 & 2;
    if ((uVar43 & 2) == 0) {
    wr16(puVar36, 0x1c);
    puVar8 = 0x1f200408;
    wr16(puVar8, 0x4035);
    wr16(puVar8 + 0x4, uVar33);
    wr16(puVar8 + 0x8, (unsigned short)local_84);
    wr16(0x1f200414, (short)(local_84 >> 0x10));
    puVar36 = 0x1f200418;
    wr16(puVar36, uVar33);
    wr16(puVar36 + 0x4, 0x3c0);
    puVar8 = 0x1f200420;
    wr16(puVar8, (short)uVar24);
    wr16(puVar8 + 0x4, (short)(uVar24 >> 0x10));
    wr16(0x1f200400, 1);
    goto LAB_a00039c8;
    }
    delay(12000);
    iVar49 = iVar49 + -1;
    /* uVar32 = "MDrv_BDMA_FLASH_TO_MEM error!! device is busy!!" */
    } while (iVar49 != 0);
    }
    uart_puts("MDrv_BDMA_FLASH_TO_MEM error!! device is busy!!");
    LAB_a00039c8:
    do {
    } while (-1 < (int)((unsigned int)rd16(0x1f200404) << 0x1c));
    wr16(0x1f200404, rd16(0x1f200404) | 8);
    iVar49 = coprocessor_movefromRt(0xf,0,in_cr14);
    coprocessor_movefromRt2(0xf,0,in_cr14);
    uVar4 = (unsigned long long)0xaaaaaaabu;
    uart_puts("Load time ");
    uVar34 = (unsigned int)((unsigned int)(iVar49 - iVar25) * uVar4 >> 0x22);
    FUN_a0001824(uVar34, 0, 0, 0);
    uart_puts(" us, ");
    FUN_a00046cc(uVar24 * 1000, uVar34);
    FUN_a0001824(0, 0, 0, 0);
    uart_puts(" KiB/s\r\n");
    LAB_a0003a0c:
    FUN_a000036c(0, 0, 0, 0);
    FUN_a000165c();
    puVar13 = 0x23c00008;
    puVar36 = 0x23c00010;
    if ((rd16(0x23c00004) == 0x434c5049u) ||
    (uVar32 = 0xa00050d9u, rd16(0x23c00004) == (int)0x23c00004u + 0x2a8c5045)) {
    if (uVar37 == 0) {
    LAB_a0004590:
    boot_record(0x206, (const char *)(0xa00051b7u));
    boot_record(0x1d7, (const char *)(0xa00051c0u));
    uVar37 = rd16(0x23c00008);
    uVar43 = 0x23c00008u[2];
    uVar34 = 0;
    for (uVar24 = 0; uVar24 < uVar37 - 0x10; uVar24 = uVar24 + 4) {
    uVar34 = uVar34 + *(int *)(0x23c00010 + uVar24);
    }
    if ((unsigned int)uVar43 == (uVar34 & 0xffff)) {
    uart_puts("Checksum OK\n\r");
    boot_record(0x1ea, (const char *)(0xa00051d3u));
    boot_record(0x209, (const char *)(0xa00051d8u));
    (*&SUB_23c00000)();
    return;
    }
    uart_puts("Checksum NG\r\n");
    uart_put_hex16((unsigned int)uVar43);
    uart_puts("\r\n");
    uVar24 = 0;
    for (uVar34 = 0; uVar34 < uVar37 - 0x10; uVar34 = uVar34 + 4) {
    uVar24 = uVar24 + *(int *)(0x23c00010 + uVar34);
    }
    uart_put_hex16(uVar24 & 0xffff);
    /* uVar32 = "\r\n" */
    }
    else {
    /* uVar32 = " -Not IPL_CUSTK header!\n\r" */
    if (rd16(0x23c00004) == 0x4e4c5049u) {
    puVar44 = &SUB_23c00000 +
    (((unsigned int)rd16(0x23c00008) - (unsigned int)rd16(0x23c00012)) - (unsigned int)rd16(0x23c00010));
    uVar24 = (unsigned int)rd16(0x23c00008);
    uart_puts("CUST Key\r\n");
    uart_puts("KEYN(0x");
    uart_put_hex32(puVar44);
    uart_puts(")\n\r");
    uart_puts("KEYN_SIZE(0x");
    uart_put_hex16(rd16(puVar36));
    uart_puts(")\n\r");
    FUN_a000036c(0, 0, 0, 0);
    FUN_a000165c();
    uVar37 = rd16(puVar13);
    uart_puts("KEYN_SIG_ADDR(0x");
    uart_put_hex32(uVar37 + 0x23c00100);
    uart_puts(")\n\r");
    iVar25 = FUN_a00010c2(puVar44, 0x100, uVar37 + 0x23c00100, 0);
    /* uVar32 = " -Authenticate Key failed!\r\n" */
    if (iVar25 != 0) {
    FUN_a000036c(0, 0, 0, 0);
    FUN_a000165c();
    uart_puts("IMG(0x");
    uart_put_hex32(&SUB_23c00000);
    uart_puts(")\n\r");
    uart_puts("IMG_SIZE(0x");
    uart_put_hex32(uVar24);
    uart_puts(")\n\r");
    uart_puts("SIG(0x");
    uart_put_hex32(&SUB_23c00000 + uVar24);
    uart_puts(")\n\r");
    iVar25 = FUN_a00010c2(&SUB_23c00000, uVar24, &SUB_23c00000 + uVar24, puVar44);
    /* uVar32 = " -Authenticate image failed!\r\n" */
    if (iVar25 != 0) {
    uart_puts(" -Authenticate image passed!\r\n");
    wr16(0x1f002410, rd16(0x1f002410) | 4);
    goto LAB_a0004590;
    }
    }
    }
    }
    }
    }
    else {
    if ((uVar24 & 0x24) != 4) goto LAB_a0003a0c;
    uart_puts("Load IPL_CUST from SPINAND\r\n");
    piVar21 = 0xa000c010;
    pbVar38 = 0xa000c014;
    if (rd8(pcVar42) == '\0') {
    LAB_a0003f5a:
    pcVar42 = rd32(piVar21);
    if ((*(int *)(pbVar38 + 4) == 0x21aa) && (rd8(pbVar38) == 0xef)) {
    local_68 = 0;
    local_66 = 0;
    iVar49 = 2;
    local_64[0] = 0;
    local_64[1] = 0;
    FUN_a0001700(&local_66, local_64, &local_68);
    /* uVar17 = "jump to next partition block: " */
    iVar25 = 0xa000a010u;
    /* uVar32 = "\r\n" */
    bVar1 = (unsigned char)local_66;
    do {
    FUN_a0001574(1, 0, 0, 0);
    FUN_a00015ac(0, 0);
    uVar43 = *(unsigned short *)(iVar25 + 0x14);
    if (pcVar42 != 0x0) {
    wr8(pcVar42, '\x01');
    }
    local_67 = 0;
    if ((pcVar42 != 0x0) && (rd8(pcVar42) != '\0')) {
    iVar30 = (unsigned int)uVar43 * (unsigned int)bVar1;
    wr8(pcVar42, '\0');
    uVar24 = FUN_a0001870(iVar30);
    if (uVar24 == 0) {
    uVar24 = FUN_a00018ec(&local_67, 0, 0);
    if (uVar24 == 0) goto LAB_a0003fbe;
    uart_puts("Cache Read: ChkDone Page");
    uart_put_hex32(iVar30);
    uart_puts(" err: ");
    uart_put_hex8(uVar24 & 0xff);
    uart_puts("\r\n");
    FUN_a0001824(0x11d, 0, 0, 0);
    uart_puts("\r\n");
    /* uVar32 = "[I]d3\n" */
    }
    else {
    uart_puts("Cache Read: Page");
    uart_put_hex32(iVar30);
    uart_puts(" err: ");
    uart_put_hex8(uVar24 & 0xff);
    uart_puts("\r\n");
    FUN_a0001824(0x10f, 0, 0, 0);
    uart_puts("\r\n");
    /* uVar32 = "[I]d2\n" */
    }
    uart_puts("[I]d2\n");
    uart_puts(" MDrv_SPINAND_EnterRead error\r\n ");
    FUN_a0001824(0x2a1, 0, 0, 0);
    uart_puts("\r\n");
    /* uVar31 = "[I]d7\n" */
    goto LAB_a00040c4;
    }
    LAB_a0003fbe:
    FUN_a000128c(0, 0xf000, &SUB_23c00000, 1);
    iVar30 = FUN_a00015ac(0, 0);
    if (iVar30 == 0) {
    FUN_a0001574(0, 0, 0, 0);
    goto LAB_a0003a0c;
    }
    /* uVar31 = "ERR_SPINAND(no backup block!)\r\n: " */
    if (CONCAT11(local_64[1],local_64[0]) == 0) goto LAB_a00040c4;
    uart_puts("jump to next partition block: ");
    uart_put_hex8(local_64[0]);
    uart_puts("[I]d2\n");
    if (iVar49 == 1) goto code_r0xa0004000;
    iVar49 = 1;
    bVar1 = local_64[0];
    } while( true );
    }
    local_66 = 0;
    uVar24 = 0;
    local_64[0] = 0;
    local_64[1] = 0;
    local_68 = 0;
    uVar43 = *(unsigned short *)(0xa000a010u + 0x14);
    uVar51 = FUN_a00015e8(*(unsigned short *)(0xa000a010u + 0x18));
    iVar25 = 0xa000a010u;
    iVar49 = (int)((unsigned long long)uVar51 >> 0x20);
    uVar28 = (unsigned int)*(unsigned short *)(iVar49 + 0x12);
    *(char *)(iVar49 + 0x32) = (char)uVar51;
    uVar34 = (int)uVar28 >> ((unsigned int)uVar51 & 0xff);
    *(short *)(iVar49 + 0x34) = (short)uVar34;
    uVar22 = FUN_a00015e8(uVar34 & 0xffff);
    wr8((extraout_r1 + 0x33), uVar22);
    uVar23 = FUN_a00015e8((unsigned int)uVar43);
    wr16((extraout_r1_00 + 0x36), uVar23);
    uVar51 = FUN_a00015e8(uVar28);
    uVar34 = uVar43 - 1;
    *(short *)((int)((unsigned long long)uVar51 >> 0x20) + 0x38) = (short)uVar51;
    uVar27 = 0xf000U >> ((unsigned int)uVar51 & 0xff) & 0xffff;
    if ((uVar28 - 1 & 0xf000) != 0) {
    uVar27 = uVar27 + 1 & 0xffff;
    }
    FUN_a0001700(&local_66, local_64, &local_68);
    uVar43 = local_66;
    LAB_a00041fa:
    uVar29 = (unsigned int)uVar43;
    local_84 = (unsigned int)local_68;
    for (uVar28 = 0; uVar28 < uVar27; uVar28 = uVar28 + 1) {
    LAB_a000435e:
    if (pcVar42 != 0x0) {
    uVar24 = uVar34 & uVar28;
    if (uVar24 == 0) {
    wr8(pcVar42, '\x01');
    }
    if (uVar34 == uVar24) {
    wr8(pcVar42 + 0x1, '\x01');
    }
    if (uVar27 <= uVar28 + 1) {
    wr8(pcVar42 + 0x2, '\x01');
    }
    }
    local_7c = (unsigned int)*(unsigned short *)(iVar25 + 0x38);
    uVar47 = (uVar29 << (*(unsigned short *)(iVar25 + 0x36) & 0xff)) + uVar28;
    if ((pcVar42 == 0x0) || (rd8(pcVar42 + 0x3) == '\0')) {
    iVar49 = FUN_a0001398(uVar47);
    if (iVar49 == 0) {
    uart_puts(" HAL_SPINAND_RFC done flag = False\r\n ");
    FUN_a0001824(0x185, 0, 0, 0);
    uart_puts("\r\n");
    uart_puts("[I]d8\n");
    }
    uVar24 = FUN_a00015ac(0, 0);
    if (uVar24 != 0) {
    uart_puts(" _MDrv_SPINAND_CHECK_ECCSTATUS error\r\n ");
    uVar32 = 0x18e;
    goto LAB_a00043e4;
    }
    LAB_a0004240:
    FUN_a000036c(0, 0, 0, 0);
    if (*(char *)(iVar25 + 0x1a) == '\0') {
    uVar43 = 0;
    }
    else {
    uVar24 = FUN_a00015e8(*(unsigned short *)(iVar25 + 0x14));
    uVar43 = 0;
    if ((uVar47 >> (uVar24 & 0xff) & 1) != 0) {
    uVar43 = 0x1000;
    wr16(0x1f002ff4, 0x1800);
    wr16(0x1f002fc0, 0x17);
    }
    }
    puVar44 = &SUB_23c00000 + (uVar28 << ((unsigned int)local_7c & 0xff));
    if (((unsigned int)puVar44 & 0xf0000000) == thunk_FUN_a0000010) {
    if (((unsigned int)puVar44 & 0xfffffff) < 0xd0000) {
    uVar32 = 0;
    }
    else {
    uVar32 = 1;
    }
    }
    else {
    uVar32 = 1;
    }
    FUN_a000128c(uVar43, *(unsigned short *)(iVar25 + 0x12), puVar44, uVar32);
    FUN_a000128c(*(unsigned short *)(iVar25 + 0x12) | uVar43, *(unsigned short *)(iVar25 + 0x10), 0xa000a800u, 0);
    if (pcVar42 == 0x0) {
    uVar24 = 0;
    }
    else if ((rd8(pcVar42 + 0x3) == '\0') || (uVar24 = 0, rd8(pcVar42 + 0x2) == '\0')) {
    uVar24 = 0;
    }
    else {
    FUN_a0001440(0, 0);
    wr8(pcVar42 + 0x2, '\0');
    }
    }
    else {
    local_67 = 0;
    if (rd8(pcVar42) == '\0') {
    LAB_a0004216:
    uVar24 = FUN_a00018ec(&local_67, 0, 0);
    /* uVar32 = "Cache Read: ChkDone Page" */
    if (uVar24 != 0) goto LAB_a00043bc;
    if (rd8(pcVar42 + 0x1) == '\0') {
    uVar24 = FUN_a000199c(0x31);
    }
    else {
    uVar24 = FUN_a000199c(0x3f);
    wr8(pcVar42 + 0x1, '\0');
    }
    if (uVar24 == 0) goto LAB_a0004240;
    }
    else {
    wr8(pcVar42, '\0');
    uVar24 = FUN_a0001870(uVar47);
    /* uVar32 = "Cache Read: Page" */
    if (uVar24 == 0) goto LAB_a0004216;
    LAB_a00043bc:
    uart_puts("Cache Read: Page");
    uart_put_hex32(uVar47);
    uart_puts(" err: ");
    uart_put_hex8(uVar24 & 0xff);
    uart_puts("\r\n");
    }
    uart_puts(" MDrv_SPINAND_GdEnterRead error\r\n ");
    uVar32 = 0x178;
    LAB_a00043e4:
    FUN_a0001824(uVar32, 0, 0, 0);
    uart_puts("\r\n");
    uart_puts("[I]d7\n");
    }
    if (rd16(0xa000a800) != -1) {
    uart_puts("BAD_BLK: ");
    LAB_a0004402:
    uart_puts("jump to next block: ");
    uart_put_hex8(uVar29 + 1 & 0xff);
    uart_puts("\r\n");
    if (local_84 == 0) {
    /* uVar31 = "no backup partition!\r\n " */
    if (CONCAT11(local_64[1],local_64[0]) != 0) {
    uVar24 = 3;
    uart_puts("no backup block!\r\n ");
    goto LAB_a0004326;
    }
    goto LAB_a00040c4;
    }
    uVar29 = uVar29 + 1 & 0xffff;
    local_84 = local_84 - 1 & 0xff;
    goto LAB_a000435e;
    }
    if (uVar24 == 0xb) {
    /* uVar31 = "no backup partition!\r\n " */
    if (CONCAT11(local_64[1],local_64[0]) == 0) goto LAB_a00040c4;
    LAB_a0004326:
    uart_puts("jump to next partition, block: 0x");
    uart_put_hex8(local_64[0]);
    uart_puts("\r\n");
    uVar43 = CONCAT11(local_64[1],local_64[0]);
    local_64[0] = 0;
    local_64[1] = 0;
    goto LAB_a00041fa;
    }
    if (uVar24 == 3) goto LAB_a0004402;
    }
    /* uVar32 = "fails\n" */
    if (uVar24 == 0) goto LAB_a0003a0c;
    }
    else {
    wr16(0x1f200808, 0x20);
    puVar8 = 0x1f203d40;
    wr16(puVar8, 0);
    wr16(puVar8 - 0x200d58, 0);
    wr16(puVar8 - 0x20206c, 8);
    iVar25 = FUN_a0001440(0, 0);
    /* uVar32 = "init failed!\r\n" */
    if (iVar25 != 0) {
    local_64[0] = 0;
    local_64[1] = 0;
    uStack_62 = 0;
    wr32(piVar21, 0xa0005568u);
    puVar8 = 0x1f002db0;
    wr16(puVar8, 7);
    wr16(puVar8 + 0x24, 0);
    wr16(puVar8 + 0x28, 0);
    wr16(puVar8 - 0x30, 0x9f);
    wr16(puVar8 - 0x8, 2);
    wr16(puVar8 - 0x4, 3);
    wr16(puVar8 + 0x4, 1);
    iVar25 = FUN_a000137c();
    if (iVar25 != 0) {
    FUN_a000134c(local_64, 0);
    wr16(0x1f002dbc, 1);
    }
    uVar24 = CONCAT22(uStack_62,CONCAT11(local_64[1],local_64[0]));
    *(unsigned int *)(pbVar38 + 4) = (uVar24 & 0xffffff) >> 8;
    wr8(pbVar38, local_64[0]);
    if (((uVar24 & 0xfffff7ff) == 0xd1c8) || (uVar24 == 0x0021aaefu)) {
    local_68 = 0;
    FUN_a00014f0(&local_68, 0xb0);
    /* iVar25 = "" */
    uVar24 = CONCAT22(uStack_62,CONCAT11(local_64[1],local_64[0]));
    wr32(piVar21, 0xa0005578u);
    uVar32 = 0xa000125du;
    if ((uVar24 & 0xfffff7ff) == 0xd1c8) {
    local_67 = 0;
    iVar49 = FUN_a00014f0(&local_67, 0xd0);
    if ((iVar49 == 0) && ((local_67 & 0x60) != 0x20)) {
    local_66 = CONCAT11(((unsigned char)((local_66) >> 8)),local_67) & 0xff9f | 0x20;
    FUN_a00014a8(&local_66, 0xd0);
    }
    if (-1 < (int)((unsigned int)local_68 << 0x1f)) {
    local_68 = local_68 | 1;
    FUN_a00014a8(&local_68, 0xb0);
    }
    *(bool *)(iVar25 + 3) = CONCAT22(uStack_62,CONCAT11(local_64[1],local_64[0])) == 0xd9c8;
    wr32((iVar25 + 0xc), 0xa000125du);
    wr8((iVar25 + 4), 1);
    }
    else if (uVar24 == 0x0021aaefu) {
    wr8((iVar25 + 3), 1);
    wr32((iVar25 + 0xc), uVar32);
    wr8((iVar25 + 4), 7);
    FUN_a0001574(0, 0, 0, 0);
    }
    else {
    wr8((iVar25 + 3), 0);
    }
    }
    else if (local_64[0] == 0xc2) {
    wr32(piVar21, 0xa0005588u);
    FUN_a000153c(1, 0, 0, 0);
    }
    else {
    FUN_a000153c(1, 0, 0, 0);
    uart_puts("QUAD MODE ENABLE\r\n");
    }
    iVar25 = 0xa000a010u;
    puVar8 = 0x1f002ff4;
    wr16(puVar8, 0x3800);
    wr16(puVar8 - 0x34, 0x17);
    wr16(local_7c, 0);
    do {
    FUN_a0001398(local_84 * *(unsigned short *)(iVar25 + 0x14) + 1);
    iVar49 = FUN_a00015ac(0, 0);
    if (iVar49 == 0) {
    FUN_a000128c(0, 0x200, 0xa000aa00u, 0);
    wr8(pcVar42, '\0');
    goto LAB_a0003f5a;
    }
    local_84 = local_84 + 2;
    } while (local_84 != 10);
    wr8(pcVar42, '\0');
    /* uVar32 = "init failed!\r\n" */
    }
    }
    }
    goto LAB_a0003b2a;
    code_r0xa0004000:
    uart_puts("ERR_SPINAND(backup block is bad!): ");
    uart_put_hex8(iVar30);
    /* uVar31 = "\r\n" */
    LAB_a00040c4:
    uart_puts("\r\n");
    /* uVar32 = "fails\n" */
    LAB_a0003b2a:
    uart_puts("fails\n");
    FUN_a00017f8();
    uVar24 = extraout_r3;
    LAB_a0003b32:
    uVar37 = 1;
    goto LAB_a0003616;
    }
}

/* entry @ a0000010 */
void ipl_entry(void)
{
    wr32(IPL_PROGRESS, 0xa001);      /* a0000018 */
    cp15_smp_fp_init();         /* a0000028..a000004c (no RIU writes) */
    wr32(IPL_PROGRESS, 0xa002);      /* a0000058 */
    ipl_main();                 /* blx a0001d50 */
}
