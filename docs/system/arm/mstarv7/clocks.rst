.. SPDX-License-Identifier: GPL-2.0-or-later

Clocks (clkgen)
===============

The clock generator is at ``0x1f207000``. It is a block of clock
mux/gate registers: each register selects a parent and gates a clock.
The mainline ``clk-msc313``/``clk-ssd20xd`` driver programs these and
reads them back to recompute rates, so the model stores and returns
them (a readback bank). There is nothing to gate in the model, so the
registers have no behaviour.

Clock mux registers (16-bit, RIU 4 byte stride) recovered from the
mainline mstar clk driver headers (``prev``, ``linux``). A value of
``0x0d`` in a byte selects the gated/default parent; the exact field
layout is per the driver's ``MSC313_MUX_*`` macros.

.. list-table::
   :header-rows: 1

   * - Offset
     - Clock
   * - ``0x04``
     - mcu / riubridge
   * - ``0x5c``
     - miu
   * - ``0x64``
     - ddr_syn
   * - ``0xc4``
     - uart0 / uart1
   * - ``0xc8``
     - spi
   * - ``0xcc``
     - mspi0 / mspi1 / movedma
   * - ``0xd0``
     - fuart
   * - ``0xdc``
     - miic0 / miic1 (i2c)
   * - ``0x108``
     - emac_ahb
   * - ``0x114``
     - sdio
   * - ``0x144``
     - ge (2D engine)
   * - ``0x14c``
     - disp_432 / disp_216
   * - ``0x150``
     - mop
   * - ``0x154``
     - dec_pclk / dec_aclk
   * - ``0x180``
     - bdma
   * - ``0x184``
     - aesdma
   * - ``0x18c``
     - sc_pixel
   * - ``0x1a8``
     - jpe (JPEG)
   * - ``0x1b8``
     - sata
   * - ``0x1bc``
     - mipi_tx_dsi
   * - ``0x1f8``
     - dec_bclk / dec_cclk

A further clock sub-block at ``0x1f207800``-``0x1f207954`` is polled
periodically during boot (a background clock/frequency monitor). Its
registers have not been decoded yet; they currently read as zero
through the RIU catch-all, which the periodic poll tolerates.
