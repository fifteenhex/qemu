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

Bringing up the pipeline from reset
===================================

The full path from a cold block to pixels on the panel, in the order
the firmware walks it. The GOP/MOP/display-top registers above are the
parts the model acts on; the rest are configuration/timing banks that
are programmed and read back but do not change what is scanned out (the
model backs them as plain readback banks - see `Config/timing banks`_).
The sequence and the register roles were recovered by tracing the
writers (a probe logging the writer PC) and disassembling the kernel's
panel-enable routine at ``0xc02f0d80`` (``obs``).

1. **Clocks.** Ungate the display clocks: ``CLK_disp_432`` /
   ``CLK_disp_216``, ``CLK_mop``, ``CLK_ge``, ``CLK_dip`` and
   ``CLK_mipi_tx_dsi`` in the main clkgen (:doc:`clocks`), plus
   ``CLK_hdmi`` / ``CLK_mipi_tx_dsi_apb`` in the ``0x1f226600`` group.
   Nothing scans out until these run.

2. **Timing generator (pnl).** Program the panel timing at
   ``0x1f225200``: horizontal/vertical totals, sync widths and the
   active (DE) window. For this 640x480 panel the IPL/mhal write
   ``HTT = 0x2b3`` (691), ``VTT = 0x1ee`` (494) and the DE
   start/size words (``0x34``, ``0xf`` ...). The display top at
   ``0x1f225000`` then generates vsync from this timing.

3. **Planes.** Point and enable the scanout planes:

   * GOP (RGB) at ``0x1f246800`` with its per-plane config at
     ``0x1f246200`` / ``0x1f246400`` (``reg127`` is a ``0x100`` commit
     bit).
   * MOP (video) at ``0x1f280a00`` with up to four overlay windows at
     ``0x1f281a00`` (each window a ``{pos 0x820, size 0xa01f,
     blend 0x801}`` triple, ``0x40`` apart).
   * The scaler/colour planes at ``0x1f284200`` / ``0x1f284a00`` /
     ``0x1f285200`` - three identical blocks the kernel enables by
     writing ``0xffc4`` to ``reg0`` and ``0x80`` to ``reg4``.

4. **MIPI DSI output.** Bring up the DSI controller and its D-PHY
   (:doc:`dsi`) and the analog trim at ``0x1f2a4a00``-``0x1f2a4e00``
   (``0x105b``, ``0xffff`` seeds), then send the panel's init command
   sequence over DSI.

5. **Panel enable.** The kernel routine at ``0xc02f0d80`` does the
   final power-on: it sets the lane-enable bits at ``0x1f224ca0``
   (``|= 0x400/0x800/0x1000/0x2000``), the clip/window enables at
   ``0x1f224cc0`` (``0xfff``, then ``1``/``1``/``0x1f``), and the
   plane-enable ``0xffc4`` writes above, in sequence. After this the
   backlight PWM (:doc:`pwm`) is turned up and the image is visible.

Config/timing banks
===================

These banks hold the configuration the bring-up programs and reads
back. They do not feed the model's scanout, so the model backs each
with a plain readback bank (``mstarv7_disp_cfg_base[]`` in
``hw/arm/mstarv7.c``). Values below are what this firmware writes
(``obs``); bit-level meanings are mostly not decoded.

.. list-table::
   :header-rows: 1

   * - Base
     - Role
     - Notable registers
   * - ``0x1f224c00`` / ``0x1f224e00``
     - display front: lane/window enables
     - ``+0xa0`` lane enables; ``+0xc0``-``+0xd8`` clip enables
       (``0xfff``, ``1``, ``0x1f``)
   * - ``0x1f225200``
     - panel (pnl) timing generator
     - ``+0x44`` HTT-ish (``0x2e3``), ``+0x60`` HTT (``0x2b3``),
       ``+0x68`` VTT (``0x1ee``), ``+0x80`` enable (``0x8000``)
   * - ``0x1f226600``
     - mipi/hdmi clock gates
     - see :doc:`clocks`
   * - ``0x1f246200`` / ``0x1f246400``
     - GOP plane 0 / 1 config
     - ``reg0``-``reg9`` plane setup; ``+0x1fc`` commit (``0x100``)
   * - ``0x1f281000``
     - GE front / display mux
     - ``+0x144`` (read)
   * - ``0x1f281a00``
     - MOP overlay windows (x4)
     - per-window ``{0x820, 0xa01f, 0x801}`` at ``+0x48`` stride
       ``0x40``
   * - ``0x1f283e00``
     - scaler front
     - ``reg0`` (read)
   * - ``0x1f284200`` / ``0x1f284a00`` / ``0x1f285200``
     - scaler / colour planes (x3, identical)
     - ``reg0`` enable (``0xffc4``), ``reg4`` (``0x80``)
   * - ``0x1f2a4a00`` / ``0x1f2a4c00`` / ``0x1f2a4e00``
     - mipi/dsi analog trim
     - ``0x2a4c00`` ``reg0`` (``0x105b``), ``reg8`` (``0xffff``);
       ``0x2a4e00`` ``+0xc4`` (``0x400``)
