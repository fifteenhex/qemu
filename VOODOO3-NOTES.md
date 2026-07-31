# Voodoo 3 3000 PCI emulation + Linux tdfxfb fixes — journal

Goal (Daniel's request):
- QEMU branch `intern-voodoo3` (this repo, clone of qemu.git): emulate a
  PCI Voodoo 3 3000 that *properly models voodoo/vga initialisation* —
  no image unless the card has been initialised properly.
- Linux branch `linux-tdfxfixes` (clone at /workspace/src/linux-tdfxfixes):
  make tdfxfb able to initialise a card that the video BIOS did NOT set
  up (today it relies on BIOS-programmed PLLs/DRAM config), boot it.

## State / what works

- `hw/display/voodoo3.c`: new device `-vga none -device voodoo3`.
  VGA-compatible (embeds QEMU standard VGA), Banshee/Avenger I/O register
  file, desktop scanout 8/16/24/32bpp (reuses the VGA core's VBE
  machinery), 2D engine subset used by tdfxfb (rectfill, s2s blt,
  host-to-screen mono expansion), hw cursor, DDC/EDID via bitbang I2C.
- Cold (no BIOS) power-up gating, this is the core of the model:
  - LFB (BAR1) reads 0xff / drops writes until: pllCtrl1 programmed to a
    plausible memory clock (50–250 MHz) AND dramInit0 AND dramInit1
    written AND at least one dramCommand write (SDRAM mode register set).
  - 2D engine discards commands until the above (gfx PLL not required,
    it falls back to a bypass clock at reset).
  - Desktop scanout additionally needs pllCtrl0 (video PLL, 15–350 MHz)
    and vidProcCfg VIDPROC_EN|DESK_EN with sane screensize/stride.
  - Legacy VGA scanout needs the DRAM init but NOT pllCtrl0 (verified
    against the real BIOS: it never writes pllCtrl0 for text mode).
  - Otherwise a black screen is shown (like a monitor without sync).
- **The real Voodoo3 3000 BIOS POSTs on the model** (SeaBIOS runs the
  option ROM, ROM does full DRAM/PLL init, text mode works, SeaBIOS/iPXE
  text visible).

## Repro / commands

- Build: `cd /workspace/src/qemu-voodoo3/build && ninja qemu-system-x86_64`
  (configured with `../configure --target-list=x86_64-softmmu
  --disable-docs --disable-user`)
- Test harness (qtest+QMP, 18 checks incl. screendump-blackness):
  `cd /workspace/src/voodoo3-test && python3 qtest_voodoo3.py`
- BIOS POST test (show on Daniel's desktop with DISPLAY=:1):
  `DISPLAY=:1 build/qemu-system-x86_64 -M pc -m 256 -vga none \
     -device voodoo3,romfile=/workspace/src/voodoo3-test/v3_3000.rom -display gtk`
- BIOS ROM: /workspace/src/voodoo3-test/v3_3000.rom = "Voodoo3 3000 BIOS
  2.15.12-SD", 64 KiB, downloaded (with Daniel's permission) from
  archive.org item `86-box`, file
  `86Box-Windows-64-b5634/roms/video/voodoo/3k12sd.rom`.

## Hardware findings (from the ROM + driver)

- The ROM validates its card by comparing PCI subsystem id with two words
  stored at the END of the ROM image (`[imagelen-8]`=subsys vendor,
  `[imagelen-6]`=subsys id, imagelen from PCIR+0x10). 3k12sd.rom wants
  121a:003a = Voodoo3 3000 PCI SDRAM. AGP 3000 is 121a:0036. On mismatch
  it writes vgaInit0=0x340 and exits without touching anything else
  (that was the "ROM runs but does nothing" symptom).
- ROM init writes observed (via `-trace 'memory_region_ops_*'` and
  register dump after POST): pciInit0=0x0584fb04, lfbMemoryConfig=0x1fff,
  miscInit1|=1, dramInit0=0x0c1fa9e9, dramInit1=0x4056c601 (bit30=SDRAM),
  dramCommand/dramData several times, pllCtrl1=0x720d (=166 MHz with
  f=14318*(n+2)/(m+2)>>k, n=[15:8] m=[7:2] k=[1:0]), vgaInit0=0x140.
  pllCtrl0 and pllCtrl2 NOT written for text mode.
- Linux tdfxfb expects the BIOS to have done: pllCtrl1/pllCtrl2 (its own
  writes are #if 0'd out!), dramInit0/1 (it only READS them to size
  memory in do_lfb_size(): draminit0 bit26 ? 8:4 chips, x 2 MB for V3),
  SDRAM mode reg init, miscInit0. It only programs pllCtrl0, VGA regs and
  the video processor. => on the cold model the stock driver sees 8 MB
  instead of 16 MB and gets a black screen (fbcon blits are discarded,
  LFB writes dropped). That's the bug to fix in linux-tdfxfixes.

## DONE: kernel side (branch linux-tdfxfixes)

- tdfxfb_hw_init() in tdfxfb.c: when the card is cold (pllCtrl1 not
  locked / dramInit0 zero) it does the BIOS's job: PLLs, dramInit0/1,
  SDRAM mode register, lfbMemoryConfig. Board values are read from the
  config table in the card's own ROM (see below) with a fallback to
  Voodoo3 3000 PCI POST values.
- 3dfx BIOS config table (verified on 6 ROM images: V3 2000/3000 PCI,
  V3 3500, Banshee, Velocity 200): search the image for the anchor
  05"4096K" 05"8192K" 06"16384K" 10, skip 3x3 bytes of size-string
  pointers, then: pciInit0, ?, miscInit1, dramInit0, dramInit1,
  agpInit, memPLL, ?, dramModeData (nine le32). Values match POST
  register dumps exactly (2000=143MHz, 3000=166MHz, 3500=183MHz).
- miscInit1 bit15 is set by BIOS/driver on SDRAM boards: it disables
  the SDRAM block-write optimisation, NOT the 2D engine (initial qemu
  model wrongly gated the 2D engine on it -> fbcon text vanished).
- The BIOS never programs pllCtrl0 (VGA modes use the VGA clock
  selects) nor pllCtrl2 (2D works off a bypass gfx clock at reset).

Kernel build needs libelf headers for objtool; host image has only the
runtime. Workaround in place (Daniel approved the download):
  ~/local/include + ~/local/lib/libelf.so symlink, build with
  export C_INCLUDE_PATH=$HOME/local/include LIBRARY_PATH=$HOME/local/lib
  make O=build -j$(nproc) bzImage
Consider `make install PKGS="libelf-dev"` on the host to make this go away.
Config: x86_64_defconfig + FB_3DFX{,_ACCEL,_I2C} + FRAMEBUFFER_CONSOLE +
LOGO, minus objtool-heavy opts (ORC unwinder->frame pointer, mitigations,
ftrace, IBT); build dir /workspace/src/linux-tdfxfixes/build.

## Test matrix (all verified 2026-07-30)

- stock driver + cold card: "memory = 8192K" (wrong) + black screen -
  faithfully reproduces the historic failure.
- stock driver + card POSTed by the real BIOS (romfile=v3_3000.rom):
  16384K, fbcon incl. accelerated scroll. Old behaviour intact.
- fixed driver + cold card, no ROM: fallback init, 16384K, fbcon works.
- fixed driver + cold secondary card with readable but unexecuted ROM
  (romfile=v3_3000_badsum.rom, bad checksum so SeaBIOS refuses to run
  it): "using the config table from its ROM", registers end up
  byte-identical to a real POST (pll1=0x720d, dramInit=0x0c1fa9e9/
  0x4056c601, pciInit0=0x0584fb04, agpInit=0x49e).
- fixed driver + warm card: detected, left alone.
- x86 quirk to know: for the boot VGA device Linux only reads the
  0xC0000 shadow (IORESOURCE_ROM_SHADOW from pci_fixup_video), never
  the ROM BAR - and when no VGA rom ran at all the flag can land on
  the wrong card. The ROM-table path is for secondary cards, which is
  also the case that matters on real hardware.

Boot command (visible on Daniel's desktop):
  DISPLAY=:1 /workspace/src/qemu-voodoo3/build/qemu-system-x86_64 \
    -M pc -m 512 -vga none -device voodoo3 \
    -kernel /workspace/src/voodoo3-test/bzImage-fixed \
    -initrd /workspace/src/voodoo3-test/initramfs.gz \
    -append "console=ttyS0 loglevel=7" -display gtk

## Maybe later

- interlace/half mode, second CLUT page, Windows cursor mode, vgaInit0
  bit9 legacy decode gate, 3D engine (nothing uses it here), qtest in
  tests/qtest/ instead of the python harness.

## Test files

- /workspace/src/voodoo3-test/: qtest_voodoo3.py (harness), v3_3000.rom,
  initramfs.gz + initramfs/ (busybox initramfs for kernel boot tests),
  dump-*.ppm screendumps.


## 3D engine (Avenger/SST core) — DONE (2026-07-30)

hw/display/voodoo3.c now implements the 3D rasteriser at BAR0+0x200000,
built from the 3dfx Glide3 spec (glide3x/h5/incsrc/h3regs.h + sst.h:
register offsets, fixed-point parameter formats, mode bitfields).

Implemented:
- full 3D register file 0x000-0x3ff
- direct triangles: [F]triangleCMD (0x080/0x100) with vertices + screen
  gradients, integer (12.4/12.12/20.12/14.18/2.30) and float forms
- setup unit: sBeginTriCMD/sDrawTriCMD (0x2a4/0x2a0), strip + fan
  (the path Glide3/Mesa use), packed ARGB or float colour
- pixel pipe: Gouraud / constant / point-sampled perspective texture
  (RGB332/565/ARGB1555/4444), depth test+write per fbzMode zfunc,
  alpha test, alpha blend
- fastfillCMD (0x124) colour+depth clear, swapbufferCMD (0x128) present
- 16bpp 565 colour buffer + 16bpp depth buffer

Validated (no OS, pure register streams):
- qtest_3d.py  : fastfill + Gouraud triangle -> RGB-interpolated tri
- qtest_3d2.py : depth ordering (near occludes far, far hidden) + a
  2x2 checker texture mapped across a quad. All PASS.

TODO for full fidelity / Linux DRI (Mesa tdfx via glide3):
- cmdFifo (0x000 AGP/CMD region) DMA submission path
- tiled framebuffer layout (lfbMemoryConfig tile bits); currently linear
- 32bpp colour + 24/32 depth (renderMode), wbuffer
- LFB read path bitfields (lfbMode) for glReadPixels
- fog, chroma-key, stencil
- combineMode / dual-TMU

## Test vehicle plan
Chose Option C then old-Linux-DRI: implement engine (done), then run a
real Voodoo3 3D app. GTA/nostalgic-demo use Glide2 (Voodoo1/2, SST-1
regs at BAR0+0) so they hang on a Voodoo3 - not usable. Next: an old
Linux distro (~2001) with XFree86 tdfx DRI + Mesa/glide3 running
glxgears/tuxracer, which drives exactly these 0x200000 SST registers.

## vblank / vsync + kernel misc interface + demo (2026-07-30)

Test vehicle became: my modern kernel (boots via -kernel; the old-2.4
Mandrake kernel hangs in real-mode setup under qemu -kernel, confirmed
device-independent by an -vga std A/B test) + tdfxfb + a nolibc demo.

- QEMU: voodoo3 now emulates a 60Hz vertical blank. swapbufferCMD with
  the wait-on-vsync bit queues a buffer swap performed at vblank
  (status[30:28] pending count); vblank raises the PCI IRQ when enabled
  via intrCtrl (0x200004), sets status[31] (PCIINTERRUPTED); ack by
  writing intrCtrl clear bit. status also reports VRETRACE.
- Kernel (linux-tdfxfixes): tdfxfb registers /dev/tdfx3d (misc device).
    * mmap -> maps BAR0 register aperture (init/2D/3D regs)
    * request_irq + vblank handler (ack, wake waitqueue, count)
    * ioctls TDFX3D_WAIT_VBLANK, TDFX3D_GET_VBLANK_COUNT
  ABI + SST register map in include/uapi/linux/tdfx3d.h (shared by the
  driver and the demo).
- Demo tools/tdfx3d/tdfx3d_demo.c (nolibc): 4 spinning depth-buffered
  Gouraud cubes bouncing off walls and each other, double-buffered,
  presented at vblank via TDFX3D_WAIT_VBLANK -> tear-free, silky. Fixed
  a face-triangulation gap (quads now tile as (0,1,3),(0,3,2)).
  Verified: card IRQ fires and paces the demo, cubes animate cleanly.

Build/run:
  # kernel (needs libelf headers for objtool, see above)
  C_INCLUDE_PATH=$HOME/local/include LIBRARY_PATH=$HOME/local/lib \
    make O=build -j$(nproc) bzImage
  make O=build headers_install
  # demo
  make -C tools/tdfx3d   (or the gcc line in the Makefile)
  # boot: -kernel bzImage -initrd <initramfs with demo as /init> \
  #   -append "tdfxfb.mode_option=640x480-16@60" -device voodoo3,romfile=...

## Tests exercise every implemented feature
qtest_3d_all.py checks (exact LFB pixels): fastfill; direct int + float
triangle paths; setup fan + strip; all 8 depth funcs; alpha test; alpha
blend; textures RGB332/565/ARGB1555/ARGB4444; constant colour; clip
rect; vsync (pending count, IRQ raise/clear, present). All pass. Plus
qtest_voodoo3.py (2D/init/cold-gating) and qtest_3d.py/qtest_3d2.py.
Not yet modelled (documented TODO): cmdFifo DMA, tiled fb, 32bpp, fog,
chroma-key, stencil, dual-TMU/combineMode.

## Disk: working clones dedup'd against the bare repos
qemu-voodoo3/.git and linux-tdfxfixes/.git now use
/workspace/git/<name>.git/objects as an alternate (objects/info/
alternates) + repack -adl: 696M->14M and 6.5G->13M.

## Amiga/Mediator port: black tdfxfb scanout — FIXED (2026-07-31)

Symptom: `-M a4000 -device mediator4000 -device voodoo3` with the m68k
kernel (/tmp/linux-m4k, branch amiga-laptop-20260627) and
`video=tdfxfb:640x480-16@60 fbcon=map:1`: mode set appears to work
("Console: switching to colour frame buffer device 80x30", surface is
640x480) but the screendump is pure black.

Root cause was NOT the hypothesised LFB tiled-address swizzling — BAR1
is a straight linear mapping of the vram RAM region (no tiling on the
LFB path at all; qtest_probe.py's vram-roundtrip relies on that).  The
640x480 surface was the *blank fallback* surface, not a live scanout:
`voodoo3_mem_ok()` was false the whole time, so the LFB read 0xff /
dropped writes (lfb_dead) and update_mode forced V3_MODE_BLANK.
Runtime evidence: with the guest up, `xp/16wx 0x52000000` (BAR1
through the Mediator window) read all-0xffffffff while vidProcCfg /
vidScreenSize / vidDeskStride were all correctly programmed
(0x08040483 / 640x480 / 1280).

Why: this branch's tdfxfb_cold_init() (Daniel's real-hardware Amiga
port; a different, simpler cold init than linux-tdfxfixes') programs
pllCtrl1 (0x0574D03 = ~70 MHz, inside the model's 50-250 MHz window)
and writes back dramInit0/dramInit1 with the refresh-enable bit
(dramInit1 |= BIT(15)) — but never issues a dramCommand write, which
the model required before declaring the SDRAM alive.  Real hardware
evidently comes up with just refresh enabled (that driver works on the
real Mediator+V3 board); only the BIOS does the explicit mode-register
set via dramCommand.

Fix (hw/display/voodoo3.c): voodoo3_mem_ok() now accepts the DRAM as
initialised when EITHER a dramCommand write happened (BIOS path,
unchanged) OR dramInit1 bit 15 (refresh enable) is set (Amiga tdfxfb
path).  Cold gating is otherwise untouched: qtest_voodoo3.py's
lfb-still-dead-before-dramcmd check writes dramInit1=(1<<30) (no bit
15) and still passes.

Also fixed to be able to regression-test: hw/m68k/Kconfig MC68EZ328 /
MVME147 / MEGADRIVE had `default y` without `depends on M68K`, which
dragged DragonBall/MVME/Lance devices into every target and broke
x86_64-softmmu builds of this tree (dragonball_intc.c uses M68kCPU).

Regression evidence (x86_64 build of THIS tree, build-x86/):
  - qtest_voodoo3.py: 18/18 PASS (incl. cold gating + screendumps)
  - qtest_probe.py: output diff-identical to tdfx_probe.expected
    (tiled/linear texture addressing semantics unchanged)
  - qtest_3d_all.py: all feature checks pass
  - qtest_3d.py/qtest_3d2.py fail identically on the ORIGINAL
    qemu-voodoo3 binary (stale early scripts: their bring-up predates
    the dramCommand gating and writes neither dramCommand nor bit 15;
    superseded by qtest_3d_all.py) — no behaviour change vs baseline.

Repro/evidence on m68k: boot line as above + QMP screendump
{"device":"gpu"} → /tmp/v3fix-final.png shows Tux, boot text and the
expected root-mount panic trace in correct colours (before the fix:
/tmp/v3fix-before.png, all black).
