.. SPDX-License-Identifier: GPL-2.0-or-later

Pinctrl / chiptop
=================

Pin muxing lives in the "chiptop" block at ``0x1f203c00``, which also
carries the chip straps. The mainline pinctrl driver
(``pinctrl-mstar``/``pinctrl-ssd20xd``) programs a set of
function-select mux registers and per-pad pull/drive registers and
reads them back; the model stores and returns them, and returns the
read-only package bond strap at ``+0x120``.

Straps
------

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Notes
     - Source
   * - ``0x120``
     - BOND
     - package/DRAM variant: ``0x1d`` SSD201 (64 MiB), ``0x1e``
       SSD202D (128 MiB). The model returns the SoC's value.
     - ``prev``

Pad-mux registers
-----------------

Function-select muxes (``prev``, from the mainline ``pinctrl-mstar.h``
``REG_*`` definitions; the per-pin bit layout varies, so only the
register is named):

.. list-table::
   :header-rows: 1

   * - Offset
     - Selects
   * - ``0x0c``
     - UART pads
   * - ``0x1c``
     - PWM pads
   * - ``0x20``
     - SDIO / NAND pads
   * - ``0x24``
     - I2C pads
   * - ``0x30``
     - SPI pads
   * - ``0x3c``
     - Ethernet / JTAG pads
   * - ``0x54``
     - sensor config
   * - ``0x58``
     - MIPI TX / uart2

Per-pad pull and drive control (``prev``):

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
   * - ``0x94`` / ``0x98`` / ``0x9c``
     - I2C1 pull enable / direction / drive
   * - ``0xa8``
     - SPI drive strength
   * - ``0xc8``
     - SDIO pull / drive
   * - ``0xe0`` / ``0xe4``
     - sensor input enable 0 / 1

Model note: the pad muxes are pure configuration with no observable
effect in the model (there are no real pads), so they are readback
storage. Everything the pinctrl driver programs it can read back;
unnamed registers behave the same way.
