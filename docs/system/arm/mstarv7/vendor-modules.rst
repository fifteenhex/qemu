.. SPDX-License-Identifier: GPL-2.0-or-later

Vendor kernel modules as a register source
==========================================

The vendor firmware ships its hardware access as a stack of kernel
modules in ``/config/modules/4.9.84`` (the ``/config`` squashfs,
mtd5). They are **unstripped** ELF objects - they keep their symbol
and relocation tables - so they are a far better register/behaviour
source than tracing one access at a time. This page records what was
mined from them; it is reference material, not something the model
consumes.

Extraction and method
---------------------

The ``.ko`` files were copied out of a running guest over the serial
console (busybox ``base64``; the modules are read-only on squashfs).
``unsquashfs`` was not available and the host ``objdump`` has no ARM
support, so analysis is ``pyelftools`` + ``capstone`` (Thumb-2).

The HAL layer (``mhal.ko`` and the leaf drivers) addresses registers by
a **fixed kernel virtual mapping** of the RIU, baked into the code as
literal constants (no relocation):

.. code-block::

   physical = virtual - 0xde000000        (0xfd000000 -> 0x1f000000)

Verified against a known register: ``kdrv_sdmmc`` loads ``0xfd207114``
for the SD clock mux, which maps to ``0x1f207114`` = clkgen offset
``0x114`` (the ``sdio`` mux, see :doc:`clocks`). So disassembling a HAL
function, reading its ``0xfd2xxxxx`` base literals and subtracting
``0xde000000`` gives the real register address - **including blocks
that are idle at boot and never appear in a runtime trace**.

Two addressing notes: the higher-level ``MI_*`` modules (mi_disp,
mi_panel, mi_venc, ...) call *through* ``mhal``/each block's HAL, so
they hold no register bases themselves; and some blocks (FCIE, the VPU)
get their base from the device tree at probe rather than hardcoding it.

Module inventory
----------------

.. list-table::
   :header-rows: 1

   * - Module
     - Size
     - What it is
   * - ``mhal.ko``
     - 564 KiB
     - the whole multimedia HAL: DISP (236 fns), PNL (123), XC/scaler
       (96), GOP, GE/GFX, CMDQ, AUDIO/bach, RGN/OSD, JPE/MFE/MHE
       encoders. Holds the PLL/analog init tables.
   * - ``mi_sys.ko``
     - 423 KiB
     - MI system layer: MMA memory, buffer pools, binding graph
   * - ``mi_vdec.ko``
     - 262 KiB
     - video decoder: the Wave5 / Coda9 VPU (licensed IP) at
       ``0x1f344800``
   * - ``mi_ai.ko`` / ``mi_ao.ko`` / ``mi_alsa.ko``
     - 240 / 97 / 29 KiB
     - audio in / out / ALSA glue (through ``mhal`` bach)
   * - ``mi_venc.ko``
     - 185 KiB
     - video encoder (H.264/JPEG)
   * - ``mi_disp.ko`` / ``mi_panel.ko`` / ``mi_divp.ko``
     - 118 / 33 / 58 KiB
     - display / panel / video-post MI layers (through ``mhal``)
   * - ``fbdev.ko``
     - 46 KiB
     - ``/dev/fb0`` framebuffer (sstar_FB), feeds the GOP
   * - ``mdrv_crypto.ko``
     - 29 KiB
     - AESDMA / RSA / SHA crypto engine at ``0x1f224400``
   * - ``kdrv_sdmmc.ko``
     - 33 KiB
     - FCIE SD/MMC host (see :doc:`fcie`); base from DT
   * - ``mi_gfx.ko`` / ``mi_ipu.ko`` / ``mi_common.ko``
     - 25 / 25 / 10 KiB
     - GFX (GE) / IPU (NN) / common MI glue

Register banks each driver hardcodes
------------------------------------

Physical RIU banks recovered from the ``0xfd2xxxxx`` literals
(``obs``). Bold are blocks **not** seen in any boot-to-menu trace -
new, because they are idle until the feature is used.

.. list-table::
   :header-rows: 1

   * - Bank
     - Driver
     - Block (modelled?)
   * - **``0x1f224400``**
     - mdrv_crypto
     - **AESDMA/RSA/SHA crypto engine - new, see below**
   * - **``0x1f225400`` / ``0x225600``**
     - mhal
     - **display timing (pnl) extension - new**
   * - **``0x1f247400`` / ``0x247600`` / ``0x247800``**
     - mhal
     - **GOP sub-blocks - new**
   * - **``0x1f280c00`` / ``0x280e00``**
     - mhal
     - **MOP overlay extension - new**
   * - **``0x1f344800``**
     - mi_vdec
     - **Wave5/Coda9 VPU (video decoder) - new**
   * - **``0x1f345400``**
     - mhal
     - **MIPI DSI extension - new**
   * - ``0x1f224a00`` / ``0x224e00`` / ``0x225000`` / ``0x225200``
     - mhal
     - display front / top / pnl timing (modelled)
   * - ``0x1f280a00`` / ``0x281000`` / ``0x281a00``
     - mhal
     - MOP / GE front / MOP windows (modelled)
   * - ``0x1f2a5000`` / ``0x345200``
     - mhal
     - MIPI D-PHY / DSI (modelled)
   * - ``0x1f203c00``
     - kdrv_sdmmc, mhal
     - chiptop / pad-mux (modelled)
   * - ``0x1f206600`` / ``0x207000`` / ``0x226600`` / ``0x226e00``
     - several
     - PLLs / clkgen / mipi-hdmi & video clocks (modelled/documented)
   * - ``0x1f003c00`` / ``0x004000``
     - mhal
     - display/PM misc (the "remaining edges", see :doc:`memory-map`)

Crypto engine (``0x1f224400``)
------------------------------

Fully recovered from ``mdrv_crypto.ko`` - a block idle at boot, so it
never appeared in a trace. Offsets from ``0x1f224400`` (``obs``):

.. list-table::
   :header-rows: 1

   * - Offset
     - Engine
     - Register (from the HAL function that touches it)
   * - ``+0x20``
     - SHA
     - CTRL (Clear/SelMode/ManualMode/Start/Reset)
   * - ``+0x28`` / ``+0x38``
     - SHA
     - source address
   * - ``+0x30``
     - SHA
     - length
   * - ``+0x3c``
     - SHA
     - status
   * - ``+0x40``
     - SHA
     - data / init-value in/out; ``+0xb8``/``+0xbc`` word count
   * - ``+0x80``
     - RSA
     - file-out / key-load base; ``+0x84`` Ind32 ctrl
   * - ``+0x88`` / ``+0x8c`` / ``+0x90``
     - RSA
     - key load (N / E / inverse)
   * - ``+0x94`` / ``+0x98``
     - RSA
     - file out
   * - ``+0x9c``
     - RSA
     - exponential start / clear-int
   * - ``+0xa0``
     - RSA
     - reset / key length / key type; ``+0xa4`` status
   * - ``+0x140``
     - AESDMA
     - start / file-out-enable / reset (``+0x19c``, ``+0x1e4`` too)
   * - ``+0x144``
     - AESDMA
     - CTRL (enable / chain mode ECB/CBC/CTR / en/decrypt)
   * - ``+0x148`` / ``+0x150``
     - AESDMA
     - file-in address / XIU length
   * - ``+0x158`` / ``+0x164``
     - AESDMA
     - file-out address
   * - ``+0x178``
     - AESDMA
     - interrupt mask / disable
   * - ``+0x17c`` .. ``+0x19c``
     - AESDMA
     - cipher key
   * - ``+0x19c`` / ``+0x1bc``
     - AESDMA
     - IV
   * - ``+0x1e4``
     - AESDMA
     - key select (cipher / efuse / hardware key)
   * - ``+0x1fc``
     - AESDMA
     - status

PLL / analog init tables (mhal.ko)
----------------------------------

``mhal.ko`` carries static ``.data`` tables of register/value pairs
that program the analog PLLs the boot chain otherwise sets up - the
values the model does not have from tracing. Notable ones (``obs``):

* ``LPLLSettingTBL`` - the LCD PLL (pixel clock). Seven frequency
  settings, each six writes to registers ``0x3380``-``0x3396`` (a
  fixed ``0x2201``/``0x0420`` head plus a per-frequency divider
  ``0x41``/``0x42``/``0x43``/``0x83``/``0xf1``/``0xf2``/``0xf3`` and a
  post-divider ``0``-``3``). This is the PLL behind the LCD power
  bring-up at ``0x1f006400``/``0x006600`` (see :doc:`display`).
* ``ST_INIT_FPLL_CTRL_TBL`` - the frequency PLL.
* ``ST_INIT_HDMITX_ATOP_TBL`` / ``ST_INIT_IDAC_ATOP_TBL`` /
  ``ST_INIT_DAC_TGEN_TBL`` - HDMI TX and internal-DAC analog top and
  the TV-encoder timing generator (analog TV out; not used by the
  Miyoo LCD panel).

Dynamic (insmod) note
---------------------

Reloading modules to catch their register init does **not** help here:
every vendor module is already loaded ``[permanent]`` at boot, so its
init accesses were already captured (and mostly modelled). The two
non-permanent ones behave badly under emulation - ``mdrv_crypto``
``rmmod`` s cleanly but segfaults on re-``insmod``, and ``kdrv_sdmmc``
is pinned by the mounted SD card. Exercising an idle block (a crypto
op, video decode) would be the way to see those registers live; static
mining above already gives the map.

Display sub-blocks recovered from mhal
--------------------------------------

Disassembling the ``mhal`` HAL functions that touch the new display
banks names them (``obs``):

* ``0x1f225400`` - **display PQ (picture quality)**, the post-scaler
  enhancement block. ``+0x08``-``+0x44`` FCC colour correction
  (Cb/Cr transform tables T1-T8, from ``_HalDispPqSetFcc*``);
  ``+0x48``-``+0x6c`` peaking/sharpness (CTI, LTI, coring, two bands,
  from ``_HalDispPqPeaking*``); ``+0x58``/``+0x5c`` black/white level
  extension (BLE/WLE); ``+0x74``-``+0x7c`` chroma coring table.
* ``0x1f247400`` / ``0x247600`` / ``0x247800`` - **XC DIP**, the
  scaler input path. ``+0x160``/``+0x168`` interrupt enable/status
  (``HAL_XC_DIP_EnableIntr`` / ``GetIntrStatus``), ``+0x1f4`` HW idle,
  ``+0x160``/``+0x164`` frame count / channel (``GetHWFrameCnt`` /
  ``GetHWCh``).
* ``0x1f280c00`` / ``0x280e00`` - **MOP colour matrix**, the video
  plane's shadow-register colour adjust: ``SetVideoSatHueMatrix``
  (``+0x3c``), ``SetVideoContrastMatrix`` (``+0xbc``), initialised in
  ``0x40``-stride windows by ``HalMopsShadowInit``.
* ``0x1f345400`` - **MIPI DSI packet interface**, how panel DCS init
  commands are sent: ``HalPnlSetMipiDsiShortPacket`` /
  ``SetMipiDsiLongPacket`` at ``+0x00``/``+0x04`` (see :doc:`dsi`).

Video decoder (Wave5 VPU, ``0x1f344800``)
-----------------------------------------

``mi_vdec`` drives a Chips&Media **Wave5** VPU (WAVE517; its firmware
is ``/config/vdec_fw/normal/chagall.bin``, "chagall" being the Wave5
codename). Nothing on the stock firmware uses it - games are software
emulated and MainUI has no decoder - so it is idle at boot and never
appears in a trace. Exercising it needs a userspace ``MI_VDEC``
channel: the ``/proc/mi_modules/mi_vdec/mi_vdec0`` command node only
exists once a channel is created, and the VPU is only touched on
``MI_VDEC_CreateChn`` -> ``INIT_VPU`` (firmware load). A minimal harness
against the on-device ``/config/lib/libmi_vdec.so`` would trigger it,
but needs an armhf cross-toolchain to build.

The register map below was instead recovered statically from
``mi_vdec.ko``: the VPU is accessed through ``vdi_write_register(core,
addr, data)`` / ``vdi_read_register`` (``str data, [addr + base]``,
base ioremapped from the device tree), so the ``addr`` argument at each
call site is the register offset. Offsets from the VPU base (bank
``0x1a24``, ``~0x1f344800``); the host-interface block is a clean match
to the Wave5 layout (``obs``, some names inferred from that layout):

.. list-table::
   :header-rows: 1

   * - Offset
     - Name / note
   * - ``+0x00``
     - W5_PO_CONF
   * - ``+0x10`` / ``+0x14``
     - interrupt reason / clear
   * - ``+0x20`` / ``+0x24``
     - interrupt status / enable
   * - ``+0x48`` / ``+0x4c`` / ``+0x50``
     - remap control / virtual / physical address
   * - ``+0x60`` / ``+0x64`` / ``+0x68``
     - firmware code base / size / param (the ``chagall.bin`` load)
   * - ``+0x70``
     - **W5_VPU_BUSY_STATUS - confirmed** (polled 17x, the busy flag)
   * - ``+0x04``-``+0x44``, ``+0x6c``
     - other host-interface control (4-byte-aligned, unnamed)
   * - ``+0xc04``-``+0xeb4``, ``+0x1044``-``+0x1448``
     - command / return-value registers (read-mostly)

Caveat: this is static and approximate - the low ``0x00``-``0x70``
block is solid, the higher offsets are from single call sites and
some may be mis-attributed. A real decode run would be authoritative.

The VPU's **clock gating** is not in the VPU bank but in the main
clkgen (:doc:`clocks`): ``_MI_VDEC_Setclock`` (the ``setclk`` proc
command) drives ``0x1f207154`` (``dec_pclk``/``dec_aclk``) and
``0x1f2071f8`` (``dec_bclk``/``dec_cclk``) - the ``dec_*`` clocks
already listed there are the decoder's. ``mi_vdec`` also touches
``0x1f226e00`` ``+0x8c`` (in its interrupt handler) and ``+0xc0``, a
VPU interrupt/clock register in the ``0x226xxx`` clock group.

Next targets
------------

The crypto engine and the display sub-blocks above are fully mapped and
could be modelled as readback/behaviour banks. The remaining detail is
the audio ``bach`` register map in ``mhal`` (``HalBach*``/``HalAud*``,
base from the device tree so it needs the same call-site analysis) and
a dynamic VPU decode run (needs an armhf harness) to confirm the map
above.
