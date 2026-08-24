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

### VM OFF — THE SPLASH STALL SOLVED (2026-07-23, session 7)

MILESTONE: the deterministic "Welcome to Macintosh" park is ROOT-CAUSED
and FIXED — three real bugs plus one self-inflicted config bit.  The
boot now clears the splash (screen goes white, desktop draw begins) and
dies later in startup with SysError 1 — the new frontier.

1. **ADB devices wired behind the Egret** (adb-kbd + adb-mouse on an
   ADB bus owned by the VIA1/Egret, cuda-style adb_request).  Wire
   format, observed: a packet is the RAW ADB COMMAND BYTE plus listen
   data — NO type byte on this machine (PRAM/RTC goes over the
   bit-bang, so packets only ever carry ADB).  [00] at startup is ADB
   SendReset; keep answering it 01 01.
2. **/XCVR_SESSION polarity was INVERTED** in the model.  Ground truth
   from the ROM transport disassembly (0x4080a5f6-0x4080a70e, a3 =
   [$0CF8]; flags +350, byte count +355, receive buffer +356):
   - receive loop consumes a byte while PB3 is HIGH at the interrupt;
     PB3 LOW ends the response (cont 0x4080a63c);
   - PB3 LOW already at the first post-turnaround interrupt sets flag
     bit5 and the count is DISCARDED at the end (seq/and 0x4080a646).
     (The session-6 notes had this backwards.)
   So: real response = PB3 high during bytes, low as end marker;
   no-response = PB3 low throughout (driver still clocks 2 junk
   bytes).  The old model delivered real Talk replies with the
   discard signature and empty probes as phantom [00 00] responses —
   the OS ADBReInit "found" devices at addresses 0/1 and wedged its
   next request forever (spin at 0xf034 on a completion flag).
3. **Listen packets were dropped**: the driver never does the ACR
   turnaround for commands without responses, and egret_process only
   ran at turnaround.  Process leftover command bytes at session
   release — the ADBReInit relocation dance (Listen R3 move-to-15,
   verify, move-back: [2b 0f fe] / [fb 02 fe] etc.) needs it.
   Also: both TIP+TACK released = host close/park, never an ack (ack
   cadence alternates 0x10/0x20 states).  And the extra "session
   closed" interrupt is GONE — with correct polarity it makes the OS
   ADB manager run its transaction-end path twice (second entry with
   busy count [0xB78]+0x48 == 0 → infinite completion wait).
   With all this the sweep finds kbd at 2 (R3=[02 01]) / mouse at 3
   ([03 02]), runs the full collision dance 3x and parks cleanly
   after Talk R0 addr 2.  ADB was NOT the splash stall.
4. **target/m68k: 030 long-descriptor LIMIT fields now checked** (real
   emulation bug).  The IIsi ROM's compact 32-bit map bounds its
   level-B RAM table to ONE entry via limit=0 in the root descriptor
   ([0x7ffce0] = 0000fc0a 007ffcd0; level-B entry 0 = 0x00000019 →
   maps 0-8MB; the rest of that table area is 0x6db6db6d RAM-test
   residue that decodes as "valid" descriptors).  Without limit
   checking, virtual 8-128MB translates into garbage instead of
   taking a limit-violation bus error.
5. **THE STALL: we were enabling VIRTUAL MEMORY ourselves.**  The
   session-4 hack forces XPRAM 0x8A reads |= 0x05.  Bit 0 = 32-bit
   addressing (needed).  Bit 2 = VM ENABLED: the 7.5.3 memory-manager
   patch then installs a 90MB logical space — 0x5A00000 == the size
   of the previous owner's "VM Storage" swap file still sitting on
   the macos753 disk (logical+physical EOF 0x05A00000 in the HFS
   catalog).  MemTop/[0x1EF4] became 0x5A00000 regardless of -m
   (8MB/64MB both), the boot world (BufPtr, stacks) moved above
   backed RAM, and everything parked at the splash with the 1Hz
   XPRAM clock loop running.  Evidence chain: MemTop writer = 
   transient patch code 0x1a6ac (gdb watch on 0x108 over the
   deterministic boot, ~400s in), size = installer argument via
   memory-manager globals [0xB78]+4, [0x1EF8] physical stayed a
   correct 0x800000 while [0x1EF4] logical read 0x5A00000, VM
   backing-file open path in the transient module (GetString -16508,
   fnfErr → create).  Fix: force only bit 0.  MemTop now 0x800000.
   (gdb-forcing MemTop back at 0x1a6ac was NOT enough — the 90MB
   lived on in the installer globals; renaming "VM Storage" on a
   disk copy also NOT enough — the size is config, not file-derived.)

Boot state after the fixes: splash clears (~4min at icount shift=7),
screen goes white, then **SysError 1 (bus error)** → ROM debug nub
(0x408496ce prompt loop, serial silent).  First-error capture (gdb
break 0x40849668): crash PC [0xC70]=0x4080b8de = the find-DRVR-by-name
loop walking the unit table ([0x11C], count [0x1D2], 'DRVR' pushed at
0x4080b90e) — a DCE handle deref bus-errors.  The 0xC30/0xC50 reg
block is contaminated by the SysError handler itself (a0=0xFFFFFAF0
is a jump-table offset at 0x40802820, not a fault address).  NEXT:
run with -d mmu fault logging to get the faulting physical address;
suspect a tagged master pointer or a stale pre-VM-era pointer.

Session tools: /tmp/adbwork-qmp.py (QMP + HMP one-liners),
/tmp/adbwork-input.py (screendump/mouse/keys), gdb-multiarch batch
scripts against -gdb tcp:127.0.0.1:412xx (watchpoints work; the
conditional-eval throttle makes busy-address watches crawl — watch
0x108 is cheap, 0xf110 is not).  Boots are deterministic enough that
two-stage capture (find PC in run N, dump live state in run N+1)
works reliably.

Addendum (session 7, later) — GRAY DESKTOP reached:
- The post-splash SysError 1 was the ROM find-DRVR-by-name loop
  dereferencing master pointer 0x80004cb5 — a 24-BIT-TAGGED handle
  (state flags in address bits 31-29), identity-mapped by the ROM
  tables into the top half of physical space.  On real hardware the
  A31 half falls through to the RAM decode with only the LOW 24 BITS
  significant (that is why the ROM maps the whole top half identity).
  Modelled as a low-priority 0x80000000..0xFFFFFFFF region forwarding
  to addr&0xFFFFFF (a plain alias at 0x80000000 is NOT enough — locked
  resource handles carry 0xE0 in the top byte).  -d mmu ("txn fail"
  lines, direct-to-file, NOT through a pipe/tail) confirmed the fault
  gone; boot now paints the full-screen gray desktop pattern
  (/tmp/shots/adbwork-run23-a.png, run24-a).
- CURRENT FRONTIER: after the desktop pattern (menu bar not yet
  drawn), the ROM heap-coalesce walk 0x4080e148-0x4080e17c spins on a
  zero-size free block: zone a6=0x2000 (the 24-bit boot heap, size
  mask d2=0xFFFFFF), free run start a1=0x7894, d1=0x360, dead block
  a0=0x7bf4 with [0x7bf4]=0.  A5=0x400a78 → the memory-manager patch
  install world is still live at that point.  Same failure shape as
  session 3's PrimaryInit stack smash, different era.  A gdb
  watchpoint on 0x7bf4 caught only the ROM RAM-test write before a
  gdb printf quirk ($a6 in the format string errored out the command
  list — print registers individually next time).  Boot remains
  deterministic; two-stage capture will find the corruptor.
- Cursor/input untested: display blocked before Finder; the ADB path
  is exercised only by the probe sweep so far.  QMP input-send-event
  reaches adb-kbd/adb-mouse (shift-at-boot test delivered the event,
  though the OS ignored it at that phase).

### HEAP HANG ROOT-CAUSED + FIXED — MacOS GUI now boots (2026-08-23, session 8)

MILESTONE: the gray-desktop heap-coalesce hang is ROOT-CAUSED and FIXED,
and the boot now advances ALL the way to the **MacOS GUI System Error
dialog** ("Sorry, a system error occurred / bus error", bomb icon,
Restart button, live cursor) — i.e. the System file loads, QuickDraw +
the Font/Window/Dialog managers run, and the OS is loading its
extensions/drivers when one specific driver bus-errors.  Two intermediate
screenshots: /tmp/shots/maciisi-24bit-welcome.png (empty Welcome box),
maciisi-syserr-dialog.png (the bomb dialog).

ROOT CAUSE of the heap hang (verified by two-stage capture):
- The whole Memory Manager picks 24-bit- vs 32-bit-FORMAT block routines
  from a **global** mode flag: lowmem 0x1EFC bit0.  The ROM builds twin
  dispatch tables at lowmem 0x1E00 (24-bit) and 0x1F00 (32-bit) from a
  template at ROM 0xCB20 (installer 0xCC60-0xCCFC), and every MM trap
  indexes the active table via ROM 0xDCEA (`btst #0,0x1EFC; +0x1E00 or
  +0x1F00`).  It is NOT per-zone; the zone's own format byte at zone+30
  is only recorded.
- The ROM builds the boot heap (SysZone at 0x2000) as a **24-bit** zone
  whenever VM is off — the zone-format choice at ROM 0x40800240-0x268
  (and the SysZone builder at 0x408004ae-0x508) keys on XPRAM 0x8A **bit2
  (VM)**, NOT bit0: `btst #2,0x1EFC; bit2? bset #0 : bclr #0` around the
  `_InitZone`.  So VM off ⇒ operative mode cleared ⇒ 24-bit boot heap.
- We were FORCING XPRAM 0x8A bit0=1 ("32-bit addressing") in the model's
  RTC read path.  The ROM read that, flipped the global operative mode to
  32-bit, and then grow/split operations on the 24-bit boot heap used the
  32-bit-FORMAT free-block writers.  Captured corruptor (gdb break on
  0x4080DDF4 with $a0<0x10000): GROW32 (ROM 0x4080E4D0, retaddr
  0x4080E4EA) wrote a 32-bit-format free block {tag:0 @off0, size @off4}
  at 0x7BF4 inside the 24-bit zone.  The 24-bit coalesce walk (ROM
  0xE128/0xE158) then read [0x7BF4]=0 as a {size=0, free} block and
  a0=a1+0 looped forever.  Confirmed: every ddf4 low-mem write had
  zoneflag(zone+30)=0 (24-bit zone) yet m1efc=1 (32-bit global).
- WHY this differs from real HW: classic MacOS boots the ROM in **24-bit
  mode**; "32-Bit Addressing" (XPRAM bit0) is engaged LATER by the System
  ("32-Bit Enabler"/Mode32), which re-inits the Memory Manager in 32-bit
  form.  During ROM boot the operative mode must stay 24-bit to match the
  24-bit boot heap.  Our forced bit0=1 created the inconsistency.

FIX 1 (hw/m68k/maciisi.c, the real root cause): stop forcing XPRAM 0x8A
bit0.  Boot 24-bit-consistent (seed PRAM[0x8A]=0x00, RTC read no longer
ORs 0x01).  The A31-half physical decode (0x80000000.. -> low 24 bits,
added session 7) already resolves 24-bit tagged master pointers, so the
old session-4 blocker does not recur.  This alone eliminates the heap
hang.

That exposed the next 24-bit blockers — the onboard-video apertures are
only mapped at their 32-bit physical addresses, but in 24-bit mode the
ROM's PMMU map reaches them through the classic 24-bit->32-bit slot
compat window, which **zeros the level-A index nibble** (bits 20-23):
- Sad Mac 0000000F/00000001 at DrawStartupScreen gray-fill (ROM
  0x4084AD12, `movel d3,(a0)+`, a0=ScrnBase=0xFBB08000).  `-d mmu`:
  `addr=fbb08000 -> phys=fb008000` (TableA[0xB] early-terminates to page
  frame 0xFB000000; TC=0x80F84500 => IS=8, PS=32K, TIA=4, TIB=5).  The
  fb only lived at 0xFBB08000.
  FIX 2: alias the framebuffer at **0xFB008000** (vram_alias24).
- Then a 4492x spin (video driver RAM pc 0x000564EC) polling the slot-$E
  aperture 0xFEE03944 -> phys 0xFE003944 (TableA[0xE] -> 0xFE000000),
  unmapped, bus-erroring every iteration.
  FIX 3: alias the same VRAM at **0xFE000000** (vram_slote_alias24) — the
  24-bit view of the slot-$E ScrnBase window 0xFEE00000.

With FIX 1+2+3 the boot clears DrawStartupScreen, paints the desktop, the
System loads over the 5380, and MacOS runs its GUI (fonts/dialogs) — a
huge jump past the old ROM-heap frontier.

NEW FRONTIER (session-8 end): a System driver/extension bus-errors and
the GUI SysError dialog appears.  Crash captured from lowmem
(0xC70=crashPC, 0xC30/0xC50=regs, 0xAF0=errcode):
- crashPC = **0x000752F6**, faulting a0 = **0x00A44000**, d0 =
  0x44617665 = **'Dave'**.  errcode 0xAF0 = 0x00010000 (SysError 1/bus
  error).  `-d mmu`: `addr=00a44000 -> phys=fa044000` (24-bit slot
  space), resp=2, pc=000752F6 — this one is NOT recovered (unlike the
  ROM's slot-scan probes at 0x40805E84 which do recover).
- The routine at 0x752F6 is a signature-scan loop: `cmp.l (a0)+,d0 ; bne
  0x752f6` searching UPWARD from 0x00A44000 for the longword 'Dave'.
  0x00A44000 is above our 8 MB RAM (and in 24-bit mode RAM caps at 8 MB,
  slot/I-O sit above), so the first read bus-errors and the driver's
  handler (if any) does not catch it.
- OPEN: is this (a) a driver that expects to bus-error-recover while
  scanning and our 24-bit-mode bus-error frame/SSW delivery to RAM-level
  handlers is subtly wrong (cf. session-7 format-$B work — but that was
  for the ROM catcher; check the RAM handler's expectations), or (b) a
  specific OpenRetro-image extension probing for a card/ROM signature
  that simply is not present.  The dialog itself says "restart holding
  shift to disable extensions" — booting with Shift held (inject via
  adb-kbd during INIT load) is the obvious next experiment; if the Finder
  then comes up we have a usable OS and can bisect which extension.
- Next-session concrete steps: (1) shift-boot to skip extensions and try
  to reach the Finder; (2) if that works, re-enable extensions and find
  the 'Dave' one (dump the driver name near 0x75000, e.g. the resource
  header / DRVR name) — it may be Ethernet/SCSI-card related and thus
  relevant to the NuBus-Ethernet goal; (3) investigate RAM-level
  bus-error recovery: break the 68k bus-error vector while this scan runs
  and see whether an installed handler RTEs (frame format $A vs $B, SSW
  DF/RW/RM bits) — the ROM slot probes recover, so compare their vector
  state with this driver's.

Model changes this session (all hw/m68k/maciisi.c only; q800 re-verified
booting to its blinking-disk desktop, /tmp/shots/q800-chk.png — no shared
code touched):
1. RTC/PRAM 0x8A read no longer ORs bit0; seed PRAM[0x8A]=0x00 (24-bit
   boot).  THE root-cause fix for the heap hang.
2. vram_alias24 @ 0xFB008000  — 24-bit view of the fb (ScrnBase
   0xFBB08000).
3. vram_slote_alias24 @ 0xFE000000 — 24-bit view of slot-$E aperture
   (0xFEE00000).

Shift-boot experiment (negative): held Shift down via QMP
input-send-event (adb-kbd) across the whole extension-load window
(repeated key-down, no up, ~180-390s into boot).  Same 'Dave' bus-error
dialog — so either the 'Dave' scan is core System startup (Slot Manager
/ a boot-time driver), not a disableable INIT, OR our ADB modifier state
is not being reported to the OS's shift-check.  Worth re-trying with a
cleaner "modifier held" path (verify the adb-kbd reports the shift bit in
its Talk R2, and that autopoll delivers it) before concluding it is core.

gdb/tooling notes for next time: the m68k remote calls a6 "fp" (use
`info registers fp`, not a6, or the whole command list errors out).
`if $a0 < ...` needs the `$`.  Two-stage capture works great here; the
`-d mmu -D <file>` "txn fail: phys=.. addr=.. type=.. resp=.. pc=.."
lines are the fastest way to pin a fatal fault (grep the tail; RAM pcs
are 0x000xxxxx, ROM 0x408xxxxx).  Boots ~4-5 min at -icount shift=7.
HMP `pmemsave 0x<addr> <declen> "<path>"` dumps RAM to disassemble
(m68k-linux-gnu-objdump -b binary -m m68k:68030 --adjust-vma=).

### THE 'Dave' BUS ERROR ROOT-CAUSED (2026-08-24, session 9)

The System-startup bus-error dialog (crash pc 0x000752F6, a0=0x00A44000,
d0='Dave') is fully explained — it is NOT a fault-recovery bug and NOT
an extension; it is a machine-model bug dating back to session 3.

Evidence chain (all from the live crashed machine + ROM disassembly):
- The crashing code is a 26-byte DRVR stub named
  ".Display_Video_Apple_VISA", copied verbatim from the system ROM's
  Declaration ROM (DeclROM file offset 0x7f7c0; DeclROM = top 0x13E6
  bytes of the 512K ROM).  The stub is a trampoline:
  `movea.l #0x00A44000,a0; move.l #'Dave',d0; loop: cmp.l (a0)+,d0;
  bne loop` then replaces dCtlDriver with the found body and jumps
  into it.  Its sibling ".Display_Video_Apple_RBV1" (offset 0x7f768)
  scans #0x40844000 for 'Fung' the same way.  The driver BODIES are in
  the main ROM: 'Fung' @ROM+0x4a5f4 (RBV1), 'Dave' @ROM+0x4ad9c
  (VISA).  0x00A44000 is the Mac LC's ROM window (LC ROM lives at
  0xA00000; body at 0xA4AD9C) — the VISA stub is ONLY runnable on an
  LC.  There is no bus-error catcher; on the machine it belongs to the
  scan always terminates.  On a IIsi 0xA44000 is empty slot-$A space:
  instant unrecoverable bus error.  (So: no fault-frame issue at all.)
- This $067C ROM is a universal "Macintosh Family 2.0" ROM; its
  DeclROM carries board sResources for II/IIx/IIcx/SE30/IIci/IIfx +
  "Macintosh A/B" (IIsi/LC) and video sResources for BOTH video chips:
  RBV1 ids 129-159 (drhw 0x18) and VISA ids 162-186 (drhw 0x1A).
- Unit table at the crash (UTableBase 0x4B60): units 48..62 = DCEs for
  EVERY RBV id (129,130,134,135,137,138,142,143,145,146,150,151,154,
  158,159), all successfully opened (dCtlDriver -> ROM body
  0x4084A5F8), then unit 63 = id 162, slot 0x0E, devBase 0xFEE00000 —
  the first VISA sResource, which died in its Open.  The System was
  enumerating and opening EVERY video sResource of pseudo-slot $E.
- Why real hardware never does that: the sResource set is pruned at
  ROM-boot time by the family PrimaryInit sExec in the DeclROM
  (code 0x4087EDFC..0x4087F232; the only PrimaryInit item, attached to
  the IIci board sResource, is shared by the whole family).  It reads
  the machine byte [[0xDD8]+18], keeps the matching board sResource
  (table @0x4087F248: IIsi = index 12 -> board id 7 "Macintosh A"),
  deletes the other boards, reads the RBV monitor sense ([0xCEC]+0x10
  >> 3), and prunes the video ids (table @0x4087F258) down to the one
  matching machine+monitor via SDeleteSRTRec/SSetSRsrcState —
  **with spSlot hardcoded to 0** (it clears SpBlock+49).
- gdb trace of the ROM PrimaryInit phase (0x4080624A: swaps to 24-bit,
  then runs slots 0,14,13,...,1): in OUR boot it found a board
  sResource and EXECUTED PrimaryInit **twice** — once for slot 0
  (sInfo 0x4244, siDirPtr 0x4087EC1A = the DeclROM read directly from
  the system ROM: the ROM registers the motherboard DeclROM itself as
  pseudo-slot 0!) and once for slot 14 (sInfo 0x425C, siDirPtr
  0xFEFFEC1A = our session-3 alias of the ROM at 0xFEF80000).  Both
  runs prune only SLOT 0's records; the duplicate slot-$E copy stays
  complete and unpruned, boot video got registered from it (first
  video id >= 0x80 present = 129 rather than the sense-matched one),
  and the System's video enumeration walked all 23 slot-E entries into
  the VISA stub.
- Conclusion: real IIsi hardware does NOT decode the DeclROM at the
  top of slot $E standard space.  The 0xFEF80000 ROM alias (added in
  session 3 to make "the Slot Manager find the card") was wrong; the
  correct discovery path is the ROM's own slot-0 registration, which
  needs no alias at all.

FIX (hw/m68k/maciisi.c only): remove the maciisi.rom-slote alias at
0xFEF80000 (rom_slot_alias).  Slot 0's registration is untouched by
this (it reads 0x4087xxxx), PrimaryInit still runs for slot 0 and now
prunes the ONLY copy of the sResources.

Continuation (session 9, same day) — FINDER DESKTOP REACHED.  Removing
the alias alone broke the boot differently (white screen, ScrnBase
garbage; then a machine-check death) and pulling THAT thread uncovered
four more real bugs, each root-caused in turn:

- PrimaryInit gdb trace (phase runner ROM 0x4080624A: swaps to 24-bit,
  runs slot 0 then 14..1; per-slot 0x4080627A finds the board sResource
  via SGetTypeSRsrc cat=1, checks sInfo, SFindStructs item 34, sExecs
  it): with the alias gone, slot 0 finds the board and EXECUTES
  PrimaryInit, slots 1-14 report smNoMoresRsrcs (0xFEC7).  Correct.
- But the boot-video path (0x40801002: SpBlock cat/typ/sw = 3/1/1,
  spSlot starts at 0, sNextTypeSRsrc; found -> sFindDevBase -> ScrnBase
  [0x824], register DCE 0x40800CA0, marker [0xDB0]=test pattern) then
  found NO video sResource: PrimaryInit had pruned ALL of them.  Its
  monitor sense read is (RBV+0x10 >> 3) & 7 — as is the RBV1 driver
  body's (ROM 0x4A674) and the mode code's (0x42088) — while the model
  reported sense 6 in the LOW bits (journal session 1 had it wrong):
  PrimaryInit saw 0 = no monitor = delete everything.  FIX: monP
  returns sense 6 in bits 3-5 (low bits are the drive side, read back
  as written).
- With sense visible, PrimaryInit painted the startup gray screen
  through the ROM's 24-bit map... into LOW RAM, destroying vectors +
  the running sExec copy (QEMU abort: PC=0xAAAAAAAA).  The dumped live
  tables (CRP {7FFF0003, 007FF920}) revealed the REAL IIsi memory
  layout: with a monitor sensed the ROM reserves PHYSICAL 0x0000-4FFFF
  (320 KiB) as the frame buffer (RBV video = bottom of RAM bank A, as
  on the IIci) and relocates logical RAM +0x50000 in BOTH PMMU maps
  (level-A/B frames 0x050000, 0x150000, ...); the video windows
  (ScrnBase 0xFBB08000, 24-bit 0xB08000, VideoInfo at [univ+8]
  rom+0x3982 says base 0xFBB08000) translate DOWN to phys 0.  All the
  session-5..8 VRAM devices/aliases at 0xFBB08000/0xFB008000/
  0xFEE00000/0xFE000000 only ever worked because the sense bug made
  the ROM build no-video IDENTITY maps that landed on them.  FIX:
  delete the fake VRAM + aliases; maciisi-fb now scans MACHINE RAM at
  offset 0 (fb.ram = machine->ram).
- The gray fill still crashed: QEMU's 030 walk masked early-
  termination page frames down to the covered span
  ((table & ~(span-1)) | offset), so frame 0x50000 with a 1 MB span
  became 0 -> the relocation map degenerated to identity and the fill
  again hit low RAM.  The 030 ADDS the residual logical bits to the
  PS-aligned frame (UM 9.5.3).  FIX in get_physical_address_030
  (+ only advertise span-sized TLB pages when the frame is aligned).
- Boot then died in a level-2 interrupt storm (a million Level 2 INTs
  at the SCSI wait 0x40807826): the 5380 IRQ was wired STRAIGHT to the
  CPU level-2 glue input, invisible to and unclearable by the OS's
  level-2 dispatcher (and fighting the RBV's own line updates).  FIX:
  route it into RBV IFR bit 3 (VIA2 layout, cf. Linux mac via2).
- Then a deterministic DOUBLE MMU FAULT during the System's boot-time
  MMU-table rebuild (RAM pc ~0x1273E/0x129BA/0x129D6): it reserves new
  table space below BufPtr [0x10C] (a workspace block from NewPtr,Sys,
  ptr stored at [0x394]) and ZEROES logical 0x7A5E00-0x7AF540 — which
  maps over the ROM's LIVE tables region — before loading its own
  root pointer, trusting the real 030's ATC to keep translating
  meanwhile.  QEMU refills the TLB in 4K subpages of the guest's 32K
  pages and re-walked the half-wiped tables mid-loop.  FIX 1: software
  ATC in the 030 walk (16 entries, guest-page granularity, keyed on
  the IS-masked address so 24-bit-tagged and plain pointers coincide;
  store-through-read-entry re-walks for the M bit; flushed by
  PMOVE/PFLUSH).  FIX 2 (the final one): PAGE descriptors carry their
  page address in bits 31-8, but the walk masked with the
  table-descriptor mask ~0xF, leaking U/M status bits into the frame
  (short desc 0x007F8019 -> 0x7F8010): the wipe loop then wrote +0x10
  high, crossed a 4K boundary at offset 0xFFC (ATC entry with a
  +0x1000 addend) and REALLY corrupted the live level-A table at phys
  0x7FF958 (caught with a per-iteration gdb breakpoint watching
  [0x7FF958]).  Masking page addresses with ~0xFF fixes it.

MILESTONE: with all of the above, MacOS 7.5.3 boots from the 5380 disk
to a RUNNING FINDER DESKTOP: menu bar (File/Edit/View/Label/Special),
ticking menu clock, desktop pattern, Launcher strip; screenshot
/tmp/shots/maciisi-finder-desktop.png (also /tmp/dave/run39-*.png).
One modal alert sits on screen: "Disk initialization failed because
the disk is locked!" — some second volume in the OpenRetro image
failed to mount (the boot volume itself is fine — the Finder runs from
it).  NOTE: run from a PRIVATE COPY of the disk image (a concurrent
lc475 session in /workspace/src/qemu-mac intermittently locks
/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda; I used /tmp/dave/hd0.hda
— possibly a torn copy, which may itself explain the unmountable
second volume; re-copy cleanly next time).

CURRENT FRONTIER: ADB input into the running Finder.  QMP
input-send-event (rel moves, buttons, keys) reaches adb-mouse/adb-kbd
but the cursor never moves and Return doesn't dismiss the dialog.  The
Egret model HAS unsolicited/autopoll delivery (maciisi_egret_adb_poll,
autopoll_mask 0xFFFF, enabled at init) — so either the OS never sees
the unsolicited sessions (check -d unimp egret log lines 'unsol/feed/
response complete' during input), the packet framing for
Egret-initiated ADB data differs from command responses (a status/type
prefix byte?), or the OS's transport rejects them at this phase.
Next: boot to Finder (~9 min at icount shift=7), send input, grep the
egret exchange log, and compare against the OS ADB driver's
expectations (RAM driver, globals at [0xB78]; session-7 notes have the
transport pinout/flow).

Verified no regressions: -M q800 ROM boot to insert-disk desktop
(/tmp/dave/q800-chk.png), -M mvme147 147Bug monitor prompt (both
exercise the shared m68k translation code; q800 is 040, mvme147 uses
the 030 walk incl. the shift>=32 early-termination identity path).

Commits: 48ad070bbe (target/m68k: three 030 PMMU translation fixes),
75639dc7cb (hw/m68k/maciisi: the four machine-model fixes).

Tooling notes for next session:
- gdb/HMP-xp reads on this machine BYPASS the 030 MMU (the debug path
  checks the 040 TCR only): with the video-relocation maps active,
  "lowmem" reads at 0xXXX actually read the VIDEO BUFFER; real lowmem
  lives at PHYS +0x50000.  All lowmem spelunking must add the offset
  (or fix m68k_cpu_get_phys_addr_debug to use the 030 walk).
- Boot-to-crash points are deterministic; conditional hbreak command
  lists (silent/if/continue) work well and ~1400-iteration polling
  loops are fine.  gdb 'watch' on this stub = software watchpoints =
  unusable crawl.
- Concurrent sessions share /workspace/files disk images — always
  check 'fuser' before blaming QEMU lock errors, never pkill.

ADB-input investigation (session 9 end, for next session):
- At the Finder desktop the OS runs the ADBReInit collision sweep
  ([2b 0f fe]/[2f]/[fb 02 fe] for kbd addr 2, [3b 0f fe]/[3f]/[fb 03
  fe] for mouse addr 3, [ff] Talk R3 addr 15) ~50 TIMES over the boot
  (grep 'cmd len' of a -d unimp log: 50-51 of each), i.e. it keeps
  REINITIALIZING adb — on a healthy machine it runs once.  ZERO
  unsolicited/autopoll packets were ever delivered (grep 'unsol' = 0)
  even though maciisi_egret_adb_poll + QEMU adb autopoll are armed
  (mask 0xffff, enabled; ACR parks at 0x0C = shift-in, so the SR_OUT
  guard is not the blocker).  QMP input events queue into adb-kbd/
  adb-mouse but nothing moves; Return does not dismiss the dialog.
- The transport ends WEDGED mid-exchange: last exchange = host sends
  [2c] (Talk R0 kbd), does the receive turnaround (model stages a
  2-byte null response, XCVR low), the ROM-transport turnaround helper
  reads SR once (log '-> 0x2c (#0/2) pc=4080a682 b=c7') and then the
  expected ORB^=0x10 TACK toggle at 0x4080a686 NEVER ARRIVES — no
  further egret log lines at all, ORB parks at 0xDF (TIP still
  asserted!), and the driver globals ([phys 0x50CF8] -> a3=0x5100,
  continuation a3+312) hold cont=0x4080A624 = armed waiting for an SR
  interrupt forever.  Straight-line ROM code was abandoned between two
  instructions => an OS-level layer (VBL task/watchdog timeout?)
  yanked control and never resumed the exchange.  Hypothesis: each of
  the 51 sweeps ends in exactly such an upstream timeout because
  autopoll data never arrives; find the OS ADB manager's timeout/
  retry logic (RAM driver, globals [0xB78! remember +0x50000 for
  physical dumps]) and what it expects the Egret to do about
  device-initiated data — the parked-Talk-R0 model (Egret holds the
  response until data exists) vs unsolicited-session model needs to be
  settled from that code, and maciisi_egret_adb_poll made to match.
- Autopoll never fires even DURING the sweeps (why? add a log line in
  maciisi_egret_adb_poll's early-return to see which guard trips; the
  QEMU adb_poll may also simply have no data when no input was sent).
- The 'Disk initialization failed because the disk is locked!' Finder
  alert reproduces on a clean image copy; possibly a second partition
  the model can't write (check whether the ncr5380 model accepts
  WRITE(6)/WRITE(10) — if writes fail the Finder treats the volume as
  locked).  Investigate after input works (the alert also blocks the
  desktop until dismissable).

### ADB INPUT CRACKED — THE FINDER IS INTERACTIVE (2026-08-24, session 10)

MILESTONE: keyboard and mouse work in the running Finder.  Injected QMP
events visibly act on the GUI: a keypress type-selects in a Finder
window, the cursor moves, a click OPENS THE APPLE MENU
(/tmp/shots/maciisi-interactive-apple-menu.png), Cmd-N CREATES A NEW
FOLDER on the desktop = a successful disk WRITE
(/tmp/shots/maciisi-newfolder-diskwrite.png; `info blockstats`: 46
write ops / 78KB through the 5380).  Three root causes, all found by
reading the ROM transport at the wedge PCs and diffing model state
against it; all fixes are hw/m68k/maciisi.c ONLY (no shared code
touched this session — q800 etc. unaffected by construction).

ROOT CAUSE 1 — the Talk-R0 wedge: the poll cadence closes and reopens
the session, and the model treated the close as "host walked away".
Ground truth (ROM disasm): after the sweep, the OS ADB manager idles by
HOST-DRIVEN POLLING — the end-of-exchange path (0x4080A70E-0x4080A734,
mirrored in the OS RAM dispatcher at 0xBEC4) walks an active-device
bitmap (transport globals a3+334) and re-issues Talk R0 to the NEXT
device after every no-data poll ([2c]/[3c] forever, kbd addr 2 / mouse
addr 3).  There is NO Egret-initiated autopoll on this machine at all.
The poll send path (kick handler 0x4080A5AA -> 0x4080A5C0) does its
receive turnaround DIFFERENTLY from the sweep exchanges:
  - sweep Talks (mode A): ACR->in, tst SR, ORB ^= 0x10 (TACK only),
    then per-byte ORB ^= 0x30 acks — session held throughout;
  - rotation polls (mode B, via 0x4080A5F6/0x4080A5FC): ACR->in, tst
    SR, ORB ^= 0x30 — which RELEASES /TIP with /TACK, i.e. transiently
    "both handshake lines high" — one SR_INT later ORB ^= 0x20
    RE-ASSERTS /TIP with the shifter inbound = a fresh RECEIVE session
    that collects the response to the command sent in the previous
    session.  (Continuation chain: cont a5fc does the ^0x20, cont a624
    is the first-data interrupt, 0x4080A68E reads SR per byte.)
The old model's release branch fired on the transient ^0x30 state,
cleared the staged response and went idle; the reopened receive session
then found nothing, no SR_INT ever came, and the driver parked forever
with cont=0x4080A624 armed (the session-9 wedge signature: ORB=0xDF,
flags(350)=0x90 — which is NOT a collision, it is the normal ori #0x90
at 0x4080A5FC).  The "51 ADBReInits" were NOT caused by this wedge (a
healthy boot still runs ~51 sweeps — INIT-time ADBReInit calls); only
the FIRST rotation poll ever wedged, at the very end of boot.
FIX (maciisi_egret_session_update rewrite):
  - session CLOSE only on a /TIP release EDGE (hs_change bit5), and a
    staged-but-undelivered response (resp_idx==0) SURVIVES the close;
    a response mid-delivery (idx>0) is dropped.  RTC bit-bang port-B
    writes (bits 0-2, /TIP parked low between exchanges!) no longer
    disturb the transport (they used to run the whole release branch
    + schedule spurious SR_INTs).
  - session OPEN is EDGE-triggered: /TIP assert edge (fresh session or
    the mode-B ^0x20 reopen), or /TACK assert edge with /TIP already
    held + shifter outbound (chained send via 0x4080A656's ORB &= 0xCF
    straight after an exchange — /TIP is never released between OS-era
    exchanges).  The old level-triggered "sys && !session" open branch
    was the LAST wedge: after the final ack of a poll response, any
    port-B write (RTC Time-Manager traffic) re-opened a session and
    idled /XCVR high underneath the armed end-of-response continuation;
    the pending "session closed" SR_INT then read XCVR-high = "more
    data" and clocked junk until the model went silent (that is how
    boot3 still parked once per boot even after the close fix).
  - receive-session open with a held response loads SR with byte 0,
    sets /XCVR per no_resp and raises the SR_INT (cont a624 expects
    the first byte ALREADY in SR); with nothing held it runs the
    no-response signature.
  - the never-used Egret-initiated autopoll injection
    (maciisi_egret_adb_poll + QEMU adb autopoll timer) is REMOVED: the
    guest polls.  (Explains session-9's "autopoll never fires": there
    is no such mechanism on this machine to begin with.)
With this the rotation runs continuously (~thousands of [2c]/[3c]
exchanges — beware -d unimp: the egret log grows at ~2MB/s at the
Finder; use HMP "log none") and keyboard data reaches the OS (typing
works — key-repeat doubling in tests is just icount virtual-time
compression tripping MacOS auto-repeat, not a model bug).

ROOT CAUSE 2 — mouse data arrived but the cursor never moved: the
cursor task never ran.  Evidence: MTemp (lowmem 0x828, phys +0x50000)
accumulated every injected delta while RawMouse (0x82C) stayed frozen;
CrsrNew=0xFF armed; jCrsrTask [0x8EE]=0x4082DF94 would couple them but
is only called from the Vertical Retrace Manager's SLOT $E VBL task —
and the session-5 model deliberately never raised the slot-E summary
interrupt (SIFR bit 6 was masked out of the CPU path).  At the Finder
the OS has SIER=0x7F and enables IER bit 1 (write 0x82 at 0x40809FE2,
gated on [0xDD1] bit 2 = Egret/RBV machine) exactly when the first
slot VBL task is installed.  FIX: include bit 6 in the slot summary
(mask 0x3f -> 0x7f in maciisi_rbv_update_irq).

ROOT CAUSE 3 — enabling that summary sad-mac'd the boot with
dsBadSlotInt (0000000F/00000033): the RBV SIFR reads ACTIVE LOW.  The
ROM's level-2 slot dispatcher (0x40806EAA: IFR ack, then
d0 = ~(0x80 | SIFR(+0x02)) & SIER(+0x12), dispatch if nonzero) inverts
the register — our active-high SIFR made every IDLE slot (bits 0-5,
empty slots 9-D) look asserted, and the dispatcher raised dsBadSlotInt
on the first summary interrupt.  The flags mirror the physical /IRQ
lines (idle 0xFF), like VIA2 port A on q800.  FIX: RSIFR read returns
~rbv_sifr (internal state stays an asserted-bits mask; RSIFR write
still clears written bits — the RBV1 driver's VBL handler acks with
0x40 -> +0x02 at 0x4084B49C/B568; the driver enables/disables the VBL
via 0xC0/0x40 -> +0x12 at 0x4084AA76/AA3E; its frame-sync helper
0x4084AC8A waits a full bit-6 cycle so it works in either polarity —
which is why the polarity bug survived sessions 3-9 unnoticed).

DISK "locked" alert: does NOT reproduce on a clean image.  The
partition map of /workspace/files/HD0-OpenRetroSCSI-7.5.3.hda has ONE
HFS volume (+ map/driver/free) — there is no second volume to fail to
mount.  Session 9 ran from a possibly-torn private copy; all session-10
boots ran the pristine image with -snapshot and no alert ever appeared,
and Finder writes succeed (new folder created + renamed; 46 WRITEs on
the block layer).  The 5380 data-out path needed no changes.

Repro/runbook (session 10):
  build/qemu-system-m68k -M maciisi -bios .../maciisi.rom \
    -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
    -snapshot -display none -monitor unix:/tmp/mon.sock,server=on,wait=off \
    -qmp unix:/tmp/qmp.sock,server=on,wait=off -icount shift=7
  (-icount shift=7 is REQUIRED: without it TimeDBRA calibration
  divides by zero -> sad mac 0000000F/00000004.  Finder in ~7 min.)
  Input via QMP input-send-event (rel/btn/key qcodes) or HMP
  sendkey/mouse_move — both reach adb-kbd/adb-mouse now.
Commits: hw/m68k/maciisi: Egret session-edge model + RBV slot-int
polarity; MacOS 7.5.3 Finder is interactive (this session).
