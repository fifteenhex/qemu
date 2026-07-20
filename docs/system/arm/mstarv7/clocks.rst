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

PM-domain clkgen
----------------

A second readback bank at ``0x1f001c00`` holds the always-on clock
gates. The device tree places these ``sstar,*-clock`` nodes in it
(``dts``):

.. list-table::
   :header-rows: 1

   * - Offset
     - Clock
   * - ``0x70``
     - CLK_pwm
   * - ``0x80``
     - CLK_spi / CLK_spi_pm
   * - ``0x84``
     - CLK_ir
   * - ``0x88``
     - CLK_rtc / CLK_sar / CLK_pm_sleep

The same bank also carries the SAR conversion trigger the kernel's
temperature read path pulses (set bit ``0x400`` at ``0x190``, bit ``4``
at ``0xbc``) before sampling the ADC in the SAR block at ``0x1f002800``;
the formula is ``(1370 * (calib - raw) + 25000) / 1000`` (``obs``). This
readback traffic is the busiest in the system after the flash
write-protect - one pass per temperature sample.
