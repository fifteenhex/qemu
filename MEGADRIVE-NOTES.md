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

## Log

### 2026-07-20 session 1
- Created branch + clone, surveyed code, wrote this journal.
