.. SPDX-License-Identifier: GPL-2.0-or-later

Timers
======

Three identical timers sit in bank ``0x30`` at ``0x1f006040``,
``0x1f006080`` and ``0x1f0060c0`` (``dts``). Each is a 32-bit
up-counter behind 16-bit registers on a 4 byte stride. The register
layout matches the mainline ``timer-msc313e`` clocksource driver
(``linux``); the boot ROM programs the first timer to free-run with
``MAX = 0xffffffff`` and polls the counter to time its SPI NOR
operations (``rom``).

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Bits
     - Source
   * - ``0x00``
     - CTRL
     - 0: enable, 1: retrigger (restarts the count),
       8: interrupt enable
     - ``linux``
   * - ``0x08``
     - MAX_LOW
     - 15..0: low half of the counter wrap value
     - ``linux``
   * - ``0x0c``
     - MAX_HIGH
     - 15..0: high half of the counter wrap value
     - ``linux``
   * - ``0x10``
     - COUNTER_LOW
     - 15..0: low half of the running count
     - ``linux``
   * - ``0x14``
     - COUNTER_HIGH
     - 15..0: high half of the running count
     - ``linux``
   * - ``0x18``
     - DIVIDE
     - 15..0: input clock divider, assumed to divide by N+1
     - ``linux``, divisor semantics unconfirmed

Model notes: reading COUNTER_LOW latches COUNTER_HIGH so a 16-bit
guest sees a coherent 32-bit value; whether hardware does the same is
unconfirmed (``model``). The counter-reached-MAX interrupt is not
modelled yet. The input clock is 432 MHz per the previous branch
(``prev``, unconfirmed); until confirmed it only scales emulated
timeout lengths.
