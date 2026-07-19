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
   * - ``0x00000000``
     - ``0x4000``
     - Boot ROM (see note below)
     - yes
     - ``rom``
   * - ``0x14000000``
     - ``0x1000000``
     - SPI NOR XIP read window (ISP/QSPI controller)
     - yes
     - ``prev``, ``rom``
   * - ``0x16000000``
     - ``0x8000``
     - Cortex-A7 PERIPHBASE (SCU, GIC, private timers)
     - yes
     - ``dts``
   * - ``0x1f000000``
     - ``0x400000``
     - RIU register bus
     - no
     - ``dts``
   * - ``0x20000000``
     - ``0x20000000``
     - DRAM (MIU0). The window decodes 512 MiB and smaller DRAM wraps
       within it; the IPL sizes memory by writing markers and finding
       where they alias, so the model mirrors DRAM through the window
     - yes
     - ``dts``; window size and wrapping ``prev``, confirmed by the
       IPL detecting "128MB" under the model
   * - ``0xa0000000``
     - per SoC
     - IMI SRAM
     - yes
     - ``dts``

The boot ROM contents are a 16 KiB dump taken from an SSD202D
(``hw``); the tail carries an ``I2m_ROM`` signature so it may be
common to the whole infinity2m family, but for now it is wired into
the SSD202D model only. The boot core comes out of reset executing
it, so it is modelled at the Cortex-A7 reset vector; whether the ROM
really sits at ``0x00000000`` or is aliased there from somewhere else
has not been confirmed (``model``).

The infinity2m IMI SRAM is modelled as 64 KiB: the boot ROM uses
addresses up to ``0xa000f8a5`` (``rom``) so it is at least that big,
but the true size has not been confirmed.

The RIU and PERIPHBASE regions are covered by low priority stub
regions in the model: unmodelled registers read as zero, writes are
ignored, and both are logged with ``-d unimp`` (``model``). On real
hardware at least some unmodelled registers will behave differently.

The Cortex-A7 CBAR reads back the PERIPHBASE value, ``0x16000000``.
The GIC is a GIC-400 (GICv2): distributor at ``0x16001000``, CPU
interface at ``0x16002000`` (``dts``), hypervisor and virtual CPU
interfaces at ``0x16004000``/``0x16006000``, 128 SPIs (``prev``).

RIU blocks
----------

.. list-table::
   :header-rows: 1

   * - Address
     - Bank
     - Block
     - Modelled
     - Source
   * - ``0x1f001000``
     - ``0x08``
     - ISP SPI NOR controller, core registers
     - no
     - ``prev``
   * - ``0x1f002c00``
     - ``0x16``
     - ISP "FSP" flash sequencer; see :doc:`isp`
     - partly
     - ``prev``, ``rom``
   * - ``0x1f002800``
     - ``0x14``
     - SAR ADC; see :doc:`sar`
     - yes
     - ``linux``, ``prev``, ``rom``
   * - ``0x1f002e00``
     - ``0x17``
     - ISP QSPI configuration
     - no
     - ``prev``
   * - ``0x1f003c00``
     - ``0x1e``
     - CHIPID; reads ``0xf0`` on SSD20xD
     - no
     - ``prev``
   * - ``0x1f006000``
     - ``0x30``
     - Watchdog; the boot ROM disables it by zeroing ``WDT_MAX_PRD``
       at ``+0x10``/``+0x14``
     - no
     - ``dts``, ``linux``, ``rom``
   * - ``0x1f006040``
     - ``0x30``
     - Timers 0/1/2 on a ``0x40`` stride; see :doc:`timer`
     - yes
     - ``dts``, ``rom``
   * - ``0x1f007000``
     - ``0x38``
     - "DID": boot-media strap in ``DID_KEY`` at ``+0x1c0``
       (see below)
     - no
     - ``prev``, ``rom``
   * - ``0x1f200400``
     - ``0x1002``
     - BDMA engine, two channels; see :doc:`bdma`
     - yes
     - ``prev``, ``rom``
   * - ``0x1f201310``
     - ``0x1009``
     - "FIQ" mst-intc, 32 lines onto GIC SPI 96+
     - no
     - ``prev``
   * - ``0x1f201350``
     - ``0x1009``
     - "IRQ" mst-intc, 64 lines onto GIC SPI 32+
     - no
     - ``prev``
   * - ``0x1f202000``
     - ``0x1010``
     - MIU DDR controller: analog block at ``+0x000`` (DDR PLL
       frequency-set pair at ``+0x60``/``+0x64``), arbiter at
       ``+0x200``, digital block at ``+0x400`` (init done bit 15 of
       ``+0x400``, BIST start/status at ``+0x5c0`` with done bit 15).
       Modelled as training/BIST always complete
     - partly
     - ``prev``, ``rom`` (BIST poll seen from the IPL)
   * - ``0x1f203c00``
     - ``0x101e``
     - "chiptop" pinctrl / straps; bond strap at ``+0x120``
       (``0x1e`` = SSD202D/128 MiB). See :doc:`pinctrl`
     - yes
     - ``prev``, ``rom``
   * - ``0x1f204000``
     - ``0x1020``
     - "smpctrl" CPU1 boot mailbox (see the boot ROM page): entry
       address as halfwords at ``+0x50``/``+0x4c``, then ``0xbabe``
       to ``+0x58``. A readback register file; software spins reading
       back what it writes here
     - yes
     - ``prev``, ``linux``, ``rom``
   * - ``0x1f204400``
     - ``0x1022``
     - "l3bridge" MIU write-flush barrier: poke the trigger at
       ``+0x14``, poll done (bit 12) at ``+0x40``. Modelled as always
       flushed
     - yes
     - ``prev``, ``linux``, confirmed by the IPL's barrier polls
   * - ``0x1f206400``
     - ``0x1032``
     - CPU PLL
     - no
     - ``prev``
   * - ``0x1f207000``
     - ``0x1038``
     - clkgen clock mux/gate block; see :doc:`clocks`
     - yes
     - ``prev``, ``linux``
   * - ``0x1f225000``
     - ``0x1128``
     - Display top (frame timing, vsync); see :doc:`display`
     - yes
     - ``prev``, ``rom``
   * - ``0x1f246800``
     - ``0x1234``
     - GOP graphics plane; see :doc:`display`
     - yes
     - ``prev``, ``linux``, ``rom``
   * - ``0x1f280a00``
     - ``0x1405``
     - MOP video plane; see :doc:`display`
     - yes
     - ``prev``, ``rom``
   * - ``0x1f2a5000``
     - ``0x152800``
     - MIPI D-PHY; see :doc:`dsi`
     - yes
     - ``prev``
   * - ``0x1f345200``
     - ``0x1a290``
     - MIPI DSI controller (MediaTek clone); see :doc:`dsi`
     - yes
     - ``linux``, ``prev``, ``rom``
   * - ``0x1f221000``
     - ``0x1108``
     - PM UART, 16550 compatible, registers on an 8 byte stride
     - yes
     - ``dts``, ``rom``

The interrupt routing of the PM UART is not known yet; the model does
not raise an interrupt (``model``). The previous branch has it on
"IRQ" mst-intc line 34 with a 172 MHz clock (``prev``).

The boot-media strap in ``DID_KEY`` (``0x1f0071c0``, vendor register
``0x70``) bits[5:2] selects where the boot ROM loads the IPL from:
``0x20`` SPI NOR, ``0x10`` NAND, ``0x08`` SPI NAND/eMMC (``prev``).
The model's DID block returns SPI NOR by default (the SoC's
``did-key`` property), which sends the ROM down its SPI NOR path and
confirmed the strap register for infinity2m (``model``).

The previous branch also claims the boot ROM window is 32 KiB and the
IMI SRAM is 128 KiB (``prev``); neither is confirmed and the model
currently uses the 16 KiB dump size and 64 KiB respectively.

RIU addressing
--------------

Vendor code and documentation address registers on the RIU bus as a
*bank* plus a 16-bit register number rather than by byte address. The
registers are 16 bits wide but sit on 32-bit boundaries, and a bank is
128 registers, so (``linux``):

  byte address = ``0x1f000000`` + bank * ``0x200`` + register * ``4``

Per-block register documentation will use the bank/register form
alongside byte addresses so it can be checked against vendor sources.
