# Macintosh 128K QEMU machine — journal

Goal: boot Mac System 1.1/2.0 from a 400K floppy image to a working
Finder on a new `mac128k` machine, with a moving/clicking mouse.

Working tree: worktree of `/workspace/src/qemu-amiga` (branch `amiga`).
Build dir: `build/`.  Session-unique names use the `mac128k-` prefix;
QMP socket `/tmp/qmp-mac128k.qmp`, screenshots `/tmp/mac128k-*.png`.

## Assets (`/workspace/src/qemu-amiga/_mac_assets/`)

- `Mac128K.ROM` — 64KB Rev A ROM, checksum dword `28BA61CE` at offset 0
  (doubling as the garbage reset SP), reset PC dword at offset 4 =
  `0040002A` — an *absolute* address, the ROM hardcodes its 0x400000
  base, so the overlay only matters for the vector fetch.
- `system11.img` / `system20.img` / `system20-tools.img` — raw 400K
  (409600 byte) MFS floppy images.

## Hardware model

- 68000 @ 7.8336 MHz (QEMU's m68000 has M68K_FEATURE_ADDR24, so the
  top address byte is ignored like on the real bus — tagged Memory
  Manager pointers work).
- RAM 128KB at 0, mirrored through the whole 0x000000-0x3FFFFF window:
  the ROM sizes memory by looking for the wrap-around.
- ROM 64KB at 0x400000, mirrored through the 1MB block.
- Overlay: at reset ROM is aliased at 0 and RAM appears at 0x600000
  (mirrored over 0x200000); cleared when the ROM writes VIA PA4 low.
  Input pins with DDRA=0 read as pulled up, which conveniently makes
  the overlay default on when the VIA resets.
- Everything unmapped is OPEN BUS (reads 0), never a bus error — the
  128K has no bus error generator, and the early ROM runs with SP=0
  pushing exception frames into the 0xFFxxxx bit bucket.
- VIA 6522 at 0xEFE1FE, reg N at +N*512 (window at 0xEFE000, reg =
  (offset>>9)&0xF).  Port A: 0-2 volume, 3 sound page, 4 overlay,
  5 SEL to the floppy, 6 screen page (1 = main), 7 SCC wait (in).
  Port B: 0-2 RTC data/clock//enable, 3 mouse button (in, 0=down),
  4/5 mouse X2/Y2 (in), 6 hblank (in), 7 sound enable.  Timers run at
  783.36 kHz.  60.15Hz VBL on CA1, one-second RTC tick on CA2.
- SCC Z8530: read space base 0x9FFFF8, write space base 0xBFFFF9
  (both decode B-ctrl/A-ctrl/B-data/A-data at +0/2/4/6; the write
  window forwards to the same escc).  escc it_shift=1 bit_swap=true,
  chn[0]=B chn[1]=A.  Mouse X1/Y1 → DCD A/B: added "dcd" gpio inputs
  to escc.c that latch RR0.DCD, raise ext/status ints (WR1 bit0 +
  WR15 bit3) and set the RR2B modified vector (ext A=0x0A/0x50,
  ext B=0x02/0x40 for status-low/high); cleared by WR0=0x10.
- IWM at 0xDFE1FF, reg N at +N*512: see hw/block/iwm.c header for the
  latch scheme and the Sony drive protocol.  ROM equates: ph0L=+0x000,
  ph0H=+0x200 ... mtrOff=+0x1000, mtrOn=+0x1200, intDrive=+0x1400,
  extDrive=+0x1600, q6L/H, q7L/H.
- Video: 512x342 1bpp, white=0, main buffer at ram_top-0x5900 (alt at
  -0xD900), framebuffer console scans main RAM directly (the
  framebuffer helpers handle VGA dirty logging on a plain RAM
  region — same trick as maciisi-fb but without a dedicated VRAM).
- Sound: not modelled (PB7/PA3 ignored).  The 400K drive's PWM speed
  control is irrelevant because the tach always reports the nominal
  zone speed.
- Keyboard: not modelled; the ROM's model-number query times out
  harmlessly (like a real Mac with the keyboard unplugged).

## Iteration log

### Skeleton to insert-disk icon (2026-07-22)

- First boot attempt executed from address 0 as zeroes: the m68k CPU
  reset does NOT load SP/PC from the vector table, and the ROM
  loader's reset callback (which copies the -bios image into the ROM
  region) runs AFTER the machine's qemu_register_reset handler — so
  ldl_phys(0x400000) at reset time read 0.  Fix: stash the two vector
  dwords at init via rom_ptr() (maciisi does the equivalent with
  stl_phys into RAM).
- With that, the ROM runs: RAM test fills the screen with noise
  (visible in the framebuffer — video path proven), memory sizing
  probes the mirrors, overlay clears, the OS world starts (A-line
  traps, level-1 VBL interrupts), and the machine reaches the
  **blinking insert-disk floppy icon** on the gray desktop.
  Milestone (a).  /tmp/mac128k-shot2.png.
- Early-ROM curiosity: the start code runs with SP=0, deliberately
  pushing into the top-of-address-space bit bucket; first insn at 0
  before the fix decoded as `movel %pc@(0x61d0),%a4@` = the checksum
  dword — nice fingerprint that the vectors weren't loaded.
