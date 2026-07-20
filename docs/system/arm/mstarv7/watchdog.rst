.. SPDX-License-Identifier: GPL-2.0-or-later

Watchdog
========

The watchdog timer at ``0x1f006000`` (``sstar,infinity-wdt`` in the
vendor device tree; the vendor kernel reserves it in ``/proc/iomem`` as
``/soc/watchdog``). A counter clocked off the crystal counts up, and
when it reaches the programmed period the block resets the SoC;
software feeds it by writing the clear register and disarms it with a
zero period.

The register meanings are taken from the mainline msc313e watchdog
driver and match what the vendor kernel writes at boot - it feeds the
counter (``CLR`` = 1) then parks the block with a zero period, i.e.
disabled:

.. list-table::
   :header-rows: 1

   * - Offset
     - Register
   * - ``0x00``
     - ``CLR`` - a write feeds / restarts the counter
   * - ``0x10`` / ``0x14``
     - period, low / high half, in counter ticks

When the period is non-zero the model arms a host timer for the
corresponding interval and, on expiry, performs the configured QEMU
watchdog action (a system reset by default). The vendor firmware
leaves it disabled, so the reset path is not exercised on the Miyoo
Mini.
