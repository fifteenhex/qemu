/* SPDX-License-Identifier: GPL-2.0-or-later */
/* IPL loader header + entry: hand control to the C port at ipl_entry(). */
    .section .header, "ax"
    .global _header
_header:
    b       _start              /* 0x00: branch over the header */
    .word   0x5f4c5049          /* 0x04: "IPL_" magic */
    .word   0                   /* 0x08: size     (mkipl fills in) */
    .word   0                   /* 0x0c: checksum (mkipl fills in) */

    .text
    .global _start
_start:
    cpsid   if                  /* IRQ/FIQ masked, as the ROM left them */
    ldr     r0, =_bss_start
    ldr     r1, =_bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b
    ldr     sp, =0xa000f000
    bl      ipl_entry
1:  b       1b                  /* hang after the ported prefix */
