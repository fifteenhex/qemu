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

The `a500` machine needs the A500/A600/A2000 Kickstart 3.1 r40.63 (512KB,
same `commodore-amiga-firmware` item, file `Kickstart v3.1 r40.063
(1993-07)(Commodore)(A500-A600-A2000)[!].zip`):

    md5  e40a5dfb3d017ba8779faba30cbd1c8e
    sha1 3b7f1493b27e212830f989f26ca76c02049f09ca

Run it with `-M a500 -bios kick31_a500.rom` (default 1MB; `-m 1536k` for
a maxed trapdoor).  Floppies attach as `if=floppy` like the others.

## Running

    qemu-system-m68k -M a3000 -bios kick31_a3000.rom
    # optional disk: -drive if=scsi,file=hd.img,format=raw
    # serial (Paula):  -serial stdio

## Current state (2026-07-20)

Three machines now: `a500` (68000, OCS/ECS), `a3000` (68030, ECS) and
`a4000` (68040, AGA).  All boot Kickstart 3.1 to the insert-floppy
screen and run Workbench 3.1 from a floppy; the A4000 needs its own
Kickstart and the A500 needs the A500/A600/A2000 3.1 r40.63 ROM (see
above).  The
A4000 renders through the ECS display path for now (no real AGA), and
its onboard IDE is not modelled, so it has no hard disk yet — the two
open follow-ups for a "real" A4000.  See the A4000 item under "Open
items" for the details and next steps a new session should pick up.

Kickstart 3.1 boots all the way to the insert-floppy screen with the
animation running (a copper split: hires logo panel over a hires drive
panel, purple background; the disk animates into the drive).  Chip RAM
(2MB) and fast RAM (-m, up to 16MB below 0x08000000) are detected
correctly; exec multitasks; scsi.device initialises and probes the bus.

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
open-bus filler; a board fills in class params (ROM, chip RAM, CIA
clock, Agnus/Denise IDs, open-bus extent) and a board_init hook.
`a3000` (hw/m68k/a3000.c) adds the 68030, fast RAM, Zorro III open bus
and the SCSI subsystem; `a4000` (hw/m68k/a4000.c) adds the 68040, fast
RAM and the AGA chip IDs.  The Ramsey memory controller + Fat Gary glue
they share is the TYPE_AMIGA_MOBO device (hw/m68k/amiga_mobo.c).
`a500` (hw/m68k/a500.c) is the minimal case: a 68000 with 512KB chip
RAM and, via `-m` (default 512KB, max 1.5MB), the A501 trapdoor "slow"
RAM at 0x00C00000.  No fast RAM (the 68000 is 24-bit), no Ramsey/Gary,
no Zorro III — just the shared base plus the trapdoor region.  It runs
the A500/A600/A2000 Kickstart 3.1 r40.63 (md5 e40a5dfb3d017ba8779faba
30cbd1c8e); chipset IDs are inherited from the base (ECS), modelling an
ECS-upgraded A500.  Verified: KS3.1 reaches the insert-disk screen and
Workbench 3.1 boots to the desktop with the default 1MB (512 chip + 512
slow) config.
Devices: hw/m68k/mos8520.c (CIA), hw/m68k/amiga_custom.c (interrupts,
beam counters, serial, blitter incl. line mode and fills, frame-atomic
copper, split-window-clipped bitplane display renderer),
hw/m68k/a3000_sdmac.c (SuperDMAC with the wd33c93 behind it, INT2).

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
  (BPLCON2), BPLCON1 fine scroll, dual playfield.  The copper is
  line-granular; effects keyed to the horizontal beam position won't
  render.  DMA sprites render (Lemmings is playable, menu and all).
- AGA display works (hw/m68k/amiga_custom.c): 8 bitplanes, the 256-entry
  24-bit palette (BPLCON3 colour bank + LOCT nibbles, held as device
  state and snapshot per frame so per-line copper palette splits still
  replay), HAM6 and HAM8, and the FMODE 32/64-bit fetch widths (only the
  per-line word count changes since the plane data stays contiguous).
  Deluxe Galaga (AGA) runs in 256 colours on the a4000; get it with the
  archive.org item `Deluxe_Galaga_v2.6B_1995-03-15_Vigdal_Edgar_AGA_SW-R`
  (its startup-sequence LoadWBs, so xdftool-replace s/startup-sequence
  with `cd Deluxe_Galaga_2.6` + `GALAGA.AGA` to auto-run).  Still ECS
  through the sprite path: AGA sprites (FMODE width, BPLCON4 offsets) are
  not widened, so sprite-drawn game objects look streaky.  Watch the
  display-height clamp: a vertical shmup needs ~315 lines, so the cap is
  512, not the PAL 313.
- Control ports: gameport 0 is the mouse; gameport 1's fire button
  (CIA-A PA7) is wired to the host middle mouse button so joystick games
  (Deluxe Galaga reads port 2) can be driven.  Directions on port 2 are
  not emulated yet.
- Super Stardust (AGA) [NOT WORKING, investigated]: the PSG-cracked disk 1
  boots its crack intro (renders fine), fire advances to the game's own
  trackloader, which sticks at "INSERT DISK 1".  Traced with the gdbstub
  (`-s`; register/memory reads come back byte-reversed per long, so read
  guest memory with QMP pmemsave and disassemble with objdump).  The
  loader's disk routine (chip RAM ~0x1e0950) selects a drive, checks
  /CHNG then /RDY, sets DSKSYNC=0x4489 + disk DMA, and waits on DSKBLK.
  It runs this against **drive 1** (register d4=1 -> selects /SEL1), i.e.
  the empty df1, and hangs.  Two findings for a future attempt:
  (1) A real CIA bug: mos8520_port_update() drives the eight port-out
  qemu_irqs low bit first, so the FDC sees the /SEL edge (PB3) before the
  MTR level (PB7) within one CIA write and latches the motor off a stale
  MTR.  Real pins change together; emitting high bit first fixes the
  motor latch (confirmed via an FDC trace: motor stayed correctly on).
  Left it OUT for now because it needs regression testing against the
  working floppy games (trackdisk.device uses separate writes, so it may
  well be safe) and it did not by itself make Super Stardust boot.
  (2) Even with the motor latch fixed, the loader still targets the empty
  df1 and hangs; it likely wants disk 1 in df1 (a two-drive setup) or
  reads df0 first and rejects it.  `-drive if=floppy,index=1,file=...`
  did not attach media to df1 in a quick test — worth checking how the
  machine binds floppy unit 1 before retrying.
- Split-window widths: the surface is now clipped to the widest display
  window the copper opens across the frame (DIWSTRT/DIWSTOP tracked
  through the journal, sampled at DIWSTOP where both halves are fresh),
  not the word-granular fetch width.  This fixed the insert-disk screen,
  a copper split whose lower panel over-fetches its window: the overrun
  used to spill and draw the whole screen twice side by side.  A few
  pixels of off-window fetch still show at the far-left edge — the
  renderer doesn't model the cycle-exact DDF-to-DIW fetch delay, so it
  can't hide the last word of left overscan.
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
  MMU and the AGA chip IDs are fine.  The AGA display is now implemented
  (see the AGA bullet above), so AGA games render in 256 colours.  The
  main remaining follow-up for a "real" A4000 is the onboard IDE (an ATA
  port at 0xdd2020, Gayle-ish; currently open bus, so no hard disk).
- mvme147_pcc has the same latent `dc->legacy_reset =` bug that bit
  the Amiga devices; its reset has never actually run.
- AMIX (Amiga UNIX SVR4) — the 68030 MMU probe.  AMIX 2.1 install disks
  (boot+root, 880K ADFs) are on archive.org
  `commodore-amiga-operating-systems-amix`.  Booting the boot disk on
  the a3000 (`-m 16M`) gets far: the loader decompresses the SVR4 kernel
  into fast RAM (~0x07000000) and starts it, which probes hardware
  (Ramsey 0xde0000-0002) — then instantly drowns in **Access Fault
  (0x8)** exceptions (414k in 50s with `-d int`), looping at
  pc=0x071234ac with a corrupted supervisor SP (0xfffffff4).
  Root cause: **QEMU's m68k MMU is 68040-only.**  get_physical_address()
  (target/m68k/helper.c) walks 68040-format tables via URP/SRP (loaded
  by MOVEC).  The 68030 PMMU is a stub: translate.c's pmmu030 stores
  PMOVE of TC/CRP/SRP/TT0/TT1 into mmu.{tc030,crp030,srp030,tt030} but
  nothing translates through them, PFLUSH/PLOAD are no-ops, and PTEST
  lies "valid".  Kickstart only *probes* those registers, so AmigaOS
  boots; AMIX actually enables demand paging, so it fails at the first
  translated access.
  IMPLEMENTED (target/m68k, get_physical_address_030): the 030 table walk
  driven by tc030 (initial shift + up to four TIA-TID index fields),
  following crp030/srp030, honouring short/long descriptors, WP and the
  supervisor bit, and setting the Used/Modified bits.  tlb_fill() gates
  on the 030 TC enable bit.  Result: the Access Fault storm is gone
  (28M -> a few thousand exceptions), AMIX runs its kernel in virtual
  memory, brings up its own 640x256 console, and prints a real
  "DOUBLE PANIC: KERNEL FAULT" register dump instead of looping the CPU.
  NEXT BLOCKER: that kernel fault — now READ OFF THE CONSOLE.  Captured the
  on-screen panic with QMP `screendump` at 10s intervals (AMIX writes its
  console to the graphics display, not serial), upscaled the text band with
  PIL, and read it:

      PANIC: assertion failed: pp >= pages && pp < epages,
             file: vm_page.c, line: 1103
      DOUBLE PANIC: KERNEL FAULT ssw=0x2704 pc=0x00C00AC0 fmt=0x8
             vector=0xD <Line F>

  So the FIRST fault is a **software assertion in AMIX's VM layer**, not an
  MMU miss — the page-frame database bounds check `pp >= pages && pp <
  epages` fails: a physical page pointer fell outside the managed page array.
  The MMU walk is therefore working; AMIX reaches VM init before tripping.
  Verified this directly by walking the live SRP tables via QMP pmemsave for
  the double-panic PC (virtual 0xc00ac0): the SRP root descriptor (entry 0,
  covering supervisor VA 0-0x3FFFFFFF) is INVALID, so 0xc00ac0 is genuinely
  unmapped — the double panic is just panic() itself touching an unmapped
  page and taking a Line-F (open-bus 0xffff decodes as F-line, vector 0xD).
  The QEMU 030 format-$A bus-fault frame layout was audited against the
  68030UM and is correct (SR/PC/vector/SSW/pipe/fault-addr all in the right
  slots), so the frame is not the cause either.

  DISASSEMBLED THE ASSERTION (dump fast RAM 0x07000000-0x08000000 via QMP
  pmemsave, m68k-linux-gnu-objdump -b binary -m m68k:68030).  The kernel runs
  **1:1 in fast RAM** (code references the assertion string at its physical
  address 0x070af518; verified 14 refs, 12 are `pea 0x70af518` call sites).
  assfail() = 0x0703ec0c.  The assertion macro is:

      jsr   0x070af5f8          ; pp = page-hash lookup(vnode,offset) -> a0
      moveal %a0,%a2            ; a2 = pp
      cmpal 0x07124a90,%a2      ; pp vs `pages`  global
      bcs   fail                ; pp <  pages  -> fail
      cmpal 0x07116508,%a2      ; pp vs `epages` global
      bcs   ok                  ; pp <  epages -> ok
      fail: pea <line> ; pea 0x070af50e"vm_page.c" ; pea 0x070af518 ; jsr assfail

  So `pages`  global lives at 0x07124a90, `epages` at 0x07116508.  Runtime
  values on our boot: pages=0x40040000, epages=0x400ab364 (span 0x6b364).
  IMPORTANT: `pp` does NOT come from a pfn formula — 0x070af5f8 is a **page
  hash lookup** (hashes with >>11, the 2KB page shift from tc030 PS=11).  So
  the failure is "a vm_page reached through the page hash has an address
  outside the managed [pages,epages) array" — a page-management inconsistency,
  not a simple array-sizing bug.

  MMU RULED OUT as the corruptor.  Walked the live SRP tables (base
  0x0712b800, tc030=0x82b02d60: IS=0, TI=[2,13,6,0], PS=11 -> 2KB pages):
  the SRP root uses an **early-termination page descriptor** mapping
  supervisor VA 0x00000000-0x3FFFFFFF 1:1 to physical (hence kernel-in-fast-
  RAM runs 1:1, and 0xc00ac0 identity-maps to the custom/open-bus region ->
  reads 0xffff -> the Line-F double panic).  VA 0x40000000+ uses the fine
  tables and correctly maps the page array (0x40040000 -> fast RAM
  0x07143800).  get_physical_address_030 handles early termination and the
  fine walk correctly and self-consistently, so the page array is not being
  mistranslated.  The out-of-range pp is therefore genuine AMIX behaviour on
  our chip+fast split memory, reached during early VM init.
  Remaining work is open-ended AMIX kernel RE: trace how a page enters the
  hash with an address below `pages` (0x40040000) or >= `epages` — most likely
  tied to how AMIX enumerates physical segments (2MB chip at pa 0 vs 16MB fast
  at pa 0x07000000) and which pages it builds structs for.  Cross-check
  against Kickstart's exec memory list (what the Ramsey/Gary model reports for
  fast-RAM base+size), since that list feeds AMIX's segment setup.
  Not-yet-modelled for the 030 walk: descriptor LIMIT fields, the
  indirect page descriptor, and function-code lookup (TC FCL) — AMIX
  seems not to need them so far.

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

## Session 2026-07-23: Gayle IDE (A600/A1200 hard disk boot) + CD32

New hardware this session: hw/ide/gayle.c (the Gayle gate array's IDE
interface, on QEMU's IDE core) and hw/m68k/cd32.c + cd32_akiko.c (the
CD32 console with Akiko).  Milestones hit: a1200 and a600 boot
Workbench 3.1 from an IDE hard disk to the desktop with no floppy;
`-M cd32` boots to the animated startup screen (spinning disc over the
starfield).  All verified by screendump.

- Gayle IDE layout (per Linux pata_gayle.c/amigayle.h and the Gayle
  spec): ATA block at 0xda0000 (data word at +0, task file byte regs
  at +2 + reg*4, control/altstatus +0x101a, bits 0x2020 undecoded so
  scsi.device's 0xda2000 accesses are mirrors), IRQ regs at
  0xda8000 (live status)/0xda9000 (change latch, write-0-to-clear,
  bits 1:0 store)/0xdaa000 (enable)/0xdab000 (config), Gayle ID at
  0xde1000 (serial read, one bit per read on D7 after any write
  resets the pointer; 0xd0).  IDE INTRQ latches into 0xda9000 bit 7
  and drives INT2 when enabled.  KS3.1 A1200/A500-A600 scsi.device
  found the disk with no fuss on the first try; KS2.05 r37.299
  (pre-A600HD) ignores it, as on real hardware.  Data port is a
  little-endian region: memory core swaps per 16-bit word for the BE
  guest = the straight D15..D0 wiring.
- Workbench HD image WITHOUT a guest install pass: amitools' xdftool
  (venv at /workspace/home/dev/amitools-venv) understands RDB images
  with `open part=<name>`.  Recipe: cp hd-golden.img (RDB, two FFS
  partitions, guest-formatted, empty), `xdftool img open part=WB_2.x
  + delete Disk.info + write <each top-level entry of the unpacked
  wb31_workbench.adf>`.  The FFS the guest formatted stays intact, so
  Kickstart mounts and boots it.  Saved as
  _amiga_assets/hd-wb31-golden.img; boots on a600/a1200 IDE and (via
  if=scsi,unit=6) presumably a3000.  Use snapshot=on for tests.
- CD32 board: a1200 clone + extended ROM 512KB at 0xe00000 (machine
  prop "extrom", qemu_find_file so -L works) + Akiko at 0xb80000.  No
  Gayle.  Akiko: ID 0xc0cacafe, C2P port at +0x38 (8 longword FIFO,
  WinUAE-equivalent bit permutation, used by the boot animation),
  NVRAM I2C at +0x30/+0x32 (SCL bit7/SDA bit6, level vs direction
  regs) bit-banged into 4x at24c-eeprom (a 24C08's four banks on i2c
  addresses 0x50..0x53), CD controller regs stored-only (no drive =
  "no disc": exactly the boot-animation behaviour).  Kickstart does
  word writes to 0x30/0x32, so the odd bytes 0x31/0x33 land in the
  UNIMP log - harmless.  KS also probes the (absent) Gayle ID at
  0xde1000, reads open bus 0xff, moves on.
- CD32 debugging saga, in order:
  1. Boot hung in audio.device (PC 0xfbc1e2, polling its channel
     struct).  audio.device starts EVERY DMA playback by hand-feeding
     one word to AUDxDAT and letting the AUDx interrupt handler
     (is_Code, switched to the "DMA start" routine 0xfbb874) set
     DMACON - Paula's manual (non-DMA) audio mode.  Implemented: DAT
     write with the channel DMA off schedules INT_AUDx after 2
     periods (QEMUTimer per channel).
  2. Then the chime crawled: the boot jingle plays a period-8 sweep
     (443kHz!), and the mixer clamped periods to Paula's fetch floor
     (124), stretching a 0.3s buffer to ~40s.  Clamp removed: a
     starved Paula repeats words but drains at the programmed rate,
     and the guest just sleeps until the buffer-done interrupt.
  3. Then CDUITask GURU'd (Line-F at PC 0xff000000): graphics'
     SetChipRev derives the Lisa REVISION from ~DENISEID >> 8 & 3.
     Real Lisa drives all 16 bits (DENISEID = 0x00f8); our 0xff00|id
     made rev = 0 = pre-production, and the CD32 UI jumped through a
     vector that revision leaves unset.  DENISEID now returns 0x00f8
     when denise-id is 0xf8 (AGA), 0xff00|id otherwise.
  4. With the revision right, KS programs REAL AGA fetch: hires WB
     screen = FMODE 3, DDF 0x38..0xd8 = 11 4-word bursts = 44 words
     (88 bytes) fetched per row, BPLxMOD 0x48, interleaved 80-byte
     rows, plane pointers pre-decremented 2 bytes for the early
     DDFSTRT.  Our renderer used the frame-start FMODE (0 -> 42
     words) and sheared the whole screen 4 bytes/row.  FMODE is now
     journalled and tracked per line like DDF/BPLCON0.  (Debug trick
     that cracked it: pmemsave the bitplanes and re-render them
     host-side with PIL - the guest's framebuffer was pixel-perfect,
     which pinned the bug on the renderer.)
- Boot timing: CD32 takes ~90-100s to the animation (real-time chime
  + C2P decompression on the emulated 020); a transient garbage frame
  around 75-85s is the animation decompressing into the visible
  buffer.  Works with no -audiodev (default backend drains); wav
  backend captures the chime (nice for regression: silence = broken).
- exec Alert debugging recipe: alert code lands in D6/at 0x100
  (0x8000000b = dead-end Line-F), crashed task from SysBase; a
  5-line temporary hook in do_interrupt_all() printing pc/sp/regs +
  stack top on EXCP_LINEF beats gdbstub for this (register reads via
  gdb come back byte-swapped, monitor xp does not).
- Gayle/board reset: AmigaMachineState gained rsto_dev[], board
  devices the RESET instruction cold-resets along with the shared
  chips (gayle on a600/a1200, akiko on cd32).
- a4000t (stretch): machine added (hw/m68k/a4000.c, subclass of the
  a4000).  KS 3.1 r40.070 probes the onboard NCR 53C710 by touching
  its registers under a Fat Gary bus-timeout watch (write 0 to
  0xde0000, access 0xdd0078, read 0xde0000 bit 7, write 0x80): plain
  open bus reads as "chip present" and the boot then hangs forever in
  the A4000T SCSI handler task.  amiga_mobo now models the Gary
  timeout latch (reg 0x0000) plus a "vacant slot" region (sysbus mmio
  1) the a4000t maps over 0xdd0000, so the probe sees the timeout and
  scsi.device skips the controller; KS boots to the insert-disk
  screen.  WIRING THE REAL NCR WAS TRIED and reverted: the in-tree
  GSoC 53C710 (hw/scsi/ncr53c710.c, + a lasi-style ^3 byte-lane
  swizzle for the BE bus) gets through scsi.device's reset/config
  dialogue (ISTAT abort/reset, CTEST7/CTEST0/SCNTL0 setup all traced
  sane) but then the driver polls ISTAT reading 0x20 (SIGP) forever
  with SCRIPTS never started - real 710 bring-up work, not a drop-in;
  a future session could resume from that trace.
  OPEN ITEM: WB 3.1 floppy boot on a4000t stalls at an empty
  "Workbench Screen" (startup-sequence runs to the end - ConClip,
  IPrefs, RAM disk all up - but no Workbench task appears after
  LoadWB; no guru).  The same disk boots to the desktop on -M a4000
  (40.068), so it is 40.070-specific.  Not caused by the timeout slot
  (same stall before it was added... actually pre-timeout the machine
  hung earlier, in scsi.device; the LoadWB stall is the next layer).

## Session 2026-07-31: direct Linux kernel boot (-kernel) on a3000/a4000

Goal: boot Linux/m68k directly with -kernel/-initrd/-append on the
MMU machines, like q800/virt do, and prove it end to end.  Result:
full kernel boot on BOTH a3000 (030) and a4000 (040) — penguin +
boot log on the amifb display (screendump), serial console on ttyS0,
VFS root-fs panic reached; on the a4000 an initramfs /init runs and
draws on /dev/fb0.  030 user space is still out (see below).

- Test kernel: 6.5 amiga_defconfig from the local linux tree,
  cross-built out of tree (the checkout belongs to another project
  and its in-tree ARM build makes O= refuse; `git archive | tar` to
  /tmp/linux-src and build there.  Two local-patch landmines: the
  tree's OF_COMPATIBLE_MAX_LEN Kconfig is inside `if OF` but
  mod_devicetable.h uses it unconditionally, and drivers/clk/mstar
  force-builds an OF-only mux — both patched in the copy only).
- Boot protocol (hw/m68k/amiga.c amiga_load_kernel, mirroring
  q800.c): vmlinux is linked at vaddr 0, head.S runs at "same offset
  in the first BI_MEMCHUNK", so load_elf with a translate_fn that
  adds the board's fastram_base; bootinfo records go at the load
  address of _end (load_elf's high).  MACH_AMIGA + CPU/FPU/MMU from
  CPU features + one BI_MEMCHUNK (fast RAM only!) + AMIGA model/
  chipset/chip-size/vblank/psfreq/eclock/serper + cmdline + RNG seed
  + BI_RAMDISK at top of fast RAM.  Tags live in a new
  standard-headers/asm-m68k/bootinfo-amiga.h (copied from Linux
  uapi); BI_AMIGA_VBLANK/PSFREQ are single BYTES and SERPER a WORD in
  the record data, so hw/m68k/bootinfo.h grew BOOTINFOBYTE/
  BOOTINFOWORD (a be32 BOOTINFO1 puts 0 in the first byte and the
  kernel reads vblank=0).  Entry: cpu reset sets pc=entry, sp=fastram
  base (head.S switches stacks immediately), MMU off.  -bios becomes
  optional with -kernel.
- THE OVERLAY BITE (hours): everything below 512K of chip RAM was
  a black hole — kernel wrote the amifb framebuffer at phys 0x1000
  through an ioremap mapping, gva2gpa said the mapping was right,
  chip RAM stayed zero.  The Kickstart overlay was back on: CIA-A
  cold-reset leaves DDRA=0 (PA0 input, pulled up = OVL asserted) and
  the kernel's read-modify-write power-LED writes kept latching the
  pulled-up bit back in.  On real hardware amiboot runs after
  Kickstart drove PA0 low as an output.  Fix: mos8520 grew reset-pra/
  reset-ddra properties; in -kernel mode CIA-A resets to Kickstart's
  state (DDRA=OVL|LED as outputs, OVL low) and the overlay reset
  keeps the ROM off.  Writes to a disabled-overlay'd ROM region are
  silently discarded — if chip RAM below 512K reads zero while
  0x141000 has data, think overlay.
- amifb display: two custom-chip model fixes.  (1) DIWHIGH (0x1e4):
  amifb positions its window via the ECS high bits; without them the
  OCS decode yields a 16-pixel-wide window.  The model now tracks
  DIWHIGH-written-and-not-invalidated (DIWSTRT/DIWSTOP writes clear
  it, per silicon), journals the register, and decodes the window
  with amiga_diw_decode().  (2) copper WAIT semantics: the compare is
  beam >= vp on the 8-bit line counter, so a WAIT for a line already
  passed falls through immediately (amifb re-enters its display list
  "at line 10" from the init list's wait at line 42), and only a wait
  that parked on line 255 pushes the next wait into the following
  256-line block.  The old "vp < line means next block" wrap sent
  amifb's list to line 261 and the screen stayed on the 16-wide dummy
  list.  Regression-checked: KS3.1 a3000 insert-disk, KS1.3 a500
  hand, Deluxe Galaga AGA title all render as before.
- amifb mode gotcha: the DEFAULT modes are pal-lace (A3000, because
  the machine reports AMBER_FF) and vga70 (AGA) — interlace uses two
  alternating per-field copper lists and programmed sync, which the
  frame renderer doesn't model; the visible result is a shifted/
  wrapped field.  video=amifb:pal (640x256 non-lace) renders
  pixel-perfect.  Debug trick that cracked it again: pmemsave the
  chip RAM and re-render the bitplanes host-side with PIL — the
  virtual framebuffer was perfect (penguin, full boot log), which
  pinned everything on the copper/window decode.
- 030 PTEST was a stub ("report valid, no faults") — Linux's
  bus_error030 re-probes every data fault with ptestr and reads the
  PSR; with mmusr=0 it concluded "020 cas fault" and exec died with
  EFAULT before user space ever started.  PTEST030 now runs the real
  030 walk (new ptest030 helper: fc from the extension word incl.
  the data-register form Linux uses, PSR I/W/S/L/B bits, no U/M
  side effects), and the 030 bus-fault SSW now carries the function
  code (Linux keys its user/kernel fault split off SSW's FC field).
  With that, exec progresses into real demand faults, but the first
  user process now dies deeper: handle_mm_fault itself reads a
  garbage pmd (__va(0x8), "BAD KERNEL BUSERR" at pc=handle_mm_fault)
  — some kernel-side structure ends up corrupt on the 030 path.
  OPEN: 030 user space (test: -M a3000 -kernel ... -initrd; a4000
  runs the same initramfs fine, so it's 030-specific — suspects are
  the 030 walk's writeback/descriptor handling or fault retry
  semantics, NOT the generic amiga code).
- Memory: -m 16M (the motherboard max).  8MB boots but OOM-deadlocks
  before reaching the panic/initramfs.  Evidence from the session:
  /tmp/linboot-p-fb.png (a3000 penguin+panic), /tmp/linboot-a4000-pal.png
  (a4000 same), /tmp/linboot-q-serial.log (a4000 /init hello + fb0
  pattern), /tmp/fb-virtual-l.png (host-side re-render).

## Session 2026-07-31 (later): Zorro III bus + Mediator 4000 PCI bridge

Goal: model the ElBox Mediator 4000 Zorro-III->PCI bridge as an
optional a4000 expansion and prove it against Daniel's real Linux
host-controller driver (drivers/pci/controller/pci-mediator4000.c on
linux branch origin/amiga-laptop-20260627; reference copy in
_amiga_assets/mediator-ref/).  DONE end to end: KS 3.1 autoconfigures
the two boards and Workbench boots; Daniel's driver binds, registers
the PCI host, enumerates the bus, ne2k-pci probes eth0 and completes
a real in-kernel DHCP handshake (TX+RX+INTx) against a host-side
responder.

- New: hw/m68k/zorro3.c (+include/hw/m68k/zorro3.h) - minimal Zorro
  III bus.  TYPE_ZORRO3_BRIDGE (sysbus) owns TYPE_ZORRO3_BUS and the
  AutoConfig config space at 0xFF000000 (mapped over the a4000's
  open-bus filler at prio 1; reads 0xFF when the chain is empty, so
  no-board behaviour is unchanged).  Cards are qdev devices on the
  bus (-device works); one card can register several logical boards
  (Zorro3Board: rom bytes + body MemoryRegion), chained in
  registration order.  Bus also carries wired-OR /INT2 into the
  custom chip's ports-irq.
- AutoConfig protocol AS TRACED against KS 3.1 r40.068 on the a4000
  (trace-events zorro3_*; hw/m68k now in meson's
  trace_events_subdirs + hw/m68k/trace.h shim):
  * Reads are BYTES, high nybble of logical register byte N at
    offset N*4, LOW nybble at N*4 + 0x100 (Z3 layout; the model also
    accepts the Z2 +2 offset).  Nybble in bits 7-4, everything but
    er_Type (reg 0) inverted.  KS reads low nybble first.
  * Configure: KS writes the base as byte 0x00 -> 0x48 (bits 23-16
    latch) THEN word (base>>16) -> 0x44, and the 0x44 write commits.
    Getting this wrong (committing on the 0x48 byte) configures the
    board at address 0 over chip RAM and the machine dies - the
    first-cut bug.  Shutup = any write to 0x4C.
  * KS 3.1 assigns exactly what our own allocator picks: 16MB board
    -> 0x40000000, 256MB board -> 0x50000000.
- Direct kernel boot: Linux does NOT scan Zorro config space - it
  enumerates from BI_AMIGA_AUTOCON bootinfo records (struct
  ConfigDev each).  amiga.c now runs the kernel load from a
  machine-done notifier (so -device boards exist), has QEMU play
  expansion.library (zorro3_bridge_autoconfig: size-aligned bases
  from 0x40000000 up, sticky across resets) and emits one AUTOCON
  record per configured board.  On the ROM path a bus reset
  unconfigures everything for KS's rescan (bridge is in rsto_dev).
- hw/m68k/mediator4000.c: everything named from the driver's
  #defines.  Control board (ElBox 0x089E prod 0x21, 16MB Z3): window
  byte reg at +0 (bits 7-4 = PCI mem address bits 31-28 -> movable
  alias offset into the PCI memory container), IRQ byte at +4
  (status = live INTx in bits 3-0, mask bits 7-4; INT2 =
  status & mask>>4), PCICONF at +0x800000 (4MB, 2KB/slot dev stride,
  256B/function, 6 slots, LITTLE-ENDIAN region - driver z_readl's
  raw and le32_to_cpu's), PCIIO at +0xC00000 (1MB alias to the PCI
  I/O container at bus address 0).  Window board (prod 0xA1, 256MB
  Z3) = the banked alias into PCI memory.  INTx line = slot % 4
  (driver's map_irq).  Root bus hangs off an internal
  TYPE_PCI_HOST_BRIDGE child (info pci works).  256MB window chosen
  so the whole card sits in 0x40000000-0x5FFFFFFF = the range Linux
  maps 1:1 (head.S transparent translation + kmap.c special case),
  which the driver's raw z_read/inb accesses rely on.
- Kconfig: AMIGA now selects PCI + implies PCI_DEVICES (brings
  ne2k_pci et al into the m68k target).
- THE BAR-0 BITE: Linux puts the PCI I/O window at bus addresses
  0-0xFFFFE (resource offset = zorro base) and really assigns the
  first I/O BAR at bus address 0.  QEMU's pci_bar_address() treats
  BAR addr 0 as unconfigured unless mc->pci_allow_0_address - so the
  ne2k ioport region was never mapped and its probe read 0xFF and
  bailed SILENTLY right after "enabling device".  Machine class now
  sets pci_allow_0_address (like other non-x86 machines).
- Guest kernel recipe (branch is v7.1): git archive
  origin/amiga-laptop-20260627 to /tmp/linux-m4k, amiga_defconfig +
  CONFIG_PCI/PCI_MEDIATOR4000/NE2K_PCI, and DISABLE
  CONFIG_AMIGA_PCMCIA - with it, CONFIG_ISA defines inb() via the
  Gayle PCMCIA window (isa_itb) and ne2k faults on a bogus address;
  without it inb() is asm-generic readb(port) on the 1:1 mapping.
  No build landmines on this branch (the 6.5-era OF/mstar patches
  from the earlier session don't apply).  Boot with swiotlb=512:
  the driver Kconfig force-enables swiotlb and the default 64MB pool
  eats the whole 16MB machine (OOM panic before init otherwise).
- Boot line that proves the lot (serial log evidence):
    -M a4000 -m 16M -kernel vmlinux -initrd initramfs.cpio
    -append "console=ttyS0 swiotlb=512 ip=dhcp"
    -device mediator4000 -device ne2k_pci,netdev=n1
    -netdev dgram,id=n1,...  (+ /tmp/med-dhcpd.py answering)
  dmesg: "Zorro: Probing AutoConfig expansion devices: 2 devices" /
  "pci_mediator4000 zorro0: PCI host bridge to bus 0000:00" / root
  bus resources io 0x40C00000 (bus 0-0xFFFFE) + mem 0x50000000 /
  "ne2k-pci 0000:00:00.0 eth0: RealTek RTL-8029(AS) found at
  0x40C00000, IRQ 32, 52:54:..." / "IP-Config: Got DHCP answer from
  10.0.2.2, my address is 10.0.2.15".  IRQ 32 = first hwirq of the
  driver's irqdomain (branch sets NR_IRQS 32+4); the
  mediator4000_irq trace shows INTA masked in, asserting INT2 and
  clearing per interrupt.  No slirp in this build, hence the dgram
  netdev + tiny python DHCP responder (TX and RX both real).
- Regressions rechecked: a4000 KS insert-disk + WB floppy desktop
  with and without the mediator; a4000/a3000 -kernel boots without
  the device ("0 devices").
- Open/left out: no AmigaOS Mediator drivers (boards sit
  unconfigured driver-wise under WB - correct per mission); vmstate
  is unmigratable=1 on bridge+card (dynamic board mapping vs
  migration punt); Zorro bus only on the a4000 family so far; PCI
  bus masters see only PCI memory space (matches the real bridge's
  DMA reach; driver carries a fixme + swiotlb for it).
   (z3ram note: FLATMEM kernels cannot use distant Zorro III RAM chunks — AmigaOS can)
