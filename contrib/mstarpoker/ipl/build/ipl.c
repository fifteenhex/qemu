/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Cleaned, compilable port of the SSD202D IPL (see ../ipl_decompiled.c for
 * the Ghidra reference). Built as an IPL image and validated by comparing
 * its RIU MMIO write trace against the stock IPL running in the model.
 * Ported incrementally; all logic is kept as it is reached.
 */
#include "rt.h"

/* entry @ a0000010 */
void ipl_entry(void)
{
    IPL_PROGRESS = 0xa001;      /* a0000018 */
    cp15_smp_fp_init();         /* a0000028..a000004c (no RIU writes) */
    IPL_PROGRESS = 0xa002;      /* a0000058 */
    /* TODO: blx main (a0001d50) port continues from here */
}
