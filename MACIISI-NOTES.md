# Mac IIsi QEMU machine — journal

Goal: boot the Mac IIsi ROM far enough to show the blinking "insert disk"
floppy icon, with the display visible on the VNC desktop (`DISPLAY=:1`).

Working clone: `/workspace/src/qemu-maciisi` (branch `amiga`, shared with other
projects — rebase before push, never force-push). Build dir: `build-maciisi/`.
All session-unique names use the `maciisi-` prefix; screenshots go to
`/tmp/shots/maciisi-*.png` (shotview.py on :1 shows the newest).

## ROM

- `_maciisi_assets/maciisi.rom` — 512 KiB, from archive.org item
  `mac_rom_archive_-_as_of_8-19-2011` (in-zip file `36B7FB6C - Mac IIsi.ROM`),
  md5 `373f0b2150bc391227b7a2e32ac5ff2c`.
- Header: checksum dword `36B7FB6C` (matches known-good IIsi dump), word at
  offset 8 = `067C` = universal ROM version $067C.
- Reset vectors: dword@0 is the checksum (not a real SP), dword@4 = `0x2A` is
  an offset from ROM base — same convention q800.c handles (SP=garbage, PC =
  ROM_BASE + offset; ROM sets up its own SP).

## Hardware target (Mac IIsi)

- 68030 @ 20 MHz, RAM bank A at 0 (1 MB soldered + SIMMs; we default 8 MB).
- ROM 512 KiB at 0x40800000, mirrored in 0x40000000 region.
- I/O at 0x50F00000-ish; like q800, the 0x40000-sized I/O slice repeats
  through 0x50000000..0x54000000, so 0x50F0xxxx == 0x500xxxxx offsets:
  - VIA1  +0x00000 (regs spaced 0x200)
  - SCC   +0x04000 (Z8530/ESCC, it_shift 1)
  - SCSI  +0x10000 (NCR5380 — not in QEMU tree; stubbed for now)
  - "SCSI handshake/pDMA" region ~+0x06000 (watch trace)
  - ASC   +0x14000 (EASC)
  - SWIM  +0x16000
  - VDAC  +0x24000 (CLUT DAC for onboard video)
  - RBV   +0x26000 (RAM-Based Video ctrl; also VIA2-ish interrupt regs)
- ADB/RTC/PRAM via **Egret** MCU on VIA1 shift register + port B handshake
  (not in QEMU; to be written — cf. hw/misc/macio/cuda.c for the successor's
  VIA<->MCU pattern, MAME egret.cpp for protocol).
- Onboard video framebuffer is stolen from main RAM (address TBD empirically).

## Key branch facts

- This branch already has 68030 PMMU *instructions* (commit 9dd51b4c30):
  pmove/ptest/pflush parse and store regs, but **translation is a no-op**.
- The IIsi ROM programs the PMMU for the classic 24-bit map. Workaround:
  physically alias the 24-bit layout so no-op translation still works:
  RAM 0..0x7FFFFF, ROM alias at 0x00800000, I/O alias at 0x00F00000.
  Requires RAM <= 8 MB.

## Iteration log

### Machine identification reverse-engineered (2026-07-20)

The ROM's startup is driven by tables at ROM+0x32b4 (decoder probes) and
ROM+0x32c8 (machine entries, 64 bytes each).  Key findings, all verified by
gdb against the running ROM:

- **Decoder probes** (entries rel-linked from 0x32b4; probe code stubs at
  0x40803064/0x40803082/...): each tests VIA1 IER mirroring.  Kind 4
  requires a mirror at +0x20000; kind 5 (**the IIsi**) requires a mirror at
  +0x40000 (free via the repeating I/O slice), NO mirror at +0x20000 (bus
  error), and RBV (0x26000) + VDAC (0x24000) present.  Unmapped I/O must
  BUS ERROR (MEMTX_DECODE_ERROR) or probes mis-match; zeros look like
  present-but-quiet devices.
- **Kind-5 device table** (at 0x4080348c): ROM, VIA1 50F00000, SCC 50F04000,
  SWIM 50F16000, SCSI 50F10000 (+DRQ 50F12000, +HSK 50F06000), "VIA2 site"
  50F02000, ASC 50F14000, RBV 50F26000, VDAC 50F24000, extras 50F18000,
  50F1E020, 50F0C020 (the last two are IOP probes — auto-increment address
  counter test; correctly ABSENT on a IIsi).
- **RBV**: native window 0x2000 long, only low 5 address bits decode a
  register (rIER at any +0x1F-multiple offset 0x13).  IFR +0x03, monP +0x10
  (low 3 bits monitor sense: 6 = 13" 640x480 RGB — verified via the ROM's
  extended-sense dance writing 0xC3 and reading back), SIFR +0x12, IER
  +0x13 with the VIA set/clear protocol.  The RBV ALSO answers at the
  classic VIA2 site 0x2000-0x3FFF with VIA register spacing (reg =
  offset>>9, low bits don't-care) sharing the same IFR/IER state.
- **Machine entry accept test** (0x40802f8c): `(d1 & entry[32]) ==
  entry[36]` where d1's top byte = VIA1 **port A pins** read with DDRA
  forced to input.  Mask 0x56, IIsi wants 0x46: **PA4 low**, PA1/PA2/PA6
  high — machine-ID straps exactly like q800's VIA1A CPU-ID bits.  PA4
  high selects a sibling config.  Input pins must read the STRAPS when
  DDR reverts to input (not the last driven output value).
- **VIA2-position PA0**: NuBus-style pull-up must read 1 (checked with
  btst after DDRA bit0 cleared) or d7 bit26 = diag-request.
- **Reset flow**: cold reset → hw-init → **the ROM debug nub IS the normal
  continuation** (0x4084639a): it runs tests (incl. full ROM checksum),
  re-identifies hardware, and only hands off to the real boot; on failure
  or debug straps it sits at a '*'-command serial prompt on the SCC
  (0x40849d9c poll loop).  SysErrors also funnel here (magic 0xdb0
  cmp → 0x40849668 idle).
- **SWIM**: ROM programs IWM mode 0x17 and polls status until mode bits
  read back (swim.c fixed: latch-only writes for non-q6/q7 addresses,
  status reads return mode bits 0-4).
- **SCC**: ROM scratch-tests WR2/RR2 (escc.c fixed: WR2 reflects into RR2
  both channels), then loopback tests.  ROM checksum, XPRAM init (~all
  256 bytes over the bit-banged RTC protocol — slow!), SCSI 5380 bus
  reset all pass with the stubs.
- **Egret**: NOT needed so far!  The IIsi ROM speaks the classic 343-0042
  RTC bit-bang protocol on VIA1 PB0-2 (Egret emulates it) — the mac_via
  RTC engine ported into the machine handles PRAM/XPRAM fine.
- **Current blocker**: boot dies (SysError→nub prompt) in SwapMMUMode
  (0x40803ea0/0x40803ed8): the ROM programs the real 030 MMU (TC =
  0x80F84500: enable, IS=8 — discard the top address byte!, TIA=4, TIB=5,
  PS=32K; tables at top of RAM, CRP 0x7fff0002/0x007ffc90) and then runs
  code with 24-bit tagged pointers (e.g. reads 0xFFFFFFCE) that REQUIRE
  live translation.  QEMU's 030 PMMU on this branch stores registers but
  translates nothing → next step: implement the real 030 table walk in
  target/m68k (TT regs, IS shift, early-termination page descriptors,
  short+long formats, WP; flush TLB on pmove/pflush).

### MMU + Egret round (2026-07-20, later)

The rebase onto origin/amiga brought a full 68030 table-walk implementation
(get_physical_address_030, wired into tlb_fill) from the parallel session.
Remaining target bugs found & fixed here while chasing the boot:

- **PLOAD decode desync**: pmmu030's PFLUSH/PLOAD case returned without
  consuming the effective-address extension words — the instruction stream
  slipped 2 bytes inside SwapMMUMode and executed garbage.  Now gen_lea
  consumes the EA for PLOAD and the PFLUSH ea-form.
- **TLB flush on PMOVE**: MMU register writes and PFLUSH now flush the QEMU
  TLB and end the TB — EXCEPT writes with the FD (flush-disable) bit set:
  MacOS's SwapMMUMode does PMOVEFD to CRP then PMOVE to TC, deliberately
  relying on cached translations across the transiently mismatched state.
- **Format B bus-fault frames**: 030 data faults now push the long format
  $B frame (84-byte payload, extra internal words zero) instead of format
  $A; instruction faults keep $A.  The ROM's recoverable-bus-error catcher
  (vector 2 → 0x40806cb0) checks SSW&0xF1C0==0x140 AND format==$B before
  resuming at the recovery address — with format A it fell through to
  SysError 1.  Also the 030 tlb_fill miss path now sets 030-style SSW
  (DF/RW) instead of 040 bits.
- SwapMMUMode = ROM 0x40803ed8/0x40803ea0; MMU config blocks live at
  *(0xDDC)-46 (24-bit, TC=0x80F84500: IS=8 → high byte ignored, TIA=4,
  TIB=5, PS=32K) and *(0xDDC)-66 (32-bit, TC=0x80F05480: IS=0, TIA=5,
  TIB=4, TIC=8), pointers cached at lowmem 0xcb4/0xcb8, mode flag 0xcb2.
  The 32-bit root: 32 long-format descriptors, top entries early-terminated
  identity (incl. 0xF8000000+ → NuBus/compat identity).
- **Slot scan**: probes 0xFEFFFFFF down to 0xF1FFFFFF (14 slots) with the
  bus-error catcher installed via the table at *(0xdb8); empty slots fault
  and recover to the "absent" path — works now with format B frames.
- **Egret**: talks via VIA1 shift register, external clock.  Port B pins
  (active low): PB5=/TIP (host session), PB4=/TACK (host byte ack),
  PB3=/TREQ (Egret has data).  ROM driver at 0x4080a600-0x4080a7xx is an
  SR_INT-driven state machine: continuation address saved at glob+312,
  SR-int dispatch (0x4080a700) re-checks PB3 each byte.  Host sends
  packet [0x00] (ADB?) then turns ACR to shift-in and expects a response;
  current model answers [00 00 00] but the ROM only consumes 2 bytes and
  then re-opens a receive session and polls forever at 0x4080a8e6 — the
  packet framing/attention semantics still need work (next: check MAME
  egret.cpp semantics — needs Daniel's OK to fetch — or reverse
  0x4080a7xx-0x4080a9xx fully).
- Added VIA1 CA1 60.15Hz VBL tick + CA2 1Hz (mac_via-style); with them the
  boot services thousands of level-1 interrupts and runs RAM-RESIDENT code
  (A-line traps from ~0x86d8c0 with the stack up at 0x4008xx) — the OS
  boot world is executing; video/VDAC still untouched.
- Debug aids: -icount shift=1 makes the RTC bit-bang delays fast and the
  boot deterministic; gdb remote needs `set endian big` FIRST or all
  values appear byte-swapped.  Temporary CPU_LOG_MMU instrumentation in
  tlb_fill/transaction_failed (left in tree for now).

### v1 skeleton (2026-07-20)

- hw/m68k/maciisi.c: m68030, ramio container (unbacked reads return 0 for RAM
  sizing), ROM + aliases (32-bit and 24-bit), I/O slice + repeat alias +
  24-bit alias, VIA1 (plain mos6522 subclass, 783360 Hz timers), ESCC, ASC
  (EASC), SWIM, RBV/VDAC/SCSI as store+log stubs, catch-all I/O trace region
  at low priority. IRQ glue: VIA1=1, RBV=2, SCC=4, NMI=7 (autovectors).
- Run recipe (headless probe):
  `build-maciisi/qemu-system-m68k -M maciisi -bios _maciisi_assets/maciisi.rom \
   -d unimp,guest_errors -display none -serial mon:stdio`
