.. SPDX-License-Identifier: GPL-2.0-or-later

CPU PLL
=======

The analog CPU PLL at ``0x1f206400``. The Cortex-A7 clock (and so the
cpufreq driver) derives from it: the device tree's ``CLK_cpupll_clk``,
a ``sstar,complex-clock``, reads a loop divider and an output divider
out of this block and computes

.. code-block::

   cpu_hz = (216 MHz << 20) * 32 / (loop * (out_div + 1))

The registers, 16-bit on the 4-byte RIU stride, relative to the base:

.. list-table::
   :header-rows: 1

   * - Offset
     - Register
   * - ``0x064``
     - output divider (rate divides by ``out_div + 1``)
   * - ``0x148`` / ``0x14c``
     - loop divider, low / high half
   * - ``0x150`` / ``0x15c`` / ``0x160`` / ``0x164``
     - set-rate control (start / latch)
   * - ``0x174``
     - lock status, bit 0

Changing the rate (the cpufreq driver's ``clk_set_rate``) writes the
loop divider, pulses the control registers, then spins reading the lock
register until bit 0 goes high, which a real PLL sets once it relocks.

The block is modelled as readback storage, seeded at reset with the
1.2 GHz loop divider (``0x5c28f6``, output divider 0) the vendor
bootloader leaves, and the lock bit always reads set so ``clk_set_rate``
completes immediately. Without it the loop divider reads zero, the CPU
clock computes as zero and the ``sstar,infinity-cpufreq`` driver never
registers, so ``/sys/devices/system/cpu/cpu0/cpufreq`` is absent and
MainUI's Settings page shows no CPU frequency. With it the kernel
reports the full 400 - 1200 MHz operating-point table and MainUI shows
1.2 GHz. The core voltage is switched by a GPIO regulator on the CPU
pads, which the GPIO model already covers.
