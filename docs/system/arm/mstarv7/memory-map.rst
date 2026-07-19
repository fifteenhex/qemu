.. SPDX-License-Identifier: GPL-2.0-or-later

MStar ARMv7 memory map
======================

Source tags
-----------

Every fact in these pages carries a tag saying where it came from, so
that later findings can be checked against how well the source is
trusted:

* ``dts`` -- mainline Linux device trees (``arch/arm/boot/dts/mstar-*``)
* ``linux`` -- mainline Linux driver code
* ``sdk`` -- vendor SDK sources or headers
* ``hw`` -- verified against real hardware
* ``rom`` -- derived from analysing a dump of the boot ROM
* ``prev`` -- the previous QEMU branch; jumbled and known to contain
  mistakes, so treat as a hint only until confirmed elsewhere
* ``model`` -- a choice made for the emulation, not a hardware fact

Global memory map
-----------------

Addresses as seen by the Cortex-A7 cores. All MStar ARMv7 SoCs are
believed to share this layout.

.. list-table::
   :header-rows: 1

   * - Base
     - Size
     - Block
     - Modelled
     - Source
   * - ``0x16000000``
     - ``0x8000``
     - Cortex-A7 PERIPHBASE (SCU, GIC, private timers)
     - no
     - ``dts``
   * - ``0x1f000000``
     - ``0x400000``
     - RIU register bus
     - no
     - ``dts``
   * - ``0x20000000``
     - DRAM size
     - DRAM (MIU0)
     - yes
     - ``dts``
   * - ``0xa0000000``
     - per SoC
     - IMI SRAM
     - yes
     - ``dts``

The infinity2m IMI SRAM is modelled as 64 KiB: the boot ROM uses
addresses up to ``0xa000f8a5`` (``rom``) so it is at least that big,
but the true size has not been confirmed.

The RIU and PERIPHBASE regions are covered by low priority stub
regions in the model: unmodelled registers read as zero, writes are
ignored, and both are logged with ``-d unimp`` (``model``). On real
hardware at least some unmodelled registers will behave differently.

The Cortex-A7 CBAR reads back the PERIPHBASE value, ``0x16000000``.
The GIC distributor is at offset ``0x1000`` and the CPU interface at
offset ``0x2000`` from PERIPHBASE (``dts``).

RIU blocks
----------

.. list-table::
   :header-rows: 1

   * - Address
     - Bank
     - Block
     - Modelled
     - Source
   * - ``0x1f221000``
     - ``0x1108``
     - PM UART, 16550 compatible, registers on an 8 byte stride
     - yes
     - ``dts``, ``rom``

The interrupt routing of the PM UART is not known yet; the model does
not raise an interrupt (``model``).

RIU addressing
--------------

Vendor code and documentation address registers on the RIU bus as a
*bank* plus a 16-bit register number rather than by byte address. The
registers are 16 bits wide but sit on 32-bit boundaries, and a bank is
128 registers, so (``linux``):

  byte address = ``0x1f000000`` + bank * ``0x200`` + register * ``4``

Per-block register documentation will use the bank/register form
alongside byte addresses so it can be checked against vendor sources.
