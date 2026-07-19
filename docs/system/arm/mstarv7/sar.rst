.. SPDX-License-Identifier: GPL-2.0-or-later

SAR ADC
=======

A small successive-approximation ADC in bank ``0x14``
(``0x1f002800``); boards hang keypads and similar analog inputs off
its channels. The Miyoo Mini's front buttons are an ADC keypad on
channel 0, which the vendor u-boot scans during its boot check: a
conversion result near zero reads as a held key and diverts u-boot
into its USB upgrade path, so the model's idle mid-scale samples are
what let it take the normal boot path (``model``).

Register map from the mainline ``msc313e_sar`` driver (``linux``)
with the vendor u-boot's usage seen under the model (``rom``):

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Bits
     - Source
   * - ``0x00``
     - CTRL
     - 2..0: channel select, 7: start conversion, 14: "load"
       (the vendor u-boot pulses this and polls it clearing as its
       done flag)
     - ``linux``, ``prev``, ``rom``
   * - ``0x44``
     - GPIO_CTRL
     - SAR pads as GPIOs: 3..0 enable, 11..8 output disable
     - ``linux``, ``prev``
   * - ``0x48``
     - GPIO_DATA
     - 3..0 output values, 11..8 pin levels
     - ``linux``, ``prev``
   * - ``0x50``/``0x54``/``0x58``/``0x5c``
     - INT_MASK / INT_CLR (W1C) / INT_FORCE / INT_STATUS
     - bit 0: conversion done
     - ``linux``, ``prev``
   * - ``0x100 + ch * 4``
     - CH_RESULT
     - 9..0: conversion result for channel *ch*
     - ``linux``, ``prev``, ``rom``

Model notes: conversions are instantaneous; each channel returns the
value of its ``channelN`` property, default mid scale. The done
interrupt is tracked but not wired to anything yet (``model``).
