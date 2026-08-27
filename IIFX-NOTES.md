# Macintosh IIfx machine ("maciifx") — journal

Goal: new QEMU machine for the Mac IIfx (68030 @ 40MHz) booting MacOS
7.5.3 from SCSI, with a NuBus macfb video card (the II family has no
onboard video).  Working tree: /workspace/src/qemu-iifx, branch
`add-iifx`.  BUILD IN TMPFS ONLY: /tmp/iifx-build (workspace is ~950MB
free — never build there).

    mkdir -p /tmp/iifx-build && cd /tmp/iifx-build && \
      /workspace/src/qemu-iifx/configure --target-list=m68k-softmmu \
        --disable-docs --disable-tools && ninja qemu-system-m68k
    # incremental: ninja -C /tmp/iifx-build qemu-system-m68k

ROM: /workspace/files/mac-roms/macIIfx.rom (512K, checksum 4147DD77).
Disk: /workspace/files/HD0-OpenRetroSCSI-7.5.3.hda (run with -snapshot,
image is shared with other sessions — check fuser, never pkill).

## Hardware references (gathered 2026-08-26)

Sources: MAME src/mame/apple/maciifx.cpp + scsidma.cpp + applepic.cpp
(fetched to /tmp), Linux arch/m68k/mac/oss.c + asm/mac_oss.h +
asm/mac_iop.h + config.c (local tree /workspace/src/linux).

### Memory map (MAME maciifx_map; mirror 0x00f00000 = A20-A23 don't care,
so Linux's 0x50f0xxxx == 0x5000xxxx)

- ROM 512K at 0x40000000, mirrored through 0x4fffffff (canonical Mac
  base 0x40800000)
- 0x50000000 VIA1 (regs every 0x200; **second full copy at +0x40000**)
- 0x50004000 SCC IOP host interface (applepic; Linux SCC_IOP_BASE_IIFX)
  - byte offsets decode as (byte>>1): +0/+2 ram addr hi/lo, +4 ctrl,
    +8.. ram data, **+0x20-0x3f = bypass window to the SCC** (reg =
    (byte>>1) & 0xf)
- 0x50008000 SCSIDMA ASIC (343S0064: embedded NCR 53C80 + DMA):
  - +0x00-0x7f 5380 regs, reg = offset>>4
  - +0x00-0x03 handshake data window (blind MOVE.L r/w)
  - +0x60-0x63 handshake read window
  - +0x80 control u32 (bit0 DMAEN, 1 IRQEN, 3 HNDSHK, 6 SCIRQEN(status),
    7 WDIRQ, 8 DMABERR, 12 ARBEN, 13 WONARB), +0xc0 DMA count,
    +0x100 DMA address.  MacOS never uses true DMA (A/UX only).
- 0x50010000 ASC (plain ASC, not EASC) — NOTE: not 0x14000!
- 0x50012000 SWIM IOP host interface (Linux ISM_IOP_BASE_IIFX);
  ADB is bit-banged by this IOP (mailbox chan 2, Linux adb_iop protocol)
- 0x50018000 "BIU" — reads 0
- 0x5001a000 OSS
- 0x50024000-0x50027fff must BUS ERROR ("so ROM can determine we're an
  FMC" — MAME comment)

### OSS (replaces VIA2; Linux mac_oss.h + MAME oss_r/w/oss_interrupt)

Byte regs at base+0x000..0x3ff:
- +0x00..0x0f: per-source interrupt LEVEL (0 = disabled, 1-7 = 68030
  IPL for that source).  Linux disables a source by writing level 0.
- +0x200: bit7 = "interrupt active" status (MAME)
- +0x202: pending bits sources 8-15 (bit = src-8); +0x203: pending
  sources 0-7 (big-endian u16 irq_pending at 0x202)
- +0x204: rom_ctrl; write 0x80 = poweroff
- +0x207: write = ack/clear the latched 60Hz tick (source 10)

Sources (Linux mac_oss.h): 0-5 = NuBus slots 9-E, 6 = SWIM/ISM IOP,
7 = SCC IOP, 8 = sound (ASC), 9 = SCSI, 10 = 60Hz tick (latched,
cleared via +0x207), 11 = VIA1, 14 = parity.  Linux/A-UX level map:
ISM=1 SCSI=2 NUBUS=3 SCC=4 VIA1=6.  The ROM programs its own levels;
the model takes IPL = max(level[src]) over pending sources.

### Everything else

- VIA1: port A = machine ID straps, MAME returns 0xd3.  Port B: classic
  343-0042 RTC bit-bang on PB0-2 (same engine as maciisi/lc475).  CA1 =
  60.15Hz tick, CA2 = RTC 1Hz.  VIA1 IRQ -> OSS source 11.
- IOPs: same host interface + mailbox protocol as the Quadra 900/950
  (quadra950.c has a working HLE model: alive flag, send/recv channels,
  ADB on ISM chan 2, in-place staged follow-up ADB commands).  IIfx SCC
  IOP at 0x4000 (Q950: 0xc000), ISM at 0x12000 (Q950: 0x1e000).
- ADB over the ISM IOP (Linux MAC_ADB_IOP), not Egret: IIfx keeps the
  classic bit-bang RTC on VIA1 for PRAM/clock.
- No onboard video: NuBus macfb card in slot 9 (as q800/quadra950),
  slot IRQs -> OSS sources 0-5.

### Carried-over gotchas from sibling machines

- Run with **-icount shift=7** (TimeDBRA calibration; maciisi note).
  quadra950 also has the SETUPTIMEK stuff-TimeDBRA hack for free-run.
- **Leave PRAM invalid** (all zero) at cold start; the ROM rebuilds it
  (LC475 finding: seeding it wrong causes reboot loops).
- Unmapped I/O must BUS ERROR (MEMTX_DECODE_ERROR), not read 0 —
  machine identification probes depend on it.
- A31-half physical decode -> low 24 bits (24-bit tagged master
  pointers), as on maciisi.
- RAM container: unbacked reads return 0 for the RAM sizing probes.
- gdb: `set endian big` FIRST.  -d mmu "txn fail" lines to a file for
  fatal fault hunting.  HMP/gdb phys reads bypass the 030 MMU.

## Iteration log

### Session 1 (2026-08-26): research + skeleton

- Collected all chipset facts above; baseline build of the consolidated
  tree OK in /tmp/iifx-build.
- Wrote hw/m68k/maciifx.c: 030 CPU, RAM/ramio, ROM at 0x40800000
  (+alias at 0x40000000), 1MB I/O slice mirrored A20-A23, VIA1 (+RTC
  bit-bang, +0x40000 mirror), OSS interrupt controller, both IOPs
  (model lifted from quadra950.c), ESCC in the SCC-IOP bypass window,
  SCSIDMA (ncr5380 + handshake windows + control regs), ASC, SWIM,
  NuBus bridge + macfb slot 9, ADB kbd/mouse behind the ISM IOP.

### Session 1 (cont.): boots MacOS 7.5.3 to an INTERACTIVE FINDER

MILESTONE: `-M maciifx` boots the 4147DD77 ROM through POST, plays the
boot chime, adopts a Radius PrecisionColor 24Xp NuBus card as the boot
display, loads System 7.5.3 from the 5380/SCSIDMA disk and reaches a
fully rendered, mouse+keyboard-interactive Finder desktop (menu bar,
ticking clock, windows, Apple menu opens on click).  Screenshots:
/tmp/iifx-menu.png (Apple menu open), /tmp/iifx-shotC.png (desktop).

Runbook (Finder in ~5 min wallclock):

    /tmp/iifx-build/qemu-system-m68k -M maciifx \
      -bios /workspace/files/mac-roms/macIIfx.rom \
      -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
      -snapshot \
      -device radius-24xp,slot=0xe,romfile=/workspace/files/radius-24xp-lane3.bus \
      -serial null -serial null -display none -icount shift=7 \
      -monitor unix:/tmp/iifx.sock,server=on,wait=off \
      -qmp unix:/tmp/iifx-qmp.sock,server=on,wait=off

    (-icount shift=7 required as on the other 030 Macs.  Input via QMP
    input-send-event rel/btn/key; screendump via QMP.)

Findings, in boot order — each was a hard blocker:

1. **Reset vector**: this ROM's dword@4 = 0x4080002A is the ABSOLUTE
   entry address, unlike the IIsi ROM's base-relative 0x2A.  (First run
   executed zeros at 0x8100002A: ram-hole reads, double MMU fault.)
   maciifx.c accepts both forms.  ROM version word @8 = 0x067C — same
   universal-ROM family as the IIsi, so most IIsi lore carries over.
2. **EXP0 (0x50F1C000) must be writable**.  The hw-init sExec at ROM
   0x40802E5C writes a 17-byte 0xF3FF shift pattern to EXP0 under a
   bus-error catcher whose continuation (0x40802E84 -> jmp fp@ ->
   0x40802E46) RETRIES the whole device init: with EXP0 bus-erroring
   the boot livelocks writing OSS rom_ctrl 0x0D + EXP0 0xFF forever.
   Modelled as scratch registers (EXP1 at 0x1E000 stays a bus error —
   its probe, 0x408039E2 st/clr/readback of +0 and +0x10, expects
   absent).  In MAME unmapped writes silently succeed, masking this.
3. **VIA1 T1 one-shot mode** (hw/misc/mos6522.c, shared fix): POST test
   0x87 (VIA timers, ISR at 0x40841EF2 counts OSS-60Hz ticks in d3, IFR
   T1 flags in d4, T2 in d5) requires T1 in ONE-SHOT mode (ACR bit6
   clear) to set IFR6 EXACTLY ONCE per T1CH load (error 12 otherwise).
   mos6522.c's read-path timer catch-up re-raised T1_INT every wrap
   regardless of mode; T1 now latches oneshot_fired like T2 (cleared on
   T1CH write).  Regression-checked: -M maciici still boots 7.5.3 to
   the Finder with this change.
4. **ASC mirrored at +0x1000**: the ASC POST (test 0x89, 0x40842026)
   does its cleanup writes at ASC+0x1001.. (a0 already advanced +0x800,
   offsets 0x801..) — the IIfx decodes the ASC 4K-mirrored in its 8K
   window.  A bus error there vectors into the ROM **death nub** via
   the VBR stub table at 0x40840EF0 (stubs OR 0x01xx-0x20xx into d7,
   d6 = old SP, jmp 0x4084315A: re-ID, error chime via 0x40807058,
   then the '*'-prompt getchar loop 0x40843F96 polling the SCC — the
   IIfx "sad mac" is chime + serial nub since there is no video).
   Debug recipe: the POST sequencer at 0x40841320 walks the test table
   at 0x408414AA (id word + routine offset from table base; ids
   0x84-0x93); per-test failures go through the tolerant recorder
   0x4084323E (d6 = -1 means SKIPPED, not failed); breakpointing
   0x4084315A and reading d6/d7 (d7.w = test id | vector code) plus
   the exception-stub d6=SP convention pinpoints any nub entry.
5. **No onboard video declaration ROM**: the IIfx ROM has no video
   driver at all; QEMU's macfb NuBus card has no DeclROM so the Slot
   Manager never sees it (slot scan traced via -trace 'nubus*': slots
   9-D probe as empty, slot E found only once a real DeclROM exists).
   Used the tree's Radius PrecisionColor 24Xp model.  Its dump
   (/workspace/files/RadiusPrecisionColor24XPv2.0.bin) is board-
   inverted: true[k] = dump[~k & 0x7FFF] ^ 0xFF; the card is a
   byte-lane-3 device, so the romfile must be the 128K bus image with
   the ROM byte at every offset 4k+3 (0xFF elsewhere) — generated at
   /workspace/files/radius-24xp-lane3.bus (byteLanes byte 0x78 lands at
   the top of slot space; Apple test pattern 0x5A932BC7 verifies).
   With it, PrimaryInit runs (monitor sense, CRTC, CLUT), the ROM
   draws the happy mac and MacOS runs the card's video driver.
6. **ADB-over-IOP quirks** (all vs. the quadra950.c engine):
   - A Listen reply with olen==0 must NOT carry ADB_IOP_TIMEOUT: the
     ROM's ADBReInit collision dance treats that as "no device" and
     loops.  Only negative adb_request returns are timeouts.
   - The OS-era RAM driver sends mailbox messages with flags 0x00 (not
     0x80/EXPLICIT); they must be serviced identically or the ADB
     manager wedges resending.
   - **Data-less Listens must not reach the QEMU ADB devices**: the
     ROM/OS sends Listen R3 with count=0 as a no-op probe, and QEMU's
     adb-kbd/mouse parse the (absent) first data byte as a new device
     address — the keyboard ended up parked at address 0 (autopoll
     Talk R0 tagged 0x0C) and input was dead.  Answer ok, skip the bus.
   - Apple's ISM IOP autopolls autonomously: MacOS never sends a
     Linux-style SET_AUTOPOLL and never host-polls Talk R0 (the 50
     boot-time ADBReInit sweeps end and the channel goes quiet).  The
     machine registers an adb autopoll callback (MUST be registered:
     a guest SET_AUTOPOLL otherwise arms the bus timer with a NULL cb
     -> host segfault on first input) and posts AUTOPOLL messages on
     mailbox chan 2 when the recv slot is idle.
   - Autopoll data must carry flags = ADB_IOP_AUTOPOLL with bit7
     (EXPLICIT) CLEAR: the OS listener dispatches on bit7 and treats
     bit7-set messages as completions of commands it never issued —
     delivering mouse data that way bombed the Finder (error type 10).
7. OSS model notes that proved right: per-source level regs at +0..0xF
   (write 0-7, ROM programs 22222214/2201 = NuBus 2, ISM 1, SCC 4,
   sound 2, SCSI 2, 60Hz 0, VIA1 1), pending word at +0x202/0x203
   read as a WORD by the ROM's VIA-timer POST ISR (bit 10 = 60Hz),
   +0x207 write acks the latched 60Hz, +0x204 rom_ctrl (0x0D written
   at hw-init).  IPL = max(level[src]) over pending sources,
   autovectored.

Open items / polish for later sessions:
- Sound: ASC plays the boot chime through the OSS (source 8 level 2).
- Floppy: SWIM sits behind the ISM IOP chan 1 mailbox — not serviced
  (no floppy boot); SCSI is the boot path.
- SCSIDMA true-DMA mode (A/UX) unimplemented; MacOS never uses it.
- The '-2 vs -snapshot' disk sharing rules and never-pkill etiquette
  as in MACIISI-NOTES apply (kill by /tmp/iifx.pid only).
- radius24xp register-access logging is chatty on stderr at the
  Finder (~MB/s); redirect stderr when running long sessions.
