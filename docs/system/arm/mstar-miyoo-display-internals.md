# How the Miyoo Mini display was made to render

This is the engineering account of *why* the Miyoo Mini's UI shows up in QEMU -
the model details and the reverse-engineering findings that turned a black
screen into the rendered MainUI menu. For the step-by-step boot/repro, see
`mstar-miyoo-display.md`.

The vendor userspace (MainUI, SDL 1.2) draws into `/dev/fb0`; the kernel's
sstar fbdev/DRM stack programs the **GOP** (Graphics Output Plane) at
`0x1f246800` to scan that framebuffer out. Getting a picture required modelling
that plane correctly *and* clearing three separate things that otherwise kept
the screen (or the whole boot) dark. Each is below with the symptom that pinned
it.

## 1. The GOP plane register layout and scanout

The GOP is an RGB primary plane that DMAs a linear framebuffer from DRAM to the
display. `TYPE_MSTAR_GOP` (`hw/display/mstar_gop.c`) stores the 16-bit-lane
registers (RIU: register N at byte `N/4 * 4`) and, each frame, reads the window
config and blits the framebuffer to its QEMU console. The offsets that matter
(same on SSD202 and mercury5):

* `0xc0` STRETCH_W - output width, stored as `crtc_w >> 1` (so `<< 1` to use it)
* `0xc4` STRETCH_H - output height
* `0x200` WIN0 - bit0 = plane enable, bits[7:4] = source pixel format
* `0x204/0x208` WIN0 addr low/high
* `0x224` WIN0 pitch, in 16-byte units

Two decode details were the difference between garbage/black and a correct
image, found by comparing the live register values MainUI programs against what
landed on screen:

* **The address register is DRAM-relative (MIU), not a full physical address.**
  MainUI programs addr `0x672100` with a `<<4` shift; the framebuffer is at
  `MSTAR_DRAM_BASE + (0x672100 << 4) = 0x26721000`, i.e. you must add
  `0x20000000`. Treating it as an absolute address scanned out the wrong memory.
* **The primary plane is 32bpp ARGB8888, not RGB565.** MainUI sets WIN0
  `0x4051` -> format field `5` = ARGB8888 (mainline DRM fbcon would use `1` =
  RGB565; `7` = ABGR8888). The draw-row path decodes fmt 5/7 as 32bpp
  (ARGB stores B,G,R,A in memory) and everything else as RGB565. Pitch `0xa0`
  `<< 4` = 2560 = 640*4 confirms 4 bytes/pixel.

## 2. The missing GOP/fbdev vsync interrupt (fb0 open blocked)

Symptom: opening `/dev/fb0` blocked forever - `dd if=/dev/fb0` hung from a bare
shell too, so it was a kernel fop block, not a MainUI bug. `sstar_FB_WaitForVsync`
does `wait_event(wq_vsync, counter++)`, and the counter is only bumped by the
fbdev vsync ISR. Per `/proc/interrupts` that ISR (`fbdev_dispVsync`) is on
**MS_MAIN_INTC line 52**, which was firing zero times; the model was only
raising the *display-top* vsync on line 82.

Note the numbering: a guest MS_MAIN_INTC line = the model's intc HWIRQ + 32, so
line 52 = HWIRQ **20** (`MSTAR_DISP_GOP_HWIRQ`). The GOP device raises this as a
separate output, pulsed once per vblank: assert it, then lower it ~200us later
via a one-shot timer. The mst-intc is level-based, so an instantaneous 1->0
pulse would be lost, and holding it high would storm the un-acked shared
handler; a short pulse latches exactly once. The ISR has no status register (it
just increments + wakes), so no ack register is modelled. With that,
`sstar_FB_WaitForVsync` returns, fb0 opens, and MainUI reaches its render loop.

## 3. The security element (kernel would otherwise BUG())

The 4.9 kernel runs a crypto challenge against the i2c1 `@0x3d` "alpu-fa"
security element during bring-up and `BUG()`s if it fails. Emulating that chip
(`hw/i2c/mstar_secelem.c`) lets the challenge pass, so the **unpatched** vendor
kernel boots (earlier a hand-patched image NOP'd the BUG; that is no longer
needed). MainUI's own userspace auth (via `librsautil.so`) runs the same kind of
challenge and `exit(2)` crash-loops without it - so this unblocks both the
kernel boot and MainUI staying alive.

## 4. The 180-degree panel flip

The Miyoo Mini's panel is mounted upside down, so the firmware writes the
framebuffer already rotated 180 degrees. With `flip` set (a device property, set
by the `miyoomini` machine), the rendered surface is reversed so a screendump
shows what is physically on the panel. A 180-degree rotation of a row-major
image is just a reversal of its pixels.

## Putting it together

With the plane decoded (1), the fb0-open unblocked (2), the kernel/MainUI auth
satisfied (3) and the flip applied (4), MainUI's SDL surface lands in DRAM at
`0x26721000` as 640x480 ARGB8888 and the GOP scans it out to console 0 - the
"Miyoo Linux Games" menu (Game / RetroArch / App / Setting). The GE 2D blitter
(`hw/display/mstar_disp.c`) is what MainUI composites that menu *with* before it
reaches the framebuffer; the GOP then just scans out the result, which is why a
screendump shows ~99% non-black instead of a blank plane.
