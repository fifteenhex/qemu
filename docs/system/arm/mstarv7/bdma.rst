.. SPDX-License-Identifier: GPL-2.0-or-later

BDMA engine
===========

A two channel byte DMA engine in bank ``0x1002`` (``0x1f200400``,
``0x40`` per channel) that moves data between the memory ports and
peripherals. The boot ROM uses channel 0 to copy the IPL from the SPI
NOR into IMI (``rom``); Linux uses it for flash reads (``prev``).
Register layout from the previous branch and the mainline bdma
driver, with the boot ROM's usage confirming CTRL/STATUS/CONFIG/MISC
and the address/size registers under the model (``rom``).

Registers, byte offsets within a channel (16-bit registers on a
4 byte stride):

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Bits
     - Source
   * - ``0x00``
     - CTRL
     - 0: trigger (self clearing)
     - ``prev``, ``rom``
   * - ``0x04``
     - STATUS, write-1-to-clear
     - 1: busy, 2: interrupt, 3: done, 4: result 0
     - ``prev``, ``rom``
   * - ``0x08``
     - CONFIG
     - 3..0: source port, 11..8: destination port. Ports: ``0``/``1``
       MIU memory channels, ``5`` QSPI (flash). The boot ROM uses
       ``0x4035``; the extra bits are not understood.
     - ``prev``, ``rom``
   * - ``0x0c``
     - MISC
     - 1: interrupt enable; 13..12: memory behind the MIU port
       (``0`` DRAM, ``2`` IMI)
     - ``prev``, ``rom``
   * - ``0x10``/``0x14``
     - SRC_ADDR low/high
     - bus address; flash relative for the QSPI port
     - ``prev``, ``rom``
   * - ``0x18``/``0x1c``
     - DST_ADDR low/high
     - bus address; MIU addresses are DRAM/IMI relative
     - ``prev``, ``rom``
   * - ``0x20``/``0x24``
     - SIZE low/high
     - transfer length in bytes
     - ``prev``, ``rom``

Observed boot ROM usage (``rom``): CONFIG ``0x4035`` (QSPI source,
MIU destination), MISC ``0x2000`` (IMI), source ``0`` (start of
flash), destination ``0`` (start of IMI), size ``0x56a0`` (the IPL
size from its header, ``0x55a0``, plus the ``0x100`` byte header),
trigger, poll STATUS.done, then write-1-to-clear the flags. It also
writes ``0x80`` to ``0x1f200e40`` (bank ``0x1007``) just before the
transfer; that register is not understood.

Model notes: transfers complete synchronously inside the trigger
write and the interrupt is not connected anywhere yet (``model``).
