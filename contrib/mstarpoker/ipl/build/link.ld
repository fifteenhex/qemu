/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The mask ROM copies the IPL to IMI SRAM at ORIGIN and jumps there. */
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0xa0000000, LENGTH = 64K }
SECTIONS {
    .header : { KEEP(*(.header)) } > RAM
    .text   : { *(.text*) }        > RAM
    .rodata : { *(.rodata*) }      > RAM
    .data   : { *(.data*) }        > RAM
    . = ALIGN(4);
    .bss (NOLOAD) : { . = ALIGN(4); _bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
    /DISCARD/ : { *(.ARM.*) *(.comment) *(.note*) }
}
