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
- [ ] **BLOCKED: the ADB device scan does not continue past SendReset**
      (see finding 14); the boot waits forever at ROM 0x4080a8e6 for
      ADB-init completion (lowmem 0xdabd bit5), so the SCSI boot scan
      is never reached
- [ ] MacOS 7.5.3 Finder
- [x] q800 regression check: unaffected (no shared code touched;
      verified booting to the MacOS boot screen alongside)

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

## Current boot sequence (quadra950 AND quadra900, ~15 s to gray)

POST (incl. SCC IOP RAM test) -> machine ID (straps ok) -> Egret PRAM
scan/rebuild via polled transport -> NuBus scan -> IOP manager init
(RAM test x2, start, alive, channel counts) -> DAFB 1152x870 GRAY
SCREEN -> SWIM driver chan-1 IOP exchanges complete -> ADB-over-IOP
SendReset completes -> STALL waiting for the ADB address scan.

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
