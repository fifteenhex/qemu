# SPDX-License-Identifier: GPL-2.0-or-later
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
PYTHON  ?= python3
POKER   := ../..
# The stock firmware; the port's IPL is overlaid at offset 0 so the
# stage-loading tail reads the real IPL_CUST/MXP/u-boot.
FIRMWARE ?= /workspace/files/MiYoo283v1.1.bin

CFLAGS  := -mcpu=cortex-a7 -mthumb -ffreestanding -nostdlib -Os \
           -Wall -Wextra -fno-pic -fno-builtin
LDFLAGS := -T link.ld -nostdlib --no-warn-rwx-segments

all: flash.bin

start.o: start.S ; $(CC) $(CFLAGS) -c $< -o $@
ipl.o:   ipl.c rt.h ; $(CC) $(CFLAGS) -c $< -o $@
ipl.elf: start.o ipl.o link.ld ; $(LD) $(LDFLAGS) start.o ipl.o -o $@
ipl.bin: ipl.elf ; $(OBJCOPY) -O binary $< $@
ipl.ipl: ipl.bin ; $(PYTHON) $(POKER)/mkipl.py ipl.bin ipl.ipl
flash.bin: ipl.ipl ; cp $(FIRMWARE) $@ && dd if=ipl.ipl of=$@ conv=notrunc status=none
clean: ; rm -f *.o *.elf *.bin *.ipl
.PHONY: all clean
