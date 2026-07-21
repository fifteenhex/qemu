# SPDX-License-Identifier: GPL-2.0-or-later
# Build the mstarpoker serial-monitor stub and package it into a 16 MiB
# SPI-NOR image the SSD202D mask ROM can load. See PROTOCOL.md.

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
PYTHON  ?= python3

CFLAGS  := -mcpu=cortex-a7 -marm -ffreestanding -nostdlib -Os \
           -Wall -Wextra -fno-pic -fno-builtin
LDFLAGS := -T link.ld -nostdlib --no-warn-rwx-segments

OBJS    := start.o stub.o

# The on-target DDR-init blob (a separate bare-metal build under scripts/).
DDR_BLOB := scripts/ssd20x/ddr_c/ddr_init.bin

all: flash.bin ddr

# Build the DDR-init blob used by scripts/ssd20x/ddr.py to train real DRAM.
ddr:
	$(MAKE) -C scripts/ssd20x/ddr_c CROSS=$(CROSS)

start.o: start.S
	$(CC) $(CFLAGS) -c $< -o $@

stub.o: stub.c
	$(CC) $(CFLAGS) -c $< -o $@

stub.elf: $(OBJS) link.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

stub.bin: stub.elf
	$(OBJCOPY) -O binary $< $@

stub.ipl: stub.bin mkipl.py
	$(PYTHON) mkipl.py stub.bin stub.ipl

flash.bin: stub.ipl mkflash.py
	$(PYTHON) mkflash.py stub.ipl flash.bin

# handy: disassemble the built stub
dis: stub.elf
	$(OBJDUMP) -d $<

clean:
	rm -f $(OBJS) stub.elf stub.bin stub.ipl flash.bin
	$(MAKE) -C scripts/ssd20x/ddr_c clean

.PHONY: all dis clean ddr
