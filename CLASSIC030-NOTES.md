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

### SE/30 continued (2026-08-25, branch `fix-se30`): ADB state-line bug found and fixed -- past the wait loop into real disk I/O

Picked up on branch `fix-se30` (worktree `/workspace/src/qemu-q630`,
based on `amiga` HEAD) with the `0x40806DD8` wait loop from above as
the target.  Reproduced it live with `-s -S`/gdbstub (note: had to use
`-gdb tcp::PORT` instead of bare `-s`, since other agents' worktrees
were already holding the default `:1234` port; `set endian big` after
`target remote` as always).

**Root cause identified.**  `a3` (0x24b0 this run) is the ROM's ADB
Manager globals struct; offset 349 is a busy/status byte, bit 5 the
"transaction in flight" flag.  The routine at 0x40806dea is
`ADBReInit`-shaped: it programs VIA1 ACR to shift-register
"output/external-clock" mode (`ACR=0x1C`, `SR_OUT` bit set), writes
the first ADB command byte (0x00 = SendReset) into VIA1's SR (reg 10),
then writes VIA1 ORB (reg 0) with the ADB transceiver state bits
(PB5/PB4, `VIA1B_vADB_StateMask`) set to `ADB_STATE_NEW` (0) to kick
the transaction off -- this exactly matches our `macse30_adb_send()`/
`macse30_adb_update()` model ported from q800's `mac_via.c`.  Added
temporary `qemu_log_mask` tracing to `macse30_adb_update()` and
confirmed: **zero** `macse30_adb_send()`/`macse30_adb_receive()` calls
ever fired across 21000+ Port-B writes leading up to and including the
kickoff.  The reason: `macse30_adb_update()` only calls send/receive
when `(s->b & VIA1B_vADB_StateMask) != (v1s->last_b &
VIA1B_vADB_StateMask)`, but an EARLIER ROM VIA1 self-test (part of
generic pre-machine-ID POST, same family of walking-bit tests that
blocks Classic II's boot on VIA1 IER -- see that section above) walks
VIA1 DDRB through an all-output/all-input pattern and, in the process,
leaves the ORB *output latch* bits for PB4/PB5 stuck at 0 once DDRB
reverts those two bits to inputs.  Real 6522 silicon reads
input-configured pins as the live external level (here, the ADB state
lines float/pull HIGH when not host-driven -- idle == state 3, all
ones); our `mos6522.c`'s generic `s->b = (s->b & ~dirb) | (val &
dirb)` has no concept of a live external level for input bits and just
retains the stale output-latch value.  So by the time the ADB Manager
does its first *real* transition to `ADB_STATE_NEW` (== 0), the
tracked "previous" state was *also* 0 (the self-test's leftover), the
transition is invisible, `macse30_adb_send()` never runs, the VIA1 SR
interrupt this whole mechanism is supposed to raise
(`v1s->adb_data_ready`, wired to `SR_INT_BIT`) never fires, VIA1's
IFR bit 2 never sets despite IER already enabling it (confirmed via
QEMU's `info via` HMP command: `IFR=0x40` (T1 only), `IER=0x87`
(CA2/CA1/SR enabled) -- SR simply never latches), and the ROM spins on
bit 5 forever.  This is exactly the class of bug the task brief
predicted ("a VIA timer tick... or a VIA shift-register/CA-CB edge"
that the wiring isn't delivering) -- except the missing edge was
entirely on the *input* side of our own VIA1 Port-B modeling, not a
GLUE routing gap.

Port A already has exactly this "board strap" fixup in
`macse30_via1_read()` (`val = (val & s->dira) | (v1s->pins_a &
~s->dira)`), but it only patches the CPU's *read* path, and only for
Port A; Port B had no such thing.  A read-side-only fix would not have
helped here anyway, since the bug is in the *internal* `s->b` value
consumed directly by `macse30_adb_update()`/`via1_rtc_update()`, not
just what the CPU sees on a register read.

**Fix** (`hw/m68k/macse30.c`, `macse30_via1_portB_write()`, previously
an empty stub copied from maciici.c): force any Port-B bits inside
`VIA1B_vADB_StateMask` that DDRB currently marks as *inputs* back to
the idle (all-ones/state-3) level on every ORB write:
`s->b |= VIA1B_vADB_StateMask & ~s->dirb;`.  This only touches bits
DDRB doesn't currently claim as outputs (so it can never clobber a
real ROM-driven state write), self-heals the very next Port-B write
after any DDRB direction change strands a stale bit, and leaves
`VIA1B_vADBInt` (bit 3, driven entirely by our own transceiver code,
not by DDRB) untouched.  Confirmed via the same temporary trace that
after the fix, `macse30_adb_send()`/`macse30_adb_receive()` fire
continuously and normally (1000+ calls within the first 25 real
seconds: SendReset, then repeated Talk/Listen register polls with the
keyboard/mouse -- classic ADB device enumeration traffic).

**Result: boot advances from the ROM (0x408xxxxx) into RAM-resident
code (PC in low memory, e.g. 0x000112xx) actively driving the NCR5380
over real pseudo-DMA.**  Traced with `-trace enable='ncr5380_*'`:
hundreds of SCSI transactions complete successfully end-to-end
(INQUIRY/TEST-UNIT-READY-shaped 6-byte commands to target 2 timing out
as expected -- no CD-ROM attached -- and dozens of READ(6)/READ(10)
commands to target 0, the hard disk, succeeding with correct data
checksums across LBAs scattered around the volume, consistent with
boot-block, driver, and early filesystem-structure reads).  This is
well past the originally-targeted interrupt-wait loop and into genuine
disk-driven boot progress -- past where machine-ID/straps would have
had to succeed already (the placeholder 0xEF strap did not visibly
block anything).  The framebuffer at this point still shows raw
uninitialized RAM noise, not a happy-mac icon yet, consistent with the
hang below occurring before the ROM's first screen draw.

**New, later, well-characterized blocker found (not yet resolved):**
after ~1830 successful SCSI command completions, one large *blind*
READ(10) for 192 sectors (98304 bytes, LBA 83548 -- large enough to
plausibly be a System-file/resource-fork chunk or extents read) stalls
forever.  Single-stepped/traced: the ROM's byte-copy loop at
0x11276-0x1128a (`moveb %a0@,%a2@+` from the pseudo-DMA handshake
aperture at 0x50F06xxx, `dbf`-counted) pulls **exactly 512 bytes**
(one sector) of the 98304 requested, then falls through to a shared
tail-dispatcher at 0x112e6-0x112f4 (`btst #5,a3@(64)` [CSB REQ] /
`btst #3,a4@` [BSR PHASE_MATCH], looping while both stay set) that
never exits, because our NCR5380 model (`hw/scsi/ncr5380.c`) pre-reads
an entire multi-block transfer into `s->dbuf` synchronously up front
and holds `PHASE_DI`/REQ/DRQ/PHASE_MATCH asserted until *all* 98304
bytes are drained via the pdma aperture -- but nothing in the guest
ever asks for bytes 512-98303.  Confirmed the 512-byte cap is a real
ROM loop-count decision (its `dbf`-pair counters exhaust, not a
DRQ/REQ deassertion), meaning the ROM's per-call PIO chunk size is
genuinely bounded and it expects to be invoked again for subsequent
chunks by an outer mechanism this session did not locate.

Tried, as a bounded experiment, reconnecting two pieces of interrupt
wiring that were leftover diagnostic disconnections/stubs from the
*previous* session's now-fixed VIA2 longword-storm hunt: (1) VIA2's
own summary IRQ output, unconditionally disconnected from GLUE with a
"DIAGNOSTIC: leave VIA2 IRQ disconnected to isolate the storm" comment
-- reconnected to `MACSE30_GLUE_VIA2` (GLUE level 2), matching the
IIci's RBV-summary wiring; and (2) the NCR5380's IRQ output, sunk to
`macse30_irq_sink()` with a "TODO: route via VIA2 CA1" comment --
wired to VIA2's `CA1_INT_BIT`, matching real Mac II/IIx/SE30 GLUE-era
hardware (Linux `arch/m68k/mac/via.c` and MAME `mac.cpp` both route
SCSI_IRQ to VIA2 CA1 on this machine family, NOT to RBV/GLUE directly
as on the IIci).  Neither caused a storm across 45+ real seconds and
hundreds more successful SCSI transactions (the earlier storm's real
cause, confirmed last session, was the VIA2 wide-access bug, already
fixed) -- both are kept as genuine hardware-correctness improvements.
Confirmed via `info via` that VIA2 IER does enable CA1 (0x02) by the
time of the stall, but the stall itself did NOT clear: our
`ncr5380_update_irq()` only asserts IRQ on `BSR_IRQ` (0x10), which our
model only ever sets at full-*transaction* completion (bus-free/
message-in), never mid-transfer -- so even with SCSI IRQ wired, no
edge is available to end this particular wait, because ending it
requires the 98304-byte transfer's remaining 97792 bytes to actually
be drained first (chicken-and-egg).  So SCSI IRQ routing is real and
now correct, but is NOT the missing piece for *this* stall.

Next-session starting points:
  - Find the outer caller of the 0x112aa/0x11276-style per-block PIO
    dispatcher (search backward from 0x112e6/0x112f4's `rts` for who
    `jsr`s into this routine, and what decides whether to call it
    again for the next 512-byte chunk) -- likely either a fixed-size
    "sector buffer" convention in the SCSI Manager/boot-block loader,
    or genuinely interrupt-driven via a source not yet identified
    (VBL? a VIA1 timer? worth re-checking whether the *tail loop's*
    other exit condition -- BSR PHASE_MATCH going to 0 -- is the
    intended path, which would need `ncr5380_do_command`'s "pre-read
    the whole transfer up front" strategy to instead honor some
    per-call transfer-size limit the ROM communicates some other way).
  - Alternatively (probably the real fix): teach `ncr5380.c` to only
    assert DRQ/REQ for the portion of the transfer the guest actually
    starts consuming and let the SCSI Manager re-arm for the next
    chunk the way real 5380 pseudo-DMA drivers do -- but note the
    existing `ncr5380_do_command` comment ("Pre-read the ENTIRE
    transfer synchronously... if the DI->ST transition waited on an
    async aiocb, the trailing loop would clock in phantom stale
    bytes") means this was already tried and reverted once for a
    *different* symptom; any fix here needs to preserve that guarantee
    for small/single-block transfers while unblocking large ones.
  - `macIIx.rom`'s own machine-ID straps are still unverified (0xEF
    borrowed from the IIci ROM) -- doesn't appear to be blocking
    anything observed so far, but worth confirming once past the SCSI
    stall.

Command line used for testing (same as above, this worktree):

    build/qemu-system-m68k -M macse30 -bios /workspace/files/mac-roms/macIIx.rom \
      -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
      -snapshot -serial null -serial null -display none -icount shift=7 \
      -monitor unix:/tmp/se30/mon.sock,server=on,wait=off

Committed to `fix-se30`.  Net result this session: the originally
assigned interrupt-wait loop is fixed and understood at the root
-cause level (ADB Port-B input-pin modeling, `macse30_via1_portB_write`
in `hw/m68k/macse30.c` only -- no shared files touched); boot now
progresses far past machine-ID into genuine SCSI-driven disk I/O
(hundreds of successful transactions), which is past the "good" tier
bar in the task brief (past interrupt-wait toward machine-ID) though
not yet at a rendered happy-mac/Finder.  A second, later, well
-characterized blocker (large multi-block blind SCSI reads) is now the
open item, documented above with concrete next steps.

### SE/30 continued #2 (2026-08-25, `fix-se30`): chunked pseudo-DMA fix -- large SCSI reads now complete, boot runs deep into System 7.5.3

Tackled the "large multi-block blind SCSI read stalls after one 512
-byte chunk" blocker documented just above.  Full root cause, via
`-trace enable=ncr5380_*` + gdb (`set endian big`) disassembly of the
Mac II ROM's blind-transfer routine (entry 0x111e2, copy loop
0x11276, tail 0x112e6):

  - The routine is the ROM SCSI Manager's blind pseudo-DMA mover.  Its
    per-call byte count arrives in `d2`; traced live to be **exactly
    512** on every invocation (breakpoint at 0x111e2 dumping `d2` --
    three calls seen, all `d2=512`).  So the ROM drains even a large
    (192-sector / 98304-byte) READ(10) **512 bytes at a time**, one
    sector per call, re-entering the routine for each subsequent
    chunk.
  - The copy loop (`moveb %a0@,%a2@+` through the handshake aperture at
    0x50F06060, dbf-counted) gates each byte on **DRQ** (BSR bit 6),
    not REQ.  After its 512-count exhausts, it falls into a tail loop
    (0x112e6: `btst #5,CSB` [REQ, bit 5] / `btst #3,BSR` [PHASE_MATCH,
    bit 3]) that spins until **/REQ deasserts** (then returns success
    to fetch the next chunk) or the phase changes.
  - Our `ncr5380.c` pre-reads the ENTIRE transfer synchronously into
    `s->dbuf` up front (deliberate -- see the long comment in
    `ncr5380_do_command`; it prevents phantom stale bytes on the DI->ST
    transition) and asserts CSB REQ/DRQ/PHASE_MATCH continuously for
    the whole data phase, only clearing REQ when the *entire* buffer
    drains (via `ncr5380_enter_status`).  So after the ROM's first 512
    -byte chunk, 97792 bytes still sat undrained, REQ stayed asserted,
    and the ROM's inter-chunk "wait for /REQ low" tail spun forever.
    Confirmed with the trace: `reg[3]<-0x01`(TCR=IO), `reg[2]<-0x02`
    (MR=DMA), `reg[7]<-0x00`(start DMA recv), then **exactly 512**
    `pdma_rd` events and **no further register writes** (ruling out the
    earlier hypothesis of an interrupt handler poking TCR), then the
    endless CSB/BSR poll.

**Fix** (`hw/scsi/ncr5380.c`, `ncr5380_pdma_read`, DI branch): model
the per-byte SCSI /REQ handshake -- deassert `CSB_REQ` after each
pseudo-DMA byte that is *not* the final one (the final byte still goes
through `ncr5380_enter_status`, unchanged).  This lets the chunked
reader's inter-chunk "/REQ low" poll make progress after each 512-byte
sector, while a **full-drain reader** (the IIci/IIsi ROMs, which read
the whole transfer in one loop) never samples /REQ mid-transfer -- it
only checks it once at the very end, by which point `enter_status` has
already moved the bus to STATUS -- so the change is invisible to them.
The chunk's byte-move loop gates on DRQ (kept asserted until the buffer
fully drains), so clearing /REQ does not disturb the copy itself.  This
is also simply *more* correct 5380 modelling (real /REQ pulses per
byte), not an SE/30-specific hack, so it lives in the shared file
rather than behind a machine flag.

**Result: the 192-sector read completes and boot advances
dramatically.**  Where the ROM previously parked forever at PC
0x000112ee, it now churns through a wide spread of System 7.5.3 code:
sampled PC across a single second lands on **19 distinct addresses out
of 30 samples** spanning 0x0003bxxx / 0x0007axxx / 0x00165xxx /
0x0037exxx-0x0037fxxx / 0x003a6xxx-0x003a8xxx / 0x000b6xxx etc. --
i.e. deep, varied System execution (driver/extension init), with SR
I:0 and no tight-loop parking.  This is well past the SCSI stall and
into real OS bring-up.

**Still NOT reaching a rendered happy-mac/Finder** -- but the
remaining gap is a **video/framebuffer-base problem, not SCSI**: our
`macse30-fb` scans a FIXED base of `ram_size - 0x5900`, whereas at
this boot stage the low-memory `ScrnBase` (0x0824) reads 0x0000357c
and `MemTop` (0x0108) reads 0x002bf3c8 (well below the 4MB top),
i.e. MacOS is placing/using the screen buffer somewhere our fixed
scanout does not point, so the display still shows static
uninitialized-RAM noise even though the CPU is executing normally.
Next session: make `macse30-fb` honor the ROM/OS `ScrnBase` (0x0824)
or the standard compact-Mac `MemTop`-relative main-buffer placement
instead of a hardcoded `ram_size - 0x5900`, and/or verify the video
base the Mac II-family ROM programs (it differs from the compact
-Mac128k formula this fb was cloned from).

**Regression check (shared `hw/scsi/ncr5380.c` was touched):**
  - `-M maciici` (macIIci.rom): re-verified -- boots all the way to a
    **fully interactive Finder desktop** (Control Panels window open,
    live menu-bar clock, all icons rendering).  No regression; this is
    the strongest available test since it exercises the exact same
    pseudo-DMA + handshake-aperture path and boots to the end.
  - `-M maciisi`: its ROM is not present in this environment
    (`/workspace/files/mac-roms/` has no IIsi image), so it could not
    be booted this session.  The fix is invisible to full-drain
    readers by construction (see above), and maciisi shares the IIci's
    $067C ROM family / full-drain blind-read style.
  - `-M q800`: uses the **ESP** SCSI controller (`hw/scsi/esp.c`), NOT
    `ncr5380.c` -- unaffected by this change by construction (different
    source file entirely).  Its ROM is also not present here.

Command line (SE/30, this worktree):

    build/qemu-system-m68k -M macse30 -bios /workspace/files/mac-roms/macIIx.rom \
      -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
      -snapshot -serial null -serial null -display none -icount shift=7 \
      -monitor unix:/tmp/se30/mon.sock,server=on,wait=off

Committed to `fix-se30`.

### SE/30 continued #3 (2026-08-25, `fix-se30`): video diagnosis -- it's a slot/declaration-ROM device, NOT a fixed main-RAM framebuffer; a base fix cannot render it

Investigated the "framebuffer-base mismatch" the previous section left
open, intending to point `macse30-fb` at the base MacOS actually uses.
The investigation instead **overturned the premise**: there is no
fixed main-RAM 1-bit framebuffer for our scanout to point at, because
the SE/30's onboard video is architecturally a NuBus **pseudo-slot
($E) declaration-ROM device** (the Mac II family way), not the
compact-Mac fixed-RAM video the `macse30-fb` model was cloned from
(`hw/m68k/mac128k.c`).  Evidence, all gathered this session via gdb
(`set endian big`) full-RAM dumps (`dump binary memory`) + PIL
rendering + monitor sampling:

  1. **No 512x342 screen exists anywhere in the 4 MB RAM** while MacOS
     is actively running.  Dumped all of RAM at ~95 s (matching the
     time maciici takes to reach its Finder) and swept a 512x342/1-bpp
     window across every 64-byte offset scoring for (a) a white menu
     bar + black separator line, and (b) a 50%-gray desktop dither
     (0xAA/0x55).  **Zero** menu-bar candidates and **zero** gray
     -desktop regions anywhere.  A compact-Mac fixed-RAM design would
     have a happy-mac and then a gray desktop sitting at a fixed base;
     the SE/30 has neither in main RAM.
  2. **`ScrnBase` (low-mem 0x0824) reads 0x0000357c** and never
     changes -- far too low to be a screen buffer (it would overlap the
     trap dispatch table at 0x1e00 and low globals); rendering a window
     there shows only structured heap, no desktop.  A working compact
     -Mac screen port would hold a valid top-of-RAM `ScrnBase`.
  3. **The ROM never draws a POST screen (gray/happy-mac) to main
     RAM.**  A gdb hardware watchpoint on a candidate framebuffer byte
     (0x3fc000), filtered to ignore the ROM's own RAM-test fill loop
     (0x40803600-0x40803900, walking pattern 0x6db6db6d up to the test
     ceiling 0x3ffd00), caught only the RAM test and a generic
     memory-clear (`clrl %a1@-`) touching that address -- never any
     screen-fill/blit routine.  On the compact Macs the ROM blits the
     happy-mac to the fixed base during POST; here it does not.
  4. **No NuBus/slot super-space probing either.**  `-d unimp,
     guest_errors` over a 30 s boot logged **no** accesses to slot
     video space (0xF9/0xFB/0xFExxxxxx) and **no** bus errors there --
     consistent with this machine having been built deliberately
     NuBus-free (`macse30.c` removed `mac_nubus_bridge` entirely), so
     the slot-$E declaration ROM + framebuffer the SE/30 video needs is
     simply absent, and neither the ROM nor MacOS can find/init a
     display.
  5. **MacOS is nonetheless genuinely up and running** (so this is
     purely a display-output gap, not a boot failure): monitor PC
     sampling shows ~20 distinct PCs per 40 samples spanning System
     code (0x0037xxxx-0x003axxxx), low-RAM OS (0x0003bxxx), and ROM
     (0x4081e444), with the busiest single PC being 0x0000afd0 -- which
     disassembles to the **A-line trap dispatcher** (`cmpiw #0xa800` /
     table index off 0x1e00), i.e. the OS is actively dispatching traps
     the whole time.

**Conclusion / why the base was never the real problem:** the earlier
`ScrnBase=0x357c` / `MemTop=0x2bf3c8` readings were red herrings -- not
"MacOS drew the screen somewhere else," but "MacOS never established a
linear framebuffer at all" because the built-in video is a slot device
we don't model.  Changing `macse30-fb`'s scanout base (to `ram_size -
0x8000`, to `MemTop`-relative, to `ScrnBase`, or anything else) cannot
produce a visible desktop: there is no coherent framebuffer image in
main RAM to display, at any base.  The top-of-RAM region our current
`ram_size - 0x5900` scanout points at is used by the ROM RAM test and
then as ordinary heap, which is exactly the "static noise" seen.

**Therefore left `macse30-fb` unchanged** (a spurious base tweak would
render identical noise and mislead the next reader) and did NOT touch
shared display code -- so `-M maciici` / `-M q800` / the compact Macs
are untouched and unaffected by this session (nothing to regress).

**Concrete path forward for the next session (the actual required
work, a real feature not a base fix):** emulate the SE/30 onboard
video as a slot-$E declaration-ROM device so MacOS's Slot Manager +
slot video driver initialise it:
  - The tree already has a full NuBus Mac video model with a
    declaration ROM in `hw/display/macfb.c` (the DAFB-style card q800
    uses -- note its `MACFB_DISPLAY_*` sResource tables and the
    `MACFB_VRAM_SIZE`/declaration-ROM plumbing).  The cleanest route is
    probably to (a) re-introduce a minimal NuBus/slot bridge for slot
    $E on `macse30.c` (the machine deliberately dropped it), (b) place
    a small declaration ROM describing the built-in 512x342 1-bit video
    (base, rowBytes=64, depth=1, dimensions) in slot-$E space, and (c)
    back the framebuffer either in main RAM (real SE/30 scans main RAM
    top) or a dedicated VRAM region the declaration ROM points at.
  - Cross-check the exact SE/30 video sResource against MAME
    `mame/src/mame/apple/mac.cpp` (`macse30` video / `jmfb`/`macpds`
    hookup) and Linux `drivers/video/fbdev/macfb.c`
    (`MAC_MODEL_SE30`) for the declaration-ROM contents and the
    framebuffer base the real machine advertises.
  - This is substantially more than adjusting a scanout base and was
    out of reach in the remaining budget this session; the two
    interrupt/SCSI blockers that WERE assigned are fixed and committed
    (boot now runs deep into System 7.5.3), and this section documents
    precisely why video is a separate, larger piece of work.

### SE/30 continued #4 (2026-08-25, `fix-se30`): slot-video hardware spec fully researched; blocked on the declaration-ROM binary/toolchain

Followed the plan from #3 to actually implement the slot-$E video.
Researched the exact SE/30 onboard-video hardware (web sources cross
-checked below) and established the complete spec, but hit a hard
supply-chain blocker on the one artefact that cannot be synthesised
quickly -- the video **declaration ROM** itself.

**Exact SE/30 onboard-video spec (now fully known):**
  - It is a *separate* 8 KB **2764 EPROM** on the logic board (chip
    **UK6**), NOT part of `macIIx.rom`.  Confirmed macIIx.rom does NOT
    embed it: the one `0x5A932BC7` (Apple decl-ROM testPattern) hit in
    macIIx.rom at file-offset 0x8b0 is an *immediate operand* of a
    `move.l #$5A932BC7,...` (`21FC 5A932BC7`) -- i.e. the ROM's own
    Slot Manager code *searching* for a decl ROM, not a decl ROM.  This
    is why the II/IIx (same ROM, no onboard video) don't falsely
    advertise a display.
  - The video is a NuBus **pseudo-slot** (the SE/30 has a PDS, not
    NuBus, so Apple faked a slot so the Slot Manager enumerates the
    built-in video like a card).  boardId **$000C**.
  - Framebuffer: a **64 KB video RAM** aperture; screen memory starts
    at base **offset $8040** into it (`dc.l $8040 ; base offset for
    start of screen memory` in the decl-ROM disassembly).
  - Base 1-bit mode (`_StandardVidParams1`): rowBytes **$40** (64),
    width **$200** (512), height **$156** (342), pixelSize **1**.  The
    framebuffer lives in main RAM (top), matching the compact-Mac
    "video in main memory at a fixed offset from RAM top" model -- so
    with VRAM = top 64 KB (0x3F0000-0x400000 on a 4 MB machine) the
    screen base would be 0x3F0000 + $8040 = **0x3F8040** (near the
    all-white cleared block observed at 5 s in section #2, and near the
    prior `macse30-fb` guess of 0x3FA700 -- but note the desktop is
    only ever drawn there once MacOS's slot video driver initialises,
    which needs the decl ROM; see below).

**Why it is blocked (the honest supply-chain problem):**
  - MacOS cannot enumerate/initialise the display without the slot-$E
    declaration ROM, and that decl ROM must contain a *functional 68k
    video driver* (Open/Control/Status: cscSetMode/GetMode/SetEntries/
    GrayPage/GetPageBase...).  A bare framebuffer + empty slot is not
    enough -- the Slot Manager needs the sResource tree AND the driver.
  - No **prebuilt binary** of the SE/30 video decl ROM is publicly
    available (Macintosh Garden hosts only the 256 KB *main* ROM =
    macIIx.rom, checksum 97221136; the 8 KB video ROM is not archived
    as a downloadable `.bin`).
  - The only source is Axel Muhr's disassembly (github.com/axelmuhr/
    Mac-SE-30-video-rom: `mySE30rom.a`, `mySE30_driver_full.a`,
    `mySE30_primaryInit.a`, `mySE30_driver_stub.a`, `mySE30_easteregg.a`
    + a `doDeclROM` build script) which per its own README **"can be
    assembled with MPW 3.2"** -- it uses MPW declaration-ROM assembler
    macros (byte-lane packing, sResource offset resolution, format
    -block CRC).  This sandbox has `m68k-linux-gnu-as` (GNU as) but NOT
    an MPW/Retro68 decl-ROM toolchain; GNU as assembles plain 68k
    instructions but does not implement the MPW decl-ROM structure
    macros / CRC / byte-lane linker step, so it cannot build this
    disassembly as-is without a large syntax+macro port.
  - Hand-writing a minimal-but-functional decl ROM + video driver from
    scratch (format block + CRC + sResource dir + board/functional
    sResources + vidMode params + a working Open/Control/Status driver)
    is a multi-day expert task, and the Slot Manager gives *no*
    diagnostic feedback when it silently rejects a malformed decl ROM
    or the video driver returns wrong params -- so it is high-risk to
    attempt blind in a bounded window.
  - Additionally the machine (`macse30.c`) was deliberately built
    NuBus-free (`mac_nubus_bridge` removed), so slot-$E enumeration
    also needs a minimal NuBus/slot decode re-added (super-slot
    0xE0000000 / standard-slot 0xFE000000 for slot $E, the decl ROM at
    the top of that space, VRAM aperture at base+$8040) plus the slot
    VBL IRQ routed through GLUE.

**Decision:** did NOT commit any half-built slot infrastructure or a
speculative hand-rolled decl ROM -- either would risk destabilising the
otherwise-healthy machine (which now boots MacOS deep into System 7.5.3
thanks to the committed ADB + SCSI fixes) for an unverifiable payoff.
No code changed this round; the committed ADB (`e40b39c3`) and SCSI
(`e3082f0d`) fixes are intact.

**Concrete implementation plan for a session that has the toolchain
(the remaining work, now fully scoped):**
  1. Obtain the decl-ROM binary: either dump a real SE/30 UK6 2764
     (8 KB), or build Axel Muhr's disassembly with **MPW 3.2** (or port
     its `doDeclROM`/macros to Retro68's decl-ROM support).  This is the
     critical-path artefact.
  2. In `macse30.c`, re-add a minimal slot-$E decode: map the 8 KB decl
     ROM read-only at the top of slot $E's standard space (0xFE000000
     region, decl ROM at the high end per Apple's format-block-at-top
     convention, byteLanes as the ROM specifies) so the Slot Manager
     finds it; also alias into the 24-bit slot window if the ROM probes
     there first.
  3. Provide a 64 KB VRAM `MemoryRegion` for slot $E and point the
     existing `macse30-fb` scanout at VRAM + $8040 (512x342x1,
     rowBytes 64) instead of `ram_size - 0x5900`.  (Real HW scans main
     RAM top; a dedicated 64 KB VRAM aperture is a valid simplification
     as long as the decl-ROM minorBaseOS/base matches what fb scans.)
  4. Route the video VBL interrupt (the driver's cscSetInterrupt /
     the OS's VBL task) through GLUE (a spare level, as q800 does via
     VIA2 nubus-irq) if the OS blocks on it.
  5. Boot `-M macse30` with the OpenRetro 7.5.3 disk; expect the happy
     -mac then the desktop (MacOS is already proven running -- trap
     dispatcher busy at 0xafd0 -- so once the display device
     enumerates it should draw).  Screendump via QMP->PPM->PIL.
  Cross-refs: Axel Muhr decl-ROM disassembly + geekdot.com "Macintosh
  Declaration ROM 101"; Apple "Designing Cards and Drivers for the
  Macintosh Family" (decl-ROM format, video sResource, slot driver
  interface); QEMU `hw/display/macfb.c` + `hw/nubus/*` for the slot
  plumbing pattern (q800 slot-9 framebuffer); Linux
  `drivers/video/fbdev/macfb.c` `MAC_MODEL_SE30`.

### SE/30 continued #5 (2026-08-25, `fix-se30`): synthesized decl ROM from scratch -- Slot Manager validates + ENUMERATES the video board; stops at driver load

Took the bounded shot at hand-authoring the pseudo-slot $E video
declaration ROM, debugging it live against the ROM Slot Manager via
QEMU traces (the coordinator's key insight: full visibility replaces
MPW/Slot-Manager diagnostics).  Result: a **from-scratch declaration
ROM that the SE/30 ROM Slot Manager fully validates and enumerates as
a video board** -- every format/CRC/byte-lane hurdle cleared.  Stops
at the next stage (loading the absent video *driver*).

**What was built (all in this worktree, non-regressing / opt-in):**
  - `scripts/se30-build-declrom.py`: a pure-Python declaration-ROM
    assembler (no MPW needed).  Emits the format block (byteLanes
    0x0F = all-lanes contiguous, testPattern 0x5A932BC7, format 1,
    rev 1, reserved 0), the Apple decl-ROM CRC (ROL-1-then-add-byte,
    skipping the CRC field), the sResource directory, a Board
    sResource (boardId $000C, sRsrcType catBoard, sRsrcName) and a
    Video functional sResource (sRsrcType catDisplay/typVideo/drSwApple,
    sRsrcName, one 1-bit 512x342 vidMode with mBaseOffset $8040 /
    mRowBytes $40, minorBaseOS/minorLength).
  - `hw/m68k/macse30.c`: maps pseudo-slot $E into the 24-bit slot
    window (VRAM 64KB at 0x00F00000, decl ROM ending at 0x00FFFFFF),
    points `macse30-fb` at the slot VRAM (base+$8040), and loads the
    decl ROM (as a logging IO region for debugging) **only when the
    `MACSE30_DECLROM` env var points at the .bin** -- so the default
    machine is byte-for-byte unaffected and still boots MacOS (verified:
    14 distinct PCs sampled, running normally).

**Live-debugged validation chain (each fixed by tracing the ROM's own
Slot Manager at 0x408041e0 / 0x40804244 / 0x4080428e):**
  1. byteLanes: high nibble must equal `~(low nibble)&0xF` -> 0x0F. OK.
  2. testPattern `cmpil #$5A932BC7,a4@(14)`: matched (a4@14 read back
     as 0x5a932bc7). OK.
  3. format block: `a4@13`(format)==1, `a4@12`(rev)<=9, `a4@18`
     (reserved)==0. OK.
  4. **directoryOffset bug FOUND+FIXED**: `0x4080427c` requires
     `(dirOffset & 0xFF000000)==0`, i.e. a 24-bit value -- a negative
     offset must be stored as `0x00FFFFxx`, not sign-extended to
     `0xFFFFFFxx`.  (First attempt used -12=0xFFFFFFF4 -> err -307.)
  5. **CRC verified byte-exact**: breakpoint at the compare
     (`0x4080434e cmpl a4@(8),d6`) showed computed d6 == stored CRC ==
     0xbcac6f67 -- my Python CRC reproduces the ROM's algorithm exactly
     (confirms the ROL-1/add/skip-CRC-field + byte-lane stride model).
  6. **Board ENUMERATED**: reached `0x40804476` ("slot enumerated OK")
     for slots 9-14 (the decl ROM currently answers every slot because
     the 24-bit slot addresses all alias to 0x00FFFFFF; harmless for
     bring-up, MacOS finds the video in slot $E).

**Exact stopping point (documented for the next step):** with the
board enumerated but the Video sResource carrying no `sRsrcDrvrDir`
(68k slot driver), MacOS's Slot Manager falls into `sReadStruct`/
`sGetDriver` (a byte-lane copy loop at RAM 0x000eccfe:
`moveb %a0@,%a3@+` with a rotating lane mask) reading a bogus driver
length and copying forever -> boot hangs at a single PC (0x000eccfe)
instead of the normal running MacOS.  This is precisely why the decl
ROM is env-gated OFF by default: enabling it advances all the way to
board enumeration but then hangs pending the driver.

**Remaining work (now a small, well-scoped list -- the hard reverse
engineering is DONE):**
  1. Add an `sRsrcDrvrDir` (id $04) to the Video sResource pointing at
     an `sMacOS68000` driver blob (assemble with `m68k-linux-gnu-as` +
     `objcopy`, embed via the Python builder).  Driver header (drvrFlags,
     offsets to Open/Prime/Ctl/Status/Close, drvrName) + minimal bodies:
     Open (noErr + init), Control cscSetMode/SetEntries/GrayPage (ack),
     Status cscGetMode/GetPageBase(base+$8040)/GetPageCnt/GetEntries.
  2. Get past the `sGetDriver` copy (correct driver length in the
     sResource) -> MacOS opens the driver -> returns the framebuffer
     base -> QuickDraw draws to VRAM -> `macse30-fb` shows it.
  3. Optionally constrain the decl ROM to only answer slot $E (avoid
     the 6x alias) and route the video VBL IRQ through GLUE.
  Reproduce this milestone:
    python3 scripts/se30-build-declrom.py /tmp/se30_declrom.bin
    MACSE30_DECLROM=/tmp/se30_declrom.bin build/qemu-system-m68k -M macse30 \
      -bios /workspace/files/mac-roms/macIIx.rom -drive file=...,if=scsi \
      -snapshot -display none -icount shift=7 -d unimp -D /tmp/trace.log

Committed to `fix-se30` (macse30.c slot infra + builder; the generated
.bin is not committed -- regenerate with the script).  ADB (`e40b39c3`)
and SCSI (`e3082f0d`) fixes remain intact; default `-M macse30` still
boots MacOS unchanged.

### SE/30 continued #6 (2026-08-25, `fix-se30`): slot video DRIVER added -- loads past the sGetDriver hang; boot advances into the ROM's post-video startup, new stop at an ADB-completion wait

Added the `sRsrcDrvrDir` + a hand-written minimal 68k slot video driver
and iterated live.  The driver now LOADS (clearing the previous
sGetDriver hang) and the boot advances measurably further -- into the
ROM's post-"video-found" startup path -- where it now stops at a
different, well-characterized point (an ADB-completion wait).

**Added:**
  - `scripts/se30-video-driver.s`: minimal `.Display_Video_SE30` DRVR
    (assembled with `m68k-linux-gnu-as` + `objcopy`, MIT syntax):
    standard driver header (drvrFlags 0x4C00 = dCtlEnable|dStatEnable|
    dNeedLock, offsets to Open/Prime/Control/Status/Close, pstring
    name), Open/Prime/Close -> noErr, Control -> noErr for the known
    video csCodes (SetMode/SetEntries/SetGamma/GrayPage/... ) else
    controlErr, Status -> cscGetMode fills a VDPageInfo (csMode $80,
    csBaseAddr = 0x00F08040 = VRAM+$8040), cscGetPageCnt -> 1 page,
    cscGetBaseAddr -> 0x00F08040, else statusErr.  Header offsets +
    name verified via objdump.
  - `scripts/se30-build-declrom.py`: now emits the driver as an sBlock
    (leading physical length + driver bytes), an sDriver directory
    (`OSLstEntry sMacOS68000 -> driver`), and adds `sRsrcDrvrDir` (id
    $04) to the Video sResource.  Auto-assembles the .s if the .bin is
    absent/stale (no committed binary; nothing but GNU binutils needed).

**Progress (live-traced):**
  - The previous hang (MacOS Slot Manager sReadStruct/sGetDriver copy
    loop at RAM 0x000eccfe, reading a bogus driver length) is GONE --
    the length-prefixed sBlock is copied correctly and the driver
    loads.
  - Boot now advances to ROM **0x40802432** and stops there:
    `tstb 0x172 / bnes 0x40802432` -- a spin waiting for low-mem byte
    0x172 to clear.  This code path is NOT taken by the default (no
    -decl-ROM) boot (breakpoint never hit) -- it is a NEW path the ROM
    runs only after enumerating the video card, i.e. genuine forward
    progress into the video/startup sequence.

**New exact stopping point (documented):** 0x172 is set to 0x80 at
early init (ROM 0x4080038a) and cleared by an **ADB command-completion
callback** at ROM 0x408074ce (`...; andib #-128,%d2; moveb %d2,0x172`;
a3 = the ADB Manager globals, a3@(356) = the reply byte) which runs
only when an ADB transaction completes (VIA1 SR interrupt -> ROM ADB
ISR -> this callback).  During the hang, `info via` shows VIA1
IFR=0x40 (T1 only), i.e. **no ADB SR interrupt pending** -- the ROM's
post-video startup expects an ADB transaction to complete here but none
is in flight, so 0x172 stays 0x80 forever.  (Note: 0x172 keeps bit 7 =
d2&0x80, so it only reaches 0 when the ADB reply byte a3@(356) has bit
7 clear -- the completion must both fire AND carry the right reply.)

**Leads for the next step (making the video-startup ADB handshake
complete):**
  - The decl ROM currently answers ALL slots 9-14 (the 24-bit slot
    addresses alias to 0x00FFFFFF), so the ROM may be processing 6
    duplicate video cards; constraining it to only slot $E could change
    this path.  Worth ruling in/out first.
  - Determine what the ROM does immediately before 0x40802432 (what
    sets up a2=0xa000bb2c and whether it initiates an ADB call whose SR
    interrupt our model should raise but doesn't in this early context)
    -- single-step from where the video-found path diverges from the
    default boot.
  - Possibly the ROM's video PrimaryInit / driver Open needs to run (or
    return something) to satisfy this; check whether MacOS/ROM actually
    called our driver's Open/Control/Status yet (add a trap-log to the
    driver, or trace the DCE), and whether it expects the video VBL IRQ
    (route slot $E VBL through GLUE).

Env-gated as before (`MACSE30_DECLROM`), so the default machine is
unchanged and still boots MacOS (verified 10 distinct sampled PCs).
Reproduce:
    python3 scripts/se30-build-declrom.py /tmp/se30_declrom.bin   # auto-builds the driver
    MACSE30_DECLROM=/tmp/se30_declrom.bin build/qemu-system-m68k -M macse30 \
      -bios /workspace/files/mac-roms/macIIx.rom -drive file=...,if=scsi \
      -snapshot -display none -icount shift=7

### SE/30 continued #7 (2026-08-25, `fix-se30`): slot-$E-only decode; boot reaches the ROM's video/cursor startup; final stop = a mouse-completion wait

Final SE/30 round.  Two things done; render still one handshake away,
now precisely characterized.

**1. Constrained the decl ROM to slot $E ONLY (done).**  Traced a5 in
the ROM's slot loop: it probes each slot $s's decl ROM at 0xF[s]FFFFFF
(32-bit standard slot space), slot $E = **0xFEFFFFFF**.  The card had
answered ALL of slots 9-14 because the machine's A31 24-bit-alias
handler masks 0xF9..0xFEFFFFFF -> 0x00FFFFFF (where the decl ROM used
to live).  Fix (macse30.c): map the decl ROM (ending at 0xFEFFFFFF) and
64KB VRAM at slot $E's own space **0xFE000000**, at a priority ABOVE the
A31 handler.  Now only 0xFExxxxxx hits the card; the other slots fall
through the A31 alias to unbacked RAM (read 0 = no card), so MacOS finds
exactly one video board.  Driver `FB_BASE` updated to 0xFE008040
(= slot $E base + minorBaseOS $8040).

**2. Boot advances into the ROM's post-video startup (real progress).**
The chain now runs: decl ROM validated -> board enumerated (once) ->
driver loaded -> ROM sets up and shows the cursor (traps 0xA851
_SetCursor then **0xA853 _ShowCursor** at 0x40802422/24) -> enables
interrupts -> then stops at **0x40802432** (`tstb 0x172 / bnes`), a spin
waiting for low-mem 0x172 to clear.

**Final stop, fully characterized:** 0x172 (set 0x80 at init) is cleared
only by the ROM handler at **0x408074ce**, which is registered (via
`lea 0x408074ce,a0; movel a0,a3@(4,d1)` in the ADB device-table setup)
as the completion callback for **ADB device 3 = the mouse**, and clears
0x172 to `a3@(356) & 0x80` -- i.e. only when the mouse-reply byte's bit 7
is 0.  Live tracing shows:
  - The ADB engine is fully active during the hang: **1047** VIA1
    shift-register STATE transitions, actively Talk-polling device 3
    (command byte 0x3c = Talk mouse R0) and device 2 (0x2c = keyboard).
  - BUT the completion callback 0x408074ce is **never invoked**
    (breakpoint never hits) and VIA1 IFR shows no lingering SR IRQ --
    so the mouse polls are not driving the registered per-device
    completion path that would clear 0x172.
  - Autopoll is OFF at this point (`en=0`), so the ROM expects the
    explicit mouse Talk it is issuing to complete through that callback.
This is the SE/30-specific twist of the same VIA1/ADB shift-register
engine the pre-video ADB fix (#1) already exercises: the post-video
startup needs the explicit device-3 (mouse) Talk to complete *through
the ROM's registered completion callback* (0x408074ce) with the reply
byte's bit 7 clear.  Either our ADB path isn't delivering that specific
command's completion into the callback, or the mouse reply byte keeps
bit 7 set (button-up) so 0x172 never zeroes.  (The coordinator's VBL
hint was chased too: the clearer is a mouse-completion callback, not a
VIA1-VBL task, and the VIA1 60Hz VBL already fires -- so a VBL route
does not clear 0x172; the missing piece is the device-3 completion.)

**Next-session leads (crisp):**
  - Single-step one 0x3c (Talk mouse R0) round-trip through
    macse30_adb_send/receive and the ROM's ADB ISR, and check why the
    per-device completion vector a3@(4, 3*12) (0x408074ce) is not
    called on it -- vs. how the pre-video keyboard Talk (which DOES
    complete) differs.
  - Check the mouse reply byte our ADB mouse returns for Talk R0 when
    idle: if bit 7 (button) is always 1, 0x172 can never reach 0 even
    once the callback fires -- may need the mouse to report button-up
    as bit7=0 here, or the ROM genuinely waits for the callback path,
    not the byte.
  - Consider whether the SE/30 mouse sits at a different ADB address
    after the ROM's startup readdressing, so device 3 Talk times out.

Status: env-gated (`MACSE30_DECLROM`) so default `-M macse30` still
boots MacOS unchanged (verified 11 distinct sampled PCs).  No render
yet -- VRAM (now at 0xFE008040) stays blank because boot stops at the
mouse-completion wait before QuickDraw's first desktop draw.  Committed
the slot-$E-only decode + driver base; ADB (`e40b39c3`), SCSI
(`e3082f0d`), decl-ROM (`515582e3`) and driver (`3aacfa0c`) commits
intact.  macse30.c + scripts only; maciici/q800/compact Macs untouched.

**Cumulative SE/30 arc (this task):** diagnosed the video as a slot
declaration-ROM device -> synthesized a from-scratch, CRC-byte-exact
decl ROM the Slot Manager ENUMERATES -> hand-wrote a 68k slot video
driver that LOADS -> constrained to a single slot-$E card -> boot now
runs through decl-ROM enumeration, driver load, and into the ROM's
video/cursor startup, stopping at one well-characterized ADB mouse
-completion wait.  The hard reverse-engineering (format/CRC/byte-lanes/
enumeration/driver-load) is all done and committed; the remaining gap
is that single device-3 completion.

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
