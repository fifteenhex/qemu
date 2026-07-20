.. SPDX-License-Identifier: GPL-2.0-or-later

DDR / MIU bring-up
==================

The MIU (Memory Interface Unit) is the DDR controller at
``0x1f202000``. It is one analog block (the DDR PLL and the DQ/DQS
PHY) plus a digital block (the DRAM protocol engine, the client
arbiter and a memory BIST). On real hardware the whole of DRAM is
brought up here, before Linux, by the vendor IPL - the second-stage
loader the mask ROM copies out of SPI-NOR into IMI SRAM and runs from
``0xa0000000`` (see :doc:`bootrom`).

Everything below was traced by watching the IPL touch the MIU under
the model (``obs``): the register addresses are real, the values are
what this board's IPL writes, but the exact bit meanings inside each
register are mostly not decoded. Register offsets are relative to
``0x1f202000``, 16-bit on the 4 byte RIU stride.

Bring-up sequence
-----------------

The IPL programs the block roughly in this order (writer PCs in IMI,
``0xa000xxxx``):

1. **DDR PLL / analog.** Set the DDR frequency and start the PLL, then
   program the PHY drive/ODT patterns.

   * ``0x060`` / ``0x064`` DDFSET low / high - the DDR PLL
     frequency-set word. This board writes ``0x8f5c`` / ``0x001e``.
     The kernel's MIU clock driver reads this back later to recompute
     the memory-bus rate, so it is one of the few MIU registers the
     model answers with a real value (see `Model`_).
   * ``0x068`` / ``0x06c`` PLL dividers (``0x2004`` / ``0x400``).
   * ``0x43c`` PLL control - pulsed ``0xc00`` -> ``0xc01`` -> ``0x8c..``
     to release the PLL.
   * ``0x0b0``-``0x0bc`` PHY drive strength / slew (``0x0a0a``,
     ``0xaaaa`` patterns), ``0x0c0``-``0x0c8``, ``0x0d8``-``0x0ec``.

2. **DRAM timings and mode.** The protocol-engine timing parameters -
   ``tRAS``/``tRCD``/``tRP``/refresh and the DDR mode registers.

   * ``0x404``-``0x42c`` and ``0x680``-``0x6a8`` - timing words
     (``0x02a3``, ``0x1570``, ``0x20dd``, ``0x2d76``, ``0xe7e9`` ...).
   * ``0x140``-``0x15c`` timing sets (``0x4045``/``0x5453``/``0x6555``/
     ``0x6666``); ``0x170``-``0x1d0`` drive/delay (``0x4444``/
     ``0x5555``).
   * ``0x3c0``-``0x3f1`` DDR mode registers (``MR0``..``MR3``:
     ``0x2``, ``0x1e``, ``0x18``, ``0x08``, ``0x40`` ...).
   * ``0x200``-``0x264`` refresh / rank enables (``0x8015``,
     ``0xffff``).

3. **DQS/DQ read training.** Four byte-lane blocks at ``0x480``,
   ``0x4c0``, ``0x500`` and ``0x540``, each identical: an enable
   (``0x15`` -> ``0x8015``) followed by eight incrementing delay taps
   (``0x10``, ``0x32``, ``0x54``, ``0x76``, ``0x98``, ``0xba``,
   ``0xdc``, ``0xfe``) and a ``0xffff`` lane mask. The tap sweep is
   the per-bit DQ deskew - one block per DRAM byte lane, so this is a
   16-bit-wide (2 lanes x2?) DDR interface.

4. **Digital enable + init done.** ``0x400`` CNTRL0 is written
   (``0`` -> ``0x8`` -> ...) to start the DRAM engine; its bit 15 is
   the *init-done* flag the IPL then polls until DRAM is ready. The
   arbiter client-bandwidth table follows at ``0x404``-``0x42c``.

5. **Memory BIST.** A built-in self test walks DRAM before use:
   ``0x5c8`` mask (``0xffff``), ``0x5cc`` (``0x7e``), ``0x5d0`` test
   pattern (``0x5aa5``), then ``0x5c0`` BIST control (``0`` -> ``1``)
   with bit 15 the *done* flag and a status step polled at ``0x5bc``.
   On a good board it passes and the IPL proceeds to size DRAM and
   load the kernel.

Kernel-side MIU
---------------

After Linux boots, its MIU driver (kernel ``0xc01cf0xx``) touches the
block again for runtime work, not re-init:

* ``0x440``-``0x44c`` arbiter / priority (``0x3fff``).
* ``0x45c``-``0x479`` a byte-indexed calibration table it reads and
  rewrites (the values are small ascending indices, ``0x70``, ``0x17``,
  ``0x18`` ...), i.e. the stored DQ training result.
* ``0x580``-``0x5bc`` BIST / arbiter status it samples.

Model
-----

There is no DRAM PHY to program, so the model
(``mstarv7_miu_ops`` in ``hw/arm/mstarv7.c``) accepts every write as a
no-op and answers just the handful of registers the boot chain blocks
on:

.. list-table::
   :header-rows: 1

   * - Offset
     - Read value
     - Why
   * - ``0x400`` CNTRL0
     - bit 15 set (init done)
     - the IPL and kernel poll it until DRAM is "ready"; emulated DRAM
       is always ready
   * - ``0x5c0`` BIST_CTRL
     - bit 15 set (done, no error)
     - emulated DRAM never fails the self test
   * - ``0x060`` / ``0x064`` DDFSET
     - ``0x8000`` / ``0x0029``
     - the kernel MIU clock driver divides these to get the memory
       rate; the seed returns a sane frequency (the real IPL programs
       ``0x8f5c`` / ``0x001e``, but the model's seed is what keeps the
       kernel's recalc happy)

DRAM itself is plain RAM at ``0x20000000`` (:doc:`memory-map`); the
IPL's sizing pass just probes how much is there, which the machine
sets up directly.
