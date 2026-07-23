# Amiga 3000 emulation — working notes

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

- Hard disk boot: scsi.device finds the disk (selection, INQUIRY and
  select-and-transfer all work) but the first TEST UNIT READY comes
  back CHECK CONDITION with a power-on unit attention, which it
  treats as "no disk" and never reads the RigidDiskBlock.  Needs RE
  of scsi.device's error path (does it expect the chip/driver to run
  REQUEST SENSE?) or a compatibility knob on the QEMU disk.  A
  bootable image will also need an RDB with a filesystem.
- Keyboard: input path is CIA-A's serial register; the model already
  has mos8520_sdr_input() for injection.  Needs the handshake
  protocol and a QEMU keyboard event handler wiring scancodes.
- Display gaps: sprites (mouse pointer is a sprite!), HAM, dual
  playfield.  The copper is line-granular; effects keyed to the
  horizontal beam position won't render.
- Not modelled: audio, keyboard (CIA-A SDR handshake), battery clock
  (RP5C01 at 0xdc0000, currently open bus), Zorro slots.  Floppy: no
  disk change/eject at runtime yet (fixed media), only DF0, only
  880KB DD ADFs.
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
