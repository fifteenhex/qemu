# Amiga 3000/4000 emulation — working notes

Status and hand-off notes for the `amiga` branch (based on `mvme147`,
which contributes the 68030 bus-error/trace fixes and the WD33C93
SCSI model).

## Getting the firmware

The machine needs a plain 512KB Kickstart ROM image, passed with
`-bios`.  Developed and tested against Kickstart 3.1 r40.068 for the
A3000:

    curl -L -o kick31_a3000.zip 'https://archive.org/download/commodore-amiga-firmware/Kickstart%20v3.1%20r40.068%20%281993-12%29%28Commodore%29%28A3000%29%5B%21%5D.zip'
    unzip kick31_a3000.zip     # extracts the .rom file

(archive.org item `commodore-amiga-firmware`; the front door sometimes
returns a transient 500 — retry, or use the `ia903109.us.archive.org`
mirror URL it redirects to.)

Checksums of the known-good image (512KiB):

    md5  413590e50098a056cfec418d3df0212d
    sha1 f8e210d72b4c4853e0c9b85d223ba20e3d1b36ee

Sanity check: starts `11 14 4e f9 00 f8 00 d2` (Kickstart magic +
reset PC 0xf800d2), contains "AMIGA ROM Operating System and
Libraries".  SuperKickstart (disk-loaded) images are not supported.

The `a4000` machine needs the A4000 Kickstart 3.1 r40.068 instead (also
512KB, same `commodore-amiga-firmware` item):

    md5  9bdedde6a4f33555b4a270c8ca53297d

Run it with `-M a4000 -bios kick31_a4000.rom`; disks attach the same way.

## Running

    qemu-system-m68k -M a3000 -bios kick31_a3000.rom
    # optional disk: -drive if=scsi,file=hd.img,format=raw
    # serial (Paula):  -serial stdio

## Current state (2026-07-19, evening)

Kickstart 3.1 boots all the way to the insert-floppy screen with the
animation running (560x145 hires, 3 planes, purple background; the
disk animates into the drive).  Chip RAM (2MB) and fast RAM (-m, up
to 16MB below 0x08000000) are detected correctly; exec multitasks;
scsi.device initialises and probes the bus.

Floppy boot works: `-drive if=floppy,file=x.adf,format=raw` gives a
DF0 (hw/m68k/amiga_fdc.c) that encodes ADF tracks to AmigaDOS MFM on
the fly for Paula disk DMA, and decodes writes back.  Lemmings (cr
Skid Row) boots through cracktro, Psygnosis/DMA logos and title to
the playable intro; both trackdisk.device and the game's own
trackloader work.  The copper is now line-granular (display writes
journalled by WAIT line, replayed by the renderer), so per-line
palettes and split screens render.  A mouse is wired to gameport 0.
Watch out: guest *byte* accesses to custom registers needed impl
min_access_size 1 with the split done in the device — a trackloader
polled INTREQR's low byte and QEMU's widening returned the wrong
half.

Structure: abstract `amiga-common` machine class (hw/m68k/amiga.c)
holds everything all classic Amigas share — chip RAM, Kickstart +
reset overlay (CIA-A PA0), the two 8520 CIAs, the custom chip block,
open-bus filler.  `a3000` (hw/m68k/a3000.c) adds the 68030, fast RAM,
Ramsey/Gary stubs, Zorro III open bus and the SCSI subsystem.
Devices: hw/m68k/mos8520.c (CIA), hw/m68k/amiga_custom.c (interrupts,
beam counters, serial, blitter incl. line mode and fills, frame-atomic
copper, bitplane display renderer), hw/m68k/a3000_sdmac.c (SuperDMAC
with the wd33c93 behind it, INT2).

See docs/system/target-m68k.rst for the user-facing feature list and
the individual commit messages for design details and the bugs found
(notably: this tree requires device_class_set_legacy_reset() — a bare
`dc->legacy_reset =` assignment is never called; and ptimer trigger
callbacks run inside the timer's transaction).

## Open items / next steps

- SCSI works, incl. writes: three wd33c93 bugs fixed in sequence —
  (1) select-and-transfer-at-0x46 was mis-run as a fresh all-zero-CDB
  command; (2) the ending disconnect interrupt was never raised; (3)
  writes deferred: the data-out transfer completed (interrupt) before
  the async block write landed, so command_complete moved the bus to
  status with no interrupt and the guest hung.  HDToolBox/HDSetup now
  partitions AND formats a blank drive end to end (real RDB+PART+FSHD
  +FFS written by the guest), volume mounts on the desktop.
  NOTE: the A3000 factory drive is SCSI unit **6** — HDSetup/HDToolBox
  default there, so attach test disks with `-drive if=scsi,unit=6`.
  Recording/replay of a GUI session: `_amiga_assets/replay_input.py`
  + a log from `-trace input_event_rel -trace input_event_btn`.
  HD BOOT WORKS: with only `-drive if=scsi,unit=6` and no floppy,
  Kickstart boot-scans the RDB, mounts the bootable FFS partition and
  (no startup-sequence) drops to the AmigaDOS CLI.  A full
  Workbench-on-HD still needs the Install program to copy the OS.
- Workbench text: FIXED — graphics.library uses the ECS BLTCON0L
  register (0x5a, minterm-byte-only write) once it sees our ECS
  chipset IDs; it was landing in the bare backing store.  Beware of
  other silently-swallowed ECS registers.  gdb notes: the gdbstub
  byte-swaps REGISTER values and MEMORY reads both (reverse each
  long); breakpoint addresses are fine; read guest memory via QMP
  pmemsave while stopped for untangled bytes.
- Keyboard: DONE (hw/m68k/amiga_kbd.c).  Feeds Amiga raw keycodes
  into CIA-A's SDR; CIA-A gained an 'sp-out' line pulsed when the
  guest drives SPMODE to output (keyboard.device's ack handshake),
  which advances the key queue.  Host key events are Linux keycodes,
  so map via qemu_input_linux_to_qcode() first.  Verified live: booted
  to the AmigaDOS CLI and typed at the '1>' prompt.
- Window close gadget: Daniel reports it doesn't work.  Investigated
  via synthetic mouse — window DRAG (title bar) works, but the close
  gadget never highlights/closes even with the pointer's hotspot
  apparently in the ~16px box (many precise tries; calibrated ~1
  count/px X, ~2 counts/px Y, corner=logical 0,0).  Real
  close-gadget/RELVERIFY bug or sub-pixel synthetic-aim miss — needs a
  human repro or Intuition-MouseXY-based positioning to settle.  Icon
  double-click, app gadgets and window drag all work.
- Games: Lemmings and The Secret of Monkey Island both boot and play
  (MI reaches full SCUMM gameplay on Melee Island; run-monkey.sh).  A
  Workbench flicker (a fifth of frames blank) was fixed by running the
  copper at render time instead of at the vblank, since Intuition
  swaps its double-buffered copper lists (COP2LC) in the VERTB handler
  that runs after our old vblank copper pass.
- Display gaps: attached sprites, sprite/playfield priority
  (BPLCON2), BPLCON1 fine scroll, HAM, dual playfield.  The copper is
  line-granular; effects keyed to the horizontal beam position won't
  render.  DMA sprites render (Lemmings is playable, menu and all).
- Zorro: II autoconfig works (hw/m68k/amiga_a2065.c) with the A2065
  Ethernet card (Am7990 LANCE via QEMU's pcnet core, 32KB onboard
  RAM, MAC 00:80:10:<serial>).  AmigaOS autoconfigures it at 0xe90000
  (ConfigDev + CSR0=STOP verified); real TX/RX untested (needs a
  SANA-II driver + TCP stack, not in stock WB).  Base write is a plain
  byte to config reg 0x48 (NOT nibble-encoded like the read side).
- Not modelled: battery clock (RP5C01 at 0xdc0000, currently open
  bus), Zorro III cards.  Floppy: only 880KB DD ADFs.  Disk swap works
  now (blockdev-change-medium device=floppy0 filename=... format=raw
  -> /CHNG latch -> Workbench remounts); needed for the HD install.
- A4000: an `a4000` machine (hw/m68k/a4000.c) boots.  It is a 68040
  with 2MB chip RAM, the AGA chipset IDs (Alice VPOSR 0x23, Lisa Denise
  0xf8), motherboard fast RAM at 0x07000000, and the shared Ramsey/Fat
  Gary glue (now TYPE_AMIGA_MOBO in hw/m68k/amiga_mobo.c, used by both
  big-box machines).  QEMU's m68040 boots the A4000 Kickstart to the
  insert-disk prompt and runs Workbench 3.1 from floppy — so the 68040
  MMU and the AGA chip IDs are fine.  Two follow-ups for a "real" A4000:
  (1) the onboard IDE (an ATA port at 0xdd2020, Gayle-ish; currently
  open bus so no HD), and (2) AGA display (8 bitplanes, 256-colour
  24-bit palette, HAM8, FMODE/sprite-res) in amiga_custom.c — for now
  the OS renders through the ECS path.  Note: the insert-disk boot
  screen renders tiled horizontally on AGA IDs (a display-setup quirk),
  but Workbench itself renders correctly.
- mvme147_pcc has the same latent `dc->legacy_reset =` bug that bit
  the Amiga devices; its reset has never actually run.

## Debugging recipes that worked

- Screenshots headless: `-display none -qmp unix:/tmp/qmp,server,nowait`,
  then QMP `screendump` (writes PPM; convert with PIL).  screendump
  drives the renderer, no display needed.
- Guest introspection via QMP human-monitor-command `xp`:
  SysBase = *(0x4); ThisTask = SysBase+0x114; task name via node+10;
  task SigWait at +0x16, saved SP at +0x36; exec task lists at
  SysBase+0x196 (ready) / +0x1a4 (wait).  Beware: monitor xp output
  of longs can be 2-byte phase-shifted relative to real alignment.
- Mapping ROM addresses to modules: scan the ROM for romtags
  (0x4afc matchword whose following long points at itself), then
  bucket stack return addresses by romtag address ranges.
- Registers not guest-readable (custom chip backing store) are best
  dumped with temporary stderr prints; wd33c93 has a DPRINTF gate at
  the top of the file.
- gdb-multiarch against the gdbstub works for breakpoints and
  watchpoints, but register values come back byte-swapped; addresses
  in commands are fine.
- The disassembly of the ROM (`m68k-linux-gnu-objdump -b binary
  -m m68k --adjust-vma=0xf80000 -D`) plus the Linux amiga drivers
  (drivers/scsi/a3000.h etc.) were the main reference sources; the
  scsi.device ISR confirmed the SDMAC register quirks (SASR mirror
  read at 0x49, ISTR INT_P gate).
