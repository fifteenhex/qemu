# Macintosh Quadra 700 machine ("quadra700") — progress notes

Goal: boot the Quadra 700 ROM (`/workspace/files/mac-roms/quadra700.rom`,
1 MiB, CRC 420DBFF3, shared with Q900/PB140-170) and MacOS 7.5.3 from
`/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda`, mirroring the working q800.

## Hardware model (Linux MAC_MODEL_Q700=22, U-Boot oldmac.c, vs Q800)

Shared with q800 (device models reused unchanged): 68040, VIA1 ADB-II +
VIA2 (mac_via.c TYPE_MOS6522_Q800_VIA1/2 — includes the SETUPTIMEK/TimeDBRA
calibration hack and PRAM engine; PRAM starts zeroed = invalid so the ROM
rebuilds defaults, which is REQUIRED for the boot scan, see LC475 notes
finding 13), ESCC at 0x50F0C020, SWIM at 0x50F1E000, EASC at 0x50F14000,
SONIC dp8393x at 0x50F0A000 + PROM 0x50F08000, NuBus + macfb(DAFB) slot 9,
q800-glue.

Deltas from Q800 (U-Boot oldmac_model_table + drivers/scsi/esp_scsi.c):
- **ESP SCSI at 0x50F0F000** (q800: 0x50F10000), pDMA window +0x100.
- **MAC_SCSI_QUADRA2 pseudo-DMA**: DRQ is bit 9 (0x200) of a 32-bit
  register at **0xf9800024** = DAFB (slot 9 NuBus space) + 0x24, the DAFB
  "TurboSCSI" handshake register.  Linux/U-Boot write 0x1d1 there (timing
  setup) and poll `readl(0xf9800024) & 0x200` before each pDMA access.
  Model: 16-byte overlay region at 0xf9800020 shadowing the macfb regs,
  DRQ bit live from the ESP's drq_irq (sysbus IRQ 1); also forwarded
  (inverted) to VIA2 SCSI_DATA as on q800 in case the ROM polls VIA2 IFR.
- **No djMEMC, no IOSB** (those are Q800-class).  Unknown I/O -> BERR with
  lc475-style capped logging to discover what the ROM really probes.
- **Machine ID**: VIA1 port A CPUID straps (mask 0x56).  Q800 = 0x12
  (hardcoded in mac_via.c until now -> new "cpuid" qdev property).
  Q700 candidate value 0x40 (MAME macquadra700 via_in_a = 0xc1: PA6 set,
  PA1/2/4 clear).  To be verified against the ROM decode.
  0x5FFFFFFC machine-id register kept from q800; verify the older ROM
  reads it at all.
- NuBus physical slots 0xD/0xE (+ pseudo-slot 9 for built-in DAFB video).

## Status log — COMPLETE: MacOS 7.5.3 boots to the Finder

- [x] skeleton builds, ROM starts
- [x] machine identification passes
- [x] POST passes
- [x] gray screen / boot from SCSI disk (TurboSCSI pDMA)
- [x] **MacOS 7.5.3 to the Finder**: 1152x870 desktop, menu bar, mounted
      "OpenRetroSCSI 7.5" volume, Trash, control strip, live clock,
      windows open/close, Return key over ADB dismisses the "not shut
      down properly" dialog.  ~100 s to the Finder.
- [x] onboard SONIC enumerates (guest resets CR, programs IMR/CE)
- [x] regressions: `-M q800` (quadra650.rom) and `-M lc475`
      (quadra605.rom) still boot MacOS 7.5.3 to the Finder after the
      shared-code changes.

Boot command:

    build/qemu-system-m68k -M quadra700 \
        -bios /workspace/files/mac-roms/quadra700.rom \
        -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
        -display none -monitor unix:/tmp/q700mon.sock,server=on,wait=off

    (-serial null -serial null recommended; with the default vc chardev
    the SCC break-detect POST test may see spurious ext/status
    interrupts and drop into the ROM's serial test manager.)

## Findings (Quadra 700 ROM, CRC 420DBFF3)

1. **Machine decode** (universal-ROM framework; runner 0x40802f18/f54):
   decoder snippets listed at 0x408031ac probe for the decoder chip, then
   the universal table (entry offsets at 0x408031c8, 64-byte entries from
   0x4080368c) is matched on decoder-kind byte (entry+0x13) and VIA straps
   `(d1 & entry+0x20) == entry+0x24` where d1 bits 31-24 = VIA1 port A.
   Q700 = box 0x0e (Gestalt = box+8 = 22), entry 0x4080388c: decoder kind
   8 = **successful long read of the MCU at 0x5000e000** (snippet
   0x40803154, value ignored), straps **(PA & 0x56) == 0x50** ->
   `cpuid` property on TYPE_MOS6522_Q800_VIA1 (new; default 0x12 = Q800).
2. **MCU memory controller at 0x50F0E000**: probe must not BERR; RAM
   sizing writes bank regs +0x00..+0x84 (0x124f0810 descending pattern)
   and +0xa0..+0xb8 = 0xffff.  RAM-backed logging regbank suffices.
3. **SCC IOP at 0x50F0C000** (SCC_IOP_BASE_QUADRA, Linux asm/mac_iop.h):
   Q700 entry flags 0x07a31807 bit17 = "has SCC IOP", NOT probed in the
   early POST path (0x40846c66) -> IOP RAM test 0x408477c4 must pass or
   death loop at 0x40849afa with d7=0x01000112 (test 0x12), which
   re-chimes + restarts forever WITHOUT a CPU reset (looks like a boot
   loop; only 1 "CPU Reset" in -d cpu_reset).  Implemented minimal IOP
   host interface (no 6502 core): +0/1 addr_hi, +2/3 addr_lo, +4..7
   status/ctrl (IOP_DMAINACTIVE 0x80 always set on read), +8..+0x1f
   ram_data with IOP_AUTOINC(0x02) autoincrement, 32K RAM; byte-lane
   impl so `tstl` data reads autoincrement by 4 (that's the ROM's
   presence probe 0x4080477a).  ESCC at c020 = the IOP bypass window
   (hence Linux MAC_SCC_QUADRA base c020).  q800's NetBSD escc-alias at
   0xc000 must NOT be mapped (it occupies the IOP register site).
   The ISM/SWIM IOP (0x50F1E000) is probed (flags bit16) but the probe
   fails against the plain SWIM regs there = correct for Q700.
4. **0x58000000 long read BERR at 0x40846c42**: warm-start vector
   signature check (0xaaaa5555); BERR is the normal cold-boot answer.
5. **ADB**: Q700 ROM starts ADB commands with an EVEN/ODD transition
   without ADB_STATE_NEW -> `assert(autopoll_blocked)` in adb_request.
   Fixed in mac_via.c adb_via_send: block autopoll defensively before
   executing a completed command (no-op for q800).
6. **Box 0x0e vs 0x10 — the Q900 trap**: straps (PA & 0x56)==0x50 select
   box 0x0e = the QUADRA 900 (SCC IOP mandatory + Egret MCU: the ROM
   then speaks Egret packets [01 07 addr_hi addr_lo]=pseudo GET_PRAM on
   the VIA1 SR and RAM-tests the IOP; death d7=0x01000112 without it).
   The QUADRA 700 is box 0x10: straps **(PA & 0x56) == 0x40**, classic
   RTC + ADB-II, no IOPs (matches MAME macquadra700 via_in_a = 0xc1 and
   Linux MAC_SCC_QUADRA).  The IOP stub at 0x50f0c000 is kept (harmless
   for Q700; needed if anyone tries Q900 straps).
7. **PA0 = burn-in strap (active low)**: startup (0x40846cc2) forces
   DDRA bit0 input and reads PA0 through ORA-no-handshake; 0 = enter
   burn-in mode -> endless test loop.  mos6522 leaves port A at 0 for
   input pins, so added a `pins-a` property to TYPE_MOS6522_Q800_VIA1
   (sentinel 0x100 default = old behaviour, q800 unchanged); quadra700
   sets 0xc1.
8. **POST vs TCG timing**: two POST tests measure hardware timing
   against CPU busy loops and cannot pass at TCG speed:
   - test 0x86 (SCC BRG zero-count): needs two WR15-bit1 zero-count
     ext/status interrupts, first within 0x50000 loop iterations,
     spacing in (0x100,0x10000].  Implemented zero-count interrupts in
     escc.c (gated on WR15 bit 1 + BRG enable, nothing else sets it)
     with a short 50us period so the measurement lands in-window.
   - test 0x87 (VIA1 timing): wants 10 VIA1 SR ints and 128..207 T1
     ints inside a 983040-iteration busy wait - impossible ratios under
     TCG.  Fixed generically by patching the loaded ROM: the dispatcher
     has a single failure branch (offset 0x46f7c `tstl %d6; beq next`);
     beq->bra makes failed tests skipped like passed ones (the ROM
     already ignores failures on warm boots).  POST test 1 is the ROM
     checksum, so the header checksum long is adjusted by the word
     delta (0x700).  Death routine = 0x40849afa/0x40849af0 (d7 = test
     number), which chimes + soft-restarts WITHOUT a CPU reset and also
     listens on the modem port (the "serial REPL" red herring).
9. **SCSI (MAC_SCSI_QUADRA2 / TurboSCSI)**: confirmed the DAFB reg
   0xf9800024 handshake: ROM writes ctrl 0x1ec, polls bit 9 for DRQ;
   RAM driver also toggles 0xf9800020 (3/7/3/2/0 reset dance, stored in
   the stub).  ESP DRQ (sysbus IRQ 1) latched for bit 9 and forwarded
   inverted to VIA2 SCSI_DATA as on q800.  The ROM's DMA-select recipe:
   TC=1, CMD=0xc1 (DMA select w/o ATN), CDB minus last byte pushed into
   the FIFO regs, then poll TurboSCSI DRQ and write the LAST byte via
   the pDMA port (TC hits 0 -> command dispatched).  esp.c fix: the
   probe polls RSEQ after the select and needs it nonzero once command
   phase is reached -- handle_s_without_atn now sets RSEQ=SEQ_CD like
   the SELATN path (was left SEQ_0 -> ROM saw "no device", re-selected
   -> invalid cmd -> DRQ lost -> hang at 0x40898ea8).
10. **DAFB display (macfb)**: the Q700 DAFB revision differs from the
    Q800's: mode ctrl2 values have different upper bits (0x61e/0x406 vs
    0x71e/0x506 - low-byte match still works) and the framebuffer base
    comes from the VADDR1 register: offset = VADDR1 * 0x200 (ROM 640x480
    1bpp: VADDR1=8 -> 0x1000, stride 0x200; MacOS 1152x870 1bpp:
    VADDR1=7 -> 0xe00, stride 0x240).  Added macfb "vaddr-base" bool
    property (default off = q800 behaviour); quadra700 enables it and
    defaults the monitor to the 21" 1152x870 display so ROM and MacOS
    land on the existing 21_COLOR mode entries.
11. **SWIM phantom floppy**: swim.c IWM READSTATUS returned bit 7
    (SENSE input) always 0 = every drive sense line asserted; the Q700
    ROM's IWM-mode .Sony driver saw an inserted, locked, unreadable
    floppy -> MacOS looped "Disk initialization failed because the disk
    is locked!".  Status now returns 0x80 | mode bits (sense
    deasserted).  q800/lc475 use the ISM register set - unaffected
    (verified by regression boots).
12. mc->no_cdrom set: the auto-created empty scsi-cd at ID 2 also
    produced the locked-disk alert on this ROM (q800 tolerates it).

## Remaining rough edges

- The ROM patch (finding 8) makes ALL cold-boot POST failures
  non-fatal, not just test 0x87 - genuinely broken hardware would boot
  anyway.  A more surgical alternative would emulate the VIA SR
  free-run interrupts, or run under -icount to restore realistic
  CPU-vs-timer ratios.
- ASC boot chime plays via the FIFO empty-cycle path; sound in MacOS
  untested.
- turboscsi +0x00 register semantics unknown (write-only stub).
- MCU regbank is a RAM-backed stub; RAM sizing works (fixed 128 MB via
  ramio zeros trick), banking registers not interpreted.
- Serial: with the default vc chardev on serial0 the break-detect test
  sometimes sees two well-spaced ext ints and enters the ROM's serial
  test manager instead of booting; -serial null avoids it.  Worth
  root-causing the spurious DCD/ext ints in escc some day.

## Build/test commands

    ninja -C build qemu-system-m68k
    build/qemu-system-m68k -M quadra700 \
        -bios /workspace/files/mac-roms/quadra700.rom \
        -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
        -display none -monitor unix:/tmp/q700mon.sock,server=on,wait=off
    # screendump via monitor socket -> /tmp/*.ppm; kill by saved PID only
