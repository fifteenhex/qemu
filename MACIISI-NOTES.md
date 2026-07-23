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

### Egret exchange, fully decoded from the ROM driver (for next session)

Wait loop: 0x4080a8a0 enables VIA1 IER=0x84 (SR int), state byte a3@(349)
= 0x24, kicks the exchange, then polls bit5 of a3@(349) until the SR-int
state machine clears it.  SR-int dispatch = 0x4080a700: a0 = continuation
from a3@(312), tests ORB bit3 (/TREQ) so every continuation branches on
"Egret has (more) data", then jmp (a0).

Observed continuation chain for the startup exchange (host packet [0x00]):
1. 0x4080a656 start-send: ACR|=0x1c (shift out), SR=byte0, save byte in
   a3@(348), ORB &= ~0x30 (drop /TIP+/TACK) → SR_INT when Egret clocks it.
2. cont 0x4080a612: if /TREQ deasserted → 0x4080a620; if asserted →
   "collision" path (bset7 of 350).  Then d1=0x10, 0x4080a67c turnaround:
   ACR &= ~0x10 (shift in), tst SR, ORB ^= 0x10 (TACK toggle).
3. cont 0x4080a624: if /TREQ ASSERTED here → bset5 of a3@(350) = "response
   coming" (CRITICAL: TREQ must be low at this interrupt or the response
   is later discarded at 0x4080a646 which clears the byte count when
   bit5(350) is unset).  count=0, then 0x4080a68e recv-byte: count++,
   buf[count]=SR, ORB ^= 0x30 (TIP+TACK ack toggle).
4. cont 0x4080a632/0x4080a63a: recv loop — each SR_INT reads SR, acks via
   ORB^=0x30, until /TREQ deasserts (checked at dispatch) or 8 bytes.

Model TODO: assert /TREQ (PB3 low) as soon as a response exists and keep
it asserted through the SECOND-TO-LAST byte's interrupt; the byte-N clock
should be driven by the host's TIP/TACK ack toggles (ORB writes), not by
SR reads — hook portB eor-0x30/0x10 transitions during a session as the
"clock next byte" strobe.  Current model advances on SR reads and the ROM
discards the response (only 2 of 3 bytes consumed, then it reopens a
receive session and polls forever at 0x4080a8e6).

MAME egret.cpp/h fetched (in _maciisi_assets/) — turned out to be full
68HC05 firmware emulation, not HLE; its pin table confirmed the PB
wiring.  The protocol was instead cracked by reversing the ROM driver —
see the commit "Egret startup sequence completes" for the full timing
rules now implemented and working (version cmd 0x00 → 01 01, channel
probes n<<4|0xF → empty, all complete exactly once).

### CURRENT BLOCKER: TimeDBRA (lowmem 0xD00) never calibrated

After the Egret sequence, an OS trap converts a delay via
`muluw #2656 / divuw (0xD00).W` at 0x40843a8c (caller 0x408422fc, via
the A-trap dispatcher; outer frames 0x40816aba/0x40809ae6) and 0xD00 is
still zero → SysError 4 → debug nub.  gdb-patching 0xD00=2656 (the
IIsi's documented TimeDBRA) at the post-Egret point (break 0x4080a96e,
script /tmp/maciisi-patch.gdb) lets the boot advance to the **SCSI
boot-device scan** (5380 arbitration: mode=0x01 etc.) — so this is the
last software gate before the boot-device hunt and, after it, video.

The ROM's calibration helpers (dbra loops timed against VIA1 T2, four
variants for different wait-sources) are at 0x40800884..0x408008d0; the
nub-era ACR writes at pc 0x40847100/0x40847106 look like a T1-based
calibration in the test phase.  Open question: which boot step populates
the OS-world 0xD00 block (Time Mgr init?) and why it hasn't run by the
time the Egret/ADB layer wants a delay — possibly our Egret exchange
timing (100us/byte) diverts the ADB layer into a timeout path that
real hardware doesn't take that early.

RESOLVED: TimeDBRA was a speed artifact — run with **-icount shift=7**
and the ROM's calibration works (no divide, no patching).  SCSI scan
then walks all 7 targets (5380 stub reports AIP during arbitration) and
loops rescanning = normal no-boot-disk behaviour.

### Video pipeline status (2026-07-20, session 3)

- The IIsi onboard video = pseudo NuBus slot $E.  The top 0x1400 bytes
  of the system ROM are its Declaration ROM (sig 0x5A932BC7 at 0x7fffa,
  byteLanes 0x0F, length 0x13E6); the hardware also decodes the ROM at
  the top of slot $E super space — modelled as an alias at 0xfef80000.
  With it the Slot Manager finds the card and loads the video driver
  into RAM (sense reads from pcs ~0x4728/0x4998 etc.).
- RBV +0x02 = RAW slot int lines (slot $E VBL = bit 6, must PULSE
  1.3ms/frame — the ROM has an explicit wait-for-edge at 0x4084ac90);
  +0x12 = slot enables (VIA set/clr protocol); IFR bit 1 = latched slot
  summary → level 2.  All implemented off the 60Hz timer.
- CURRENT BLOCKER: slot PrimaryInit phase — its temp stack grows down
  from ~0x57f4 INTO the boot heap zone based at 0x2000 (bkLim 0x57f4).
  Heap allocations climb to ~0x522c (suspiciously many — the video
  driver seems to be loaded/inited repeatedly), stack pushes then smash
  block headers, and the heap-coalesce walk at 0x4080e158 spins forever
  on a zero-size block.  Catch it with /tmp/maciisi-heap.gdb (watch
  0x522c; writer pcs: legit MM at 0x4080ddf0/0x4080eb30/0x4080e2ee,
  then stack pushes from pc 0x4080b170 with sp=0x522c).
  Next: find why the driver init retries (its failure exit — does it
  reject something after the sense read? trace the driver instance's
  flow), or whether the zone SHOULD be bigger/stack elsewhere (zone
  header at 0x2000: 57f4 2034 210c 0498...).
- gdb + icount note: interrupting with gdb during icount runs can
  trigger a QEMU 'bql_lock' assertion abort — reconnect rather than
  interrupt, or use breakpoints only.

### v1 skeleton (2026-07-20)

- hw/m68k/maciisi.c: m68030, ramio container (unbacked reads return 0 for RAM
  sizing), ROM + aliases (32-bit and 24-bit), I/O slice + repeat alias +
  24-bit alias, VIA1 (plain mos6522 subclass, 783360 Hz timers), ESCC, ASC
  (EASC), SWIM, RBV/VDAC/SCSI as store+log stubs, catch-all I/O trace region
  at low priority. IRQ glue: VIA1=1, RBV=2, SCC=4, NMI=7 (autovectors).
- Run recipe (headless probe):
  `build-maciisi/qemu-system-m68k -M maciisi -bios _maciisi_assets/maciisi.rom \
   -d unimp,guest_errors -display none -serial mon:stdio`

### FIRST PIXELS (2026-07-20, session 4) — sad mac rendered!

Video works end to end: ScrnBase = 0xFEE00000 (slot $E aperture), 640x480
1-bit rowbytes 80, drawn by the new maciisi-fb console device.  Current
screen: sad mac 0000000F/00000001 (SysError 1, bus error).  Screenshots
in /tmp/shots/maciisi-*.png; take them via QMP screendump (see below).

The crash: startup-screen gray fill (ROM 0x4084ad0a-ad14, pattern
0xAAAAAA in d3) derefs a TAGGED master pointer a3=0x800050b4 under the
32-bit MMU root (0x80000000+ early-terminated identity) → fault.  This
code path expects 24-bit addressing where IS=8 ignores the tag byte.
Crash regs are saved by SysError at lowmem 0xC30 (d0-d7, then a0-sp at
0xC50); error code word at 0xAF0.

Tried: forcing XPRAM 0x8A |= 0x05 ("32-bit addressing") in the RTC
sector-read path — no change (flag location/semantics wrong for this
ROM, or the decision is made elsewhere).  NEXT: trace who reads the
addressing-mode decision (watch SwapMMUMode d0 arguments over the boot;
find the boot's 24-bit switch that SHOULD have happened before
DrawStartupScreen — on real HW this fill runs in 24-bit mode).  Two
options: (a) make the 24-bit switch happen (find why it didn't), or
(b) find the real 32-bit-boot flag for ROM $067C.

Screendump recipe:
  qemu ... -qmp unix:/tmp/maciisi-qmp.sock,server,nowait
  then QMP: qmp_capabilities; screendump filename=/tmp/shots/x.ppm
Boot to sad mac takes ~4min real time at -icount shift=7.

### DISK BOOT (2026-07-20, session 5) — insert-disk icon reached, NCR5380 added

MILESTONE: the machine boots the ROM to the **blinking insert-disk floppy
icon** (gray desktop, live cursor) — the stated goal.  Screenshots
/tmp/shots/maciisi-blink-*.png.  Getting there needed, after the video
work: real framebuffer base 0xFBB08000 (aliased to slot-$E ScrnBase
0xFEE00000); VIA T2 one-shot (mos6522.c) so T2 sets IFR once per T2CH
load; RBV slot-$E VBL as a POLLABLE line (SIFR bit6 pulses 1.3ms/frame)
that does NOT raise a CPU interrupt (raising it = dsBadSlotInt / SysError
0x33).

NCR5380 SCSI controller written from scratch: hw/scsi/ncr5380.c (backed by
QEMU's SCSI bus), wired at 0x50F10000 (regs, reg-shift 4) with pseudo-DMA
aperture at 0x50F12000.  Boot media: _maciisi_assets/macos753.hda (OS
7.5.3, archive.org item hd-0-imaged-001, BlueSCSI raw image, 234MB, valid
Apple DDM 'ER' at block 0).  Attach with:
  -drive file=_maciisi_assets/macos753.hda,format=raw,if=none,id=hd0 \
  -device scsi-hd,drive=hd0,scsi-id=0

5380 protocol as the Mac SCSI Manager drives it (all reverse-engineered
from the ROM at 0x408076xx-0x408079xx):
- Arbitration wins immediately; ICR bits 6/5 (AIP/lost-arb) are read-only
  status the ROM polls — DO NOT mask them off on ICR read (that was the
  first bug: no arbitration ever completed).
- Selection completes on the ICR write where SEL+DATA are asserted and BSY
  is released; target id = ODR & 0x7f. Disk is at ID 0.
- Command/status/message bytes: register REQ/ACK handshake.  Wait-for-REQ
  helper 0x408078a4 reads CSB (reg4) bit5; phase-match helper 0x40807930
  reads BSR (reg5) bit3 = TCR-programmed phase == bus phase.  CRUCIAL: REQ
  for a new phase must be asserted only when the initiator RELEASES ACK on
  the previous phase's last byte, else the ROM's "wait REQ clear" deadlocks.
- Data-in: the ROM does BLIND reads (read CSD repeatedly, no per-byte ACK).
  CSD read in DI phase must deliver the current byte and auto-advance.
- Power-on UNIT ATTENTION must be cleared before each command (the ROM's
  blind reader issues no TEST UNIT READY / REQUEST SENSE).
- scsi-disk reads are ASYNC (aiocb): single-buffer with an xfer_pending
  guard or you hit `r->req.aiocb == NULL` in scsi_read_data.

CURRENT STATE: the ROM selects the disk and reads block 0 / partition map /
boot blocks (READ(6), GOOD status) but only ~4-8 reads happen then it
returns to the insert-disk search (or earlier it sad-mac'd at ROM
0x40836180 deref of a disk-loaded structure).  The data path is not yet
coherent across sustained multi-block transfers — completes:commands is
~1:2, so half the commands stall.  NEXT:
1. Verify byte-exact data integrity: dump the bytes my controller delivers
   for block 0 and diff against the image (should be 45 52 02 00 ...).
2. Find why half the commands don't complete — likely the DI→ST phase
   transition or a REQ/ACK edge case when the ROM mixes blind reads with
   handshake polling; trace a single READ end-to-end.
3. The transfer_data async callback asserting REQ vs the blind-read
   advance may race — consider making data-in fully synchronous by
   pre-reading the whole transfer into the buffer on do_command.
4. Only then will the loaded System boot; may also need real ADB (mouse/kbd
   via Egret) for the Finder.

### WELCOME TO MACINTOSH (2026-07-22, session 6) — SCSI data path fixed

MILESTONE: **"Welcome to Macintosh" renders** (screenshot
/tmp/shots/scsifix-10a.png) — the ROM boot, the disk's Apple_Driver43
and the System file all load over the 5380.  Three separate bugs stood
between the insert-disk icon and the splash; all found by tracing with
guest PCs on every register access and then reading the ROM at those
PCs.

1. **Async data-in raced the blind reader** (hw/scsi/ncr5380.c).  The
   old chunked-async path waited on the aiocb to flip DI→ST.  The ROM's
   blind loop counts its bytes and then polls for STATUS *immediately*;
   while the phase change sat in the aio queue, SCSIComplete's
   wrong-phase handler (ROM 0x408078c8: TCR=3 phase check, TCR=1,
   CSD read, ACK pulse, repeat) clocked in PHANTOM stale bytes — 515
   extra after a 512-byte READ → guest buffer overrun → the double MMU
   fault / sad macs, and completes:commands ~1:2.  Fix: pre-read the
   ENTIRE transfer synchronously in do_command (pump blk_drain until
   the completion callback fires), enter STATUS the moment the guest
   consumes the last byte (pdma read, or ACK-release after the last CSD
   read).  Data-out mirrors this: collect whole transfer, then feed the
   device in one synchronous pump.  Also: bus-free must clear ALL of
   CSB (stale phase bits 0x1c made the driver's wait-for-bus-free poll
   time out and fail perfect transfers).
2. **Wide pseudo-DMA accesses** (hw/m68k/maciisi.c): the pdma region
   handlers ignored access size (1 byte per move.w/move.l!).  .impl
   min/max = 1 lets the memory core split them MSB-first.
3. **The handshake aperture was unmapped** — THE big one.  The IIsi has
   TWO pdma windows: +0x12000 (plain, polled; the ROM's single-block
   reads) and +0x6000 = 0x50F06000 ("SCSI+DRQ", Linux mac_scsi's drq
   region).  The multi-block blind routine (ROM 0x4080924e, aperture
   ptr = SCSIGlobals+72 + 0x60) reads through the handshake window and
   *expects a bus error* when DRQ stops (bus-error handler 0x40808a64
   with a retry counter, gives up via rte→0x40808a9c → error 5).
   Unmapped, every access BERR'd, the transfer moved 0 bytes, the data
   was silently DRAINED by SCSIComplete (looked perfect on the wire!)
   and the ROM re-read the boot blocks 20x → X-floppy.  Mapped it to
   the same byte pump; it faults only when no byte is available in the
   current data phase (= real timeout semantics under the synchronous
   model).  Diagnosis trick: trace ncr5380_datain (whole-transfer
   lba/len/checksum at do_command) proved the SCSI layer byte-exact
   while the guest still failed — the bug HAD to be in delivery.

Egret, post-splash: the OS-level ADB manager (RAM driver at ~0x15000-
0x19700, globals ptr lowmem 0xB78) is stricter than the ROM driver:
- /XCVR_SESSION must go idle when the host releases the session (model
  parked it low = "response pending" forever; OS never started the next
  exchange).
- After release, the Egret clocks one final SR interrupt ("session
  closed"); the OS parks its state machine on it.
Both added.  OS-era Egret flow observed: Talk R3 probes 0x0f..0xff,
then 0xfc, then the classic RTC bit-bang from ROM 0x4080b240 — the OS
runs a 1Hz loop backing the time up into XPRAM 0xB8-0xBB (write,
verify, +1s, repeat).  Whether that loop is normal or a failed clock
init is still open — the boot sits at the splash with SCSI idle
(198 commands, all complete) while it runs.

RAM sizing mystery (open): lowmem MemTop reads 0x5A00000 (90MB!) on
the 8MB machine — BufPtr/boot stacks point into unbacked space (only
'Tina' bank-B probes at 0x4000000 ever touch the holes though).  With
-m 64M/-m 128M MemTop comes out exactly right (0x4000000/0x8000000)
but the System boot then crash-loops (X-floppy, endless retries) —
worse than 8MB.  Mirroring the 8MB bank across the 64MB bank-A window
(real SIMM partial decode) did NOT change the 90MB result and turned
the harmless phantom reads into aliased writes = memory corruption →
reboot loops; reverted.  The ROM's size decision is NOT a plain
pattern probe — needs the sizing routine (around pc 0x4084a390) read
properly next time.

Boots are somewhat FLAKY run-to-run (same build: Welcome+stall one
run, early X-floppy the next) — suspect marginal Egret/RTC timing
feeding the ROM garbage PRAM occasionally.  NEXT:
1. Decode the 1Hz XPRAM loop's exit condition (is it SetDateTime
   verify?  does it want Egret packet-time commands answered?), and
   what the boot waits for after ADB probe 0xfc.
2. RAM sizing: find where 0x5A00000 comes from (watch lowmem 0x108
   writes; gdb watchpoint works but PCs come back garbled — use the
   ram-hole/pc logs instead).
3. ADB devices behind the Egret (QEMU adb-kbd/adb-mouse via
   adb_request, cuda.c-style) for Finder input once the desktop shows.
4. The 5380 model is solid now — don't suspect it first anymore
   (commands:completes 1:1, byte-exact, ncr5380_datain proves it).

Addendum (same session, later): the splash stall is DETERMINISTIC —
every successful boot parks at exactly 198 SCSI commands (all
complete), after the ADB Talk-R3 probe sweep ends with 0xfc, with the
1Hz XPRAM clock-backup loop (write 0xB8-0xBB, +1s, verify) running
forever.  Ruled out since the last entry:
- No classic seconds-register access AT ALL (cmd bytes are only WP
  0x35, XPRAM writes 0x38-0x3f, XPRAM reads 0xb8-0xbf) — so the
  ignored-seconds-write theory is dead; the OS keeps time via XPRAM.
- The OS rewrites ALL XPRAM sectors early (invalid-PRAM rebuild);
  seeding 'NuMc'@0x0C + 0x8A=0x05 at init removes the churn but the
  stall is unchanged.
- Egret session-close interrupt + /XCVR idle fixes: correct per the
  Linux via-maciisi model, but A/B against the pre-fix binary shows
  identical behaviour (both reach Welcome, both park at 198).
- Waited 30+ min: no crawl, hard wait.  Ticks advance, 60Hz+1s ints
  fire, SCSI idle, ADB request queue empty (head=0 at [0xB78]+220).
Best next theory: after the probe sweep the ADB manager expects
Egret-INITIATED traffic (autopoll data or the Egret time packets) that
our reactive-only model never sends; or command 0xfc (addr 15 Talk R0
— or an Egret function 0xC?) demands a real answer, not the "no
response" turnaround.  Wire QEMU's adb-kbd/adb-mouse behind
egret_process (cuda.c-style adb_request) and answer Talk R3 probes
with real device registers — that changes the whole post-probe flow
and is needed for Finder input anyway.
