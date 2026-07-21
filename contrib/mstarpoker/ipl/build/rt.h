/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Runtime support for the cleaned IPL port. */
#ifndef IPL_RT_H
#define IPL_RT_H

/* short type names, as the Ghidra reference uses them */
typedef unsigned char  byte;
typedef unsigned short ushort;
typedef unsigned int   uint;
typedef unsigned long long ulonglong;

/* Sized volatile RIU register access (keeps the compiler from reordering,
 * merging or dropping the MMIO, and preserves each access width). */
#define R8(a)  (*(volatile unsigned char  *)(a))
#define R16(a) (*(volatile unsigned short *)(a))
#define R32(a) (*(volatile unsigned int   *)(a))

/* Boot-progress marker the mask ROM/IPL write to the SMP scratch register. */
#define IPL_PROGRESS (*(volatile unsigned int *)0x1f200800)

/*
 * Entry CP15 bring-up (a0000028..a000004c), kept verbatim as the IPL does
 * it: enable SMP (ACTLR.bit6), grant non-secure cp10/cp11 (NSACR), full
 * cp10/cp11 access (CPACR) and enable the VFP/NEON unit (FPEXC).
 */
static inline void cp15_smp_fp_init(void)
{
    unsigned int r;
    __asm__ volatile("mrc p15,0,%0,c1,c0,1" : "=r"(r));
    __asm__ volatile("mcr p15,0,%0,c1,c0,1" :: "r"(r | 0x40));
    __asm__ volatile("mrc p15,0,%0,c1,c1,2" : "=r"(r));
    __asm__ volatile("mcr p15,0,%0,c1,c1,2" :: "r"(r | 0xc00));
    r = 0xf00000;
    __asm__ volatile("mcr p15,0,%0,c1,c0,2" :: "r"(r));
    r = 0x40000000;
    __asm__ volatile("vmsr fpexc, %0" :: "r"(r));
}


/* Ghidra helpers used by the decompiled reference */
#define CONCAT11(a,b) ((unsigned short)(((unsigned int)(a)<<8)|((unsigned char)(b))))
#define CONCAT22(a,b) (((unsigned int)(a)<<16)|((unsigned short)(b)))
#define SUB41(x,n)    ((unsigned char)((unsigned int)(x) >> ((n)*8)))
#define SUB81(x,n)    ((unsigned char)((unsigned long long)(x) >> ((n)*8)))
#define SUB42(x,n)    ((unsigned short)((unsigned int)(x) >> ((n)*8)))

#endif
