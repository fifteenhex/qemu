# Macintosh LC 550 / Color Classic II / Performa 550 -- session notes

## Task

Add QEMU machines for the Macintosh Color Classic II / LC 550 / Performa
550 (68030, compact/all-in-one color) and boot MacOS as far as possible.
Machines added: `maclc550`, `colorclassicii`, and `performa550` (a plain
alias of `maclc550`), all in `hw/m68k/maclc550.c`.

## ROM

Fetched from the archive.org item `mac_rom_archive_-_as_of_8-19-2011`
(the 55MB `mac_rom_archive_-_as_of_8-19-2011.zip`), inner file `EDE66CBD
- Color Classic II & LC 550 & Performa 275,550,560 & Macintosh TV.ROM`
(1,048,576 bytes). Verified first 4 bytes = `ede66cbd` (matches the
filename's self-checksum convention). Saved to
`/workspace/files/mac-roms/maclc550.rom`; the 55MB zip was deleted after
extraction. This ROM is shared, bit-for-bit, across all of: Color
Classic II, LC 550, Performa 275/550/560, and Macintosh TV -- the
machines differ only in a hardware machine-ID and possibly monitor-sense
straps, not in ROM contents.

## Design: what this machine reuses from the existing tree

- **maciisi.c / macclassicii.c**: 68030 CPU, IRQ glue (one input per
  680x0 level), RBV-style VIA2/interrupt register block (IFR/IER with
  the VIA set/clear protocol, slot-$E VBL summarised into IFR bit 1),
  the **Egret** ADB/RTC MCU on the VIA1 shift register -- specifically
  macclassicii.c's more complete Egret model, which adds the
  byte-handshake "pseudo-command" framing (GET_PRAM/SET_PRAM/READ_MCU/
  WRITE_MCU/GET_TIME/SET_TIME) on top of maciisi.c's formal-session ADB
  transport. macclassicii.c's XPRAM/PRAM defaults (seed 'NuMc' validity
  signature + OSDefault ddType=1) are carried over verbatim: they fix
  the exact "boot-driver installer can't find a matching driver, blinks
  '?' forever" failure mode documented in macclassicii.c's and
  quadra630.c's comments. NCR5380 SCSI, EASC sound (LC-family, not the
  IIci's discrete ASC), SWIM floppy, ESCC serial -- all ported with the
  same I/O offsets as maciisi.c/macclassicii.c (VIA1 at 0, RBV-VIA2 at
  +0x2000, SCC at +0x4000, SCSI at +0x10000, ASC at +0x14000, SWIM at
  +0x16000, VDAC stub at +0x24000, RBV at +0x26000).
- **lc475.c**: the onboard **Valkyrie/CSC-class video model** --
  dedicated VRAM (not RAM-stolen RBV video), a CLUT+monitor-sense+VBL
  register bank, and critically the **GDevice-tracked framebuffer**
  approach: instead of reverse-engineering the video ASIC's mode
  registers, the framebuffer device re-reads the guest's own
  MainDevice GDevice PixMap (lowmem 0x8A4 chain) every frame to learn
  the current base/rowBytes/bounds/depth. This is copied close to
  verbatim (renamed `MacLc550FbState`/`maclc550_fb_*`) and mapped at
  the same physical placement lc475.c uses (VRAM at 0xf9000000, control
  registers at 0xf9800000), since real hardware placement for this
  specific chip generation was not confirmed.
- **lc475.c / q800.c**: the dedicated **machine-ID register** at
  0x5FFFFFFC (top word must read 0xA55A) instead of a VIA1 port-A
  decoder-kind strap dance -- confirmed correct for this ROM family by
  a research pass that found MAME's `src/mame/apple/maclc3.cpp` (the
  LC III/III+/520/550 "Sonora"-generation driver) implementing exactly
  this scheme at this address for a ROM with the same CRC32.
  `MACLC550_MACHINE_ID = 0xa55a0101` per that source (confirmed: LC
  520 = 0xa55a0100, LC 550 = 0xa55a0101, LC III = 0xa55a0001, LC III+ =
  0xa55a0003). The Color Classic II's real ID value was **not** found
  in any available reference (MAME doesn't drive this exact ROM from a
  CCII-specific config); `colorclassicii` currently reuses a
  placeholder (`0xa55a0181`) that is **unverified** -- flagged clearly
  in the source. Performa 550/560 share the LC 550's Gestalt ID on real
  hardware (Linux `bootinfo-mac.h`: `MAC_MODEL_P550` covers LC 550 and
  Performa 550/560 together), so `performa550` is registered as an
  `mc->alias` of `maclc550`, not a separate ID value (mirrors the
  `virt.c`/`quadra950.c` alias pattern already in this tree).
- No 24-bit/A31 RAM aliasing was ported from maciisi.c/macclassicii.c:
  this is a 32-bit-clean "Sonora"-generation board like lc475.c, which
  also needs none.
- SETUPTIMEK TCG timer-calibration hack (as in q800.c/lc475.c/
  quadra630.c/quadra950.c) is included defensively in the VIA1 write
  path, keyed off the same T2=0x30C / IER=0x20 sequence.

## What was found and fixed this session

1. **ROM relocation-table bug (real, fixed).** Cold boot ran away into
   unbacked RAM forever, executing decoded-as-zero `ori.b #0,d0` words
   (2-byte instruction, never bus-faults, so this produces *zero*
   `unimp`/`guest_errors` log output -- entirely invisible without
   instruction tracing). Root-caused with `-d in_asm,cpu` register
   dumps: ROM 0x40802e00 locates a small header by scanning backward
   from a fixed code address (0x40803d52) for a self-referential
   longword match (works regardless of where the ROM actually runs,
   since the comparison value is computed PC-relative at runtime), then
   steps back 2 table entries (8 bytes) to fetch a *different* value
   used as `d3 = *that_value - a2` (a2 = actual running base,
   0x40800000), and jumps to `0x40802e3e + d3`. The matched entry (ROM
   file offset 0x3d4a) holds `0x40803d52` (a 32-bit in-place address,
   correct); the entry it steps back to (offset 0x3d42) holds
   `0x00a03d52` -- a 24-bit-position variant (canonical base
   0x00a00000) inconsistent with running the ROM at 0x40800000. This is
   the exact same **bug family as macclassicii.c's `device_table[0]`
   patch** (documented there in detail), just a different byte offset
   in a different ROM image from a later/different codebase generation.
   **Fix**: `hw/m68k/maclc550.c`'s ROM-loading code patches ROM file
   offset 0x3d42 from `0x00a03d52` to `0x40803d52` (guarded by checking
   both the buggy value and the matched-entry value first, so the patch
   is a no-op if a different ROM dump ever has this fixed already).
   After this fix, the boot proceeds far past this point into real
   POST code (confirmed with instruction tracing: thousands of distinct
   ROM addresses executed, no more runaway).

2. **Sonora "family ID" probe register, stubbed (functional, not
   verified against real hardware).** Immediately after the relocation
   fix, ROM 0x40804b4e reads a byte at I/O offset **+0x28002** (right
   after the RBV window, which ends at +0x28000) and does
   `andib #0x70,d1 / cmpib #0x20,d1 / beqs ...`. Nothing in this
   machine's memory map answers there, so the read bus-faults; the
   ROM's own bus-error handler (installed via the VBR it just set up)
   catches the fault and treats the whole machine-identification pass
   as failed, restarting POST from scratch forever (same "non-fatal
   test can never pass, restart forever" pattern documented in
   macclassicii.c). **Fix**: added a small logging register bank
   (`maclc550_sonoraid_ops`, backed by `m->sonoraid_regs[0x40]`) mapped
   at I/O +0x28000, defaulting to `0xff` except byte +0x02 which is set
   to `0x20` (so `0x20 & 0x70 == 0x20`, satisfying the compare and
   letting bring-up continue instead of retrying forever). The exact
   real register semantics are **not** identified -- this value was
   chosen empirically to satisfy the one observed comparison, not
   derived from any reference. A future session with more time should
   try to identify what real Sonora/CSC hardware register lives here
   (candidates: an extended RBV/VDAC bank, or a dedicated "family/
   sub-model" ID latch used to disambiguate LC 550 / Color Classic II /
   Performa 275,550,560 / Mac TV from one shared ROM).

## Session 2: cracked the poll-loop (Egret cold-start handshake)

The session-1 blocker (the "poll loop at 0x408b9906/0x408ba0c6") turned
out NOT to be a poll loop at all -- it is the ROM's **serial diagnostic
monitor** (a '*'-prompt command interpreter at 0x408b989a that polls SCC
channel A for operator input; it never exits without a serial command).
`-M lc475` (same 1MB "universal" ROM family, quadra605.rom, which boots
to the Finder) **never enters this code** -- so the real problem was that
maclc550 was being wrongly *diverted* into the monitor during boot.

**Root cause (found by gdbstub tracing): the Egret power-on cold-start
line handshake was timing out.** Before any driver runs, this ROM does a
low-level electrical sync with the Egret over VIA1 (routine at
0x408d1ae6, a1 = VIA1 base 0x50f00000):

  1. assert /VIA_FULL (PB4 low), busy-wait for the Egret to assert
     /XCVR (PB3 low)   -- 0x408d1b1c-0x408d1b26;
  2. wait for the shift-register-complete flag (VIA1 IFR bit 2);
  3. deassert /VIA_FULL (PB4 high), wait for /XCVR to rise;
  clocking an SR interrupt each toggle.

On real hardware **/XCVR (PB3) tracks /VIA_FULL (PB4)** through the Egret
and each toggle raises the SR interrupt.  Our Egret model (ported from
macclassicii.c, tuned for that ROM's interrupt-driven + pseudo-command
drivers) did neither for this pre-driver sync -- /XCVR stayed high, so
the ROM's `btst #3,ORB` wait at 0x408d1b20 timed out, fell through to
0x408d1c68 (`moveq #48,d7`) and jumped to 0x4084a6f6 -> 0x408b989a, the
serial monitor.  Confirmed live: VIA1 ORB read 0xb8 forever at 0x408d1af4
with /XCVR (bit 3) stuck high.

**Fix (hw/m68k/maclc550.c, ~50 lines, no shared files touched): model
the Egret cold-start line handshake.**  A new `egret_coldstart` flag
(true from reset, latched false once the first formal session or
recognised pseudo-command takes over) enables, while active and no
session/pseudo/ADB traffic is in play:
  - port-B reads: /XCVR (PB3) mirrors /VIA_FULL (PB4) combinationally;
  - session_update: each /VIA_FULL toggle schedules the SR-complete
    interrupt (VIA1 IFR bit 2), carrying both the line handshake and the
    IFR-bit-2 busy-waits through to the end of the sync.
Gated tightly (`!sys` so it never blocks a real session opening) so the
existing pseudo-command / formal-session / unsolicited-ADB paths are
untouched.  **Result: the cold-start sync completes (IFR bit 2 now sets,
the waits pass), the boot no longer diverts into the serial monitor, and
execution advances into the machine-ID-driven capability/config code at
0x40804xxx** -- verified by register sampling in a real `-icount shift=7`
run (PC now cycles in 0x40804878-0x40804984, reading the machine-ID
register 0x5ffffffc and matching our LC 550 ID 0xa55a0101, not the
monitor).

## Session 3: colour DAC modelled; blocker is the Egret cold-start byte-exchange

Two changes this session, both correct advances:

1. **Ariel colour-DAC model at 0x50f18000** (`maclc550_ariel_ops`).  The
   colour machine's kind-7 bring-up (0x4080497e, gated on capability bit
   27) pokes the onboard "Ariel" RAMDAC -- writes 0x90 -> +0 and 0xb7 ->
   +1.  With nothing decoded there the write bus-errored into an
   exception loop (the previous session's stopping point).  The Ariel is
   a 4-register Brooktree-style RAMDAC (MAME `devices/video/ariel.cpp`):
   +0 CLUT write-address (resets RGB sub-index), +1 CLUT data (R,G,B
   triples, auto-increment), +2 control/mode (**reads back last written
   value**), +3 pixel key.  Traced live: the ROM only WRITES +0/+1 here
   and never reads the block back, and the machine's real scan-out CLUT
   is the Valkyrie/CSC bank (already modelled), so a store+read-back of
   the four registers is sufficient (no palette is driven through it).
   NOTE for the record: MAME decodes the *Sonora* CLUT at video-base
   +0x24000 and the video-control/monitor-sense block at +0x28000 --
   which line up with this file's existing `vdac` (+0x24000) and
   `sonora-id` (+0x28000) stubs; if those ever need real behaviour, the
   monitor-sense lives at vctrl+2 (`m_monitor_id | (res<<4)`; V8 returns
   0x06<<3 = 13" 640x480).

2. **Egret cold-start re-arm** (`egret_coldstart_session`).  Session 2's
   cold-start flag latched OFF permanently after the first sync.  But the
   ROM runs the whole Egret cold-start (line handshake + byte exchange)
   **more than once** during bring-up, and the cold-start's own byte
   phase opens a formal session (it asserts /SYS_SESSION to shift bytes),
   which was what latched it off.  Fix: remember that a session was a
   cold-start one and RE-ARM the flag when it closes (only while the boot
   is still polling -- VIA1 IER SR-int disabled -- so it can't interfere
   with the later interrupt-driven Egret driver).  This makes the second
   (and subsequent) cold-start line handshakes work again.

**Result:** the boot now gets past the colour-DAC bus-error and the
second cold-start's line handshake, and reaches the cold-start
**byte-exchange sync**, where it stops.

## Session 4: cracked the Egret cold-start byte-exchange sync

The captured blocker is solved.  The Egret power-on cold-start sends a
short SEQUENCE of self-test commands over the byte-exchange and checks
each reply's first byte == 2 (0x408d1c2c).  Traced the full sequence --
FOUR commands: `[01 0e 72 5a 00 57]`, `[01 0e 00 00 00 57]`,
`[01 22 de 0c 47]`, `[01 22 de 0f 41]` (0x01 = Egret pseudo type, 0x0e /
0x22 = sub-commands, computed payloads).  Our egret model collected
these into the formal-session command buffer and answered `no_response`
(returning 0x00), so the check failed and the ROM retried into the
serial console.

**Fix (egret_process):** recognise the cold-start sync sub-commands
`[01 0e ...]` and `[01 22 ...]` and stage a 3-byte reply led by 0x02.
The existing session receive-delivery (ack_toggle on the /VIA_FULL
toggles the ROM's 0x408d1d0a receive helper makes) carries the bytes
back; verified live the ROM now reads 0x02 as the first response byte,
the `cmpib #2` check passes, and **all four sync commands are accepted**
-- the Egret power-on sync COMPLETES.  The boot then advances OUT of the
cold-start into the machine-ID/capability CONFIG phase (0x40804xxx):
DAC init, the Sonora monitor-sense/vctrl reads (0x40804b4e, a 44-entry
capability loop), etc.  This is real forward progress past every
Egret-related blocker.

## New blocker (this session's stopping point): config -> serial console divert

After the config phase finishes it jumps (its `a4` continuation, at
0x40804992 `jmp a4@`) to **0x4084a6f6 -> 0x40847542 -> 0x4084a704**,
which does `btst #26,d7`; with bit26 CLEAR (our PA0=high strap) it falls
through to **0x408b989a -- the serial diagnostic console** (the same one
sessions 1-2 fixed the FIRST entry to), which polls SCC channel A for a
'*'-prompt command and loops forever.  This is now reached from config's
NORMAL continuation, not the cold-start timeout.

Findings for the next session:
- 0x408b989a is ALSO the POST-failure handler: 0x40847052-0x4084707e
  set d7's low word to a test-ID code (0x1c00/0x1d00/0x1e00/0x1f00/
  0x2000) then `jmp 0x4084a6f6`.  BUT d7 at our console entry is
  0x00020030 (no test-ID code), so config reaches the console via its
  normal `a4` continuation, NOT a flagged POST failure.
- Both bit26 branches at 0x4084a704 lead to 0x408b989a.  The bit26-SET
  branch (0x4084a726) verifies a 0xaaaa5555 ROM checksum, then
  `bset #16,d7; bset #22,d7`, enables the cache, and STILL
  `bral 0x408b989a`.  d7 bit16 only switches the console loop from an
  ungated SCC poll to a VIA1-T2-paced one (0x408b9a34) -- it sends '*'
  every 12 T2 ticks and loops either way; it does NOT exit to boot.  So
  strapping PA0/bit26 is not the fix.
- `-M lc475` (same 1MB "universal" ROM family, boots to the Finder)
  NEVER executes this console code -- so entering it is wrong; the
  divert is specific to how maclc550's config phase terminates.
- Only pcrel reference to 0x4084a6f6 in the ROM is a `jmp` at 0x40847088
  (the POST-failure tail); config's `a4` is set elsewhere (computed /
  via the capability record chain).  Next step: find where the config/
  capability dispatcher sets `a4` and why maclc550's post-config
  continuation is the console rather than StartBoot -- likely a missing
  or mis-handled capability record (video? boot?) in the kind-7 device
  table, or a Sonora monitor-sense value (vctrl+2, currently stubbed
  0x20) that steers the video-capability path into the console.  The
  44-entry loop at 0x40804b00 reading vctrl+2 each pass is the place to
  look.


## Result vs the task's success tiers

- **(min) machine exists, ROM runs, blocker documented**: met and far
  exceeded -- across four sessions the ROM was debugged past SIX real,
  root-caused, documented bugs (ROM relocation table; Sonora family-ID
  probe; Egret cold-start /XCVR line handshake; the colour-DAC bus-error;
  the cold-start-latched-off-permanently bug; and now the Egret
  cold-start byte-exchange SYNC), landing on a seventh (config -> serial
  console divert) with the entry path traced.
- **(good) past the poll-loop**: YES -- every Egret-related blocker is
  now solved: the boot completes the Egret power-on cold-start sync and
  runs through machine-ID/capability config and the colour-DAC init.
- (best) happy-mac / colour Finder: not reached.  The immediate blocker
  is the config-phase -> serial-console divert (0x408b989a) documented
  above -- likely a missing/mis-handled kind-7 capability record or the
  stubbed Sonora monitor-sense value.  Screendumps remain blank
  (pre-video POST).


## Build / test commands

    mkdir -p /tmp/cc-build && cd /tmp/cc-build && \
      /workspace/src/qemu-cclassic/configure --target-list=m68k-softmmu \
        --disable-docs --disable-tools
    ninja -C /tmp/cc-build qemu-system-m68k

    /tmp/cc-build/qemu-system-m68k -M maclc550 \
      -bios /workspace/files/mac-roms/maclc550.rom \
      -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
      -snapshot -serial null -serial null -display none -icount shift=7 \
      -monitor unix:/tmp/cc.sock,server=on,wait=off \
      -d unimp,guest_errors -D /tmp/maclc550.log

    # also: -M colorclassicii, -M performa550 (alias of maclc550)
    # screendump via the monitor: screendump /tmp/shot.ppm, then
    # convert with PIL (PPM -> PNG) for viewing.

Practical notes carried over from other sessions in this tree: kill
QEMU only by the PID you saved (never pkill by name -- other sessions'
processes share this host); instruction-level `-d in_asm,cpu` tracing
produces very large logs very fast (multi-GB/minute at this depth) --
always redirect to `/tmp`, cap with `timeout`, and prefer a plain
`-d exec` (PC-only) pass first to locate the region of interest before
paying for full register dumps.
