.. SPDX-License-Identifier: GPL-2.0-or-later

GE (2D graphics engine)
=======================

At ``0x1f281200``. A bitblt/fill/line accelerator (device tree
``sstar,ge``). The vendor MI_GFX middleware composites the MainUI
user interface through it into the framebuffer, polling the engine
idle between operations - the firmware issues tens of thousands of
GE register accesses while rendering a single menu screen, so nothing
appears on the panel unless its blits actually execute.

16-bit registers on the usual 4-byte RIU stride (``prev``, register
names from the linux-chenxing ``ip/ge.md`` notes, behaviour confirmed
against the rendered output, ``hw``):

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Bits
   * - ``0x1c``
     - STATUS
     - ``GE_WaitIdle`` polls bits[7:3] for 0x10 (bit7 = command FIFO
       empty); bit0 = not busy; bits[15:11] = free command FIFO slots
       (``GE_WaitCmdQAvail`` waits until enough are free). The model
       always reports ``0xf880``: idle with a fully free FIFO, so
       neither wait blocks.
   * - ``0x80`` / ``0x84``
     - SRC_ADDR low / high
     - source surface base, a MIU (DRAM bus) address
   * - ``0x98`` / ``0x9c``
     - DST_ADDR low / high
     - destination surface base, a MIU address
   * - ``0xc0``
     - SRC_PITCH
     - source stride, bytes
   * - ``0xcc``
     - DST_PITCH
     - destination stride, bytes
   * - ``0xd0``
     - FMT
     - bits[3:0] source, bits[11:8] destination pixel format
       (0x8 RGB565, 0x9 ARGB1555, 0xa ARGB4444, 0xf ARGB8888)
   * - ``0x164``
     - ROTATE
     - bits[1:0]: 0/1/2/3 = 0/90/180/270 degrees
   * - ``0x180``
     - CMD
     - bit6 kicks a bitblt
   * - ``0x1a0`` / ``0x1a4``
     - DST_X0 / DST_Y0
     - destination rectangle *bottom-right* corner: the top-left is
       ``(x0 - (w - 1), y0 - (h - 1))``
   * - ``0x1b8`` / ``0x1bc``
     - WIDTH / HEIGHT
     - blit size, pixels

The model executes the bitblt when CMD bit6 is written: it copies
WIDTH x HEIGHT pixels from the source to the destination rectangle,
converting between the programmed pixel formats, honouring the
180-degree rotation MainUI uses to pre-rotate for the upside-down
panel. Alpha blending, ROP, stretch and 90/270-degree rotation are
not modelled. The blit lands in DRAM, where the :doc:`display` GOP
plane scans it out.

The GE completion interrupt (device tree ``interrupts = <0 57 4>``) is
not modelled: with the engine completing synchronously and reporting
idle, the firmware's polling paths never need it.
