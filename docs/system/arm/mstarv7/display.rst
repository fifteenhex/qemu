.. SPDX-License-Identifier: GPL-2.0-or-later

Display controller (GOP / MOP / display top)
============================================

The front of the display pipe that feeds the :doc:`dsi` output path.
Two planes are scanned out of DRAM and composited: the GOP graphics
plane over the MOP video plane. The result is presented on a QEMU
console, so a ``screendump`` (or a graphical UI) shows what the panel
would.

The GOP is blended over the MOP with a *constant* alpha, not the
per-pixel alpha in the framebuffer: the vendor framebuffer driver
programs blending type 1 with a constant alpha of 255
(``sstar_FB_SetBlending aType=1 constAlpha=255``), i.e. the plane is
opaque while enabled. This matters because the UI leaves the alpha
byte at 0 on most pixels - compositing with per-pixel alpha makes
most of MainUI invisible (``hw``).

Only the primary window of each plane (WIN0) is modelled. MainUI
composites its UI into the GOP framebuffer through the :doc:`ge` 2D
engine before the GOP scans it out; the vendor u-boot boot logo is
decoded to the MOP plane.

GOP (graphics plane)
--------------------

At ``0x1f246800``. The RGB plane the vendor fbdev and the mainline DRM
fbcon draw to. 16-bit registers (``prev``, ``linux``):

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Bits
   * - ``0xc0``
     - STRETCH_W
     - [11:0] crtc width >> 1
   * - ``0xc4``
     - STRETCH_H
     - [11:0] crtc height
   * - ``0x200``
     - WIN0
     - bit0 enable, bits[7:4] pixel format (1 RGB565, 5 ARGB8888,
       7 ABGR8888)
   * - ``0x204`` / ``0x208``
     - WIN0_ADDRL / ADDRH
     - framebuffer address (in 16 byte units)
   * - ``0x224``
     - WIN0_PITCH
     - stride, in 16 byte units

MOP (video plane)
-----------------

At ``0x1f280a00``, window base ``+0x200``. A semi-planar YUV420 (NV12)
overlay. The vendor u-boot points it at the JPEG-decoded boot logo
(luma at ``0x27c00000``, chroma following). Window registers (``prev``,
confirmed by the boot logo, ``rom``):

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
   * - ``0x00``
     - WIN_EN (bit0)
   * - ``0x08`` / ``0x0c``
     - luma address low / high
   * - ``0x10`` / ``0x14``
     - chroma address low / high
   * - ``0x28``
     - luma pitch, in 16 byte units
   * - ``0x2c`` / ``0x30``
     - source width - 1 / height - 1

Display top
-----------

At ``0x1f225000``. The frame timing generator. It raises a vsync
interrupt each frame, which the framebuffer driver waits on:

* ``0x08`` VSYNC_FLAG - bit3 pending, write-1-to-clear
* ``0x0c`` VSYNC_MASK - bit3, 0 = interrupt enabled

The model runs a 60 Hz timer that latches VSYNC_FLAG (raising the
display-top interrupt, "IRQ" mst-intc line 50 / GIC SPI 82) and pulses
a second, status-less GOP/fbdev vsync interrupt (mst-intc line 20 /
GIC SPI 52) that the vendor fbdev counts.

The GOP/fbdev vsync has no status or ack register: its handler only
increments a counter and wakes ``sstar_FB_WaitForVsync``, which
``/dev/fb0`` blocks on (opening the framebuffer hangs forever without
it). The interrupt path is level based, so the line must be *held* for
a short pulse each frame - an instantaneous raise/lower is lost before
the GIC samples it, while holding the line high storms the never-acked
handler (``hw``).

Rendering
---------

With the Miyoo Mini firmware, the vendor u-boot software-decodes its
JPEG splash into the MOP plane and enables it; the model scans it out
and the boot logo appears. The kernel later disables the MOP and
MainUI takes over the GOP plane: its SDL draws into ``/dev/fb0``
(composited through the :doc:`ge`), the GOP scans the result out, and
the MainUI menu appears on the console.
