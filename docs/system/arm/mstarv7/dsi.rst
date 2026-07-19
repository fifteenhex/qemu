.. SPDX-License-Identifier: GPL-2.0-or-later

Display output path (MIPI DSI)
==============================

The panel is driven over a MIPI DSI link. The path is:

  GOP (framebuffer) -> display top / MOP -> DSI controller ->
  D-PHY -> panel

Only the output half - the DSI controller, the D-PHY and the panel -
is modelled so far; the GOP/display-top/MOP front end that feeds it
pixels is not, so nothing is scanned out yet.

DSI controller
--------------

At ``0x1f345200``. It is a MediaTek DSI clone: the register interface
matches mainline's ``mtk_dsi`` (``linux``), confirmed against the
vendor bring-up code (``rom``). 32-bit registers.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Bits
     - Source
   * - ``0x00``
     - START
     - bit 0 kicks the queued command-mode packet
     - ``linux``, ``rom``
   * - ``0x08``
     - INTEN
     - interrupt enable, same bit layout as INTSTA
     - ``linux``
   * - ``0x0c``
     - INTSTA
     - bit 1 CMD_DONE, bit 2 TE_RDY, bit 31 BUSY. The vendor code
       polls this after START and reports "CMD Done Time Out" if
       CMD_DONE never sets
     - ``linux``, ``rom``
   * - ``0x10``
     - CON_CTRL
     - bit 0 DSI_RESET, bit 1 DSI_EN, bit 2 DPHY_RESET
     - ``linux``, ``prev``
   * - ``0x14``
     - MODE_CTRL
     - 0 command mode, 1 sync-pulse video, 2 sync-event, 3 burst
     - ``linux``, ``prev``
   * - ``0x1c``
     - PSCTRL
     - [13:0] word count (width*bpp), [17:16] pixel format select
     - ``linux``, ``prev``
   * - ``0x60``
     - CMDQ_SIZE
     - [5:0] number of 32-bit words queued in the command queue
     - ``linux``, ``rom``
   * - ``0x200+``
     - CMDQ0..
     - command queue. word0 is the packet header (byte 0 config,
       byte 1 MIPI data type, bytes 2/3 the two short-packet data
       bytes); a long packet's payload follows in the next words
     - ``linux``, ``rom``

On START the model decodes the queued packet and delivers it to the
panel, then raises CMD_DONE. Writing a display mode to MODE_CTRL also
latches TE_RDY so a tear-effect poll makes progress.

D-PHY
-----

At ``0x1f2a5000``. The analog MIPI D-PHY that serialises the DSI
output onto the physical lanes. It has no behaviour to model (there
are no real lanes); it stores and returns register values. The known
register meanings are documented in the device source, from the
values the vendor u-boot programs (``prev``). No PLL-lock or ready
status that software polls has been found.

Panel
-----

The panel is a board-specific device on the far end of the DSI link
(MIPI DSI is point to point, so it is a QOM link from the controller,
not a bus). The generic ``dsi-dcs-panel`` interprets the standard
MIPI DCS command set and tracks the state each command sets (sleep
in/out, display on/off, the column and page address windows, address
mode, pixel format, tear-effect).

The vendor firmware's observed bring-up sequence (``rom``), each a
DCS write decoded by the controller and applied by the panel::

  01 soft_reset       36 set_address_mode  3a set_pixel_format
  2a set_column_addr  2b set_page_addr     3b (vendor)
  51 set_brightness   53 write_ctrl_disp   55 write_cabc
  5e (vendor)         35 set_tear_on       44 set_tear_scanline
  11 exit_sleep_mode  29 set_display_on

so the panel ends up awake with the display on, as expected.
