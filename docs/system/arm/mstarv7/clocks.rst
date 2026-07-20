.. SPDX-License-Identifier: GPL-2.0-or-later

Clocks (clkgen)
===============

The clock tree
--------------

The whole SoC clock tree hangs off one 24 MHz crystal and a handful of
PLLs. The device tree (``dts``) enumerates the tree as a large set of
``sstar,*-clock`` nodes; the shape below is recovered from those nodes
and their frequencies, with the PLLs cross-checked against the
registers the boot chain programs (``obs``, ``rom``).

::

   XTAL 24 MHz ─┬─ /2 ─ xtali_12m ─┬─ /2/4/8/12/16/40/64/128 ... (slow gates)
                │                   └─ RTC path
                │
                ├─ MPLL (core PLL, "mpll")  ── 432 MHz  ─┬─ 432m
                │     programmed by the boot ROM/IPL      ├─ /1.5 288m ─ /2/4/8/32
                │     (analog block, not on the RIU       ├─ 216m ─ /2/4/8
                │      readback banks)                    ├─ 172.8m
                │                                         ├─ 144m ─ /2/4
                │                                         ├─ 123.4m ─ /2
                │                                         └─ 86.4m ─ /2/4/16
                │
                ├─ UPLL / UTMI ── 480 MHz ─┬─ utmi_240m (/2)
                │   (USB PLL)              ├─ utmi_192m (x2/5) ─ /4
                │                          ├─ utmi_160m (/3) ─ /4/5/8
                │                          ├─ upll_384m (x4/5)
                │                          └─ upll_320m (x2/3)
                │
                ├─ LPLL ── (display/LCD PLL) ─ lpll_clk ─ /2/4/8
                │
                ├─ CPU PLL  (see :doc:`cpupll`, RIU 0x1f206400)
                │     cpu_hz = (216 MHz << 20) * 32 / (loop * (out_div + 1))
                │     -> 1.2 GHz on this board
                │
                └─ DDR PLL  (see :doc:`ddr`, MIU DDFSET at RIU 0x1f202060)
                      the miu / miu2x / axi clocks the memory bus runs at

   RTC:  external 32.768 kHz (and an internal 32 kHz) -> rtc dividers

The MPLL output taps (``mpll_432m`` .. ``mpll_86m``) appear as
*fixed-clock* nodes in the device tree: the boot ROM brings the MPLL up
to a fixed rate before Linux runs, so Linux treats the taps as
constants and only programs the muxes/dividers below them. The 24 MHz
crystal (``xtali_24m``) and its /2 (``xtali_12m``) are the ultimate
roots; ``xtali_12m`` also feeds most of the slow peripheral gates
(UART, PWM, watchdog) through the /8../128 factor chain.

Two PLLs *are* driven from the RIU and modelled, because software reads
them back to compute a rate: the CPU PLL (:doc:`cpupll`) and the DDR
PLL (:doc:`ddr`). The rest of the analog PLLs (MPLL, UPLL, LPLL) are
configured by the boot ROM before the kernel runs and are not on the
readback banks, so the model does not need them.

Below the PLLs sit the *mux/gate* clocks: a composite clock
(``sstar,composite-clock``) selects one parent tap and optionally
divides it; a gate clock enables it. These live in three readback
banks - the main clkgen at ``0x1f207000``, the PM-domain clkgen at
``0x1f001c00`` (always-on gates, see below), and a smaller
mipi/hdmi group at ``0x1f226600``. Software programs a mux and reads
it back to recompute the rate, so the model just stores and returns
them; there is nothing to actually gate.

clkgen (main mux bank)
----------------------

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

mipi/hdmi clock group
---------------------

A third readback bank at ``0x1f226600`` holds the display-output clock
gates. The device tree puts two composite clocks here (``dts``):

.. list-table::
   :header-rows: 1

   * - Offset
     - Clock
   * - ``0xd4``
     - CLK_hdmi
   * - ``0xdc``
     - CLK_mipi_tx_dsi_apb

The kernel's clock driver programs and reads back the gates at ``0x88``,
``0x8c``, ``0xcc``, ``0xd0`` (enable value ``1``, gated ``0x100``) and a
mux at ``0x94`` (``obs``); the DSI/HDMI blocks it feeds are the
:doc:`dsi` path. This bank is covered by the display config banks in the
model (see :doc:`display`).
