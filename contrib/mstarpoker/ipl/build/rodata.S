/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The stock IPL's rodata (strings, tables, config struct) placed at its
 * real address so the port's DAT_ reads resolve to the same bytes. */
    .section .ipldata, "a"
    .incbin "ipl_rodata.bin"
