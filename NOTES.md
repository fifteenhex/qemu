# 3dfx Voodoo 3 3000 (Avenger/SST) QEMU model — journal

`hw/display/voodoo3.c`: a PCI Voodoo 3 3000 device (`-vga none -device
voodoo3`) that models voodoo/VGA initialisation faithfully — no image until
the card has been brought up properly — plus the full 2D and 3D engines.

## State / what works

- VGA-compatible (embeds the QEMU standard VGA), Banshee/Avenger I/O register
  file, desktop scanout at 8/16/24/32bpp (reuses the VGA core's VBE
  machinery), a 2D engine (rectfill, screen-to-screen blt, host-to-screen mono
  expansion, colour host blits), hardware cursor, DDC/EDID via bitbang I2C.
- **Cold (no-BIOS) power-up gating** — the core of the init model:
  - LFB (BAR1) reads 0xff / drops writes until pllCtrl1 is programmed to a
    plausible memory clock (50–250 MHz) AND dramInit0 AND dramInit1 are
    written AND at least one dramCommand write (SDRAM mode register) has
    happened.
  - The 2D engine discards commands until the same point (the gfx PLL is not
    required; it falls back to a bypass clock at reset).
  - Desktop scanout additionally needs pllCtrl0 (video PLL, 15–350 MHz) and
    vidProcCfg VIDPROC_EN|DESK_EN with a sane screensize/stride.
  - Legacy VGA scanout needs the DRAM init but NOT pllCtrl0 (the real BIOS
    never writes pllCtrl0 for text mode).
  - Otherwise a black screen is shown (like a monitor without sync).
- **The real Voodoo3 3000 BIOS POSTs on the model** — SeaBIOS runs the option
  ROM, the ROM does full DRAM/PLL init, text mode works, SeaBIOS/iPXE text is
  visible.

## 3D engine (Avenger/SST core at BAR0+0x200000)

Built from the 3dfx Glide3 spec (`glide3x/h5/incsrc/h3regs.h` + `sst.h`:
register offsets, fixed-point parameter formats, mode bitfields). Implemented:

- Full 3D register file 0x000–0x3ff.
- Setup unit — sBeginTriCMD/sDrawTriCMD assembled as a triangle strip/fan (the
  path Glide3/Mesa use), packed-ARGB or float colour; plus the direct
  [F]triangleCMD paths with explicit screen gradients.
- Pixel pipe — Gouraud / constant / perspective-correct textured, full
  fbzColorPath + textureMode colour/alpha combine units, depth test+write per
  fbzMode zfunc, W-buffer + depth-bias, alpha test, alpha blend, chroma-key
  (exact + range), fog, ordered dithering (4x4 / 2x2 Bayer), stipple, Y-origin
  swap.
- Textures — RGB332/565/ARGB1555/ARGB4444, palette (P8/P8_RGBA/AP88), NCC
  (YIQ/AYIQ); tiled texture memory addressing with per-level mip offset;
  point/bilinear sampling, and trilinear with an analytic /w-space LOD
  derivative and a nearest-mip floor.
- Second TMU (single-pass dual-texture multitexturing) with the MLOD/MLODFRAC
  combine factor sources.
- fastfillCMD colour+depth clear, swapbufferCMD present; 16bpp 565 colour
  buffer + 16bpp depth buffer.
- 60 Hz vertical blank: swapbufferCMD with the wait-on-vsync bit queues a swap
  performed at vblank (status[30:28] pending count); vblank raises the PCI IRQ
  when enabled via intrCtrl, sets status[31], ack by writing the clear bit;
  status also reports VRETRACE. Plus a 3D-idle interrupt (intrCtrl idle
  enable/clear/pending) and an opt-in first-order engine-timing model
  (reflected in the STATUS command-FIFO-free field; fill timing defaults off so
  the frame rate stays interactive).
- Multithreaded triangle rasteriser (worker pool), clamped to the colour-buffer
  width.

## cmdFifo0 DMA command submission

- A DMA command ring the card pulls from: the VRAM ring with the PKT0/PKT1
  parser (control / register bursts), PKT3 native setup-unit vertex packets,
  and PKT5 linear data bursts (e.g. texture uploads written straight into
  VRAM).
- AGP mode places the ring in system RAM, read back over PCI DMA.

## Build & test

- Build: `cd build && ninja qemu-system-x86_64`
  (configured `../configure --target-list=x86_64-softmmu --disable-docs
  --disable-user`).
- qtests (no OS, pure register streams, exact LFB-pixel checks):
  - `qtest_3d_all.py` — fastfill; direct int + float triangle paths; setup fan
    + strip; all 8 depth funcs; alpha test/blend; textures
    RGB332/565/ARGB1555/ARGB4444; constant colour; clip rect; vsync (pending
    count, IRQ raise/clear, present).
  - `qtest_3d.py` / `qtest_3d2.py` — Gouraud + depth ordering + a checker
    texture across a quad.
  - `qtest_probe.py` — mirrors the hardware-reference feature batch.
  - `qtest_vbios.py` — cold-boot via the synthesised VBIOS.
- BIOS POST (visible on `DISPLAY=:1`):
  `DISPLAY=:1 build/qemu-system-x86_64 -M pc -m 256 -vga none \
     -device voodoo3,romfile=/workspace/src/voodoo3-test/v3_3000.rom -display gtk`
- BIOS ROM: `/workspace/src/voodoo3-test/v3_3000.rom` = "Voodoo3 3000 BIOS
  2.15.12-SD", 64 KiB, from archive.org item `86-box`, file
  `86Box-Windows-64-b5634/roms/video/voodoo/3k12sd.rom`.

## Hardware findings (from the ROM + silicon)

- The ROM validates its card by comparing the PCI subsystem id with two words
  at the END of the ROM image (`[imagelen-8]`=subsys vendor,
  `[imagelen-6]`=subsys id; imagelen from PCIR+0x10). 3k12sd.rom wants
  121a:003a (Voodoo3 3000 PCI SDRAM); the AGP 3000 is 121a:0036. On mismatch it
  writes vgaInit0=0x340 and exits without touching anything else (the "ROM runs
  but does nothing" symptom).
- ROM init writes observed (via `-trace 'memory_region_ops_*'` + a register
  dump after POST): pciInit0=0x0584fb04, lfbMemoryConfig=0x1fff, miscInit1|=1,
  dramInit0=0x0c1fa9e9, dramInit1=0x4056c601 (bit30=SDRAM), dramCommand/
  dramData several times, pllCtrl1=0x720d (=166 MHz, f=14318*(n+2)/(m+2)>>k with
  n=[15:8] m=[7:2] k=[1:0]), vgaInit0=0x140. pllCtrl0 and pllCtrl2 are NOT
  written for text mode.
- miscInit1 bit 15 disables the SDRAM block-write optimisation, NOT the 2D
  engine (an early version of the model wrongly gated the 2D engine on it, so
  text vanished).
- The BIOS never programs pllCtrl0 (VGA modes use the VGA clock selects) nor
  pllCtrl2 (2D runs off the reset bypass gfx clock).

## Hardware-validated corrections

Behaviours confirmed against real silicon with userspace register probes and
folded into the model:

- nearest-mip floor + analytic /w-space texcoord LOD derivative (was rounding /
  float LOD);
- the 2x2 ordered-dither Bayer matrix;
- the alpha-blend COLOR factor and the top-left fill rule;
- the fog blend and the bilinear shift-decode range;
- chroma-key range matching;
- the 8-wide × 4-tall pattern-mode stipple;
- TMU1 as the downstream dual-TMU combiner.

## Known divergences / not modelled

- **P8_RGBA palette.** Decoded through the same palette path as P8/AP88, but
  silicon uses a separate RGBA palette/download for P8_RGBA (P8 and AP88 are
  bit-exact against hardware, P8_RGBA is not). Low impact — the RGB is
  redundant with P8.
- **Line/rotate stipple.** The non-pattern stipple is modelled as an
  absolute-x 32-wide pattern; silicon *rotates* it — consecutive scanlines flip
  bit order and the phase tracks the span start. Legacy dashed-line feature;
  needs a line-drawing probe to pin the per-pixel rotate step.
- **Fixed-point setup/rasterisation.** The model interpolates in float; silicon
  uses fixed-point, so Gouraud, edge coverage and texcoords differ by ≤1 LSB.
  This is the source of the residual whole-frame checksum differences.
  Cosmetic; matching it exactly means porting the fixed-point setup math.
- **Not modelled** (out of scope / infrastructure): LFB pixel-pipeline writes
  (the Glide glDrawPixels/readback path — the linear aperture covers the normal
  case); big-endian swizzle (miscInit0 30/31); dramInit memory sizing (always
  16 MB); a non-565 3D colour/depth pipeline (8/24/32bpp desktop scanout works,
  but the 3D colour/depth buffers are 16bpp); Windows hardware-cursor mode;
  interlace/half mode; the second CLUT page; the native 2D line opcode.
