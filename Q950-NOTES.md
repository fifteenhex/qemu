# Macintosh Quadra 950 / 900 machines ("quadra950", "quadra900") — progress notes

Goal: boot the Quadra 950 ROM (`/workspace/files/mac-roms/quadra950.rom`,
1 MiB, header checksum 3DC27823, shared Q900+Q950) to MacOS 7.5.3
(Finder) over SCSI.  The hard part: Q900/950 route SCC and SWIM through
6502 IOPs and use an Egret MCU for ADB/PRAM/RTC.

## Design plan

Base = quadra700.c (q800-family: 68040, VIA1/VIA2, MCU memory
controller at 0x50F0E000, ESP at 0x50F0F000 + DAFB TurboSCSI pDMA,
SONIC, ESCC, DAFB/macfb slot 9, NuBus).  The Q700/Q900 shared a ROM
(420DBFF3), so the Q900 hardware layout ~= Q700 apart from:
- VIA1 PA straps: Q700 notes: (PA & 0x56) == 0x50 selects the Q900 path
  in the 420DBFF3 ROM.  Q950 straps TBD from this ROM's universal table.
- Egret on the VIA1 shift register (maciisi.c transport: /SYS_SESSION
  PB5, /VIA_FULL PB4, /XCVR_SESSION PB3) instead of direct ADB + RTC
  bit-bang.  Q700 notes say this ROM family speaks Egret *pseudo*
  packets ([01 07 hi lo] = GET_PRAM), so the engine needs the pseudo
  command set (PRAM/RTC/autopoll), not just raw ADB like maciisi.
- SCC IOP at 0x50F0C000 AND SWIM IOP at 0x50F1E000 (Linux asm/mac_iop.h
  register layout, already stubbed for SCC in quadra700.c).  Strategy:
  minimal IOP host interface + bypass; only as much mailbox as the ROM
  demands.  Linux iop.c constants for reference: shared-RAM
  IOP_ADDR_ALIVE=0x031f (alive flag 0xff), send channels state
  0x0201.., msgs 0x0220.., recv state 0x0301.., msgs 0x0320..,
  status_ctrl bits BYPASS=0x01 AUTOINC=0x02 RUN=0x04 IRQ=0x08
  INT0=0x10 INT1=0x20 HWINT=0x40 DMAINACTIVE=0x80.

## ROM universal table (quadra950.rom, 3DC27823) — PARSED

Same layout as the Q700 ROM (420DBFF3): entry offset list at ROM+0x31c8
(12 entries), 0x40-byte entries.  Kind-8 entries (decoder = successful
long read of the MCU at 0x5000e000), strap mask 0x56 on VIA1 PA:

| entry  | box  | straps (PA & 0x56) | machine     | gestalt (box+6) |
|--------|------|--------------------|-------------|-----------------|
| 0x387c | 0x0e | 0x50               | Quadra 900  | 20              |
| 0x38bc | 0x14 | 0x10               | Quadra 950  | 26              |
| 0x38fc | 0x10 | 0x40               | Quadra 700  | 22              |

Q900/Q950 entries are byte-identical apart from box+straps: flags
+0x18 = 0x07a31807 (bit17 = SCC IOP, bit16 = ISM/SWIM IOP present),
flags2 +0x1c = 0x00040924 (Q700: 0x05a0183f / 0x00000900 — one of the
extra bits = Egret).  The Q700-ROM Q900 entry (0x388c there) is
byte-identical to this ROM's => the Q900/950 hardware layout equals the
Q700 apart from IOPs + Egret, confirming quadra700.c as the base.

POST-failure dispatcher branch (`tstl %d6; beq.s +0x1a` = 4a86671a) is
at ROM+0x46f7c — the SAME offset as the Q700 ROM, so quadra700.c's
beq->bra patch + checksum fixup applies verbatim.

## Protocol references (fetched, recorded here because transcript dies)

- Linux `asm/mac_iop.h`: status_ctrl bits as in the plan above; shared
  RAM: MAX_SEND_CHAN 0x200, SEND_STATE 0x201..0x207, PATCH_CTRL 0x21f,
  SEND_MSG 0x220 (7 chans x 32 bytes), MAX_RECV_CHAN 0x300, RECV_STATE
  0x301.., ALIVE 0x31f (0xff = alive), RECV_MSG 0x320..;
  states IDLE=0 NEW=1 RCVD=2 COMPLETE=3.  Channel 0 = SCC, 1 = ISM.
  Host sends: copy msg to SEND_MSG+chan*32, SEND_STATE[chan]=NEW,
  write status_ctrl = IOP_IRQ|IOP_RUN|IOP_AUTOINC.  IOP completes:
  SEND_STATE[chan]=COMPLETE, raises INT0.  IOP->host msgs: RECV_STATE
  NEW + INT1; host sets RCVD, then COMPLETE.  iop_alive() = read ALIVE
  == 0xff, then writes 0.
- Linux `drivers/macintosh/via-cuda.c` (supports Egret = Q900/950):
  Egret uses the SAME Cuda packet format (type byte 0=ADB, 1=pseudo;
  responses [type, flags, cmd, data...]) but INVERTED TIP/TACK: host
  asserts PB5(TIP)/PB4(TACK) HIGH (idle low), Egret asserts
  PB3(TREQ) LOW.  (Cuda = all active low.)  quadra630.c's real-Cuda
  packet engine is therefore the right semantic base; transport
  polarity flipped.

## Status log

- [x] parse ROM universal table (straps per box, decoder kind, flags)
- [x] machine skeleton (quadra950.c, Kconfig, meson)
- [x] machine identification passes (both quadra950 and quadra900)
- [x] POST passes (incl. the SCC IOP RAM test)
- [x] Egret exchanges work: PRAM rebuild completes, clock reads work,
      both the polled boot drivers and the trap dispatcher are served
- [x] both IOPs pass their RAM tests, are started, alive-flagged, and
      exchange mailbox messages (SCC chan0 init, ISM chan1 SWIM cmds,
      ISM chan2 ADB)
- [x] **GRAY SCREEN at 1152x870** (~15 s), on quadra950 AND quadra900
- [x] ADB-over-IOP: the SendReset exchange completes END TO END
      (send msg -> in-place+recv-channel reply -> ROM listener 0x4080a566
      -> ring pop -> op completion -> receive re-arm/ack)
- [x] **FIXED (session N+1): the ADB device scan now completes all 16
      Talk-R3 probes** (see finding 15) — root cause was NOT an
      interrupt/DT timing race at all, it was a mailbox ADDRESSING
      mismodel: follow-up explicit ADB commands are staged in place in
      the RECEIVE slot, not sent via a fresh SEND_MSG slot.
- [x] ADB mouse cursor renders and tracks; keyboard/mouse addresses
      found and reassigned (Listen R3 traffic seen after the scan)
- [x] boot reaches the classic flashing "?" disk icon (SCSI boot
      territory) — meets the task's "good" tier
- [x] **CORRECTION to the earlier "NEW BLOCKER" note: it is NOT an
      Egret PRAM loop and 0x40898e14 is NOT Egret — 0x40898e14 is the
      ESP SCSI "wait for interrupt" spin (reads ESP RSTAT/RSEQ at
      a3+0x40/+0x60).  The SCSI/ESP path IS reached and works: 12+
      SCSI READ(6) commands complete, pseudo-DMA delivers data, 450+
      ESP IRQ pairs.  The Egret PRAM 0x1b8 read/write is a *symptom*
      (boot-failure logging on each "?" rescan), not the cause.  See
      finding 16.**
- [ ] **REAL NEW BLOCKER (finding 16, CORRECTED — the "sbDrvrCount==0 /
      buffer truncation" theory below and in the first draft of finding
      16 was WRONG, a physical-vs-logical-address + stack-variance
      misread): the DDM block 0 is read into memory PERFECTLY
      (sbSig='ER', sbBlkSize=0x200, sbDrvrCount=1, driver descriptor
      ddBlock=64/ddSize=19/ddType=1 — all correct).  The boot-driver
      installer (ROM 0x40807224, dispatched via the SCSI/Device-Manager
      trap 0x408099f0→0x40809a04) then does a DRIVER-TYPE MATCH at
      0x40807264: `cmpw ddType(=0x0001), d1` where d1 = the machine's
      expected driver ddType.  d1 = 0x00F8 (from the caller's d3 =
      0x00f8ff40, byte 1), so 0xF8 != 0x0001 → NO driver matches → the
      installer returns -1 → no disk driver installed → no boot volume
      → "?".  The block-0-forever "retry" is the "?" rescan, a symptom.
      **ORACLE CONFIRMED (finding 17): the installer code at 0x40807224
      is BYTE-IDENTICAL in the quadra700 and quadra950 ROMs; broken at
      the SAME breakpoint, -M quadra700 has d3=0x0001ffff (expected
      ddType byte1 = 0x01 → matches disk → BOOTS) while -M quadra950 has
      d3=0x00f8ff40 (byte1 = 0xF8 → mismatch).**  So 0xF8 is definitely a
      wrong machine value, not a code difference.  ROOT SOURCE of the
      0xF8 still not pinned (built in chained/dispatched code; see
      finding 17 for the exact leads).  NOT an icount artifact.**
- [x] **FIXED (session N+2): the ddType-mismatch "?" blocker (findings
      16/17) is RESOLVED.**  Root cause: `EGRET_CMD_READ_PRAM` (cmd 2)
      only ever returned ONE data byte, but a second ROM consumer
      (0x40801390/0x408013b0, reached via traps $A07D/$A084 ->
      $A051 -> 0x40815204 -> $A092) clocks *2 or 4* bytes from a
      single cmd-2 exchange to read a PRAM word/longword.  With only
      one real byte supplied, the caller's stack scratch buffer was
      never fully written by the transport and the 2nd (and 3rd/4th)
      bytes were leftover stack garbage — that garbage is exactly
      what landed in the boot-driver-installer's expected-ddType
      register (0xf8 instead of the disk driver's ddType=1).  Fixed
      by making `EGRET_CMD_READ_PRAM` stream consecutive PRAM bytes
      the same way `EGRET_CMD_GET_PRAM` already does (host-terminated,
      not device-terminated) — the existing single-byte consumer
      (interrupt-driven trap dispatcher, 0x40814a58) is unaffected
      since it only ever clocks one byte and stops, but the
      2/4-byte consumer now gets real PRAM[addr..addr+n] data.  After
      the fix, `PRAM[0x76..0x79]` (`00 01 ff ff`, the ROM's own
      default block for that PRAM record, see finding 18) is
      delivered correctly, `d3` byte1 resolves to `0x01`, the driver
      installs, and the boot proceeds PAST the flashing "?" to the
      "Mac OS Starting up…" screen (screendump proof:
      `/tmp/q950_after.png`).  See finding 18 for the full derivation
      and finding 19 for a NEW, separate stall found immediately
      after this fix (the "Starting up" progress bar does not
      advance) — that is NOT the ddType bug; it is a fresh blocker
      for a future session.
- [x] **MacOS 7.5.3 FINDER REACHED (session N+5).**  Finding 19's
      post-"Starting up" hard hang is FIXED: the missing event was the
      SIM 4.3 bus scan of the Quadra 900/950's SECOND (external) SCSI
      bus — the towers have TWO 53C96 chips and the SIM scans both
      before starting async I/O; the model only had one, so the scan
      of the nonexistent chip at 0x50F0F402 never completed and the
      queued target-0 read was never dispatched.  Fixed by modeling
      the second ESP (see finding 20).  Screendump proof:
      `/tmp/q950_finder.png` — full Finder desktop (menu bar, Control
      Panels window, Trash, mounted "OpenRetroSCSI 7.5" volume).
      `-M quadra900` boots to the same Finder desktop
      (`/tmp/q900_finder.png`).
- [x] q800/q700 regression check: only `hw/m68k/quadra950.c` changed
      (no shared code touched).  Confirmed `-M quadra700` still boots
      all the way to the Finder after the finding-18 fix (screendump:
      `/tmp/q700_check.png`, shows Finder + Control Panels window);
      the finding-20 session again touched only quadra950.c (its
      pre-fix q700 reference run also booted normally).

## Findings

1. **IOP RAM test + start**: POST RAM-tests the SCC IOP shared RAM via
   the host regs (pc 0x408477c4, as on Q700) and then writes ctrl 0x32
   (clear INT0/INT1 + AUTOINC) and 0x36 (+RUN) — it starts the IOP
   without loading firmware first at this stage.  Stub sets the alive
   flag (0x31f=0xff) on the RUN rising edge.
2. **Egret ROM driver decoded** (exchange runner at ROM 0x4b258, XPRAM
   read entry; disassembled 0x4b200-0x4b4d0):
   - Dispatch: `(entry_flags2/a1@(0x1c) & 0x70) == 0x20` selects the
     Egret path, else the classic RTC bit-bang path (0x4b396, cmd
     0xb8|sector) — flags2 0x00040924 & 0x70 == 0x20 ✓ (that's the
     "Egret present" field).
   - a2 = VIA1; ORB bit5=TIP, bit4=TACK (host, active HIGH), bit3=TREQ
     (Egret, active LOW).  IFR bit2 = SR int, polled; SR reads clear it.
   - SEND: ACR |= 0x1c (SR out); TIP high; SR <= byte; TACK high; wait
     IFR; tst SR; TACK low; (delay 256 loop); repeat per byte via
     0x4b3ca.  After byte 1 it checks TREQ still HIGH (low = collision).
     GET_PRAM sends [01 07 addr_hi addr_lo]; NO SR preload before the
     session opens (unlike lc475/q630 Cuda drivers).
   - TURNAROUND (0x4b324): ACR &= ~0x10 (SR in); TIP LOW; then busy-wait
     for (IFR bit2 && TREQ low); moveb SR (DISCARDED - just clears
     IFR); TIP high again.
   - RECEIVE (0x4b3ea helper): TACK low; wait IFR; TACK high; read SR.
     I.e. bytes are clocked on the TACK FALLING edge only.  After each
     of the first 3 reads TREQ must still be LOW or abort; 3rd byte
     must equal the cmd echo (7); 4th byte = data.  So the response
     format is exactly Cuda's [type, flags, cmd, data] — the
     turnaround SR read is NOT part of the packet.
   - END: after the data byte the ROM drops TIP and busy-waits for
     TREQ HIGH (no interrupt), then drops TACK.  So the Egret must
     deassert TREQ on the first handshake toggle after the last byte.
   - Unsolicited receive (0x4b41e): if TREQ low, TIP high, then 0x4b3ea
     reads in a loop while TREQ stays low; needs the end-marker toggle
     to also raise an SR int (the helper waits on IFR before it can see
     TREQ high).
3. Egret polled engine model (superseded by 6/7): response delivery is
   SR-READ driven — each SR read hands over the next byte + reraises
   the int (the first "bclr TACK" has no edge, so edges can't clock).
   The turnaround SR read is a discard.  Host ends a block read by
   dropping TIP with TACK high.
4. **GET_PRAM (0x07) / trap read (0x02) are BLOCK reads**: [01 cmd
   addr_hi addr_lo], then the Egret streams bytes while the host keeps
   reading (1 byte for XPRAM scans; 32 for the PRAM-validity read at
   addr 0 (0x4081528e, checks byte 0x10 == 0xa8); up to 224 in the
   readback).  Writes: [01 08 addr_hi addr_lo]+data block writes (PRAM
   init at 0x408152c4: (0x110,16),(0x108,4),(0x10c,4),(0x120,224),
   (0x176,19)).  Address space: 0-3 clock, 0x100-0x1ff PRAM (trap
   dispatcher 0x15212 adds 0x100), flat 0x00-0xff also PRAM for the
   boot scans.  Treating 0x02 as write crashed boot (addressing-mode
   byte 0x18a read garbage -> Illegal instruction @ 0xc6).
5. With that: PRAM rebuild completes, machine ID passes, NuBus scan
   runs, SCC+SWIM IOPs pass POST + get started, DAFB comes up: **GRAY
   SCREEN at 1152x870** ~15s after start.
6. **The ROM has TWO Egret transports.**  After the gray screen it
   installs the interrupt-driven OS ADB driver (handler 0x4080a700 via
   lowmem 0x19a, state at 0xcf8; init at 0x4080a890, completion wait
   at 0x4080a8e6 on bit5 of state+0x15d) — and that driver is the
   maciisi.c engine's protocol VERBATIM (same continuation addresses
   as MACIISI-NOTES: 0xa624/0xa63c/0xa646/0xa656/0xa68e):
   - raw ADB command bytes (d3 = (dev<<4)|reg, no Cuda type byte),
     SR PRELOADED before the session assert (0xa656: ACR|=0x1c,
     SR=cmd, ORB&=0xCF);
   - ACTIVE-LOW TIP/TACK (ORB &= 0xCF = open, eor 0x10/0x30 toggles
     keep exactly one asserted mid-exchange, both high = released);
   - receive turnaround: eor 0x30 (release both) then eor 0x20
     (re-assert TIP, shifter in) — first byte must be in SR at the
     reopen; each eor 0x30 toggle acks a byte (a68e reads SR then
     toggles);
   - PB3 (/TREQ) HIGH while response bytes flow, LOW at an interrupt =
     end-of-response; LOW at the first post-turnaround int = discard/
     no-response (bit5 of flags+0x15e, count zeroed at 0xa646).
   So: polled boot drivers = Cuda packets, active-HIGH sessions,
   SR-read-driven delivery, /TREQ LOW while flowing; OS driver = raw
   ADB, active-LOW sessions, toggle-ack delivery, /TREQ HIGH while
   flowing.  Same pins, opposite senses.  (PRAM/clock ops in the OS
   phase go through the _EgretDispatch trap, which still uses the
   POLLED Cuda-packet transport — the transports interleave at
   runtime.)
7. Dual-mode engine: per-session mode latch.  A session opened by an
   SR write with no session active is OS-mode iff TIP is LOW (polled
   drivers raise TIP before writing SR; the OS driver writes SR while
   the lines are still low/asserted).  Line-edge session opens/acks/
   closes are interpreted per the CURRENT latch: polled = q630-style
   active-high; OS = the maciisi engine (ported) with active-low
   edges.  SR-read-driven feeding only in polled mode; toggle-ack
   feeding only in OS mode.  (NOTE: with ADB living on the IOP — see
   below — the OS-mode ADB path is probably never exercised on this
   machine; the polled Cuda-packet transport serves the PRAM/clock
   traps at runtime.  The engine keeps both anyway.)
8. **Polled Egret response format cracked** (three ROM drivers
   reconciled + the trap driver's live param block at [0xde0]):
   FOUR-byte header [type, status, flags, cmd-echo] then data, with
   resp[0] PRELOADED into SR at the turnaround.  status 0 = OK, 2 =
   error (trap driver turns it into paramErr at 0x40814a18); flags
   bit6 = no-data.  Drivers 1/2 discard the first (preloaded) SR read;
   the trap dispatcher's ISR (0x40814912, installed at lowmem 0x19a)
   collects it as hdr[0].  GET_PRAM (07) streams; trap read (02)
   returns exactly ONE data byte and ends by deasserting /TREQ
   (the trap driver computes data length as received_count - 4).
9. **PRAM traffic**: boot rebuild writes blocks (0x110,16) (0x108,4)
   (0x10c,4) (0x120,224) (0x176,19) via [01 08 hi lo]+data; validity
   = byte at addr 0x10 == 0xa8 within a 32-byte read at addr 0
   (0x4081528e).  Address space: 0-3 = clock, 0x100-0x1ff = PRAM,
   flat 0x00-0xff also PRAM for the boot XPRAM scans.
10. **ADB is NOT on the Egret**: the Q900/950 route ADB through the
    ISM/SWIM IOP mailbox, channel 2 (mailbox-0-based; the ROM's
    request API numbers channels 1-based).  Linux agrees (adb-iop.c,
    ADB_IOP=IOP_NUM_ISM, ADB_CHAN=2).  The ADB dispatcher selection at
    0x4080a534: (flags2[0xdd4] & 0x0e)==6 -> 0x4080b0f0 flavor; else
    lowmem 0xdd1 bit0 -> IOP flavor init 0x4080a8ae, else the VIA/
    Egret driver 0x4080a890.  Messages: [flags, count, cmd, data[8]]
    (adb_iopmsg): flags 0x80 explicit | 0x40 autopoll | 0x20
    set-autopoll (data = 16-bit device bitmap) | 0x04 SRQ | 0x02
    timeout.
11. **IOP interrupt wiring** (from the ROM vector tables at runtime):
    Level-4 autovector (the GLUE "SCC" input) -> 0x40809b10 ->
    0x40809d60 -> IOP-manager ISR (0x40805040) for IOP #0 = the SCC
    IOP: its interrupt is OR'd with the ESCC's.  VIA2 CA2 (Level2DT
    slot [0xd70] = 0x40809d70: clears VIA2 IFR bit0, then ISR for IOP
    #1) = the ISM IOP, active low.  VIA2 CA2 is therefore NOT the
    SCSI DRQ on the towers (TurboSCSI reg only).
12. **IOP manager structures** (built at 0x40804cc0/0x40805218):
    [0xc28] -> per-IOP struct table (entry 0 = SCC at 0xcd44-ish,
    1 = ISM); struct: +0 addr-reg ptr, +4 data-reg ptr, +8 status
    ptr, +12 ISR base, +16/17 max send/recv chans (read from IOP RAM
    0x200/0x300 after start — the stub must publish 7/7 or every
    message send fails with paramErr), +28+chan*16 per-channel
    request queues (channel = API 1-based).  Send: addr reg <-
    0x220+mailboxchan*32, message bytes via autoinc data reg,
    SEND_STATE <- NEW via addr 0x201+chan, ctrl <- 0x0e.  The ISR
    scans states via a single addr-reg write + autoincrementing data
    reads (relies on post-increment).  Send-complete: state <- IDLE,
    request unlinked; requests WITH a completion routine get queued
    on a completed-list ([0xc28]-12) drained by a DEFERRED TASK the
    ISR epilogue installs via [0xd9c] (flag byte [0xc28]-15);
    requests without one just clear their pending byte (+11).
13. **ADB replies go over the RECEIVE channel**: the boot flow arms a
    listen request on ISM chan 2 (API 3); replies [flags, count, cmd,
    data...] land there; the listener completion 0x4080a566 checks
    flags bit7 (set = reply to explicit command -> pop the command
    ring 0x4080a45e and call the op's coroutine continuation via the
    patch tail 0x408855b4; clear = autopolled data -> 0x4080a494) and
    bit2 = SRQ.  After consuming, the driver acks the recv message
    (state <- COMPLETE + kick) and re-arms.  The stub posts explicit
    replies BOTH in place in the send message and as a recv-channel
    message (flags 0x80 / 0x82-timeout).
14. **THE BLOCKER — ADB scan does not continue past SendReset.**
    Confirmed end-to-end under deterministic icount tracing: reset
    sent, reply delivered, 0x4080a566 runs (bit7 path), ring popped
    (0x4080a484 marks it idle — the 16 Talk-R3 ops of the address
    scan are queued as coroutine continuations, ONE at a time), the
    re-arm/ack goes out — and then the reset op's continuation
    (0x4080a8f4, which would queue Talk #0) never runs, so the boot
    waits forever at 0x4080a8e6 on lowmem 0xdabd bit5.  The
    continuation should be called by the completed-list deferred
    task (finding 12).  Observations:
    - Host-slowed runs (gdb breakpoints on the send path, or -d exec
      tracing without icount) get 2-3 commands further — the failure
      is exquisitely timing-sensitive.  Synchronous completion (INT0
      inside the kick write), 100us/300us/2ms deferred completions,
      icount shift=7, nochain, and a gratuitous INT0 on every kick
      (to retrigger the ISR epilogue's deferred-task install) were
      all tried; the free-running result is always exactly one ADB
      command.
    - Next lever: instrument the deferred-task install/drain
      (0x408050f4 flag at [0xc28]-15, [0xd9c] install, [0x6e4] drain
      at interrupt exit — which requires the interrupted context at
      IPL 0, checked at 0x40809b76) and find which leg starves; or
      compare against the 0x4080a8ae flavor's expectations of INT
      timing.  The mailbox protocol itself is believed correct now.

15. **THE ACTUAL ROOT CAUSE (found via live QEMU register/RAM tracing,
    `-d exec,cpu` + a temporary `DBG ram[...]` read/write logger over
    the full IOP shared-RAM range, NOT gdb — gdb remote-stepping this
    workload was too slow to reach the stall, see below): the
    "continuation never runs" framing in finding 14 was wrong.  The
    continuation (0x4080a8f4) DOES run — under a slowed-down/traced
    run it reaches all the way through the dispatcher
    (0x4080aa22 -> 0x4080a3dc -> 0x4080a42c -> 0x40804de4, the generic
    IOP-manager "submit" routine) and issues a real `ctrl <- 0x0e`
    kick.  The bug is that the ROM's ADB-explicit-command driver does
    NOT allocate a fresh SEND_MSG slot for a follow-up command in an
    established request/reply "conversation" on a channel: once the
    host has fully drained a reply (RECV_STATE walked
    NEW -> RCVD -> COMPLETE, i.e. back to IDLE), the ROM STAGES THE
    NEXT OUTGOING adb_iopmsg IN PLACE in that SAME RECEIVE slot
    (`RECV_MSG + chan*32`, i.e. 0x360 for ADB chan 2) and kicks —
    there is no corresponding SEND_STATE transition to NEW anywhere.
    Confirmed byte-for-byte with the widened RAM logger: right after
    the SendReset reply is drained, `ram[0x360..0x364] <- 80 00 0f 00
    00` (an EXPLICIT adb_iopmsg, cmd 0x0f = Talk R3 addr 0) appears at
    pc=0x40804fd8, immediately followed by the RECV_STATE ack and the
    kick — never a SEND_STATE/SEND_MSG write.  The stub's
    `q950_iop_process()` only ever scanned SEND_STATE for new work, so
    it saw "no NEW msg" and the chain died after exactly one command,
    matching every earlier symptom (including why slowing things down
    got 2-3 commands further under gdb/exec-trace: pure luck reading
    stale vs fresh RECV_MSG content, not a real interrupt race).
    **Fix** (`q950_iop_adb_check_staged()` in quadra950.c): the ISM IOP
    now remembers the last content it posted into the ADB channel's
    RECV_MSG slot (`Q950IOP::adb_last_recv`); on every kick with no
    SEND_STATE work, if RECV_STATE for that channel is IDLE and the
    slot's content has both changed and has the EXPLICIT flag set, it
    is serviced exactly like a SEND_MSG explicit command (same
    `q950_iop_adb_msg()` handler) and the reply is posted back over
    the same slot.  With this fix the ROM completes all 16 Talk-R3
    probes, finds the mouse/keyboard, reassigns their addresses
    (Listen R3 traffic), the mouse cursor renders and tracks, and the
    boot reaches the classic flashing "?" (no-startup-disk) icon —
    past the ADB blocker entirely, into SCSI-boot territory.  A NEW,
    unrelated stall appears after that (see status log); the mailbox/
    ADB work itself is done.
    - Two other temporary diagnostic aids were added and kept (both
      gated behind existing `q950_log`/LOG_UNIMP, so silent by
      default): `DBG ram[addr] -> / <- val` read/write logging for the
      whole mailbox header+message range (0x200-0x340), which is what
      found the 0x360 write; grep a log taken with `-d unimp,
      guest_errors` for `DBG ram\[0x3` if this area needs revisiting.
    - gdb (`-s -S`, `set architecture m68k:68040`) technically works
      for register/breakpoint inspection but is impractically slow for
      this workload once a breakpoint is hit anywhere hot (TB chaining
      disabled + remote-protocol round trips): a `continue` from reset
      to the stall point did not return in 5+ minutes.  `-d exec` /
      `-d exec,cpu` with a tight `-dfilter LOW..HIGH,...` (note: `-`
      means SUBTRACT a length, not "range to"; use `..` for a literal
      start..end range) plus the QEMU HMP monitor's `xp` for point-in-
      time memory reads was the combination that actually worked here.

16. **SCSI BOOT BLOCKER — driver-TYPE mismatch (ddType 0xF8 vs 1) stops
    the boot-driver install (investigated in depth; NOT yet fixed).**
    After the ADB fix (finding 15) the machine proceeds into the ROM
    SCSI boot scan.  Full characterization:
    - **The SCSI/ESP path works.**  The QEMU ESP (real device, shared
      `hw/scsi/esp.c`, `it_shift=4`, pseudo-DMA / `dma_memory_read=NULL`)
      is driven correctly: `esp_command_complete` x12, `esp_do_dma`,
      450+ `esp_raise_irq`/`esp_lower_irq` pairs, and — critically — a
      temporary logger in `esp_pdma_read` proved the pseudo-DMA delivers
      **all 512 bytes of block 0 byte-for-byte identical to the disk
      image** (`45 52 02 00 00 0f a0 00 …` = the Apple 'ER' Driver
      Descriptor Map), repeating exactly every 512 bytes.  So the data
      path is NOT the problem.  (ESP register/PDMA visibility:
      `-trace events=<file>` with `esp_mem_readb/writeb`,
      `esp_pdma_read/write`, `esp_command_complete`,
      `scsi_req_parsed_lba` — no code edits needed.  ESP register map at
      it_shift=4: reg0=TCLO@+0x00, reg1=TCHI@+0x10, reg2=FIFO@+0x20,
      reg3=CMD@+0x30, reg4=RSTAT@+0x40 (bit7=INT bit4=STAT_TC
      bits0-2=phase), reg5=RINTR@+0x50 (bit5=INTR_DC disconnect,
      bit4=BS, bit3=FC), reg6=RSEQ@+0x60, reg7=RFLAGS@+0x70(fifo count),
      reg8=CFG1@+0x80; pseudo-DMA window = ESP_PDMA 0x50f0f100 =
      a3+0x100.)
    - **The boot only ever reads LBA 0 of target 0, forever** (33 reads
      in 100 s, all block 0; `scsi_req_parsed_lba`).  For contrast,
      **quadra700** (working baseline, same disk, same ESP model, its
      own ROM 420DBFF3) reads `0, 0, 64, 1, 2, 1, 2, 3, …` — i.e. block
      0 then immediately block 64 (the driver) then the partition map.
      So the q950 divergence is precisely at the block-0 -> block-64
      (driver-load) step.
    - **CORRECTION of the mid-session "sbDrvrCount==0 / buffer
      truncation" claim: that was WRONG.**  It came from `xp` (PHYSICAL)
      memory dumps of a stack buffer while the 68040 MMU is ON
      (TCR=0xc000) and from comparing addresses captured in DIFFERENT
      runs (the boot stack pointer varies run-to-run).  Re-measured
      correctly (see method below): the DDM lands in RAM byte-perfect.
      The store routine is 0x408992cc (`movew %a1@(0x100),%a2@+` x8 per
      16-byte DMA burst, base a1=ESP alias 0x50f4f000≡0x50f0f000; that
      is why the earlier `a3@(0x100)` grep missed it), driven 32 bursts
      = 512 bytes, `esp_pdma_read`-logged FIFO `used=16→1` every burst,
      **zero UNDERRUN**, delivering `45 52 02 00 00 0f a0 00 … 00 01 …`
      i.e. sbDrvrCount=**1** at offset 0x10, ddType=**1** at offset
      0x18.  So the buffer is NOT under-populated.
    - **THE ACTUAL BLOCKER: a driver-TYPE mismatch in the boot-driver
      installer.**  ROM 0x40807224 (dispatched through the SCSI/Device
      Manager trap dispatcher 0x408099f0 → `jsr @(0x400,%d2:w:4)` at
      0x40809a04) reads block 0, passes the 'ER' check (0x40807246),
      passes the sbDrvrCount!=0 check (0x40807256 → 0x40807258 IS
      reached — drvrCount=1), then at 0x40807264 does
      `cmpw %a0@(6),%d1` where a0=DDM+18 (the driver descriptor),
      a0@(6)=ddType, d1=fp@(-10)=the machine's EXPECTED driver ddType.
      Measured at the compare (gdb breakpoint + HMP monitor, see
      method): **a0@(6)=0x0001 (disk's ddType), d1=0x00F8 (expected).**
      0xF8 != 1 → the drvrCount-length match loop (0x40807260-0x4080726e)
      exhausts → 0x40807270 → 0x408072d6 returns -1 → the SCSI-ID scan
      (caller 0x4080720e) marks the ID done with NO driver installed →
      no boot volume → "?".  The block-0-forever reads are the "?"
      rescan (a symptom), NOT this installer (which runs only ~1-2x).
    - **d1=0xF8 comes from the caller's d3 = 0x00f8ff40** (at 0x40807224
      entry, confirmed via gdb+monitor): 0x4080722c `swap %d3` →
      0x40807230 `moveb %d3,%d0` = 0xF8 → 0x40807232 `movew %d0,fp@(-10)`.
      So only d3 byte 1 matters; it is the expected driver ddType and it
      is 0xF8.  A standard Mac SCSI driver is ddType=1 and this very disk
      boots on -M quadra700 (which has d3 byte1 = 0x01), so 0xF8 is WRONG
      — it should be 1.  See finding 17 for the full quadra700-oracle
      diff and the crisp next-lead on where the 0xF8 machine value is
      built (prime suspect: the persisting A4 = 0x91 q950 / 0x8a q700).
    - **NO fix made this session; no functional code changed** (esp.c
      instrumentation was reverted; `git diff` shows only Q950-NOTES.md,
      and quadra950.c already committed).  Confirmed no regression:
      still reaches the post-ADB "?" state.
    - **Next leads:** (a) find where d3/expected-ddType is built — trace
      the A-line trap invoker that calls the 0x408099f0 dispatcher for
      the boot-driver install (it sets d3 byte1); it is very likely a
      Gestalt/universal-table/hardware-register read that my q950 model
      stubs to a value whose byte1 becomes 0xF8 instead of 0x01.
      (b) diff against the quadra700 ROM's analogous driver-installer to
      see what its expected ddType is and how it is derived.  (c) once
      the source is found, correct the machine-config value it reads (a
      quadra950.c fix, no shared-code risk) — success = 0x40807264 sees
      d1=1, matches, 0x40807272 taken, block-64 read fires, drive
      installed, boot past "?".
    - **METHOD that finally worked for reading paused state** (m68k
      gdb-multiarch's own register reads are GARBAGE here — a register-
      map mismatch prints 0xb6dbb6dd… — but its breakpoints DO pause the
      VM): launch qemu `-S -gdb tcp::PORT` + a unix `-monitor`; run
      `gdb -x cmd` (NON-batch, background) with `break *ADDR` + `continue`
      so it holds the VM paused at the breakpoint; then read state via
      the HMP MONITOR (`info registers` gives correct PC/A7/Dn; `x`/`xp`
      give memory — but MIND the MMU: low RAM here is identity-mapped so
      `x`≡`xp`, elsewhere use `x`).  Poll `info registers` until
      `PC==ADDR` to know it is paused.  Do NOT trust `xp` of a stack
      buffer across runs (stack pointer varies).

17. **ORACLE DIFF quadra700 vs quadra950 on the ddType blocker (this is
    the crisp next-lead; the exact machine value was NOT yet located).**
    - The boot-driver installer 0x40807224 and the ddType-match at
      0x40807264 are **byte-identical** in both ROMs (verified: file
      offsets 0x7224 and 0x7260 match).  So the 0xF8 vs 0x01 difference
      is a HARDWARE/MACHINE VALUE the two models return differently, fed
      into identical code — exactly as suspected.
    - Broke BOTH machines at 0x40807264 (ddType compare) and at
      0x40807224 (installer entry) via gdb + HMP monitor:
        | reg            | quadra700  | quadra950  |
        |----------------|------------|------------|
        | d1 (@0x40807264, expected ddType) | 0x00000001 | 0x000000f8 |
        | d3 (@0x40807224 entry)            | 0x0001ffff | 0x00f8ff40 |
        | a0@(6) (disk ddType)              | 0x0001     | 0x0001     |
        | A4 (persists into installer)      | 0x0000008a | 0x00000091 |
      d1 = d3 byte1 (0x4080722c `swap d3`; 0x40807230 `moveb d3,d0`;
      0x40807232 `movew d0,fp@(-10)`).  q700's byte1=0x01 MATCHES the
      disk's ddType=1 and it BOOTS; q950's byte1=0xF8 does not.  So the
      whole fix reduces to: **make q950 produce d3 byte1 = 0x01 (like
      q700).**
    - d3 is built in chained / interpreted / trap-dispatched code that
      TB-chaining hides.  What I could see (q950): d3 first shows as
      0x00f8ff40 arriving at the trap dispatcher 0x40806e16 (`cmpiw
      #61,d0; jmp @(0xdb8)@(0,d0:w:4)`, selector d0=0x30) with A4=0x91;
      from there the flow is 0x40806e16 → (dispatch) → …0x40806460 →
      0x408071f0 (`btst #7,0xb22`) → 0x408071fc (the SCSI-ID `dbf d5`
      scan, which saves d1-fp so d3 is frozen from here) → 0x40807224.
      The setter of d3 runs BEFORE 0x40806e16 and is NOT any of: the
      0x40806ef8 resource-dispatch loop (that loop's d3 is a resource
      type e.g. 'FONT'=0x464f4e54 — a red herring), the 0x408992cc SCSI
      store, or the 0x40899342 PIO store.  It is reached indirectly (no
      static branch to 0x40806e16 exists; the call to 0x408071f0 has no
      preceding call instruction at its return addr 0x40806460 — pure
      table/vector dispatch).
    - **A4 = 0x91 (q950) / 0x8a (q700) is the prime suspect** — it is a
      persisting machine-derived byte that rides into the installer.
      Note 0x91 == the q950 VIA1 port-A pins value (Q950_VIA1_PINS_A);
      q700 pins are 0xc1 but its A4 is 0x8a, so A4 is a DERIVED value
      (port A possibly rewritten at runtime, or a table lookup), not raw
      pins.  Next step: find where A4 (and d3 byte1) is computed — the
      only reliable way given TB-chaining is `-d exec,cpu,nochain`, but
      a broad 30 s nochain trace overflows the ~1 G disk (an
      0x40800000..0x40808000 nochain trace hit 842 MB in <35 s and still
      had not reached the installer).  So either (a) free up disk and do
      a tightly-time-boxed nochain trace of the installer window only,
      or (b) drive a gdb loop: break at 0x40806e16, poll HMP `info
      registers` for d0==0x30 (the installer's selector) with
      d3==0x00f8ff40, and when it hits, read the register/memory the
      preceding indirect jump used (that holds the machine value); then
      read the SAME location on quadra700 and see what differs.  The fix
      is a quadra950.c change (a VIA1 port-A bit, a gestalt/box byte, or
      an MCU/config register) that makes that value match q700 so d3
      byte1 resolves to 0x01.  Keep it in quadra950.c (no shared-code
      risk); confirm `-M quadra700`/`-M q800` still boot if you touch
      anything shared.

18. **THE FIX for findings 16/17 — traced `d3` all the way back to a
    real PRAM byte using `-d exec,cpu -dfilter <narrow-range>` (NOT
    nochain — plain `exec,cpu` logging with a tight `-dfilter` around
    the suspect call site gives full per-instruction register dumps
    without the nochain disk-overflow problem; this is the method to
    use next time, cheaper than the gdb-poll loop).  Method and full
    derivation:
    - Oracle-diffing the two machines' registers at `0x40807264` (see
      finding 17) plus a stack unwind at the installer entry
      (`0x40807224`, `A7` identical on both machines: `0x400a1a`)
      found the *outer* caller by computing the moveml/byte-push frame
      size by hand and reading `[A7+62]`: `R_outer = 0x4080157e`,
      i.e. the call site is `0x4080157a: jsr @(0x40801574,%a0:l)`
      with `a0 = 0x5c7c` (a fixed literal) → target `0x408071f0`
      (verified: `0x40801574 + 0x5c7c == 0x408071f0`).  That routine
      (the SCSI-ID-scan entry, `moveml d1-fp` at `0x408071fc`) does
      **not** set `d3` — entering it, `d3` is already the final wrong
      value, so the setter is further back still.
    - A `-d exec,cpu -dfilter 0x40801400..0x40801700` full-boot trace
      (2-2.5 MB per machine — small, safe for /workspace even, but
      written to /tmp per instructions) caught the actual setter
      in-line: at `0x40801430`, `d3` starts as leftover garbage
      (`0xfffffff6`); by `0x40801440` (after `0x40801434`'s `.short
      0xa07d`, an **A-line trap $A07D**) `d3`'s low word has become
      `0xff40`; by `0x40801454` (after `0x4080143e`'s trap `$A084`,
      plus two `swap d3` instructions bracketing a second
      `movew sp@+,d3`) `d3` is the final `0x00f8ff40`.  So `d3` is
      built from **two A-line OS traps**, byte-for-byte:
      `d3 = (result of trap $A084) : (result of trap $A07D)`
      (high word : low word).
    - Read the Line-1010-emulator vector (`VBR=0`, physical `0x28` =
      `0x408099b0`, the ROM's generic old-style-OS-trap dispatcher:
      `andiw #0x100,d2` selects "old" traps in `$A000-$A0FF`, then
      `jsr @(0x400 + (trapword&0xff)*4)` — so trap handlers for
      low-byte `0x7D`/`0x84` live at whatever's stored in RAM at
      `0x400+0x7D*4=0x5F4` and `0x400+0x84*4=0x610`).  Read those two
      RAM longs live via HMP `xp`: `0x5F4 -> 0x40801390`,
      `0x610 -> 0x408013b0`.  Disassembled (byte-exact, ROM file
      bytes: `20 3c 00 04 00 78 a0 51 4e 75` /
      `20 3c 00 02 00 76 a0 51 4e 75`):
      - `0x40801390`: `movel #0x00040078,d0; TRAP $A051; rts`
      - `0x408013b0`: `movel #0x00020076,d0; TRAP $A051; rts`
      Both call the SAME trap `$A051` (vector RAM `0x400+0x51*4=0x544`
      -> `0x4080b186`), passing `d0` = `(subcmd:16 | pram_addr:16)`.
      `0x4080b186` forwards to `0x40815204` (gated by `lowmem[0xdd4] &
      0x70 == 0x20`, the **Egret-present flag** — Q700 lacks an
      Egret so it takes the *other* branch at `0x40815260`, a
      completely different RTC-bit-bang code path; this is WHY q700's
      code differs here despite the ROM being "byte-identical" at the
      installer itself — the two machines legitimately fork upstream).
      `0x40815204` builds an Egret pseudo-packet param block: **the
      address gets `+0x100` added** (`0x78`->`0x178`, `0x76`->`0x176`
      — this is the "trap dispatcher adds 0x100" from finding 4/9),
      the packet's actual on-wire command byte is **hardcoded to `2`**
      (`READ_PRAM`) regardless of the caller's `d0` high word, and
      the caller's original `d0` high word (`4` or `2`) is stashed as
      a **length** field instead — i.e. this ROM helper always issues
      a real Egret `READ_PRAM` (cmd 2) exchange, then **clocks
      `length` bytes out of the SAME exchange** to read a PRAM
      word (`length=2`) or longword (`length=4`), not just one byte.
    - **The bug**: `quadra950.c`'s `EGRET_CMD_READ_PRAM` handler always
      set `egret_resp_len = 5` (exactly one data byte after the
      4-byte header), unlike `EGRET_CMD_GET_PRAM` which streams up to
      260 bytes for as long as the host keeps clocking SR reads.  A
      caller that clocks 2 or 4 bytes from a single `READ_PRAM`
      exchange (as this ROM helper does) got byte 1 correct and then
      ran past the end of `egret_resp[]`'s populated region — the
      *stack scratch buffer* the ROM had reserved for the result
      (`subaw #2,sp` / `subaw #4,sp` right before the trap) was only
      partially written by the transport, so the un-delivered byte(s)
      were **whatever garbage was already on the stack**, not real
      PRAM data.  For the critical call (`addr=0x76` -> `0x176`,
      length 2, this is `R2`/`d3`'s high word/the byte the installer
      compares): confirmed live via `-d unimp,guest_errors` (which
      includes the existing `q950_log("q950 egret: read 0x%03x ->
      0x%02x\n", ...)` call) that `PRAM[0x176] == 0x00` (correct —
      matches the ROM's own default-PRAM-rebuild data table at ROM
      file offset `0xc68a`, dumped: `00 01 ff ff ff df 00 00 …`,
      written via the `(addr=0x76, len=19)` block write logged as
      `q950 egret: write 0x176 len 19 [00 ...]`), but the SECOND byte
      (`PRAM[0x177]`, should be `0x01`) was never supplied by the
      emulated transport at all — no `read 0x177` log line ever
      appears — so the caller's second stack byte was leftover
      garbage (`0xf8`), giving the observed `d3` high word `0x00f8`
      instead of the correct `0x0001` (which is exactly what q700's
      RTC-bit-bang path legitimately produces, reading the same
      logical default-PRAM bytes `00 01` via a different transport).
    - **Fix applied** (`hw/m68k/quadra950.c`, `EGRET_CMD_READ_PRAM`
      case only): stream consecutive PRAM/clock bytes into the
      response buffer exactly like `EGRET_CMD_GET_PRAM` already does
      (`for (i = 0; i < 260; i++) { ... r[4+i] = ...; }`,
      `egret_resp_len = 4 + 260`), instead of writing only `r[4]` with
      `egret_resp_len = 5`.  This does not change behavior for the
      existing single-byte consumer (interrupt-driven trap dispatcher
      at `0x40814a58`, per the original comment) since it clocks
      exactly one byte and stops (TREQ-style host-driven
      termination, unaffected by more bytes being available behind
      it) — it only fixes the multi-byte consumer.  Rebuilt, and
      `PRAM[0x176..0x177]` now delivers `00 01` correctly; `d3` byte1
      resolves to `0x01`; the driver installs.
    - **Verified past "?"**: screendump `/tmp/q950_after.png` shows
      the classic "Mac OS / Starting up…" boot screen (NOT the
      flashing "?") shortly after boot.  This is further than this
      machine has ever booted (previous best was the flashing "?").
    - **Regression check**: only `hw/m68k/quadra950.c` was touched;
      `-M quadra700` still boots to a fully working Finder desktop
      (screendump `/tmp/q700_check.png`, shows a Control Panels
      Finder window) — no shared-code risk, confirmed no regression.

19. **NEW, SEPARATE blocker after the finding-18 fix — boot reaches
    "Mac OS / Starting up…", loads ~303 KB of the System off SCSI,
    then HARD-hangs (deterministic under `-icount shift=7`).  Deeply
    characterized this session; NOT yet fixed.  It is emphatically a
    software deadlock, NOT the ddType bug and NOT the SCSI/ESP.**
    - **Hang signature (deterministic — identical registers every run):
      PC spins at `0x800a1f72` (the `_SystemTask`/idle "run the queues"
      loop).  Registers frozen: `A6=0x5f9f36`, `A2=0x800a9afe`,
      `A3=A4=0x0000ef10`, `D3=0xffffff07`, `SR=2004` (IPL 0).  The A6
      frame-chain is stable over time (verified) = truly wedged, not
      slow.**
    - **Read-count proof of hard hang:** with an ESP trace
      (`-trace events=<esp_mem_readb/writeb,esp_command_complete,
      esp_raise_irq,scsi_req_parsed_lba,…>`), q950 executes **exactly
      606 SCSI reads and then stops forever** (read count frozen at 606
      for >120 s at three checkpoints; last LBA 82932).  For contrast,
      **quadra700 booting the SAME disk does 4027+ reads and keeps
      going** — so q950 stalls partway through loading the System file
      (the last ~40 reads are *scattered distinct* LBAs 81972–82932 =
      a normal File-Manager extent-walk of one file, which simply
      ceases — an upstream stop, not a SCSI retry).
    - **The SCSI/ESP layer is byte-for-byte identical to q700 and works
      perfectly.**  The frozen call-chain (walked via the A6 links +
      HMP `x`) is: ROM Device-Manager trap dispatch (`0x40809a0a`, the
      `0x408099f0`→`jsr @(0x400,%d2:w:4)` DM dispatcher) → System-heap
      SCSI code `0x800a942a` → `0x800a99dc` → wait.  `0x800a99dc` is a
      **SCSI CDB builder**: `moveb #40,%a3@` (0x28 = READ(10)) or
      `moveb #8,%a3@` (0x08 = READ(6)), big-endian LBA into `a3@(2..5)`,
      length into `a3@(7..8)`, CDB length (10/6) into the request block
      at `+53`, buffer ptr at `+40`, byte count (blocks×blockSize) at
      `+44`.  So the hung op **builds CDB #607 then waits for it to
      complete** — but the ESP never receives it: at the hang the ESP
      is fully idle (`RSTAT=RSEQ=RFLAGS=0x00`, live-read at
      `0x5000f0{40,60,70}`), quiescent in the normal post-command
      ENSEL/disconnected state.  q700 uses the *same* ENSEL(0x44)/
      MSGACC→INTR_DC(0x20)/asc_mode=DIS cycle after every command
      (3433 ENSELs, ends in the same ENSEL-idle state) and boots — so
      **CMD_ENSEL / QEMU-esp.c's lack of reselection support is NOT the
      cause** (both machines rely on it identically).
    - **The wait is a classic ioResult poll.**  `R_B` at `0x800a1ec2`
      loops `while (a4@(10) == 1) call 0x800a1f64(a3)` where
      `a4 = 0x155b0` and `[0x155ba] == 0x0001` forever (== ioInProgress).
      `0x155b0` is the parameter block the ROM DM was called with
      (`a0=0x155b0` on the stack at the `0x40809a0a` frame).
      `0x800a1f64` just runs the deferred/VBL "SystemTask" queue (checks
      IPL via `0x800a446e`, walks `[0xc0c]`-based queues) — it does NOT
      poll any device, confirming this is a wait for an *async
      completion* that never fires.
    - **Ruled out (each checked live at the hang):**
      (a) *interrupt storm / stuck IRQ* — VIA1 IFR=0x41 **without** the
      0x80 summary bit, VIA2 IFR=0x00; no pending interrupt; CPU at
      IPL 0.  (b) *deferred-task-drain starvation* (the finding-14
      worry) — the Deferred-Task-Mgr queue at `0x0d92` is EMPTY
      (qHead=0), and we're already at IPL 0 with VBL (Ticks `0x16a`)
      still incrementing, so tasks could drain.  (c) *ADB* — autopoll
      Talk-R3 (cmd 2b/2f/3b/3f/fb/ff) runs normally; the lone `cmd 0x21`
      → timeout happens exactly once (a reserved ADB command, timeout is
      correct) and is not in the loop.  (d) *Time-Manager/VBL dead* —
      Ticks advance, VBL(L1) + VIA2(L2) both fire.
    - **CONCLUSION / crisp next-lead:** after ~606 reads the boot stops
      dispatching File-Manager disk reads and wedges on an async
      ioResult=1 that is never completed *and never handed to the ESP*
      (ESP idle).  Since the ROM DM/SCSI code is almost certainly
      byte-identical q700↔q950 (as the installer was in finding 17),
      this is very likely **the SAME shape of bug as finding 18: a
      machine-specific value/condition that q950 resolves differently,
      steering the SCSI-Manager/File-Manager async state machine into a
      "wait" instead of "dispatch next".**  The next session should:
      break at the ROM DM dispatch `0x40809a04` and log `d2`(DM
      routine selector)/`a0`(PB)/refNum for the LAST call before the
      freeze to name the exact driver+opcode; then diff the deciding
      register/lowmem between q700 and q950 at that call (prime suspects:
      a gestalt/`0xdd0-0xdd8` universal-flags bit the SIM tests for
      "disconnect/async supported", or a Time-Manager reselection-timeout
      task that never gets armed on q950).  All heavy traces to /tmp;
      the ESP-trace + read-count method above is the fastest way to
      confirm any fix (read count must climb past 606 toward q700's
      4027 → Finder).  Screenshot at the hang: still the "Starting up"
      progress screen (`/tmp/q950_after.png` from finding 18 is
      representative).
    - **DEEPER TRACE of the wedge loop (session N+3, still no fix — the
      manager is identified, the exact completion condition is not).**
      The `_SystemTask`-style spin at `0x800a1f72` is a MANAGER on the
      private OS A-trap **`0xA089`** (handler patched to `0x800a86be` →
      `0x800a9c84` → `0x800a1780` → `0x800a17a6`).  Its globals live at
      `[0xc0c] = 0xc8f0`; the "current request list" is
      `[[0xc0c]+436] = 0xef10` (= the frozen `A3=A4`).  Reconstructed
      loop each iteration: `R_B` (`0x800a1ec2`) does
      `while ([0x155ba] (== a4@(10)) == 1) 0x800a1f64(0xef10)`;
      `0x800a1f64` (the queue-runner) passes its IPL<1 +
      `[[0xc0c]+192]==0` + `[[0xc0c]+190]==0` gates (all true here →
      it takes the *process* path `0x800a1fb2`, NOT the defer path),
      clears bit0 of `a4@(72)=[0xef58]`, and calls `0x800a1fd2` which
      builds a 36-byte pblock and fires trap `0xA089` (twice) →
      re-enters `0x800a1780`/`0x800a17a6`, whose dispatcher
      `0x800a1b98` switches on the request's opcode `a4@(8)` (== **1**
      for the wedged req at `0x155b0`) to `0x800a1c0e`, which arms the
      wait.  So the manager IS actively re-servicing its queue every
      iteration — this is NOT a starved/never-run path — yet the
      request's status word `[0x155ba]` never leaves 1 and the SCSI
      CDB it built is never handed to the (idle) ESP.
    - **What 0xA089 is:** a private System dispatch trap this manager
      installed (handler in the System heap, `0x800a86be`, a 3-way
      `braw` table → `0x800a9c84`/`0x800a9dce`/`0x800a9c52`).  The
      manager iterates a client/unit array at `a4@(104)`/count
      `a4@(68)`, and the config-feature predicates right next to it
      (`0x800a9c9a`..`0x800a9cf6`) test the universal-ROM flags
      **`0xdd0` bit0/1/3, `0xdd1` bit5, `0xdd4` bit23, `0xdd8@(13)==15`,
      `0xdd8@(-7)`** — q950 has `0xdd0=0x07a31807`, `0xdd4=0x02040924`,
      `0xdd8=0x0000c8b0` (these come from the ROM universal table for
      the q950 box and legitimately differ from q700's
      `0x05a0183f`/`0x00000900`; they are NOT emulation-settable
      without breaking machine-ID, so — unlike finding 18's PRAM byte —
      a flag diff here is probably a *correct* HW-feature difference the
      manager acts on, meaning the missing piece is the *event/IRQ that
      path expects*, not a wrong value).  **Identifying which real
      manager owns trap 0xA089 (Process Mgr? File/Disk-cache async?
      the SIM's client-notify?) is the key unknown for next session** —
      grep a System-file trap table or a live `_GetTrapAddress(0xA089)`
      symbol, then find where opcode-1 requests are *completed* (who
      writes `a4@(10) = 0`) and what hardware event that completion
      waits on that q950 doesn't deliver.
    - Method notes for next session: the System heap at `0x800xxxxx`
      is MMU-mapped — use HMP **`x`** (virtual), NOT `xp` (fails with
      "Cannot access memory").  The OS trap table is at physical
      `0x400` (`handler = [0x400 + (trap&0xff)*4]`), so any A-trap in
      the loop can be resolved to its patched handler instantly.  The
      wedge is fully deterministic (identical regs every run:
      `A6=0x5f9f36 A2=0x800a9afe A3=A4=0xef10 D3=0xffffff07`), so a gdb
      watchpoint on the status word `*0x155ba` (write) will catch the
      completer the instant it ever runs — it never fired in a multi-
      second window, confirming the completion truly never happens.
    - **THE MISSING EVENT, NAMED (session N+4 — ESP-CMD dispatch trace).**
      Method: temporary logging in `esp_reg_write` (ESP_CMD case) of the
      guest PC (`((M68kCPU *)current_cpu)->env.pc`) + WBUSID for
      SELECT/ENSEL (0x41–0x44) — reverted afterwards; MMIO gdb
      watchpoints DO fire (tried `watch *(char*)0x50f0f030`, hit
      old=0/new=1) but gdb then detaches choking on the register read,
      so the esp.c PC-log is the reliable capture.  Results:
      - Reads 1–~612 go through **two OLD-SCSI-Manager paths**: the ROM
        (`pc=0x40898de0`, 405×) and a loaded disk driver
        (`pc=0x000418dc` on q950 / `0x000440ec` on q700, ~207×), all
        `SEL|DMA` (0xc1) to **target 0** — these load the initial System
        and work identically on both machines.
      - Then the boot hands off to the **SCSI Manager 4.3 (SIM)** in the
        System heap.  On BOTH machines the SIM first ENSELs target 0
        (q950 `pc=0x800a571c` / q700 `0x800c4a5c`) then runs a **bus
        scan**: `SELECT` (0x41) targets **6→1** (q950 `pc=0x8009fdee` /
        q700 `0x800bf12e`), each empty → INTR_DC — **identical on both.**
      - **THE DIVERGENCE (exact):** immediately after the 6→1 scan,
        **quadra700 issues `SEL|DMA` (0xc1) to target 0
        (`pc=0x800bf25e`) and then does THOUSANDS of SIM reads** of the
        disk (`0x800bf25e`/`0x800c2b92`, 3026+3432 dispatches) → boots.
        **quadra950 WEDGES right there — it never dispatches a single
        SIM `SEL|DMA target 0`** (0 System-heap 0xc1 dispatches after the
        scan).  So the missing event is precisely the **SIM 4.3
        scan-complete → first-async-read-of-target-0 transition**: the
        target-0 read PB is built (`0x800a99dc` CDB, finding 19 above)
        and queued, but the SIM's bus engine never picks it up to select
        target 0.
    - **DRQ→VIA2-CA2 wiring difference examined and RULED OUT as the
      fix.**  q700 forwards the ESP DRQ to VIA2 CA2 (`VIA2_IRQ_SCSI_DATA_BIT
      == CA2_INT_BIT`, via `q700_esp_drq`→`esp_drq_via2`); q950's
      `q950_esp_drq` does NOT (it only latches `m->esp_drq` for the DAFB
      TurboSCSI reg).  BUT on q950 **VIA2 CA2 is deliberately the ISM/SWIM
      IOP interrupt** (`m->swim_iop.irq = invert(gpio_in(via2,
      VIA2_IRQ_SCSI_DATA_BIT))`, quadra950.c ~2543; matches finding 11 —
      the ROM's L2 handler treats VIA2 IFR bit0/CA2 as the ISM IOP).
      This is a genuine tower design point (the 900/950 route ADB/SWIM
      through the IOP on CA2 and take SCSI DRQ only via the TurboSCSI
      register, which q950 already provides and which the OLD-path reads
      use successfully).  Routing DRQ to CA2 would collide with the IOP
      and almost certainly break the ADB/IOP boot that already works, so
      it is not the fix.
    - **The SIM engine's own state is NOT reachable from the frozen
      manager context** (the wedge is in the 0xA089 manager, whose
      `a5=0x600190` is a different struct than the SIM engine's `a5`
      HBA — its `a5@(174)` "advance/wait" callback and `a5@(438)`
      RSTAT-phase reads need the SIM engine's live `a5`).  **Next
      session's single concrete step:** set a gdb breakpoint at the SIM
      scan-SELECT `0x8009fdee`, let it hit the LAST scan target (WBUSID=1),
      then single-step the post-scan flow (`0x8009fe08`.. reads `a4@(16)`,
      `a5@(422)` phase, `a5@(57)`, `a0@(103)` gates) to the branch that
      diverts to "wait" instead of the target-0 `SEL|DMA` — that branch's
      guard IS the missing event/flag.  Compare the same guard live on
      quadra700 (which takes the read branch).  Read regs via HMP (gdb's
      own reg reads are byte-swapped garbage / detach on stop).

20. **THE FIX for finding 19 — the "missing event" was the scan
    completion of the Q900/950's SECOND SCSI BUS (external 53C96),
    which the model did not have.  FINDER REACHED.**
    - Method (no gdb single-stepping needed): the SIM module was
      dumped from BOTH machines' guest RAM via HMP `memsave` (virtual;
      q950 base 0x80090000, q700 base 0x800b0000) and proved
      byte-identical over `0x8009fb90..0x800a4548` ↔
      `0x800beed0..0x800c3888` (delta 0x1f340), so q700 SIM addresses
      map to q950 ones by subtracting 0x1f340.  Then a
      `-d exec,cpu -dfilter 0x8009fc00..0x800a0100` (q950) /
      `0x800bef40..0x800c0440` (q700) trace of just the SIM bus-engine
      range captured every engine entry with full registers (~1 MB on
      q950; on q700 kill it soon after the SIM handoff — the async
      read flood grows the log at ~GB/min).  Disassembly of the dumped
      module (m68k objdump on the memsave carve) gave the engine
      structure: a3 = ESP register base, a4 = transaction block,
      a5 = per-HBA globals, a0 = PB; scan-SELECT at 0x8009fdee (CMD
      0x41), start-transaction/SEL|DMA routine at 0x8009fef2 (CMD 0xc1
      at 0x8009ff1e = q700's 0x800bf25e), post-select dispatch on the
      latched RINTR at 0x8009fe76-0x8009fec4.
    - The register dumps at each scan SELECT told the whole story
      instantly: q950 runs the 6→1 scan on HBA #1 (a5=0xf850,
      **a3=0x50f0f000**) exactly like q700 — and then issues an 8th
      SELECT from a DIFFERENT HBA instance (a5=0x94e80,
      **a3=0x50f0f402**, target 6 again): the SIM registered TWO
      buses and began scanning the SECOND one.  0x50f0f402 was
      unmapped in the model (reads 0/discarded writes), so that
      select never raised INTR_DC; the fe76 dispatch fell through to
      its `pea 0x8009ffb6; trap $ABFF` "wait" tail and the XPT never
      proceeded to dispatch the queued target-0 read PB (0x155b0) on
      bus 1.  The wedge was thus not a flag at all — it was a real
      missing DEVICE.  (This is also why the 0xdd0/0xdd4 flag
      differences were correct machine values: they tell the SIM the
      towers are dual-bus.)
    - Hardware truth (Linux `drivers/scsi/mac_esp.c`,
      MAC_SCSI_QUADRA2 = Quadra 900/950): chip N registers at
      `0x50F0F000 + N*0x402` (reg stride 0x10, so chip 2's byte lane
      is +2), PDMA IO at regs+0x100 (chip 2: 0x50F0F502), TurboSCSI
      handshake regs at `0xF9800024 + N*4`, and BOTH chips share the
      single VIA2 SCSI interrupt — Linux's ISR just polls both chips'
      RSTAT INT bits, matching the SIM's per-HBA polling.
    - **Fix (quadra950.c only):** second `SysBusESPState esp2` mapped
      at `ESP2_BASE 0x50F0F400` / `ESP2_PDMA 0x50F0F500` (the esp-regs
      MMIO decodes `addr >> it_shift`, so the +2 lane lands on the
      right registers); its IRQ OR'd with the internal chip's through
      a 2-input `esp_irq_orgate` feeding the inverted
      `VIA2_IRQ_SCSI_BIT` gpio (DRQ→CA2 untouched, per finding 11 CA2
      stays the ISM IOP); its DRQ latched into a second TurboSCSI reg
      (`turboscsi_ctrl2`/`esp2_drq`, reg 0xF9800028, same DRQ bit 9);
      `scsi_bus_legacy_handle_cmdline` called for it too (external
      devices could attach as bus=1; none by default, exactly like a
      real tower with nothing plugged in).  With no devices on bus 2,
      each scan SELECT times out to INTR_DC in the stock ESP model —
      the scan completes, the SIM starts the target-0 async read on
      bus 1, and the read count blows past 606 into the thousands.
    - **Result: `-M quadra950` boots MacOS 7.5.3 all the way to the
      FINDER** (`/tmp/q950_finder.png`: desktop + Control Panels
      window + mounted volume); `-M quadra900` identically
      (`/tmp/q900_finder.png`).  Only `hw/m68k/quadra950.c` changed —
      no shared-code regression surface.

## Current boot sequence (quadra950 AND quadra900, ~15 s to gray)

POST (incl. SCC IOP RAM test) -> machine ID (straps ok) -> Egret PRAM
scan/rebuild via polled transport -> NuBus scan -> IOP manager init
(RAM test x2, start, alive, channel counts) -> DAFB 1152x870 GRAY
SCREEN -> SWIM driver chan-1 IOP exchanges complete -> ADB-over-IOP
SendReset completes -> full 16-address Talk-R3 scan completes (mouse +
keyboard found, addresses reassigned) -> mouse cursor renders/tracks
-> ROM SCSI boot scan runs: selects target 0, reads DDM block 0
(pseudo-DMA delivers the full 512 bytes correctly; sbDrvrCount=1,
ddType=1 both land in RAM correctly) -> the boot-driver installer
(0x40807224) reads the expected ddType via a PRAM word read
(**FIXED, finding 18**: was 0xF8 from an under-filled Egret
READ_PRAM response, now correctly 0x0001 matching the disk's
driver) -> driver installs -> boot proceeds PAST the flashing "?"
to the "Mac OS / Starting up…" screen -> loads ~303 KB of the System
via 606 SCSI reads through the old SCSI Manager -> hands off to SCSI
Manager 4.3 (SIM), which bus-scans BOTH tower SCSI buses (**FIXED,
finding 20**: the second/external 53C96 at 0x50F0F402 is now
modeled, so the second scan completes instead of wedging) -> SIM
async reads take over (thousands of SEL|DMA target-0 reads) ->
**MacOS 7.5.3 FINDER desktop** (`/tmp/q950_finder.png`; quadra900:
`/tmp/q900_finder.png`).

## Build/test commands

    ninja -C build qemu-system-m68k
    build/qemu-system-m68k -M quadra950 -icount shift=7 \
        -bios /workspace/files/mac-roms/quadra950.rom \
        -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
        -snapshot -serial null -serial null -display none \
        -monitor unix:/tmp/q950.sock,server=on,wait=off \
        -d unimp,guest_errors -D /tmp/q950.log
    # -M quadra900 identical (same ROM, straps 0x50)
    # kill by saved PID only (NEVER pkill by name — see Q630 notes;
    #   pgrep matches the launching bash wrapper too: use
    #   pgrep -f "build/qemu-system-m68k -M quadra950")
    # screendump via monitor socket -> PPM -> PIL
    # -d exec dfilter tracing fills /tmp at GB/min if a busy-wait loop
    #   is inside a filter range — keep ranges tight, kill promptly
