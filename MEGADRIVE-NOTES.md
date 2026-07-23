# Mega Drive machine bring-up journal

Goal: the `megadrive` machine currently exists to run a Mega Drive port of
Linux (loaded via the Everdrive-style setup). Extend it until a real
commercial game (Sonic the Hedgehog 1) plays completely, WITHOUT breaking
the Linux use case: new behaviour goes behind machine options, defaults
keep the current semantics.

Branch: `qemu-megadrive` (based on `amiga` @ e3580ae7ec).
Clone: `/workspace/src/qemu-megadrive` (dedicated; other sessions use
sibling clones - keep build dirs/ports/displays megadrive-unique).
Build dir: `build-megadrive/`.

## Reproduce

```sh
cd /workspace/src/qemu-megadrive
mkdir -p build-megadrive && cd build-megadrive
../configure --target-list=m68k-softmmu
ninja qemu-system-m68k
```

Run (on Daniel's VNC desktop, guest X display :1):

```sh
DISPLAY=:1 ./build-megadrive/qemu-system-m68k -M megadrive -bios <rom.bin>
```

## State as found (2026-07-20)

Machine (`hw/m68k/megadrive.c`):
- `-bios` image loaded into `machine->ram` = 4 MB *RAM* ("psram") at 0x0
  (models an Everdrive SSF2 mapper exposing PSRAM). ROM area is writable.
- 64 KB work RAM at 0xFF0000 (NOT mirrored across 0xE00000-0xFFFFFF).
- VDP at 0xC00000, Everdrive mailbox/mapper at 0xA130D0, IO at 0xA10000.
- Reset loads SP/PC from vector table at 0x0. CPU: 68000.

VDP (`hw/display/md_vdp.c`):
- Has: reg writes, control-word address setup, VRAM/CRAM/VSRAM r/w,
  renderer for planes A/B + window + sprites with scroll, H32/H40 width,
  60 Hz VINT timer, level-6 VINT (cleared by status read).
- Missing: DMA (68k->VDP, fill, copy), HV counter (reads 0), H-int
  (level 4 IRQ wired but never raised), scanline timing, plane/sprite
  priority bits, shadow/highlight, PSG (writes at +0x11 just logged),
  proper status bits (only FIFO_EMPTY|VBLANK|VINT).

IO (`hw/input/md_io.c`): version reg 0xA0 (overseas NTSC, no TMSS since
version nibble = 0), 3-button pad protocol with TH mux but pad state is
hardwired "nothing pressed" - no host input wired.

Everdrive (`hw/misc/md_everdrive.c`): mailbox command FIFO bridged to a
chardev + host dir; SSF2 bank regs at 0xA130D0-DF are stored but do NOT
remap anything (TODO in code).

Not present at all (Sonic 1 needs these to boot/play):
- Z80 area 0xA00000-0xA0FFFF (Sonic uploads its DAC driver to Z80 RAM).
- Bus request/reset 0xA11100/0xA11200 (Sonic busy-waits on bus grant).
- YM2612 at 0xA04000-7 (Sonic's SMPS driver polls the busy bit).
- VDP DMA - Sonic transfers ALL tile/sprite/palette data by DMA.
- Pad input (need Start + d-pad + ABC to actually play).

## Plan

1. Cartridge option: machine property `mapper` - default keeps today's
   PSRAM-at-0 Everdrive behaviour; `mapper=cart` maps the -bios image as
   read-only ROM at 0x0 (real cartridge), still 64K WRAM at 0xFF0000.
2. System control region: Z80 RAM + busreq/reset handshake + YM2612 stub.
3. VDP: DMA (transfer/fill/copy), HV counter + scanline-based V/H-int
   timing, priority bits in renderer.
4. Pad input via qemu input layer (keyboard: arrows + A/S/D = A/B/C,
   Enter = Start).
5. Iterate with Sonic 1 until fully playable; sound emulation (YM2612 +
   PSG audio output) is a stretch goal, tracked here.

## ROM

Sonic the Hedgehog (USA, Europe) REV00, No-Intro verified dump:
- `/workspace/src/megadrive-roms/sonic1.md`, 524288 bytes,
  md5 `1bc674be034e43c96b86487ac69d9293`
- source: archive.org item `sega-genesis-romset-ultra-usa`, file
  "Sonic The Hedgehog (USA, Europe).zip" (download OK'd by Daniel)

Run it:

```sh
DISPLAY=:1 ./build-megadrive/qemu-system-m68k -M megadrive,mapper=cart \
    -bios /workspace/src/megadrive-roms/sonic1.md \
    -qmp unix:/tmp/md-megadrive-play.qmp,server,nowait
```

Keys: arrows = d-pad, A/S/D = A/B/C, Enter = Start.

## Scripted playtesting

`scripts/md-playtest.py` (in this repo) drives a running instance over
the QMP socket: key injection, screendumps, guest-RAM peeks, and the
Sonic 1 REV00 level-select sequence.  Example:

```sh
./scripts/md-playtest.py --socket /tmp/md-megadrive-play.qmp \
    wait-title levelselect 3 sleep 5 shot /tmp/lz.ppm
```

(3 downs = Labyrinth 1, 19 = Special Stage; see the script header.)

NOTHING under /tmp needs to survive a VM reset: screendumps (.ppm/.png),
debug logs and QMP sockets there are all disposable and regenerable with
the commands in this file.  Everything durable lives in this repo, in
`/workspace/git/qemu.git` (branch `qemu-megadrive`), and in
`/workspace/src/megadrive-roms/`.  After a reset, rebuild per
"Reproduce" above (build-megadrive/ survives in the workspace anyway)
and relaunch with the command above; the desktop QEMU instance does not
survive a reset and must be restarted by hand.

## Log

### 2026-07-20 session 1
- Created branch + clone, surveyed code, wrote this journal.
- Bare boot of Sonic 1: double MMU fault - work RAM had no 0xE00000+
  mirrors (Sonic uses 0xFExxxx addresses) and nothing else was mapped.
- Commit 504bc110c4 "megadrive: enough hardware to run Sonic 1" - see
  the commit message for the full list (mapper=cart option, WRAM
  mirrors, open-bus region, md-sys Z80/arbiter/YM stub, VDP DMA +
  HV/VBLANK/H-int timing + priority renderer, keyboard pad,
  target/m68k ADDR24 masking).  Findings along the way:
  - QEMU's m68k core does NOT mask addresses to 24 bits; Sonic keeps
    tag bits in the top byte of pointers (e.g. reads via 0x0503bcf2
    = ROM 0x03bcf2).  Fixed properly in the CPU with a new
    M68K_FEATURE_ADDR24 (68000/68010 set it, 68020+ unsets) rather
    than hacking aliases into the machine.
  - Writes to a memory_region_init_rom region fault on m68k instead
    of being ignored; the open-bus background region absorbs those
    too (real MD has no bus-error generator at all).
  - Sonic's own "BUS ERROR" screen = its unhandled-exception handler;
    useful smoke signal that a QEMU-level access fault leaked in.
- RESULT: attract demo plays in Green Hill Zone, graphics look right
  (screendumps verified: SEGA logo, title, GHZ demo with HUD, water,
  scroll, sprite priorities).
- Verified end-to-end via QMP input events + screendumps + monitor
  peeks of Sonic's RAM (Game_Mode at 0xFFF600, pad vars at 0xFFF604):
  - Start at title begins the game; Sonic runs/jumps under control in
    GHZ (playtested by walking right and jumping over a crab).
    Gotcha for scripted tests: a Start press during the attract demo
    exits the demo and eats the press edge - press Start again AT the
    title (hold-to-catch-it does not work, the game wants an edge).
  - Level select cheat works (title: U D L R, then hold A + Start).
    REV00 menu order is GH, LZ, MZ, SLZ, SYZ, SBZ (LZ1 = 3 downs,
    SPECIAL STAGE = 19 downs).
  - Labyrinth Zone: plays; H-int fires (info irq shows level 4 = 556
    after a swim) and the game keeps running.
  - Star Light Zone and the Special Stage render and play.
- Still open:
  - underwater palette is applied per-frame, not per-line (whole
    screen tints when the beam is "below water"); needs a per-line
    CRAM snapshot in the renderer for the real split
  - sprite masking (x=0) and shadow/highlight not implemented
  - no audio at all (YM2612/PSG are stubs) - stretch goal
  - everdrive/Linux regression: default machine boots, but the Linux
    image itself has not been re-run (image location unknown to me)

## 2026-07-22 — the "Sonic on top of the grass" report: demo desync, not VDP priority

Daniel spotted Sonic rendered over the Marble Zone foreground grass
during the attract demo.  Instrumented the VDP mixing at pixel level
(probe data in the session log): sprite/plane priority extraction and
the mix order (sprite-hi > A-hi > B-hi > sprite-lo > A-lo > B-lo) are
all correct; at the reported pixels plane A is genuinely transparent
and the "grass" behind Sonic is the low-priority plane-B backdrop —
correct layering for a sprite at that position.  The real anomaly is
Sonic's POSITION: the attract demos are recorded joypad streams, and
input application depends on the exact lag-frame pattern the game
experiences.  Our VDP/DMA timing is not cycle-accurate, so the demo
diverges from the recorded path (Sonic goes airborne over the grass,
later roams the ruins interior) — the same class of desync the real
demos exhibit across console revisions.  Gameplay is unaffected.
A cycle-accurate 68k/VDP/DMA timing pass would be needed for
frame-exact demo playback; parked as a known limitation.
