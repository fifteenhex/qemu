# Classic 68030 Macs (IIci / Classic II / SE30) — journal

Goal: add QEMU machines for three 68030 Macs, reusing the maciisi
(RBV/Egret/030) infrastructure: **Mac IIci** (RBV + classic VIA-ADB),
**Mac Classic II** (Egret + fixed 512x342 fb), **Mac SE/30** (classic
VIA-ADB + GLUE + fixed fb).  Working tree `/workspace/src/qemu-mac2`,
branch `mac-q630`, incremental build in `build/`.

ROMs (in /workspace/files/mac-roms/):
- `macIIci.rom`    512K, checksum 368CADFE, version word **$067C**
- `macclassic2.rom` 512K, checksum 3193670E, version word **$067C**
- `macIIx.rom`     256K, checksum 97221136, version $0178 (II/IIx/SE30;
  reset vector is an ABSOLUTE address 0x4080002A, unlike the $067C ROMs'
  base-relative 0x2A)

## ROM analysis: the $067C universal tables (2026-08-25)

Big find: the IIci and Classic II ROMs are the SAME "$067C universal"
family as the IIsi ROM that MACIISI-NOTES.md reverse-engineered — same
identification machinery: decoder-probe list at ROM+0x32b4, machine
entry list at ROM+0x32c8 (rel-offset longs, terminated by 0), machine
entries 64 bytes; accept test at ROM+0x2f8c: `(d1 & entry[32]) ==
entry[36]` with d1's top byte = VIA1 port A pins (read with DDRA
input); entry+19 = decoder kind.

Machine entries parsed straight out of the ROM images:
- macIIci.rom kind-5 (RBV) entries: mask 0x56 with match **0x46**
  (the IIci: PA4 low, PA1/PA2/PA6 high — the SAME straps the IIsi ROM
  uses for the IIsi, so pins-a = 0xEF carries over), match 0x56
  (sibling/test config) and match 0x54; kind-4 entries (Mac II class
  VIA2 machines: II/IIx/IIcx probes) and kind-6 (IIfx/OSS) also present.
- macclassic2.rom = the same base tables PLUS patched-in entries:
  kind-5 match **0x16** at 0x3b66 (entry byte +22 = 04, extra table ptr
  at +28), kind-7 match 0x54 at 0x3ba6, kind-7 match 0x12 at 0x3d14
  (kind 7 = V8/Eagle-style decoders).  Existing kind-5 match-0x16 entry
  at 0x3740 was neutered to match 0x00.  Which of these is the Classic
  II is TBD (bring-up will tell; Egret + kind-5-ish is expected since
  Linux calls CLII via_type MAC_VIA_IICI i.e. RBV-style interrupt regs).

## Mac IIci — hw/m68k/maciici.c

Approach: clone of maciisi.c (kept bit-exact upstream) with the Egret
transport REMOVED and the classic VIA1 ADB transceiver (PB4/PB5 state
lines + PB3 /INT + shift register, ported from hw/misc/mac_via.c q800
model) in its place.  The 343-0042 RTC bit-bang engine stays — on the
IIci it's the real RTC chip rather than the Egret's emulation of it.
ASC type = original ASC (IIsi uses EASC).  Everything else (RBV +
VIA2-site emulation, VDAC, monitor sense 6 in monP bits 3-5, NCR5380 +
pdma/handshake apertures, SWIM, NuBus bridge, VRAM-in-RAM fb scanning
RAM offset 0, A31-half low-24-bit decode, ramio 0-fill container, ROM
at 0x40800000 + alias at 0x40000000) is identical to maciisi.

Machine: `-M maciici`, 68030, default 8MB, `-icount shift=7` required
(TimeDBRA calibration, as on maciisi).

### IIci boot debugged to interactive Finder (2026-08-25, session cont'd)

Picked up the "first boot in progress" IIci and diagnosed the hang with
live gdb (`gdbserver`/`target remote :1234`, `set architecture m68k` +
`set endian big` — the m68k gdbstub replies little-endian-looking bytes
if you forget `set endian big`, which makes every register/PC readout
garbage).  Attaching cold requires the QEMU process to be launched with
`-s -S` — trying to retrofit a stop via the HMP `stop`/`system_reset`/
`gdbserver` dance races the vCPU (each HMP round trip lets it run
another few hundred thousand instructions) and reliably lands you
already inside whatever loop you were trying to catch the *start* of.

Symptom: boot froze forever with SR forced to interrupt level 7 (all
autovectors masked), PC cycling through a tight ~9-instruction ring
(0x40843dce -> 0x408442ce SCC-status check -> 0x40843dd6 -> bmi
0x40844224 -> 0x4084429a -> braw back to 0x40843dce) with d5/d7/a7
bit-for-bit identical across 15 samples spanning 32 real seconds —
confirmed via single-stepped gdb trace, not just polling sample bias.
The loop is the ROM's SCC-status ("wait for a character on the modem/
printer port") poll; MACIISI-NOTES.md's session log already identified
this exact construct as **the ROM debug nub's serial `*`-command
prompt** (0x40849d9c-class loop) that cold reset falls into whenever an
earlier self-test throws a SysError — i.e. this is a symptom, not the
bug.  Grepping our own `-d unimp,guest_errors` log for the actual root
cause turned up:

    maciici io: write +0x15001 (1) <- 0x00000001 -> BERR pc=0x4084510a

0x50f15001 is `ASC_OFS + 0x1001`: inside the ASC device's own 0x2000
window, but past the last populated sub-range (FIFO 0-0x400, regs
0x800-0x860, extregs 0xf00-0xf40) — a real gap in QEMU's `hw/audio/
asc.c` container.  The IIci ROM's ASC bring-up (original discrete ASC,
not IIsi's EASC — different probe code) pokes an extended-register
offset in that gap; our generic "unmapped I/O bus-errors, matching the
real machine's incomplete NuBus/slot decode" catch-all
(`maciici_iotrace_ops`, deliberately BERR-on-purpose so slot-probing
still works) took the bus error literally, SysError'd, and dropped into
the nub.  A real ASC chip fully decodes its own small window — it
doesn't leave holes that bus-fault — so the fix is narrow: in
`maciici_iotrace_read/write`, special-case addresses in
`[ASC_OFS, ASC_OFS+0x2000)` to return `MEMTX_OK`/0 instead of
`MEMTX_DECODE_ERROR`, leaving every *other* unmapped region (NuBus slot
space etc.) bus-erroring exactly as before.  One-line-of-logic fix,
machine-local (`maciici.c` only, nothing shared touched), immediately
unstuck the boot.

Result: **Finder loads interactively** — booted straight through to the
"this computer wasn't shut down properly" dialog (expected with
`-snapshot` + no prior clean shutdown), Return dismissed it, desktop
and a Control Panels window are fully live and redrawing (menu bar
clock visibly ticking 7:52 PM -> 7:56 PM across screenshots while idle).
This is the **best** tier. SCSI HD0 (OpenRetroSCSI 7.5.3) mounted and
browsable.

Not yet exercised this session: opening apps, ADB mouse/keyboard
interaction beyond the Return keypress, sound. The classic VIA1 ADB
transceiver code (ported from q800's mac_via.c, not Egret) that the
previous session already wrote appears to work fine as-is — no changes
were needed there.

Command line used for testing:

    build/qemu-system-m68k -M maciici -bios /workspace/files/mac-roms/macIIci.rom \
      -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
      -snapshot -serial null -serial null -display none -icount shift=7 \
      -monitor unix:/tmp/c030/mon.sock,server=on,wait=off

Committed to `mac-q630`.

## Mac Classic II — hw/m68k/macclassicii.c

Approach: clone of maciici.c (RBV-style VIA2/interrupt register block,
RTC, SCC/ASC/SWIM/SCSI address map -- all unchanged, since Linux calls
the Classic II's via_type MAC_VIA_IICI, same register semantics as the
IIci) with two swaps:

  - ADB: the classic VIA1 transceiver is replaced with the Egret engine
    ported *verbatim* from maciisi.c (Linux's adb_type MAC_ADB_IISI
    covers both the IIsi and Classic II -- same wire protocol). No
    behavioural changes from the maciisi.c version; only identifier
    renames plus the `struct MacClassicIIMachineState *machine`
    back-pointer maciisi.c's version already carried.
  - Video: no RBV video/VDAC and no NuBus (mac_nubus_bridge, the NuBus
    IRQ router, and the two bridge memory windows are all removed).
    Replaced with a FIXED 512x342 1-bit framebuffer scanned from the
    TOP of main RAM at (ram_size - 0x5900), ported from mac128k.c's
    compact-Mac screen model (same formula Apple uses across the
    128K/512K/Plus/SE/Classic/Classic II lineage). The RBV register
    block (IFR/IER/SIFR/SIER) stays mapped at RBV_OFS for VIA2/
    interrupt purposes even though there's no RBV *video* behind it.

Machine: `-M macclassicii`, 68030, default 4MB, `-icount shift=7`.
ROM: macclassic2.rom (3193670E, same "$067C universal" family as the
IIci/IIsi -- see the ROM-analysis section above).

The Classic II ROM carries a patched-in kind-5 machine-entry matching
VIA1 port-A straps `(PA & 0x56) == 0x16` (PA4 HIGH -- notes on the
IIci/IIsi entry call this "a sibling config"; on this ROM the sibling
*is* the Classic II) alongside kind-7 "V8/Eagle-style" entries at
match 0x54 and 0x12 (mask unconfirmed for those).  Modelled straps as
`pins-a = 0xBF` (PA & 0x56 == 0x16).  Also tried `0xFD` (PA & 0x56 ==
0x54, hoping the ROM wants the kind-7 V8/Eagle path instead) — see
blocker below, made no observed difference: the hang happens before
the kind-5-vs-kind-7 decision point.

### Status: stuck in early ROM POST, below the "min" (gray/happy-mac) tier

Verified with `-s -S` + gdb (`set endian big`, single-stepped 6000
instructions from a cold `-S` start, then a further ~3 minutes of free
running): PC parks in the narrow range **0x40803122-0x4080314e**, a
generic (pre machine-ID) hardware self-test that walking-bit-tests a
register at physical **0x50F01C00** (VIA1's own IER, reg 14 in our
`maciici_via1_ops`-style decode: `(0x1C00 >> 9) & 0xf == 14`).

Traced the register values instruction-by-instruction: the test writes
successive negated/doubled patterns to the IER register and reads them
back through two address forms (`a2@(0,d2:l)` and `a2@`, d2 pinned at
0 the whole time); the byte visibly walks `FF, FE, FC, F8, F0, ...`
across single-stepped iterations -- our `mos6522.c` IER set/clear
emulation is behaving exactly like real 6522 silicon here, and the
INNER walking-bit test does complete (there's a clean `beqs
0x40803144` exit once the doubled pattern hits zero, ending in a
`jmp %fp@` return to the caller). The problem is what happens *after*
that return: minutes of continuous (non-single-stepped) execution
never move PC outside this ~0x2C-byte window, meaning whatever calls
this self-test at 0x40803122 gets an answer it doesn't like and
retries forever, structurally the same shape as the IIci's SCC/ASC
detour into the ROM debug nub (MACIISI-NOTES.md: "on failure ... it
sits at a '*'-command serial prompt") but a DIFFERENT trigger -- this
one is pre machine-ID, so it can't yet be the Egret/RBV video code
that differs from the IIci.  Whatever the ROM expects the VIA1 IER
walking-bit test to conclude about the hardware, our answer doesn't
satisfy it, and unlike the IIci's ASC gap this isn't a bus-error
question (no BERR is logged at all here) -- it is a real,
successfully-completing register readback the ROM is unhappy with for
some other reason (possibly it wants a NUL/absent response at this
exact probe address on the Classic II specifically, i.e. VIA1 IER
might not be the intended probe target here and 0x50F01C00 should
bus-error the way it does on some other kind).

Next-session starting points (not yet tried, ran out of session time):
  - Get a slot-marker/kind identifier: single-step from
    `main_cpu_reset` all the way to 0x40803122 (rather than breaking
    directly there) to see what sets up `a2` and what the CALLER does
    with the self-test's return value/flags, to find the actual retry
    condition.
  - Cross-check against the IIci trace: IIci hits the *same* 0x40803124
    momentarily (logged 6 BERRs at a *different* offset, +0x21c00, not
    this one) before moving on -- diff the two ROMs' bytes in this
    function to see whether Classic II takes a materially different
    branch immediately after the walking-bit test that IIci's ROM
    doesn't.
  - Try feeding IER reads that intentionally DON'T echo the written
    pattern (e.g. make 0x50F01C00 answer through the `iotrace` BERR
    catch-all instead of real VIA1, on the theory the ROM wants this
    particular address to come back absent on this machine) as a
    disposable experiment.

Command line used for testing:

    build/qemu-system-m68k -M macclassicii -bios /workspace/files/mac-roms/macclassic2.rom \
      -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
      -snapshot -serial null -serial null -display none -icount shift=7 \
      -monitor unix:/tmp/c030/mon.sock,server=on,wait=off

Committed to `mac-q630` in this state (compiles, registers, boots and
runs without crashing, reaches a well-characterized -- if unresolved
-- early POST loop) so the next session has a clean, documented
starting point rather than nothing.

## Mac SE/30 — hw/m68k/macse30.c

Approach: clone of maciici.c, keeping the classic VIA1 ADB transceiver
UNCHANGED (proven working there -- Linux's adb_type MAC_ADB_II covers
both the IIci and SE/30) and the RTC/SCC/ASC/SWIM/SCSI address map
unchanged, but swapping the RBV-emulated VIA2/video for:

  - A REAL discrete VIA2: TYPE_MOS6522 is abstract, so a trivial
    concrete subclass (`mos6522-macse30-via2`, no board behaviour of
    its own) is instantiated at the classic VIA2 site (slice +0x2000),
    with a thin `macse30_via2_ops` wrapper applying the same
    addr>>9 VIA-register spacing as VIA1, plus NuBus-style pull-ups
    (unconnected input pins read 1) on port A. Its own IRQ output
    feeds GLUE level 2 (renamed `MACSE30_GLUE_VIA2`, was
    `..._GLUE_RBV`).
  - Fixed 512x342 1-bit framebuffer at (ram_size - 0x5900), same
    mac128k.c-derived model as macclassicii.c.
  - No NuBus (mac_nubus_bridge and its IRQ router removed entirely --
    the SE/30 has only the 030 PDS, and the task scope excludes it).

Machine: `-M macse30`, 68030, default 4MB, `-icount shift=7`. ROM:
macIIx.rom (97221136, the shared II/IIx/IIcx/SE30 ROM, version $0178);
its reset vector is an ABSOLUTE address at ROM+4, which the
IIci-derived entry-detection logic already handles (it tries
base-relative first, falls back to absolute).

Machine-ID straps: not yet determined for this ROM (it is a DIFFERENT
ROM family from the $067C IIci/IIsi/Classic II ROMs analysed above --
decoder kind 4, "Mac II class VIA2 machines" per the earlier ROM scan,
whose own machine-entry table hasn't been extracted this session).
Reused the IIci/IIsi 0xEF strap as a placeholder; it hasn't visibly
caused a machine-ID mismatch (see below -- the blocker is later and
unrelated to identification) but should be verified against
macIIx.rom's own tables next session.

### Two bugs found and fixed this session; one blocker remains

1. **VIA2 longword-access storm (FIXED).** The ROM's VIA2 bring-up at
   0x4080071A does `movel a0@(0x1e00),d0` -- a 4-byte read of VIA2
   register 15 (ANH). `macse30_via2_ops` (like `maciici_via1_ops`,
   which it was copied from) originally declared `.valid = {1, 1}`
   with no `.impl` override. For VIA1 this is harmless because real
   Mac ROM/OS code only ever touches VIA registers with byte
   instructions; this ROM's VIA2 bring-up is the first case in this
   codebase of a wider access to a VIA-spaced region, and it made the
   memory core refuse/mis-split the access, which surfaced as a
   full-blown **interrupt storm**: PC oscillated forever between the
   VIA2-init subroutine (0x4080071A) and a ROM-resident spurious/quick
   -exit exception-vector stub (0x40802A12: `rte`), confirmed via
   single-stepped gdb trace showing zero forward progress across 32+
   real seconds and two independent 15-sample windows. Disconnecting
   VIA2's IRQ from GLUE entirely (to rule out a device-side storm)
   made no difference, isolating the cause to the access itself, not
   an IRQ source. Fix: give `macse30_via2_ops` an explicit
   `.valid = {1, 4}` / `.impl = {1, 1}` (guest may access 1-4 bytes,
   callback handles 1 byte at a time, memory core splits) -- this
   alone got the boot dozens of ROM-addresses further (from
   0x408xx7xx into 0x408x6dxx).
2. **SCSI/ASC IRQ routed into the new VIA2 (REVERTED, not root-caused).**
   Initially wired NCR5380 and ASC IRQs into VIA2 CA1/CB1 as a
   best-guess approximation of real hardware pin assignments (unverified
   -- exact wiring wasn't found in the time available). This was
   *not* actually the storm's cause (see above; disconnecting VIA2's
   IRQ entirely didn't change the symptom), but left unwired for this
   session pending verification, same caution as maciici.c's own
   "TODO: route via VIA2 interrupt inputs; direct CPU wiring storms"
   comment for ASC. Next session: reconnect one at a time with the
   longword-access fix already in place and see if either alone causes
   a *real* storm now that the false lead is ruled out.
3. **Current blocker (unresolved):** past the VIA2 fix, PC settles into
   a poll loop at 0x40806DD8-0x40806DDE: `btst #5, a3@(349); bnes
   0x40806dd8`, reached right after `andiw #-1793,%sr` (SR mask forced
   to 0 -- ALL interrupt levels unmasked) and a `bsr 0x40806dea` that
   presumably kicks off whatever sets bit 5. This is a "wait for an
   interrupt handler to clear a busy flag" idiom -- confirmed
   genuinely waiting (SR I:0, not masked) rather than another storm.
   `a3` is a driver/global-table pointer (not identified further this
   session); the earlier code in the same routine pokes offsets 0x400/
   0x1800/0x1c00 off a different pointer loaded from low-memory global
   0x1d4 in a VIA-IER/IFR-like pattern, suggesting this may be ADB,
   SCC, or SWIM bring-up waiting on ITS OWN interrupt to fire and
   never does (plausibly because that specific autovector still points
   at the ROM's generic spurious-interrupt stub rather than a real
   handler at this point in boot, or because the source device -- SWIM
   is a candidate, it has no IRQ wired in this model at all -- never
   asserts).

Next-session starting points:
  - Identify `a3` and the offset-349 flag (disassemble the `bsr
    0x40806dea` target and whatever sets up `a3` before 0x40806D84).
  - Try wiring SWIM's IRQ (currently unconnected, unlike ASC/SCSI
    which are at least stubbed to a sink) into VIA2 or GLUE directly.
  - Reconnect ASC/SCSI IRQs into VIA2 CA1/CB1 now that the longword
    storm is fixed, one at a time, watching for a genuine storm this
    time.
  - Extract macIIx.rom's own decoder-probe/machine-entry tables (same
    method as the $067C ROM analysis above) rather than reusing the
    IIci's 0xEF strap guess.

Command line used for testing:

    build/qemu-system-m68k -M macse30 -bios /workspace/files/mac-roms/macIIx.rom \
      -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
      -snapshot -serial null -serial null -display none -icount shift=7 \
      -monitor unix:/tmp/c030/mon.sock,server=on,wait=off

Committed to `mac-q630` in this state (compiles, registers, boots and
runs without crashing, past the first hard blocker found this
session, into a second well-characterized wait loop) for the next
session to continue.

## Summary across all three machines this session

- **Mac IIci (`-M maciici`)**: root-caused and fixed a boot-blocking
  bus error in the ASC gap (`hw/audio/asc.c`'s discrete-ASC container
  has unmapped register-space holes that a real chip wouldn't
  bus-faulton); reaches a **fully interactive Finder desktop** over
  SCSI. Best tier achieved, matching the task's guidance to prioritize
  this machine.
- **Mac Classic II (`-M macclassicii`)**: boots and runs without
  crashing; stuck in an early, pre-machine-ID ROM POST self-test whose
  caller retries forever (root cause not found this session, precisely
  documented above with next-session leads). Below the "min" tier.
- **Mac SE/30 (`-M macse30`)**: boots and runs without crashing; found
  and fixed a VIA2 access-size bug that was causing a hard interrupt
  storm, then hit a second, later, well-characterized wait-loop
  (documented above). Also below the "min" tier, but with concrete
  further-progress leads and one bug already fixed.

All three machine definitions compile cleanly together; `-M maciici`
and `-M maciisi` were re-verified to still initialize (ROM-not-found
is the only expected error with no ROM given) after each round of
Kconfig/meson.build changes, since those files are shared build
plumbing even though no shared *source* was touched by any of the two
new compact-Mac machines.
