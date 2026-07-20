.. SPDX-License-Identifier: GPL-2.0-or-later

ISP SPI NOR controller
======================

The ISP is the SPI NOR flash controller. It has several register
windows on the RIU (core at ``0x1f001000``, FSP at ``0x1f002c00``,
QSPI config at ``0x1f002e00``, all ``prev``) plus a 16 MiB
memory-mapped XIP read window at ``0x14000000`` where the flash
contents appear (``prev``, confirmed by the boot ROM fetching and
verifying the IPL through it, ``rom``).

FSP flash sequencer
-------------------

Register numbers below are vendor register indices (16-bit registers
on a 4 byte stride from ``0x1f002c00``); names from the vendor SDK
``regSERFLASH.h`` as relayed by the previous branch (``prev``).

.. list-table::
   :header-rows: 1

   * - Register
     - Byte offset
     - Name
     - Bits
     - Source
   * - ``0x60``-``0x64``
     - ``0x180``
     - WD0-9, write buffer, 2 bytes per register
     - 15..0
     - ``prev``
   * - ``0x65``-``0x69``
     - ``0x194``
     - RD0-9, read buffer, 2 bytes per register
     - 15..0
     - ``prev``
   * - ``0x6a``
     - ``0x1a8``
     - WBF_SIZE, three 4-bit write byte counts
     - 11..0
     - ``prev``
   * - ``0x6b``
     - ``0x1ac``
     - RBF_SIZE, three 4-bit read byte counts
     - 11..0
     - ``prev``
   * - ``0x6c``
     - ``0x1b0``
     - CTRL; the boot ROM writes ``2``, ``4``, ``0x8000``, ``0``
       here while setting up, meaning unknown
     - ?
     - ``prev``, ``rom``
   * - ``0x6d``
     - ``0x1b4``
     - TRIGGER, bit 0 fires the sequence
     - 0
     - ``prev``, ``rom``
   * - ``0x6e``
     - ``0x1b8``
     - DONE_FLAG, bit 0 set when the sequence completed
     - 0
     - ``prev``, ``rom``
   * - ``0x6f``
     - ``0x1bc``
     - DONE_CLR, writing bit 0 clears DONE_FLAG
     - 0
     - ``prev``, ``rom``

Observed use by the boot ROM (``rom``, under the model): it fires a
two byte sequence ``66 99`` - the standard SPI NOR enable-reset +
reset command pair - then reads the IPL through the XIP window. This
also explains the ``SPINOR reset timeout!`` string: the poll of
DONE_FLAG is timed against timer 0.

QSPI config bank
----------------

The second RIU bank at ``0x1f002e00`` carries the flash write-protect
control. Register indices are relative to the FSP base ``0x1f002c00``
(so bank 0x17 register ``n`` is index ``0x80 + n``); the behaviour was
observed under the model by tracing the Linux SERFLASH driver
(``obs``).

.. list-table::
   :header-rows: 1

   * - Register
     - Byte offset
     - Name
     - Notes
     - Source
   * - ``0xf1``
     - ``0x3c4``
     - WP_CFG
     - written ``1``, ``0x10``, ``0x100`` once at init
     - ``obs``
   * - ``0xf2``
     - ``0x3c8``
     - WP_CTRL
     - write protect: the driver writes ``0xa`` to unprotect before a
       write/erase and ``1`` to protect after, reading it back each
       time. By far the busiest register in the system - roughly 40000
       accesses through boot to menu.
     - ``obs``

Both are modelled as readback storage: the driver only ever reads back
what it wrote, so no side effects are needed. Covering them here stops
that heavy traffic from drowning the ``-d unimp`` log.

The model completes any fired sequence immediately and leaves the
read buffer as it was (there is no SPI flash model behind it yet; the
XIP window is filled from the ``-drive if=mtd`` image directly). The
command bytes are logged with ``-d unimp``.
