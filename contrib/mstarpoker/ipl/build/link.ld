/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0xa0000000, LENGTH = 64K }
SECTIONS {
    .header  0xa0000000 : { KEEP(*(.header)) } > RAM
    .ipldata 0xa0004000 : { KEEP(*(.ipldata)) } > RAM
    .text    : { *(.text*) }   > RAM
    .rodata  : { *(.rodata*) } > RAM
    .data    : { *(.data*) }   > RAM
    . = ALIGN(4);
    .bss (NOLOAD) : { . = ALIGN(4); _bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
    /DISCARD/ : { *(.ARM.*) *(.comment) *(.note*) }
}
