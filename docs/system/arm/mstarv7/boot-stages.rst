.. SPDX-License-Identifier: GPL-2.0-or-later

Boot stages (mask ROM to u-boot)
================================

Where control passes from one loader to the next, from reset up to the
point u-boot starts executing. Traced under the model (``model``)
booting the Miyoo Mini firmware, and confirmed by breaking at each
entry and checking the serial banner emitted just after it. Each stage
runs from a different address window; the block internals are on their
own pages (:doc:`bootrom`, :doc:`ipl`).

.. list-table::
   :header-rows: 1
   :widths: 14 22 18 46

   * - Stage
     - Source
     - Entry PC
     - Banner (first serial line)
   * - mask ROM
     - on-chip ROM
     - ``0x00000000``
     - *(silent on success)*
   * - IPL
     - NOR ``0x00000`` (magic ``IPL_``)
     - ``0xa0000000`` (IMI SRAM)
     - ``IPL g5da0ceb``
   * - IPL_CUST
     - NOR ``0x10000`` (magic ``IPLN``)
     - ``0x23c00000`` (DRAM)
     - ``IPL_CUST g5da0ceb``
   * - u-boot
     - MXP partition, XZ-compressed
     - ``0x23e00000`` (DRAM)
     - ``U-Boot 2015.01 ...``

Handoffs:

* **mask ROM -> IPL**: the ROM copies the IPL from NOR into IMI SRAM and
  branches to ``0xa0000000`` (:doc:`bootrom`).
* **IPL -> IPL_CUST**: the IPL brings up the clocks and DDR, loads the
  ``IPLN`` image from NOR ``0x10000`` into DRAM at ``0x23c00000`` and
  jumps to it.
* **IPL_CUST -> u-boot**: it reads the XZ-compressed u-boot from the MXP
  partition, decompresses it to ``0x23e00000``, disables the MMU and
  D-cache, and jumps there.

The ROM and IMI entries (``0x00000000``, ``0xa0000000``) are fixed by
the SoC; the two DRAM entries (``0x23c00000``, ``0x23e00000``) are
chosen by this firmware.
