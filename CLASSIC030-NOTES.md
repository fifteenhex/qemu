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

### Root cause found and fixed: missing VDAC blocked decoder-kind ID (2026-08-25, branch `fix-classicii`)

Picked this up in worktree `/workspace/src/qemu-clie` (branch `fix-classicii`,
based on `amiga` HEAD). Re-verified the hang exactly as documented above,
then went past the "inner test completes cleanly" observation the previous
session stopped at, using `-s -S` + a scripted gdb-python trace (breakpoints
on the handful of addresses the walking-bit self-test's *callers* live at,
auto-continuing and logging registers on each hit) instead of single-stepping
blind. This is fast to redo: `gdb-multiarch -q -batch -x script.py` with a
`gdb.Breakpoint` subclass whose `stop()` logs regs and returns `False`.

**The mechanism** (this ROM's machine-ID protocol, shared "$067C universal"
code with the IIci/IIsi — cf. `MACIISI-NOTES.md`'s "Machine identification
reverse-engineered" section, which already reverse-engineered most of this
for the IIsi and was the key that unlocked it here):

- `ROM+0x32b4` is a **decoder-probe list** (2 entries found: kind 4 stub at
  `0x40803064` wants a VIA1-IER mirror at physical `+0x20000`; kind 5 stub at
  `0x40803082` wants a mirror at `+0x40000`, **no** mirror at `+0x20000`, AND
  — the part that bit us — **`tstb` of RBV (`+0x26000`) and VDAC (`+0x24000`)
  must both respond**, addresses read out of the kind-5 device table at ROM
  `0x4080348c` (offsets +52/+56). This table is IDENTICAL between the IIci
  and Classic II ROMs (same `0x4080348c` address, verified byte-for-byte).
- If NEITHER decoder probe succeeds, the "current kind" byte (compared via
  `cmpb a1@(19),d2` at `0x40802f84`) stays **0**, and the machine-entry table
  scan (`ROM+0x32c8`, 64-byte entries) matches a **kind-0 catch-all entry**
  (found at runtime address `0x40803900`, mask=0/match=0 — i.e. it accepts
  *any* strap value) whose declared capability word (`entry@(-20)` relative
  to a shared kind-dispatch table, not the entry itself) is **all zero**.
  Every one of that word's bits is individually probed-and-conditionally-set
  by the capability-dispatch routine at `0x40802f98`-`0x40803044`, but **bit
  0 is never touched by any of them** — it can only come from the ROM's own
  declared word, which is 0 for this entry. Bit 0 is exactly the bit tested
  at `0x40802e2a` (`btst #0,d0; beqs 0x40802e20`) to decide "identification
  successful, proceed to hardware bring-up" vs "retry". With bit 0
  permanently 0, `0x40802e20` unconditionally resets the search kind to 0
  and rescans — matching the *same* kind-0 entry again, forever. This is a
  **structurally infinite, deterministic loop**: confirmed with a persistent
  gdb-python trace (`Breakpoint.stop()` logging + auto-continue) showing
  10000+ iterations with byte-for-byte identical register state every single
  time (a0/a1/a2/d0/d1/d2/fp all repeat exactly).
- Our own earlier ROM-table analysis (top of this file) had already found a
  **`0x40803b66` kind-5 entry patched into the Classic II ROM specifically**,
  mask 0x56 match 0x16 — i.e. Apple's ROM engineers *do* have a real,
  presumably-capable Classic II identification entry, it's just gated behind
  successfully resolving **kind 5**, which our machine could never reach
  because it had no VDAC device at all (deliberately omitted: "no RBV video,
  no VDAC" — real, since Classic II has no RBV video hardware) causing the
  `tstb` of `+0x24000` to bus-fault, failing the kind-5 decoder probe outright
  and stranding identification at kind 0.

**Fix** (`hw/m68k/macclassicii.c` only): ported the IIci's minimal VDAC
stub (`maciici_vdac_ops`/`vdac_regs[0x40]`: plain store + log, no real DAC
behaviour) onto the Classic II at the same `VDAC_OFS` (`0x24000`) the
`#define` already reserved but never mapped. This doesn't add any video
functionality (the fixed framebuffer is untouched) — it just makes the
address *present but inert* so the decoder-kind probe's `tstb` doesn't
bus-fault. New struct fields: `MemoryRegion vdacmem`, `uint8_t
vdac_regs[0x40]`.

**Verified fixed**: with a fresh `-s -S` boot, the walking-bit self-test
(`0x40803122`) is no longer hit in an infinite loop — a breakpoint at
`0x40802e2a` (the identification-success/retry fork) now hits with **`d0 =
0x0000773f`** (bit 0 set!), and `a1 = 0x40803b66`-relative (confirmed via
register dump showing `a1=0x3b66` in a later stopped-instruction sample) —
i.e. it resolves the **actual Classic-II-specific kind-5 ROM entry**, not
the kind-0 catch-all, exactly as hypothesized. `d0=0x773f` is the same
capability word value the working IIci boot resolves to for its own entry
(cross-checked with an identical gdb trace against `-M maciici`). The debug
log (`-d unimp,guest_errors`) now shows the probes succeeding in sequence:
`macclassicii vdac: read +0x00 -> 0x00`, `macclassicii rbv: read +0x00 ->
0x00`, then real VIA2/RBV register bring-up (`rbv-via2: read/write reg0-15`)
— genuine forward progress past the old blocker, into real hardware init.

### Second bug found and fixed: ROM relocation-base slot; RAM-alias experiment REVERTED (2026-08-25, session cont'd)

**The "runaway past top of RAM" from the previous note was NOT a RAM-sizing
bug — it was a bogus code-relocation delta.** Root-caused with a scripted
gdb-python trace (breakpoints on the post-identification bring-up addresses,
auto-continue + register logging; single-stepping the ~15 instructions from
the `0x40802e2a` identification-success fork):

- After machine ID succeeds (`btst #0,d0` at `0x40802e2a` taken), the shared
  "$067C universal" bring-up computes a **relocation delta** and rebases
  a0/a1/a4 and PC by it, so a ROM running at a non-canonical physical base
  can transparently continue executing.  The **IIci** does this inline
  (`0x40802e34`: `movel a0@,d3 ; subl a2,d3`) reading `device_table[0]` =
  `0x40800000`, with `a2` = the PC-relative actual ROM base = `0x40800000`,
  so the delta is **0** (no relocation, keep running from ROM) — and the
  IIci boots straight through to Finder.
- The **Classic II ROM PATCHES that site**: the 4 bytes `26 10 96 8a` become
  `4e fa 0c 60` (`jmp 0x40803a96`), a routine that instead reads the base
  from `device_table[28]` (offset `0x70`) and *also* rebases a0/a1.  But
  `device_table[28]` is **0** in this ROM image (the table's live entries
  stop at offset ~0x44; 0x70 is zero padding), so the delta becomes
  `-0x40800000` and bring-up `jmp`s to `0x40800000 + 0x2e3e - 0x40800000 =
  0x00002e3e` — empty low RAM — then PC "climbs through zeros" (each `0x0000`
  word = `ori.b #0,d0`) exactly as the previous note observed.  The overrun
  was the *symptom*, this bad delta the *cause*.
- **Verified**: forcing the delta `d3=0` at runtime via gdb advances the boot
  ~0x14000 bytes further into real ROM code (to `0x40814cc8`), confirming
  `d3=0` (the IIci's natural value) is correct.  `device_table[28]` plainly
  wants the canonical ROM base `0x40800000` (so the relocation is the no-op
  it is on the IIci); this dump just leaves that slot zero.

**Fix** (`hw/m68k/macclassicii.c`, in `macclassicii_machine_init` after the
reset-vector setup): patch the in-memory ROM image so `device_table[28]`
(ROM file offset `0x34fc`) holds `0x40800000` via `stl_be_p(ptr + 0x34fc,
MACCLASSICII_ROM_ADDR)` on the `rom_ptr` blob (survives reset re-copy).
Machine-local, touches one padding slot.  *Open question for a future
session:* why real Classic II hardware doesn't hit this — either the dump
differs from shipped silicon, or there's an unmodelled step that fills that
slot; but the no-op-relocation behaviour is unambiguously what the code
wants for an in-place ROM, and the patch is the minimal way to get it.

**The previous session's RAM-alias experiment was REVERTED.** That change
(making `ramio_read`/`_write` mirror `addr % ram_size` onto real RAM) was a
wrong turn: the **working `-M maciici` uses the *identical* flat-"reads
return 0, writes discarded" `ramio_ops`** (diffed the two files — byte-for-
byte the same RAM/ramio/`ramio_a31` setup) and boots to Finder, so flat-zero
IS the correct, proven model.  The `sp=0xfffd1348` "stack wraparound" the
previous note worried about was an *artifact of the alias change itself*
(the bogus relocation ran different code against aliased memory); with the
alias reverted and the relocation delta fixed, `sp` stays a healthy
`0x00002600` throughout.  `ramio` is now back to the maciici-identical
flat-zero form.

### New frontier: Egret MCU cold-start sync handshake (precisely characterized, unresolved)

With both fixes in, boot now runs cleanly through machine ID → relocation →
VIA2/RBV bring-up → into **Egret ADB/system-MCU startup communication**, and
parks in a tight loop at **`0x40814cc8`–`0x40814ea2`** (a1 = VIA1 =
`0x50f00000`).  Fully decoded:

- `a1@`(reg0)=port B, `a1@(5120)`=SR (shift reg, reg10), `a1@(5632)`=ACR
  (reg11), `a1@(6656)`=IFR (reg13).  Port-B lines: PB5=`/SYS_SESSION`(0x20),
  PB4=`/VIA_FULL`(0x10), PB3=`/XCVR_SESSION`(0x08, Egret-driven).
- The loop is the ROM's **Egret power-on sync**: it opens a session (`bset
  #5`), waits (unbounded `btst #2,a1@(6656); beqs`) for IFR bit 2 (SR
  interrupt) after each shift-register byte, clocks bytes from the Egret, and
  keeps looping while the received byte is `0x00` (`tstb a1@(5120)` /
  `beqs`).  It only leaves this state when **PB3/XCVR asserts** (Egret says
  "I have a session/data", taking the send path at `0x40814d02` which *has* a
  `dbf d4` timeout and a real exit to `0x40848eda`) or a non-zero sync byte
  arrives.
- **Our Egret IS responding correctly** — the SR-interrupt timer fires, the
  log shows the full `feed 0x00 … response complete` cadence every iteration
  — but it always feeds `0x00`/no-response (`macclassicii_egret_no_response`)
  and holds PB3 low (observed port-B `b=40/60/70`, bit3 always 0), so the
  ROM's sync never sees the non-zero byte / XCVR assert it's waiting for and
  loops forever.  This is a **content/semantics** gap, not a missing
  interrupt: at cold start the real Egret sends an *unsolicited* power-on/
  reset packet (à la Cuda's reset notification) that this sync loop is
  waiting to receive; our `macclassicii_egret_*` engine (ported verbatim
  from maciisi.c, tuned to the IIsi ROM/OS ADB driver's edge cadence) never
  stages one for this cold-sync phase.

Next-session starting points:
  - Stage an Egret power-on packet (or assert `/XCVR_SESSION` PB3) at cold
    reset so this sync loop receives a non-zero sync byte / sees XCVR and
    takes the timeout-bounded send path out to `0x40848eda`.  Find the exact
    expected sync byte by disassembling that exit path and what it checks.
    Keep it Classic-II-local (`macclassicii.c`), maciisi.c is a separate file
    and its IIsi-tuned Egret must not be disturbed.
  - Alternatively determine whether this sync is *meant* to time out even on
    real HW (the send path's `dbf d4` counter) and, if so, why our receive
    path never reaches that counter — possibly our Egret should decline the
    session (leave SR int unset for this specific cold-sync opening) so the
    ROM's own timeout fires.
  - Reproduce/trace with `-d unimp -D /tmp/clii/x.log` (tmpfs!) — the egret
    cadence is fully logged (`macclassicii egret: …`); this phase emits
    ~680k egret lines / 20 s, so cap the run and grep, don't let it fill disk.

Command line for testing (unchanged):

    build/qemu-system-m68k -M macclassicii -bios /workspace/files/mac-roms/macclassic2.rom \
      -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
      -snapshot -serial null -serial null -display none -icount shift=7 \
      -monitor unix:/tmp/clii/mon.sock,server=on,wait=off

Screen is still blank white (512x342) — the ROM does this Egret sync before
drawing anything, so no happy-mac yet.  `-M maciici`/`-M maciisi`/`-M q800`
re-verified to still init/run after these changes (all edits are
`macclassicii.c`-local; nothing shared was touched).

Committed to `fix-classicii`: two real, verified bug fixes (VDAC decoder
probe; ROM relocation-base slot) advance the Classic II from "stuck in
pre-machine-ID POST forever" all the way through machine ID, code
relocation, and VIA2/RBV bring-up to the **Egret cold-start sync handshake**
— a precisely-decoded new frontier — with the earlier session's incorrect
RAM-alias experiment reverted back to the proven maciici-identical model.

### Egret cold-start sync SOLVED — three fixes; now past the sync into real boot (2026-08-25, session cont'd)

Got the Classic II **past the Egret cold-start sync** (the previous
frontier) and into ~150 000+ instructions of real post-sync ROM boot code.
The previous note's guess ("stage an unsolicited power-on packet") turned
out to be the wrong framing; the actual mechanism, decoded instruction-by-
instruction (a1 = VIA1 0x50f00000; `a1@(2)`>>9 reg decode: reg10=SR,
reg11=ACR, reg13=IFR):

The sync loop at `0x40814cc8` branches on **PB3 (/XCVR_SESSION)**:
`btst #3,a1@; beqw 0x40814e00` — PB3 low (XCVR asserted) → the RECEIVE path
that clocks bytes and loops while they read 0; PB3 high (XCVR deasserted) →
the SEND path (`0x40814d02`) which, with no Egret responding, hits its
`dbf d4` timeout and exits to the boot continuation at `0x40848eda`.  Three
distinct emulation bugs kept the ROM trapped in the RECEIVE path:

1. **PB3 was read from the ROM's own ORB latch, not the Egret line.**
   `mos6522_read` returns `s->b` for port B, so the ROM's port-B *writes*
   (it writes 0x40 during bring-up) clobbered PB3 — but PB3 is an
   Egret-driven **input**, not a host output.  With the latch reading PB3=0,
   the ROM always saw XCVR asserted → RECEIVE → hang.  Fix: track the Egret's
   /XCVR line state in a dedicated `egret_xcvr_asserted` field and **override
   PB3 on port-B reads** in `macclassicii_via1_read` (mirroring the existing
   port-A input-strap override), reporting the Egret's line regardless of the
   ORB write.
2. **A spurious Egret "session" opened during VIA port-B initialisation.**
   The ROM's one-time VIA init write (pc `0x40802ea4`, b=0x48, last_b=0xff,
   **acr=0x00**) dropped /SYS_SESSION as an edge and was misread as a receive
   session, asserting /XCVR before the sync even ran.  Fix: gate
   `egret_session_update`'s session-open on `s->acr & SR_CTRL` — real Egret
   transactions always run with the shift register enabled (acr `0x0c`/`0x1c`);
   the init write has SR disabled (acr 0), so it's excluded.
3. **/XCVR was asserted for the "no data" cases.** The maciisi-ported engine
   asserts /XCVR-low as its no-response/discard marker (the IIsi ADB driver
   reads it that way); on the Classic II ROM, /XCVR-low means "receive", so
   any no-response assertion dropped the sync back into RECEIVE.  Fix: the
   port-B read override reports /XCVR asserted **only for a genuine staged
   Egret reply mid-delivery** (`egret_xcvr_asserted && !egret_no_resp &&
   egret_resp_len > 0`).  At power-on the Egret has no unsolicited packet, so
   /XCVR stays high and the sync runs its send/timeout path to boot.

Plus a supporting change so the send/receive *framing* completes: on real
hardware the Egret provides the SR shift clock for every byte, so
`macclassicii_egret_sr_written` now raises the SR-completion interrupt for
**every** SR-out shift (not only inside a formal session), and
`macclassicii_egret_sr_read`/`_acr_changed` raise it for external-clock
SR-input outside a session — this carries the cold-sync's byte-framing
helpers (`0x40814e4a`/`0x40814e6a`) which clock bytes with /SYS_SESSION
released (no formal session).  All changes are in `macclassicii.c` and its
Egret path; maciisi.c is untouched, and `-M maciici`/`-M maciisi`/`-M q800`
still init/run (spot-checked).

Verified with `-s` gdb: PC now sits at **`0x4084a278`**, far past the sync
(`0x40814cc8`–`0x40814f00`).  A `-d int,guest_errors` run shows only the
*expected* early machine-ID probe Access Faults (`0x40803124`/`0x40803a8a`,
the deliberate bus-error decoder probes) — **no fault after the sync** — and
a break at `0x40848eda` single-steps 150 000+ instructions of genuine boot
code before eventually reaching the loop.

### New frontier: ROM MicroBug '*' serial prompt after ~150k instructions of boot

PC parks in a tight loop **`0x40849b68`–`0x4084a296`** (a3 = SCC
`0x50f04000`).  Decoded: `0x4084a268` is the ROM's **GetChar** (reads SCC RR0
bit 0 for an Rx char; RR0 reads 0x44, bit0=0 → "no char", returns 0x8000),
and `0x40849cb0` loads `#42` (`'*'`) and calls PutChar (`0x4084a182`) — this
is the ROM's **MicroBug serial debugger** at its `*` command prompt.  The
loop is pure polling (GetChar → no char → `braw 0x40849b68`); it does no boot
work and never exits because `-serial null` never delivers a character.
`d7` bit 17 (serial-debug enable) is set, so the ROM polls the SCC.  The
stack is empty (sp=0x2600, no exception frame) and `-d int` logs no late
hardware fault, so MicroBug was entered by a **software** path (a SysError
call or an explicit debug entry) at the *end* of a long, real boot sequence
— i.e. the machine now does substantial post-sync work and then drops into
the debugger, rather than hanging in the transport.  Screen still blank white
(MicroBug is entered before the framebuffer draw).

### MicroBug entry was REAL (a missing PA0 strap), not spurious — FIXED (2026-08-25, session cont'd)

Chased the MicroBug entry to root cause.  **d7 bit 17 (the serial-debug
lead) was a red herring** — `bset #17,d7` at `0x4084a13c` fires
*unconditionally* right after MicroBug's own SCC-init table loop, so it just
means "SCC initialised", not "debugger requested".  MicroBug is entered by a
normal software branch, not an exception (empty stack, no `-d int` fault).

Traced the real decision with a `-d exec` trace (to tmpfs; grep the PC column
for the first `0x40849b1c`): the path in is `0x40807xxx → 0x40848f04 (braw
0x40849b1c)`.  `0x40848f04` is reached from **`0x40848ee8`**:

    0x40848ee8  btst #26,d7
    0x40848eec  bnes 0x40848f08     ; bit26 SET -> real boot continuation
    0x40848eee  ...                 ; bit26 CLEAR -> falls through to MicroBug

So **d7 bit 26 gates real-boot vs MicroBug**, and our machine had it CLEAR.
Bit 26 is set (or not) during the post-ID hardware setup at `0x40846420`,
which branches on the decoder kind (d2).  For **kind 5** (the Classic II) the
check at `0x40846440` is:

    bclr #0,a2@(1536)   ; DIRA bit0 = 0  -> make VIA1 PA0 an INPUT
    bclr #1,a2@(7680)   ; ANH
    btst #0,a2@(7680)   ; read PA0
    bnes 0x40846494     ; PA0 == 1 -> DON'T set bit26  (=> MicroBug)
    bras 0x40846462     ; PA0 == 0 -> bset #26,d7       (=> real boot)

This is exactly the "VIA2-position PA0 NuBus pull-up" check MACIISI-NOTES.md
flagged: the IIci/IIsi read PA0 **high** there via a NuBus pull-up on that
pin, so *they* don't set bit 26 and take the other branch to boot.  The
Classic II has **no NuBus**, so that pin floats **low** — PA0 must read 0,
setting bit 26 and routing to the real boot.  Our strap was `pins-a = 0xBF`
(PA0 = 1), so the ROM took the MicroBug branch.

**Fix** (`macclassicii.c`): `pins-a = 0xBF → 0xBE` (clear PA0).  Bit 0 is
outside the 0x56 machine-ID mask (`0xBE & 0x56 == 0x16`), so identification
is unchanged.  Verified: d7 now reads `0x04000000` (bit 26 set) at the fork,
the ROM takes the real-boot path (`0x40848f08`), and PC advances past
MicroBug into genuine ADB/Egret initialisation.

### New frontier: real Egret ADB transaction (byte-handshake framing) at 0x4084a556

Past MicroBug, boot reaches a **real Egret ADB command→response transaction**
and hangs in its receive loop at **`0x4084a5f8`–`0x4084a604`** (a2 = VIA1):

    0x4084a5f8  btst #2,a2@(6656)   ; wait SR interrupt (fires — our SR clock)
    0x4084a5fe  beqs 0x4084a5f8
    0x4084a600  btst #3,a2@         ; PB3 (/XCVR_SESSION)
    0x4084a604  bnes 0x4084a5f8     ; loop while XCVR deasserted (PB3=1)
    0x4084a606  moveb a2@(5120),d2  ; read the Egret's response byte

i.e. the ROM has sent a command and is waiting for the **Egret to assert
/XCVR and hand back a response**.  Our Egret keeps /XCVR deasserted (no
staged reply) → infinite loop.  The command (captured: first byte `0x01`, an
Egret **pseudo-command** packet, host→MCU control) is shifted out by the send
helper `0x4084a694` with **/SYS_SESSION *released* (PB5 high)** and only a
per-byte /VIA_FULL (PB4) toggle — the opposite of the maciisi ADB driver's
framing, where sends happen *inside* a /SYS_SESSION frame.  So our
session-based `egret_sr_written` (which only collects command bytes while
`egret_session` is true) never captures this command, `egret_process` never
runs, no response is staged, and the receive turnaround finds nothing to
deliver.  `-d unimp` confirms **zero** `macclassicii egret:` lines during
this transaction — no session opens at all.

This is a genuine protocol gap, not a small tweak: the Classic II ROM drives
the Egret with a byte-level PB4/PB5 handshake and reserves /SYS_SESSION-low
for the receive turnaround, whereas the maciisi-ported engine models
/SYS_SESSION-low as the whole session.  Fixing it needs the Egret to (a)
collect SR-out bytes as a command **regardless of /SYS_SESSION**, (b) detect
the send→receive turnaround (ACR SR_OUT→SR_IN, or the /SYS_SESSION-low edge)
as end-of-command, (c) run `egret_process` / a pseudo-command handler on it,
and (d) stage the reply and assert /XCVR for the receive — **without
disturbing the now-working cold-start sync**, which relies on the current
session/XCVR behaviour.  It also needs the Egret **pseudo-command** semantics
(0x01-type control packets: self-test, set-autopoll, read/write PRAM, real
-time-clock) modelled, not just raw ADB pass-through.

Next-session starting points:
  - Decode the full command: single-step the send helpers `0x4084a694`
    (SR-out byte) / `0x4084a6b4` (SR-in byte) from `0x4084a556`, skipping the
    256-iter delay loops at `0x4084a6dc`, to capture all command bytes and the
    exact turnaround sequence.  Map byte[0]=0x01 pseudo-command byte[1]=code
    against the Egret/Cuda pseudo-command table (MAME `apple/egret.cpp`).
  - Rework the Egret command collection to be handshake-driven (PB4/PB5 +
    SR-out), independent of the /SYS_SESSION frame, and add a pseudo-command
    responder; keep the cold-sync path (which sends with /SYS_SESSION released
    too — careful not to treat those framing bytes as ADB commands) working.
  - Screen is still blank white — the framebuffer draw is *after* this ADB
    init, so happy-mac is gated on completing (or safely timing out) this
    transaction.

Command line for testing (unchanged from above).  `-M maciici`/`-M maciisi`/
`-M q800` re-verified to still init/run after the strap change (macclassicii.c
-local).

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
