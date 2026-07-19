.. SPDX-License-Identifier: GPL-2.0-or-later

MStar/SigmaStar ARMv7 SoCs (``miyoomini``)
==========================================

The MStar (now SigmaStar) ARMv7 SoCs are a range of Cortex-A7 based
SoCs with a shared lineage. The range is split into families that
share a die or are close derivatives of each other; the families in
turn share many hardware blocks with the rest of the range.

The QEMU models mirror that structure as a QOM class hierarchy:

* machine (``miyoomini``) -- the board or device the SoC is mounted in
* SoC (``ssd202d``) -- a concrete part number
* SoC family (``infinity2m``) -- parts sharing a die
* base (``mstarv7``) -- what the whole range has in common

Emulated devices and known limitations
--------------------------------------

* Two Cortex-A7 cores. The secondary core is modelled as powered
  off; on real hardware it runs the mask ROM and parks in the
  smpctrl mailbox loop, so SMP release is not modelled yet.
* 128 MiB of in-package DDR3, mirrored through the MIU window so
  the IPL's wrap-around size probe works.
* 64 KiB of IMI SRAM.
* The PM UART, on the first serial port. No interrupt yet.
* The three timers, the DID boot-media strap (SPI NOR), the BDMA
  engine, the FSP flash sequencer, the SPI NOR XIP window (filled
  from the ``-drive if=mtd`` image), an MIU DDR controller stub and
  the l3bridge barrier.
* The boot ROM, loaded from ``ssd202d_bootrom.bin`` (a ``-bios``
  image overrides it). With no flash image it prints
  ``Check IPL Header failed! [HALT]`` and halts, matching the real
  ROM with nothing to boot.
* The GIC-400 and the Cortex-A7 generic timers, so the kernel gets
  timer interrupts. Peripheral interrupts still go nowhere: the two
  mst-intc instances between the peripherals and the GIC are not
  modelled yet.

With the Miyoo Mini firmware image attached the whole vendor boot
chain runs: mask ROM, IPL (DRAM sizing and memory BIST pass),
IPL_CUST (checksum passes, finds the MXP partition table), then
u-boot 2015.01 decompresses, detects the flash and reads its
environment. Known gaps at this point:

* Flash JEDEC ID reads zero (no SPI flash model behind the FSP), so
  u-boot complains and falls back to a default 16 MiB flash type.
* Flash writes/erases are not modelled: the erase path and the BDMA
  "write to flash" port (``0xb``) are still unimplemented.
* With the SAR ADC reading idle, u-boot takes the normal boot path:
  it JPEG-decodes the boot logo (the MIPI DSI panel writes time out,
  no display yet), reads the kernel from flash, decompresses it and
  jumps to it. The kernel is silent from there: the GIC and the
  interrupt fabric are the next things to model.

Booting
-------

With no kernel argument the machine boots into the ROM, and with a
flash image attached the ROM loads the IPL out of it and runs it:

.. code-block:: bash

  $ qemu-system-arm -M miyoomini -drive if=mtd,format=raw,file=nor.bin \
      -nographic

A kernel can also be loaded directly into DRAM with the usual options:

.. code-block:: bash

  $ qemu-system-arm -M miyoomini -kernel zImage -dtb board.dtb -nographic

Hardware documentation
----------------------

These SoCs have no public documentation, so a goal of this emulation
is to double as documentation of the hardware: the memory map and the
registers of each block, at bit level, are documented alongside the
device models and every documented fact carries a tag saying where it
came from. The conventions and the tags are described at the top of
the memory map page. A block's register page is added or extended in
the same commit as the device model change it describes.

.. toctree::
   :maxdepth: 1

   mstarv7/memory-map
   mstarv7/bootrom
   mstarv7/timer
   mstarv7/isp
   mstarv7/bdma
   mstarv7/sar
