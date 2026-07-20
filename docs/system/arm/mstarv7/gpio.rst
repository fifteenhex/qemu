.. SPDX-License-Identifier: GPL-2.0-or-later

GPIO (pad banks)
================

The SoC's general-purpose pads, in two banks: the "main" bank at
``0x1f207800`` (``gpio@207800`` in the mainline device trees) and the
"PM" bank at ``0x1f001e00`` inside the always-on power-management
domain. Boards hang buttons, straps and bit-banged buses off them.

Each pad has one register on the usual 4-byte RIU stride. The bit
positions differ between the two banks, taken from the vendor kernel's
GPIO HAL pad table (``hw``; one 24-byte entry per pad giving the
output-enable, output and input register and mask):

.. list-table::
   :header-rows: 1

   * - Function
     - Main bank
     - PM bank
   * - IN (level on the pad)
     - bit0
     - bit2
   * - OUT (driven level)
     - bit4
     - bit1
   * - OEN (output disable, set = input)
     - bit5
     - bit0

The model stores the output configuration each bank writes and, on a
read, returns the pad level: what the pad drives when it is configured
as an output, otherwise the external level a board device (a button,
say) drives onto it. Each pad exposes one GPIO input line - named
``main-pad`` / ``pm-pad`` and indexed by the pad register offset / 4 -
for a board to drive.

One PM-bank pad is special-cased: the SD card-detect (``SD_CDZ``) sits
at register ``0x47`` (byte offset ``0x11c``) bit 2, active low. A card
in the slot holds the pad low and an empty slot reads high behind its
pull-up, so the model forces that bit from whether the machine was
given a card with ``-drive if=sd``. The FCIE host's sdmmc driver reads
it to decide whether to enumerate a card.

The pads are also interrupt sources on real hardware (through the
"main" interrupt nexus). That is not modelled: the Miyoo Mini's
buttons are the only consumers so far and its kernel polls them
(``gpio-keys-polled``, every 100 ms), so it never uses the interrupt.
