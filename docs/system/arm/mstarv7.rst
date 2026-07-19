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

* Two Cortex-A7 cores. The secondary core is held in reset; the
  register interface that releases it is not modelled yet, so SMP
  kernels will only bring up the boot core.
* 128 MiB of in-package DDR3 at the start of the MIU0 address space.
* 64 KiB of IMI SRAM.
* The PM UART, on the first serial port. No interrupt yet.
* No other peripherals are modelled yet.

Booting
-------

A kernel can be loaded into DRAM with the usual options:

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
