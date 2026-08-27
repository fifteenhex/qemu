# Macintosh LC 550 / Color Classic II / Performa 550 — session notes (cc2-finish)

Continuation of `CCLASSIC-NOTES.md`.  Prior sessions got the boot past the
Egret cold-start handshakes and machine-ID/capability config but parked it
diverting into the serial diagnostic console ("MicroBug", 0x408b989a) at the
end of machine-config.  This session root-caused that divert and four more
blockers, advancing the boot all the way through the machine-config POST
(including the onboard-video memory test) and into the post-MMU,
interrupt-driven Egret ADB driver.  Screenshot of the furthest state:
`/tmp/maclc550-furthest.png` (the onboard framebuffer scanning out the raw
video-POST walking test pattern that is still sitting in VRAM — i.e. VRAM is
now correctly mapped and displayed; the OS has not yet set up QuickDraw).

## Root-cause fixes this session (all in hw/m68k/maclc550.c)

1. **ROM self-checksum re-sign (THE config→console divert).**
   The relocation-table patch prior sessions apply at ROM file offset 0x3d42
   (0x00a03d52 → 0x40803d52) changes the ROM image, and the power-on
   self-test at ROM `0x40847a24` sums the ROM as 16-bit words (into a 32-bit
   accumulator) from ROM+4 and compares the result against the stored 32-bit
   checksum longword at ROM+0 (0xede66cbd).  The patch raised the word-sum by
   (0x4080−0x00a0)=0x3fe0, so the self-test failed (`d6`=0xffff), and
   `0x408472d0` diverted via `0x4084a6f6 → 0x40847542 → 0x4084a704 →
   0x408b989a` to the console.  **Fix:** after the relocation patch, bump the
   stored checksum at ROM+0 by the same 0x3fe0 (ROM+0 is read as the compare
   target before the sum loop starts at ROM+4, so it is itself outside the
   summed range).  Confirmed live: computed sum 0xede6ac9d now matches the
   re-signed stored value; `d6`=0 and the boot no longer diverts.

2. **Onboard VRAM base is 0x60B00000, not 0xf9000000 (THE video POST).**
   The 0xf9000000 Valkyrie placement was copied unconfirmed from `lc475.c`
   (a 68040 Quadra ROM).  The Sonora "Eric" ROM instead probes and
   memory-tests its VRAM at physical **0x60B00000**: the kind-7 video
   capability init writes the `'256K'`/`'768?'` size signatures and a
   0x6DB6DB6D walking pattern there, and stores 0x60B00000 as the video
   device-globals base (`a4@(34)`).  With that region unmapped, the bit-18
   video capability's dereference faulted and the machine-config POST bailed
   to the console (`d0`=0x40000 at `0x40847042` → `0x4084a6f6`).  **Fix:**
   `MACLC550_VRAM_BASE = 0x60B00000`; the framebuffer + its aliases are mapped
   there (overlap priority 1) over a low-priority catch-all logging stub
   (`sonora_probe`, 0x60000000+16 MiB) that keeps any still-unmodelled
   register probes in that window from faulting the POST.  Found by mapping
   the catch-all stub first and observing the `'256K'` signature + walking
   pattern writes to 0x60B0xxxx.

3. **Session-framed SET_PRAM byte-handshake reply (Egret PRAM writes).**
   The polled boot driver frames pseudo-commands whose reply it reads back
   over the /XCVR byte-handshake as a formal-session SEND followed by that
   read (ROM 0x408b3a80: release the session, spin at 0x408b3a9c until PB3
   goes low, then clock the reply straight out of SR).  The generic session
   `[01 xx]` handler answered no-response, which never lowers /XCVR, so the
   driver hung.  **Fix:** `maclc550_egret_session_pseudo_reply()` — for
   write/set commands (SET_PRAM 0x0c &c.) build the real reply via the shared
   pseudo builder (SET_PRAM actually writes PRAM now) and drive it out through
   the byte-handshake delivery, closing the formal session.  Plus a teardown
   for the stale, fully-drained delivery when the driver reads fewer bytes
   than staged (its 3-byte `[00 00 00]` status vs our 4-byte header) and then
   opens a fresh send.

4. **Session-framed GET_PRAM returns real data (XPRAM validity).**
   The GET_PRAM driver (ROM 0x408b3b6e) does a receive-turnaround and keeps
   the *last* of four reply bytes; a no-response turnaround left junk there,
   so the ROM read a corrupt XPRAM (the seeded 'NuMc' signature, the ddType at
   0x76/0x77, etc. all came back 0), decided parameter RAM invalid and
   reinitialised it.  **Fix:** stage `[00 00 00 <value>]` so the real byte
   lands in the 4th slot the receive-turnaround delivers.

## Current blocker (this session's stopping point)

Boot reaches the **post-MMU, interrupt-driven Egret ADB driver** (0x408d17xx,
VBR relocated).  It sends an autopoll-parameter upload `[01 08 addrHi addrLo
… 12 bytes]` = **WRITE_MCU to MCU addr 0x0110** over the *formal-session*
framing, then spins at **0x408d180a** on VIA1 IFR bit 2 waiting for a
transaction-complete shift interrupt that never comes.

Both obvious completions REGRESS to a **DOUBLE MMU FAULT** (wild PC, VBR=0):
- routing WRITE_MCU through the no-response receive-turnaround → the ISR
  (0x40814912-family) copies the junk reply through the request block's
  uninitialised data pointer (exactly the hazard the existing
  `egret_process` "generic-ACK vs interrupt-only" comments warn about);
- scheduling a final completion interrupt after the byte-handshake reply
  drains → the ISR fires against no pending request and walks a stale
  pointer.

So the WRITE_MCU here is neither a plain byte-handshake reply nor a plain
receive-turnaround.  **Next step:** determine whether this ISR-driven driver
actually opened a real formal session (in which case the session-close ack
interrupt is what it wants, delivered without re-entering the ISR against a
stale request), or whether the session-open detector is mis-flagging its
byte-handshake framing as a session (in which case it should be routed to the
direct pseudo path `pseudo_try_process` the ISR was designed around).  The
unused `egret_session_pseudo` flag was left in place as the hook for the
former.  Instrument VIA1 IER/ACR and the A092 request block at lowmem 0x1d8 /
0x192 across the WRITE_MCU exchange.

## Build / test commands

    # build dir is on tmpfs; source worktree is /workspace/src/qemu-cclassic
    ninja -C /tmp/cc-build qemu-system-m68k

    /tmp/cc-build/qemu-system-m68k -M maclc550 \
      -bios /workspace/files/mac-roms/maclc550.rom \
      -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
      -snapshot -serial null -serial null -display none -icount shift=7 \
      -monitor unix:/tmp/cc.sock,server=on,wait=off \
      -d unimp,guest_errors -D /tmp/maclc550.log
    # gdbstub: add -S -gdb tcp::1234 ; gdb-multiarch: set architecture m68k / set endian big
    # screendump via the monitor: screendump /tmp/shot.ppm  (PPM→PNG with PIL)

## Session 2: cracked the WRITE_MCU completion; new frontier is READ_MCU stream length

**The WRITE_MCU wall is solved.**  The post-MMU interrupt-driven Egret
driver (0x408d17xx) sends its autopoll-parameter uploads
(`[01 08 addrHi addrLo …]` = WRITE_MCU) over the FORMAL-SESSION framing
and then reads the reply back over the /XCVR byte-handshake, exactly like
the polled SET_PRAM driver — but with two crucial differences that made
both naive completions double-fault:

1. **It needs a final "session-closed" shift interrupt.**  After draining
   the whole reply and seeing /XCVR rise, the driver tears the session
   down (`orib #48` -> both handshake lines high at 0x408d17de) and then
   spins at **0x408d180a** on VIA1 IFR bit 2 for one completion interrupt
   to run its completion routine (0x408d1838).  Fix: raise that single
   interrupt in `maclc550_egret_session_update`'s pseudo-closing handler,
   on the teardown write (flagged by `egret_session_pseudo`).  Raising it
   any earlier (at the last reply byte) makes the driver read a phantom
   extra SR byte -> fault.  The poll is at **I:7 (masked)**, so the
   interrupt is consumed by the poll, not the ISR — no re-entry.

2. **It discards one turnaround byte, so byte counts must line up.**  Its
   completion computes `d0 = stored_byte_count - 4` and `dbf`-copies d0+1
   bytes; if fewer than the 4 header bytes land in the request block, d0
   underflows to ~0xFFFE and smashes the stack (reaches _InitGraf at
   0x40802842 on a corrupted A7 -> double fault).  The driver reads+discards
   one turnaround byte (0x408d1732) before it starts STORING, so
   `session_pseudo_reply` prepends a dummy byte for WRITE_MCU: the discard
   eats it and the full `[00 00 00 cmd]` header still lands
   (stored_count==4, d0==0).  Verified live: request block at 0x17ff9c,
   a2@16==4, clean completion.

**Result:** WRITE_MCU no longer double-faults; the boot runs the whole
autopoll/PRAM/MCU setup and advances into config/init regions
(0x40800xx/0x40802xx/0x40814xx/0x40848xx) it never reached before.

**New frontier — READ_MCU stream length.**  READ_MCU (`[01 02 addrHi
addrLo]`) over the same ISR framing is a *streamed* read (the wire carries
no length; the host reads until it has enough, then closes).  Routed
through the byte-handshake it now returns real MCU data and the driver's
count-exhausted close is detected (`orib #48` -> both lines high, the new
deassert-edge teardown that drops /XCVR — without it the driver deadlocks
at its next send 0x408d15a2 waiting for /XCVR to rise).  BUT the model
stages the whole 240-byte MCU window, so the driver reads its entire
16-byte header buffer (~21 bytes total) instead of just the bytes it
wants; its completion then computes `d0 = 21-4 = 17` and over-copies
18 bytes -> stack smash on the completion interrupt.  Without the
completion interrupt it instead hangs at 0x408d180a.

The request block encodes the wanted length in `a2@14` (data count) /
`a2@18` (header count, =16 observed); the fix needs the READ_MCU reply to
END (/XCVR rise) after exactly that many bytes so `a2@16` matches what the
completion's `-4` expects.  The QEMU model can't see the guest a2 register
directly; options for next session: (a) read the request block via the
low-memory Egret queue head (0x1d8 chain) to recover the length, or
(b) stage exactly the 4-byte header + only the addressed MCU bytes the
WRITE_MCU wrote (track written extents), so the driver's own count
exhausts at the right place.  Furthest screenshot: `/tmp/cc-s2-furthest.png`
(still the onboard VRAM POST test pattern; pre-QuickDraw).

Debug tip: gdbstub on **:1237** (the IIvx agent uses :1234).  Break at
0x408d187c to read the completion's byte-count math
(`a2@16`/`a2@14`/`a2@18`), and at 0x408d1838 for the request block.

## Session 3: READ_MCU length matched by snooping the request block

Implemented the coordinator's principled fix for the streamed READ_MCU
(0x02).  The interrupt-driven driver's request block is in low RAM and
`a2` points at it while it shifts the reply in; the two byte counts it
will read live at **a2@14** (its a2@24 buffer -- the completion strips
the 4-byte protocol header via `d0 = a2@16-4`) and **a2@18** (its a2@20
buffer, where the bulk of the READ_MCU payload lands).  In
`maclc550_egret_session_pseudo_reply`, for READ_MCU we now snoop those
counts directly from guest memory:

    CPUM68KState *env = &M68K_CPU(current_cpu)->env;
    uint32_t rb = env->aregs[2] & 0x00ffffff;          /* low RAM = 1:1 */
    uint16_t ndata = lduw_be_phys(&address_space_memory, rb + 14);
    uint16_t nhdr  = lduw_be_phys(&address_space_memory, rb + 18);

and rebuild the reply to exactly `[00 00 00 02] + real MCU bytes` filling
both the (ndata-4) and nhdr regions, so the driver reads precisely what it
declared and closes on its own count.  Verified live with gdb (:1237):
`a2=0x17ff9c a2@16=4 a2@14=0 a2@18=1 -> d0=0`, a clean completion with no
over-run.  Snoop log confirms the reads: `ndata=4 nhdr=16 addr=0x0110`,
`ndata=4 nhdr=4 addr=0x0108`, etc.

Also: the completion shift interrupt (WRITE_MCU-style) is now gated on the
CPU running its completion spin with interrupts MASKED (I:7).  WRITE_MCU
spins at 0x408d180a at I:7 and needs it; READ_MCU runs its tail at I:1 and
must NOT get it (the raised SR line would re-enter the Egret ISR against an
un-armed request).

**Result:** READ_MCU completes cleanly; the boot runs the whole
autopoll/ADB-parameter round-trip (WRITE_MCU 0x0110/0x0108 then READ_MCU
0x0110/0x0108/0x010c/0x018a/0x01ae/0x0156...) and advances OUT of the
Egret MCU setup into the ADB Manager / OS bring-up (new code at
0x40809xxx/0x4080axxx) it never reached.

### New frontier: level-1 interrupt during warm re-init (VBR=0)

Past all the READ_MCUs the boot reaches early-ROM 0x4080023e running at
**I:1 with VBR=0** -- i.e. an apparent warm re-init before interrupt
vectors are (re)installed -- and takes a **level-1 (VIA) autovector
interrupt (frame vector 0x64)** through the still-zero vector table:
`*(0x64)` reads garbage (0x00040102, low RAM holding the early RAM-test
walking pattern 0x6DB6DB6D that also litters 0x180000+ on the stack), the
CPU executes it, bus-errors, and the bus-error vector `*(0x8)` is also
zero -> jump to 0xE -> DOUBLE MMU FAULT.  Deterministic, disk-independent.

This is one layer past READ_MCU: a spurious/early level-1 VIA interrupt
(60Hz tick, one-second tick, or an Egret SR edge) firing in the window
where VBR=0 and no handler is installed.  Next session: catch the level-1
vector dispatch (break where the OS installs *(0x64), or on the VIA IFR
read in the ISR) to identify which VIA source fires, and either hold it
off until the handler is armed or confirm/fix why the re-init runs at I:1
with VBR=0 (is 0x4080023e a legitimate second-boot entry that should mask
interrupts first?).  Furthest visible state unchanged: the onboard-VRAM
POST pattern (`/tmp/maclc550-s2-furthest.png`), still pre-QuickDraw.

### Session 3 addendum: the crashing interrupt is VIA1 T2

Pinned the level-1 source with gdb halted at 0x4080023a (the `movew
#8192,%sr` that enables interrupts), reading the VIAs directly:

    VIA1_IFR=0xe2  VIA1_IER=0xa4        -> IFR&IER&0x7f = 0x20 = T2 pending+enabled
    RBV_IFR=0x00   RBV_IER=0x80
    vec *(0x64)=0x00040100  (level-1 autovector, VBR=0)
    vec *(0x8) =0x00000000  (bus-error vector)

So a **VIA1 Timer-2 interrupt** is pending and enabled at the instant the
ROM startup enables interrupts, but the level-1 handler is not installed
yet: `*(0x64)` points at 0x40100 in still-uninitialised low RAM (the
handler body is written by the 0x40800be0 call one instruction later, at
0x4080023e).  The T2 int fires into that garbage -> bus error -> the
bus-error vector `*(0x8)` is also 0 -> double fault.  The ROM clearly
expects NO VIA interrupt pending across that enable->install window, so
the real question is why VIA1 T2 is left pending+enabled here.

Candidates for next session:
 - Our mos6522 T2 may be latching IFR bit5 without the ROM's expected
   clear (T2 is used by the SETUPTIMEK calibration and the Time Manager;
   the SETUPTIMEK hack stuffs TimeDBRA/TimeSCCDB but does not touch the T2
   IFR).  Check whether real flow clears T2_INT before this point, and
   whether the hack should also clear IFR bit5 / IER bit5.
 - Or VBR is legitimately 0 here and the ROM relies on the 0x40800be0
   install running before any tick -- in which case hold the VIA1 T2 (and
   any other level-1 source) off until the guest has armed *(0x64), or
   confirm the enable/install ordering matches a sibling that boots.
Reproduce: gdb :1237, break *0x4080023a, read 0x50f01a00/0x50f01c00.

## Session 4: T2 root-caused, Egret control-reply underflow fixed; boot reaches QuickDraw gray desktop + ADB Manager

Two fixes (both local to `hw/m68k/maclc550.c`, commit `acc1c361d8`) take the
boot from the post-READ_MCU double fault all the way into QuickDraw (a clean
gray boot desktop -- the walking VRAM POST pattern is gone) and the ADB
Manager device install.  Furthest screenshot: `/tmp/cc-s4-furthest.png`
(uniform 50% gray desktop, pre-menu-bar).

### (a) The T2 root cause: NOT the SETUPTIMEK teardown -- a genuine one-shot

Instrumented every VIA1 IER/T2CL/T2CH write and IFR read up to 0x4080023a.
The pending T2 at the crash is **not** left over from the SETUPTIMEK
calibration (that runs far earlier at 0x40800858 and its T2 is separate).
It is the **Time Manager's own one-shot T2**: at 0x4080b0fa the OS enables
IER T2 (writes 0xa0) and at 0x4080b12e loads T2 with ~0xfffb (~84ms).  That
one-shot then expires during the long *polled* Egret shift-register spin
(0x408d15f6, hundreds of IFR-bit2 polls) that runs before the OS reinstalls
its vectors -- so VIA1 IFR bit5 latches legitimately.

The deeper structure: 0x4080023a is a **warm re-init** reached AFTER the
Egret setup.  The POST had installed a valid vector table (confirmed: at the
early 0x408d15f6 spin `*(0x8)=0x408026f0`, `*(0x64)=0x40809b60`), but the
OS-bring-up re-entry **clears low RAM**, zeroing that table, and only
reinstalls handlers a few instructions AFTER `movew #8192,%sr` unmasks
interrupts.  So the pending T2 dispatches through a zeroed `*(0x64)` ->
bus error -> zeroed `*(0x8)` -> double fault.

**Fix (local, glue):** model what real hardware does -- never dispatch an
interrupt into an uninstalled vector table.  `maclc550_glue_set_irq` now
gates the CPU IPL on the bus-error vector `*(VBR+8)` being non-zero
(`maclc550_vectors_uninstalled`).  While the table is zeroed the latched
sources stay pending in their IFRs; the next glue re-eval after the table
is installed -- guaranteed <=16ms later by the 60Hz CA1 tick -- delivers
them cleanly.  Verified: single-stepping 0x4080023a now goes ->0x4080023e
->0x40800be0->0x40800242 with no interrupt taken; T2 is delivered later
against valid vectors.  **No shared mos6522.c change was needed.**

### (b) Second blocker, unmasked by (a): Egret control-reply completion underflow

Past the T2 fix, a new double fault: something copied ~64KB across low
memory from destination 0, wiping the vectors and the boot globals
(MemTop/BufPtr at 0x108/0x10c, MMFlags/SysZone-adjacent at 0x2a6), so the
OS's stack computation at 0x40800490 (`A0 = ((BufPtr + [0x2a6])>>1) - 1024`)
produced a wild `A0=0x562c43b6`, `moveal a0,sp` (0x40800246) corrupted SP,
and the next A-trap cascaded to a double fault.

Traced the copy to the interrupt-driven Egret driver's completion
(0x408d1838-0x408d1892): `d0 = a2@16 - 4; dbf d0 { *dest++ = *src++ }`.
For the `[01 1b]` autopoll-enable control command the driver stored only
**3** reply bytes, so `d0 = 3 - 4 = 0xFFFF` (word underflow) and dbf copied
0x10000 bytes to dest 0.  Root cause: the post-MMU interrupt-driven driver
discards ONE turnaround byte (the read+discard at 0x408d1732) for **every**
one of its pseudo-commands, but the Session-2 dummy-byte pad only covered
WRITE_MCU 0x08 / READ_MCU 0x02 / GET_TIME 0x03 -- the `[01 1b/1c]` control
commands were unpadded, lost the discard from their own 4 bytes, and
underflowed.

**Fix (local):** key the pad on the interrupt-driven driver being active
rather than on the command byte -- that driver is exactly the one running
with VIA1's SR interrupt enabled (IER bit 2), whereas the polled boot-ROM
SET_PRAM driver (0x408b3a80, which does NOT discard) runs with it disabled.
`if ((s->ier & SR_INT) && ...)` prepend `0x00`.  Verified: no more low-mem
corruption; boot runs PRAM init, MCU reads, ADB reset and reaches QuickDraw.

### New frontier: ADB Manager device install hangs (a3@349 bit5 never clears)

The boot now spins forever at **0x4080a870**:
`andiw #-1793,%sr` (enable interrupts) then `btst #5,%a3@(349); bnes .`,
with `a3 = ADBBase` (lowmem 0xcf8 = 0x5890 -- confirmed, this is the ADB
Manager globals).  `a3@349` is initialised to 0x24 (bits 5+2) at 0x4080a846
and the loop waits for bit5 to clear.  It is a bitmask of pending async ADB
device operations: **bit2's operation completes (0x24 -> 0x20) but bit5's
never does.**  The interrupt-driven Egret ADB driver runs its whole init
sequence in the level-1 SR interrupt (that is the ~187k level-1 "storm"
during the spin: real work, ~100us/exchange, not an idle storm) -- PRAM
init (`[01 0c ..]`), GET_PRAM, a long run of READ_MCUs of the autopoll
buffers (`[01 02 01 56/80/8b/ab/bc..]`), self-test `[01 01]`, ADB reset
`[00 00 00 00]` -- then **settles** (egret command count goes flat) with
bit5 still set.  Deterministic; disk is never touched (0 SCSI-region
accesses), so this is upstream of boot-device selection.

Diagnosis: the Egret model does not actually run ADB **autopoll** and
populate the MCU autopoll buffers with real device (keyboard/mouse) data,
so the ADB Manager's readback of those buffers comes back empty and the
device whose completion clears bit5 never reports.  Next session: model the
Egret autopoll -- when the driver reads the MCU autopoll-buffer addresses
(the `[01 02 01 xx]` READ_MCUs) return the actual ADB device register data
(via `adb_request` to the keyboard at addr 2 / mouse at addr 3), or drive
the ADB completion/service-request that clears the ADBBase pending bit.
Reproduce: gdb :1237, break *0x4080a870; `a3=ADBBase=0x5890`, watch
`*(char*)0x59ed` (starts 0x24, reaches 0x20, stuck).  Furthest screenshot:
`/tmp/cc-s4-furthest.png`.

## Session 5: ADB Manager rescan unblocked (interrupt-driven plain-ADB completion); autopoll-data delivery is the new frontier

The Session-4 ADB Manager hang (spin at 0x4080a870 on ADBBase[349] bit5) is
FIXED (commit `ff643a3716`).  Root cause and fix:

**ADBReInit's SendReset had no completion.**  ADBBase (lowmem 0xcf8) offset
349 is a producer/consumer bitmask; bit5 clears only at 0x4080a958 once the
ADB bus rescan finishes.  The rescan is kicked off by a plain ADB SendReset
`[00 00]` that ADBReInit sends over the POST-MMU interrupt-driven Egret driver
(0x408d17xx) -- the same driver that sends the `[01 xx]` pseudo-commands, which
discards one turnaround byte at 0x408d1732 and whose completion strips a 4-byte
header via `d0 = stored_count - 4`.  The model delivered plain ADB commands as
the raw 2-byte no-response turnaround (only 1 byte survived the discard), so the
reset completion never fired and the rescan never advanced.  **Fix:** when that
driver is active (VIA1 SR interrupt enabled, `s->ier & SR_INT`) deliver plain
ADB replies exactly like pseudo-commands -- a `[00 00 00 adbcmd]` header (+ any
register data) + a dummy discard byte + the completion shift interrupt (in the
`c[0] != 0x01` branch of `maclc550_egret_process`).

**Result:** bit5 clears, ADBReInit returns, and the boot runs the full ADB bus
rescan -- Talk R3 to every address (`[00 3f] [00 4f] ... [00 ff]`, plus
`[00 21]` to addr 2) -- then advances to a new wait at **0x40802a38**
(`tstb 0x172; bne`), reached right after it sets up the cursor
(_SetCursor/_ShowCursor A-traps) and enables interrupts.  Still the gray
QuickDraw desktop; furthest screenshot `/tmp/cc-s5-furthest.png`.

### New frontier: autopoll device-data delivery (MCU 0x1b8 poll loop)

lowmem **0x172** is set to 0x80 (waiting) and cleared only at **0x408b67ea**
(`clrb 0x172`), inside an ADB completion handler (0x408b67c4) that fires when a
completion arrives *for a specific awaited device* -- `a2@4 == ExpandMem
(0x2b6)[480]@4` and `a1@21 == 0` -- after which it posts an event (_PPostEvent,
A-trap 0xa02f).  While the main thread spins at 0x40802a38, the interrupt-driven
ADB driver loops **forever** on `READ_MCU 0x1b8` / `WRITE_MCU 0x1b8` (it writes
`[56 e0 00 0e]` and reads it straight back -- the model just echoes MCU RAM).
Confirmed with gdb (:1237, break `*0x408b67c4`): that handler is **never
reached** during the stall, so 0x172 never clears.

Diagnosis (matches the coordinator's autopoll hypothesis): the Egret model does
not run **autopoll** -- it never polls the ADB keyboard(2)/mouse(3) and delivers
the awaited device's data through the MCU 0x1b8 autopoll buffer (or as an
unsolicited /XCVR packet) in the shape that routes a completion to 0x408b67c4.
So the awaited-device completion never happens.  Next session: model Egret
autopoll -- when the driver arms autopoll (the `[01 1b]/[01 1c]` control
commands + the `WRITE_MCU 0x1b8` poll config), periodically `adb_poll()` the
bus and stage the polled device's register-0 data so `READ_MCU 0x1b8` returns a
real device response (not the echoed config), making the driver dispatch a
completion for the awaited device and clear 0x172.  Reproduce: gdb :1237, break
`*0x40802a38` (main spin) and `*0x408b67c4` (the 0x172 clear, never hit);
`egret: pseudo READ_MCU addr=0x01b8` / `WRITE_MCU addr=0x01b8` loop in the log.

## Session 6: mapped the autopoll completion chain; devices DO register, the awaited autopoll completion is the gate

Chased the MCU 0x1b8 autopoll loop deeper (temp-logged the guest call chain
from the model's READ_MCU 0x1b8 handler, then disassembled + gdb-dumped state
at the 0x40802a38 stall).  Findings:

**The ADB rescan actually SUCCEEDS.**  With the Session-5 plain-ADB completion
fix, the bus rescan registers both real devices in the ADBBase (0xcf8 = 0x5890)
device table:
  - e0 (ADBBase+0):  addr 2, handler 2  -> keyboard, compl 0x0007bc8a
  - e1 (ADBBase+12): addr 3, handler 3  -> mouse,    compl 0x408b6582 (ROM
    mouse register-0 parser: btst #6 button / delta decode)
ADBBase[349] is 0x04 (bit5 cleared -> rescan finished).  So registration is
NOT the problem.

**The gate is an awaited autopoll completion.**  Main thread spins at
0x40802a38 (`tstb 0x172; bne`); lowmem 0x172 (set 0x80) clears only at
0x408b67ea, inside the ADB completion handler 0x408b67c4, whose guard is
`a2@4 == ExpandMem(0x2b6)[480]@4`.  Dumped live: ExpandMem[480] = 0x5820,
0x5820@4 = **0x5840, an all-zeros block** -- so the awaited completion targets a
device-control block that no completion ever names.  0x408b67c4 is never
reached during the stall (gdb bp confirms), so 0x172 never clears.

**The autopoll poll chain (mapped).**  The interrupt-driven driver loops
forever on READ_MCU 0x1b8 / WRITE_MCU 0x1b8, writing `[56 e0 00 0e]` and reading
it straight back (the model just echoes egret_mcu_mem).  The guest call chain
for the poll is: VIA1 ISR dispatcher 0x40809bc0 -> jump-table dispatch
0x40809a04 -> ADB autopoll state machine 0x40801640 (d6-bit state machine) ->
Egret MCU-access helper 0x408b37xx (builds READ/WRITE_MCU 0x1b8 = 0x100+0xb8,
uses movew #258/259/264/265 = MCU-addr command selectors) -> interrupt-driven
driver 0x408d17xx.  It is polling the keyboard (addr 2).

**What is still needed (next session).**  The Egret must run autopoll: on
READ_MCU 0x1b8 return the *polled device's* real register-0 data (keyboard
`[0xFF,0xFF]` no-key / mouse no-move-button-up) instead of echoing the config,
in the buffer layout the 0x40801640 state machine parses, so it dispatches a
completion whose a2@4 == 0x5840 and 0x408b67c4 clears 0x172.  The exact 0x1b8
record layout (status/flags + polled-addr + register bytes) still needs a
clean single-step of the parse: the ROM around 0x40801640 / 0x408b37xx
disassembles misaligned (jump-table + parameter-block calling convention), so
linear disassembly wasn't enough.  Recommended: break at the driver send for
the READ_MCU 0x1b8 (model logs `pc=408d172c` for it), `finish` back into
0x408b37xx, and single-step until the read result in the a2@20 buffer is
compared -- that comparison reveals which byte/bit means "device @addr N
returned data D".  Then synthesize that record from `adb_poll()` and deliver it
over the existing interrupt-driven framing (discard byte + `[00 00 00 ..]`
header + SR completion), fired on the autopoll cadence.

Furthest screenshot unchanged: `/tmp/cc-s5-furthest.png` (gray QuickDraw boot
desktop; cursor set up; past ADB reset + full rescan + device registration;
stalled on the awaited autopoll completion).  Reproduce: gdb :1237, break
`*0x40802a38` (main spin) and `*0x408b67c4` (the 0x172 clear, never hit);
`READ_MCU addr=0x01b8` / `WRITE_MCU addr=0x01b8` loop in the -d unimp log.

## Session 7: autopoll mouse-rendezvous SOLVED + A31 24-bit forwarding; boot reaches System startup (QuickDraw dialogs); new frontier is an illegal-instruction bomb

Two fixes take the boot off the gray QuickDraw desktop all the way into
System startup -- QuickDraw now draws full dialogs (cursor, bomb icon,
text) where it previously sat on uniform gray.  Commits `118501462f`
(autopoll) and `2b6e9010d1` (A31 forwarding), both local to
`hw/m68k/maclc550.c`.

### Diagnostic correction: the 0x1b8 loop is NOT autopoll

Sessions 5-6 identified the steady-state `READ_MCU 0x1b8` / `WRITE_MCU 0x1b8`
loop (`[56 e0 00 0e]` written and read back) as the ADB autopoll.  It is
NOT.  Single-stepped it out: it is the ROM's periodic **XPRAM counter task**
at `0x4080c8ec` -- `_ReadXPRam`/`_WriteXPRam` of XPRAM offset 0xb8 (d0 =
0x400b8) on a 300ms Time Manager one-shot (`a05a`/`PrimeTime` re-arm), which
reads a longword, increments a 19-bit counter field and writes it back.
MCU address 0x1b8 = 0x100+0xb8 is XPRAM (the model already maps 0x100-0x1ff
to via1.PRAM), so the loop is self-consistent and harmless -- and unrelated
to ADB.  A gdb breakpoint at the poll state machine 0x40801640 is never hit
at the stall; only the VIA1 ISR 0x40809bc0 fires (the XPRAM timer task).

### The real gate: the boot-time mouse rendezvous (like macse30.c)

`hw/m68k/macse30.c` already documents and solves this exact ROM behaviour
(its `adb_mouse_force_report()` comment names "the SE/30 ROM's boot-time
0x172 mouse rendezvous").  The Sonora ROM does the same:
  - 0x40802a38 is the startup **dispatch loop** (`tstb 0x172; bne` spin,
    then it walks a 12-byte-entry table calling 0x40802a6e per entry and
    loops back).  It only advances once 0x172 (set 0x80 at 0x4080067a)
    clears.
  - 0x172 clears at 0x408b67ea inside the ADB MOUSE completion 0x408b67c4
    (guard `a2@4 == ExpandMem(0x2b6)[480]->[4]`).  Live: ExpandMem[480] =
    0x5820, 0x5820->[0] = 0x5a50 (the mouse's ADBBase data record, addr 3),
    0x5820->[4] = 0x5840 (the mouse DCB) -- so the awaited completion is the
    MOUSE (addr 3).  0x408b67c4 also posts _PostEvent(1)=mouseDown and needs
    DCB[+21]==0.
  - On real hardware the Egret autopolls (mask ADBBase[334]=0x000c = kbd+
    mouse) and the mouse's first Talk-R0 reply drives this; our Egret models
    no autopoll, so the mouse is never polled and boot hangs.

### The 0x1b8-independent Egret autopoll delivery format (reverse-engineered)

Real autopoll data is an **unsolicited** Egret packet, not a 0x1b8 read.
The interrupt-driven transport re-arms a receive for the registered autopoll
op after EVERY transaction (ROM 0x40814bb4: `a2@52 = a2@48`, in-count 12,
into the a2@24 buffer, with a2 = transport request block at *(0xde0)=0x21b8;
the autopoll op is ADBBase+388 = 0x5a14, `op@16 = 0x408b2fcc`).  When an
unsolicited packet arrives whose stored **byte 1 == 0** (autopoll type), the
transport completion (0x408d18bc, taken because a0 == a2@48) calls the
op callback 0x408b2fcc with `a1 = reply+2`, `d0 = count-4`.  0x408b2fcc
reads `reply[3]` as the **ADB command byte** (addr<<4 | Talk-R0 = 0x3C for
the mouse), `reply[2]` as status (bit0), `reply[4..]` as register data, then
dispatches the addressed device's completion (mouse -> 0x408b6582 -> the
0x408b6774 button-bit dispatch -> 0x408b67c4).  So the stored packet is
`[b0, 0x00, status, adbcmd, data0, data1]` (6 bytes -> count 6, d0 2).

### The fix (commit 118501462f)

Port macse30.c's boot-time crutch to the LC 550 Egret.  A 20ms timer, while
the ROM is parked on the 0x172 spin (PC in [0x40802a30,0x40802a60], 0x172
bit7 set), holds a synthetic mouse button DOWN and calls
`adb_mouse_force_report()` (re-announce the held state WITHOUT cursor
movement -- the ROM's post-rendezvous cursor-position check must not see the
mouse move), then `adb_poll()`s the bus and delivers the reply as an
unsolicited packet `[00 00 00 0x3C dy dx]` via a new
`maclc550_egret_deliver_unsolicited()`.  Delivery reuses the interrupt-
driven /XCVR framing (stage bytes in SR, assert /XCVR, feed via the sr_read
chain, drop /XCVR on the last byte so the transport's 0x408d1792 /XCVR-rise
check completes the receive), guarded to fire only when the transport is
idle AND its pending receive is the autopoll op (a2@52==a2@48 != 0), plus a
new `egret_unsolicited` flag so the sr_read last-byte handler finishes clean
without arming the host-session `egret_pseudo_closing` teardown.  The button
is released and the crutch disarmed once 0x172 clears.  Verified live (clean
run): one `autopoll unsolicited pkt [00 00 00 3c ..]`, 0x408b6582 and
0x408b67c4 both hit with a2=0x5a50/a2@4=0x5840, 0x408b67ea clears 0x172, the
spin exits.

### Second fix, unmasked by the first: A31 24-bit tagged-pointer forwarding

Past the rendezvous the dispatch loop's `_PtInRgn` (0x40802a76) bus-errored
dereferencing region handle **0xA0010730** -- a 24-bit-tagged locked+
resource master pointer (real address 0x00010730).  The machine boots the
ROM in 24-bit mode; Handle master pointers carry state flags in the top
byte, and our no-op PMMU left them in the unmapped top half.  Ported the
IIsi/Classic II A31-half forwarding verbatim: a low-priority (-2) catch-all
over 0x80000000.. that masks to the low 24 bits and forwards to RAM (the
Valkyrie window at 0xf9800000 keeps its higher-priority decode).  This is
exactly the case the original "32-bit-clean, no A31 aliasing" comment
anticipated.  Fixes the bus-error bomb; boot advances further.

### New frontier: illegal-instruction bomb (jump into the low vector table)

Boot now reaches a "Sorry, a system error occurred / illegal instruction"
bomb (full QuickDraw dialog -- screenshot `/tmp/cc-s7b.png`; the earlier
bus-error stage `/tmp/cc-s7.ppm`->png).  The QEMU guest_errors log shows
`Illegal instruction: 7fee @ 2e`: the CPU jumped to ~0x2e and executed the
**low exception-vector table as code** (0x2e is the low word of the line-F
vector longword *(0x2c)=0x00017fee), i.e. a call/jump through a bad/near-
null function pointer.  The fault does NOT go through the ROM vector
handlers (illegal *(0x10)=0x408026f4, line-F *(0x2c)=0x00017fee): the System
(loaded from disk by this point) has installed its own, so those ROM
breakpoints never catch it, and the bomb is terminal (not looping), so it
must be caught on a fresh boot with a breakpoint on execution at low
addresses (0x0/0x2e/0x17fee) set before the fault -- the gdb boot is slow
under -icount so give it several minutes.  Stack at the terminal state holds
dispatch-loop PCs (0x40802a46/0x40802a76) and more 24-bit-tagged pointers
(0xa00108a0), so the bad pointer likely comes from the same 24-bit Handle
family -- candidate next steps: (a) widen/verify the A31 forward (does a
tagged pointer with a DIFFERENT top-byte tag, or a word/odd access, still
resolve?); (b) catch the low-address jump to find which handler pointer is
bad and why it computes to ~0x2e.  This is downstream of, and unblocked by,
the Session-7 fixes; the mouse-rendezvous gate itself is fully solved.

Build/run unchanged (`ninja -C /tmp/cc-build qemu-system-m68k`; add
`-S -gdb tcp::1237`).  gdb: `set architecture m68k`, `set endian big`.

## Session 8: null jmp(a0) root-caused (an [01 22] Egret control-reply underflow); BOOTS TO FINDER

The Session-7 illegal-instruction bomb (`jmp (a0)` with a0=0 -> PC=0 ->
`Illegal instruction: 7fee @ 2e`) is FIXED (commit on `cc2-finish`).  The
machine now boots MacOS 7.5.3 all the way to the **Finder desktop** --
menu bar (File/Edit/View/Label/Special + clock), the mounted OpenRetroSCSI
7.5.3 volume, an open "Control Panels" window and the Trash.  Furthest
screenshot: `/tmp/cc-s8-finder.png`.

### Root cause: the same completion underflow as Session 4, but on cmd 0x22

Caught the fault under gdb (:1237, break `*0x0`; the System replaced the
ROM vectors so ROM-address breakpoints miss it, but a BP_GDB breakpoint on
execution at address 0 catches the null jump cleanly -- it does NOT need
guest memory to hold a trap, so low-RAM overwrite is irrelevant).  Live at
the fault: pc=0, a0=0, a2=0x83df0, a5=0x839ba (both point at RAM jump-island
tables full of `4ef9 <abs>` = jmp.l), sp=0x5fa0ec, SR=0x2718 (**I:7**,
supervisor) -- i.e. inside the interrupt-driven Egret A092 driver's
completion, running with interrupts masked.

The `-d unimp` log showed the trigger unambiguously: the last Egret exchange
before the bomb is a lone **`[01 22 df 02]`** sent by the post-MMU
interrupt-driven driver (pc 0x408d172c), which our model handled through the
**cold-start-sync branch** (`c[1]==0x0e||0x22` -> bare 3-byte `[02 00 00]`
reply via the polled ack_toggle path).  That branch was grown for the
*polled* pre-driver cold-start sync (0x408d1b84, which checks reply byte0==2);
but this 0x22 arrives over the *interrupt-driven* framing, whose driver
discards one turnaround byte (0x408d1732) and whose completion (0x408d1838)
computes `d0 = a2@(16) - 4` then `dbf`-copies `d0+1` bytes through the request
block's data pointer `a0@(8)`.  A 3-byte reply loses the discard, stores only
2, so `d0 = 2 - 4 = 0xFFFE` underflows and the loop scribbles ~64KB of the
reply buffer across low memory -- wiping a dispatch/jump slot that a later
`jmp (a0)` then takes to 0.  (Disassembled 0x408d1838 to confirm the
`a2@16-4` / `moveb a1@+,a2@+` / `moveal a0@8,a2` shape -- exactly the
Session-4 `[01 1b/1c]` control-reply underflow, which had been fixed for
every command EXCEPT 0x0e/0x22 because those were siphoned off by the
cold-start branch before they could reach `session_pseudo_reply`.)

### The fix (local to hw/m68k/maclc550.c)

Gate the cold-start `[02 00 00]` branch on the driver being POLLED
(`!(MOS6522(&m->via1)->ier & SR_INT)`).  During the real cold-start sync the
SR interrupt is disabled, so `[01 0e]/[01 22]` still get `[02 00 00]`
(byte0==2, as the sync checks).  Once the interrupt-driven driver is
installed (IER SR-int enabled), an `[01 22]/[01 0e]` control command falls
through to the existing session-pseudo generic-ACK path
(`maclc550_egret_session_pseudo_reply` -> `[00 00 00 22]` header + the
turnaround-discard dummy byte + completion shift interrupt), so
`stored_count==4`, `d0==0`, one clean byte copied -- identical to how the
`[01 1b/1c]` autopoll-control commands are already handled.  No shared file
touched; macclassicii.c is untouched (separate machine).

### Result

`Illegal instruction` count drops from 2 to **0**.  Post-fix the System
runs a burst of `[01 22 22 xx]` generic-ACK control commands (device-list /
autopoll setup), then `[01 08 01 01]`, then settles into the normal
steady-state (`[01 02/08 01 b8]` XPRAM counter task + `[01 03]` GET_TIME +
60Hz RBV/VIA slot-VBL servicing at 0x40806eaa/0x40809be6).  A screendump 20s
apart is byte-identical: a stable Finder desktop.  Deliverable met.

Reproduce: run to Finder in ~90s (no gdb); `python3` monitor socket ->
`screendump /tmp/shot.ppm` -> PNG.  Break `*0x0` under gdb to re-catch the
(now-gone) null jump if regressed.
