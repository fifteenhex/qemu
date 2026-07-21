.. SPDX-License-Identifier: GPL-2.0-or-later

IPL (first-stage loader)
========================

The IPL is the first-stage loader the mask ROM copies to IMI SRAM and
runs (see :doc:`bootrom`). This page records only what has been
**confirmed on real SSD202D hardware** by driving the chip over serial
with ``contrib/mstarpoker`` - a bare-metal stub flashed in place of the
vendor IPL, which lets the host read and write registers and time
clocks on the live SoC. Facts marked ``hw`` were measured on silicon.

The IPL does much more than this (DDR bring-up, the other PLLs, the
``clk_init`` clock routing); those steps have been traced under the
model but are **not yet confirmed on hardware**, so they are left out
here until they are. This document grows as each step is verified.

Entry
-----

The ROM loads the IPL image from SPI NOR to IMI SRAM at ``0xa0000000``,
verifies its header and jumps to it there (``bootrom``). This entry is
confirmed on hardware (``hw``): the ``mstarpoker`` stub is an IPL image,
and the ROM loads and runs it at ``0xa0000000`` on the real chip.

Main PLL (MPLL) enable
----------------------

The MPLL is the main system PLL (block ``0x1f206000``); it supplies the
fast timer and bus clocks. On a live chip its output is gated off until
the IPL enables it, by clearing the byte at ``0x1f206005`` (``hw``):

* out of the ROM, ``0x1f206005`` reads ``0x0f``;
* the IPL writes ``0x1f206005 = 0x00``, which enables the MPLL output.

With the MPLL enabled it runs at **~448 MHz** measured on hardware
(``hw``; ``scripts/ssd20x/mpll_test.py``). Until this write is done,
selecting the MPLL as a clock source hangs the SoC - the clock is not
yet there to switch onto.

Timers moved onto the MPLL
--------------------------

Out of reset the timers count off the always-on ~12 MHz crystal. The
IPL moves timer[0] onto the (much faster) MPLL and divides it back down
(``hw``):

* select the MPLL as timer[0]'s source: clkgen ``0x1f207004 = 0x30``;
* set timer[0]'s divider: ``0x1f006058`` (TIMER_DIVIDE) ``= 0x23``,
  i.e. divide by ``0x23 + 1 = 36``.

Measured on hardware (``hw``; ``scripts/common/timer_test.py``):

* with the source selected and no divider, timer[0] counts at the full
  MPLL rate, **~448 MHz**;
* with the ``/36`` divider it counts at **~12 MHz** (448 / 36), the rate
  the IPL's delay loops assume.

The order matters: the MPLL must be enabled (``0x1f206005 = 0``) before
the timer source is switched to it, or the SoC hangs.
