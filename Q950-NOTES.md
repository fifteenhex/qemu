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
- [ ] MacOS 7.5.3 Finder
- [x] q800 regression check: N/A — only quadra950.c changed (no shared
      code).  q800 unaffected by construction.  Confirmed q950 still
      reaches the post-ADB "?" state after this session (no regression).

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
(0x40807224) rejects the disk's driver because it wants ddType=0xF8
but the disk's driver is ddType=1 -> no driver installed -> flashing
"?" (no startup disk) icon, rescanning forever (see finding 16).

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
