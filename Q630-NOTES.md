# Macintosh Quadra 630 / LC 630 / Performa 630 machine ("quadra630") — progress notes

Goal: boot the Quadra 630 ROM (`/workspace/files/mac-roms/quadra630.rom`,
1 MiB, header checksum 0x06684214) to MacOS 7.5.3 (Finder) over SCSI,
with the F108 MMIO IDE controller present (ideally enumerating a disk).

## Design: lc475.c + IDE

Per Linux `arch/m68k/mac/config.c` MAC_MODEL_Q630: 68040, Cuda ADB,
VIA_QUADRA, ESP SCSI (MAC_SCSI_QUADRA), MAC_IDE_QUADRA, ESCC, SWIM 2,
PDS/comm slot; video is Valkyrie.  That is exactly the lc475.c device
set plus IDE, so `hw/m68k/quadra630.c` is a copy/adapt of lc475.c
(hand-built Cuda-on-VIA1 engine, Valkyrie regbank + guest-published
framebuffer, ESP w/ VIA2 pseudo-DMA, ESCC, SWIM, EASC, MEMCjr/PrimeTime
regbank stubs — on the real Q630 those ASICs are F108 + PrimeTime II,
but the stubs are RAM-backed and grown empirically anyway).

## IDE (MAC_IDE_QUADRA) — the new piece

From Linux config.c + old drivers/ide/macide.c:
- Taskfile at **0x50F1A000**, register stride 4 (`pata_platform`
  `ioport_shift = 2`), i.e. reg n at base + 4*n, window 0x38.
- Alt-status/device-control at **0x50F1A038** (4 bytes).
- **IDE_IFR at 0x50F1A101** (byte, odd address!): bit5 = IDE IRQ flag
  (write 0 to clear), bit6 = IRQ enable, bit7 = any-interrupt; from
  macide.c which was reverse engineered from the MacOS driver.
- IRQ = **IRQ_NUBUS_F** = VIA2 "nubus" slot $F input — QEMU
  `via2 "nubus-irq"` gpio input 6 (named VIA2_NUBUS_IRQ_INTVIDEO in
  mac_via.h; on the Q630 that port A bit is the IDE interrupt).

Modelled with QEMU's TYPE_MMIO_IDE (hw/ide/mmio.c) with shift=2:
region 0 aliased (first 0x38 bytes) at macio +0x1A000, region 1
(alt/ctl) aliased (4 bytes) at +0x1A038, plus a hand-written IFR
register at +0x1A101 that latches the IDE IRQ and gates the VIA2
slot-F line (enable default ON at machine init, so polling drivers
that never touch the IFR still get slot interrupts... may need
flipping if a storm appears).  Drives attach with `-drive if=ide`
(mmio_ide_init_drives / drive_get(IF_IDE,0,0/1)).  mmio-ide's LE data
region = the "straight-through, data appears in correct byte order"
wiring, same convention that pata_platform + ata_sff expect.

## Machine identification (ROM 06684214 analysis)

- The ROM uses the same 0x5FFFFFFC machine-ID register scheme
  (0xA55A/0x2BAD constants near ROM+0x494e/0x4bfc code that reads
  0x5FFFFFFC) — kept lc475's 0xA55A2BAD register.
- Universal table: entries are 0x48 bytes; a low-ROM table at 0x3760
  (older boxes 5..0x14, decoder kinds 5..9) and the interesting one at
  **ROM+0xa7a30..0xa81c8**: kind-10 entries (boxes 0x17/0x1a/0x47,
  strap mask 0x56000000 val 0), kind-11 (boxes 0x22,0x15,0x38,...,
  mask 0), **kind-13 = the 630 family: boxes 0x57,0x58,0x59,0x5a,0x5b,
  0x5c(x6),0x5d(x3) — ALL with strap mask 0 val 0**, i.e. no VIA1 PA
  strap matching at all; the family is presumably disambiguated by
  decoder probe / other means.  Kept lc475's pins_a=0xF9; expected to
  be irrelevant for identification.  Box->machine attribution TBD
  empirically via lowmem BoxFlag 0x0CB3 once booted.
- The ROM references the IDE base 0x50F1A0xx at ROM+0x3464 (early,
  probably decoder probe) and in a driver at ~0xcee6c/0xcfb38 — so the
  ROM does contain an IDE driver.

## Status log

- [x] **DONE: MacOS 7.5.3 boots to the Finder desktop** (session N+6;
      commit "quadra630: boot MacOS 7.5.3 to the Finder"): SCSI boot
      scan accepts the disk, Welcome/extensions run, Finder desktop
      with menu bar / Control Strip / mounted OpenRetroSCSI volume;
      the "not shut down properly" dialog is dismissed with a working
      ADB keyboard (sendkey ret), and the ADB mouse tracks (cursor
      moves, clicks land) via the Valkyrie VBL slot interrupt.
- [x] baseline m68k-only build OK
- [x] quadra630.c (lc475 copy + IDE), Kconfig QUADRA630 (+IDE_MMIO),
      meson entry; builds clean
- [x] machine ID cracked: 0xA55A2224 at 0x5FFFFFFC (see below)
- [x] Cuda: Q630 ROM speaks the REAL Cuda packet protocol (polled +
      int-driven drivers); engine reworked (see below); PRAM rebuild
      (GET_PRAM/SET_PRAM scans) completes
- [x] F108 I/O slice granularity = 0x20000 (decoder probe)
- [x] video: 1152x870 1bpp ROM UI renders (gray desktop) at an
      intermediate stage
- [x] Singer sound codec stub at 0x50F04000 (mailbox handshake + done
      latch)
- [x] ASC boot-chime FIFO shim (fast-drain so the chime "plays"
      instantly under TCG instead of timing out to the death monitor)
- [x] 24-bit-mode support: ROM window at phys 0x00800000 (enabled by
      PrimeTime +0x200 write) + RAM mirror at 0x80000000 (tagged
      DCE/handle pointers); .Sony DCE now resolves
- [x] (was BLOCKED) Sad Mac `0000000F / 00000001`: RECLASSIFIED in
      session N+3 (record/replay + gdb): this is NOT dsLoadErr(15) —
      it is dsBusError(1), a guest CPU/MMU page fault dereferencing a
      corrupt graphics pointer `0x500FB964` at ROM PC 0x40837310.  The
      top line `0000000F` is a HARDCODED boot-stage class (`moveq
      #15,d7` when lowmem 0x2BA==0), NOT the error code; the real code
      is the BOTTOM line = DSErrCode(0xAF0) = 1 = dsBusError.  See the
      "Session N+3" block below for the full derivation.  FIXED in
      session N+5 (committed 3a547dd369): XPRAM 0x8A forced to 0x05 +
      cache sync -> 32-bit no-op translator -> boots past the Sad Mac.
      Video renders fine.  IDE controller present and enumerates an
      attached disk.

## FINAL STATE: MacOS 7.5.3 Finder, working input (sessions N+5/N+6)

`-M quadra630` now boots MacOS 7.5.3 off the SCSI disk all the way to
an interactive Finder desktop (1152x870).  Boot command = the one under
"Build/test commands"; ~50 s wall under TCG; the "computer may not have
been shut down properly" dialog appears (the HFS volume dirty bit AND
the OS's own shutdown bookkeeping both re-trigger it under -snapshot;
patching the MDB drAtrb bit 8 alone does NOT suppress it) and is
dismissed with `sendkey ret` over the monitor — the ADB keyboard works.
The ADB mouse tracks and clicks (RawMouse/MTemp follow, cursor redraws,
clicks change window focus / land on the desktop).

The four fixes that took it from "gray desktop + flashing ? " to this
are documented as session N+6 below; the dsBusError fix (XPRAM 0x8A /
translator vector) from session N+5 is in commit 3a547dd369.

`-drive file=/tmp/ide0.img,format=raw,if=ide` attaches an ide-hd to the
F108 mmio-ide bus and the machine boots to the Finder with it present,
no interrupt storm (the slot-D IDE pending bit stays quiet); MacOS
7.5.3 itself shows no "initialize this disk" dialog for the blank IDE
disk — this System has no IDE support pieces (it was built for
SCSI-only lc475-class machines), so guest-side IDE remains unverified
beyond "the OS is undisturbed by the controller".

`-M lc475` re-verified after all changes (same disk, boots to Finder,
keyboard dismisses the same dialog).  All changes are confined to
hw/m68k/quadra630.c.

## OLD final state (pre-N+5, historical)

`-M quadra630` runs the ROM through: machine identification (box 0x57 =
Quadra 630), full real-Cuda exchange + PRAM rebuild, MEMCjr/F108 RAM
sizing, Valkyrie-630 video init, EASC/Singer sound init, and 24-bit
Memory-Manager / Device-Manager startup — then stops at a **Sad Mac
0000000F/00000001 (dsLoadErr, SysError 15)**: the ROM fails to load /
install a driver or code segment during startup.  It does not reach the
"Welcome to Macintosh"/Finder stage, so neither SCSI nor IDE disk
enumeration by MacOS itself was observed.

The **MAC_IDE_QUADRA controller IS present and wired** (QEMU `mmio-ide`
shift=2 at 0x50F1A000 + IFR at +0x101 -> VIA2 slot $F); `-drive if=ide`
attaches an `ide-hd` onto its `ide.0` bus (confirmed in `info qtree` /
`info block`).  Because MacOS never boots, the guest's own IDE
enumeration (Drive Setup / "uninitialized disk" dialog) could not be
reached.

A gray 1152x870 desktop WAS rendered at an intermediate bring-up stage
(before the Singer/ASC/ROM-window/RAM-mirror work); after those changes
the boot advances further in CPU terms but the ROM reaches its
driver-load diagnostic and paints the Sad Mac over it early (~6-16 s,
timing-sensitive since Cuda/VIA are wall-clock paced).

**Update (Session N+2):** the Cuda-PRAM-truncation cross-machine lead
(from the Q950's fixed Egret `READ_PRAM` bug) was tried and **causes a
regression** for Q630 (hangs earlier than the Sad Mac) — reverted, root
cause understood (Q630's Cuda session-close path doesn't cleanly
support an early close with excess staged bytes). The actual
`SysError(15,1)` call site is now precisely located
(`0x408b98aa`, `d7=15`/`d6=1` confirmed) with a full forward trace into
the already-known freeze epilogue and a full backward trace to the
ROM's generic OS-trap dispatcher; the last-recorded trap PC before it
lands in Memory-Manager handle/zone code. The exact instruction that
first decides `d7=15` is still not found — next lever is QEMU
record/replay + gdb reverse debugging. See "Session N+2" below for
full detail.

**Prior update (see "Session N+1" under "Suspected next step" below):** the
previously-documented 0x1B964 F108-register lever was re-traced with
gdb and does not fire in the current tree (three verified boot-to-Sad-Mac
runs, zero hits on a wide watch window) — that lead is stale, do not
pursue it without re-verifying first. The gdb session did map the
terminal state precisely (it's the ROM's own "identify hardware, then
freeze with interrupts masked forever" Sad Mac epilogue, working as
intended) but did not yet locate the earlier `moveq #15,d0` /
`SysError(dsLoadErr,1)` call site that actually triggers it — that's
the real next lever, and it's most likely inside RAM-resident
`_LoadSeg`ed driver code rather than another ROM/F108 MMIO gap.

### Suspected next step for dsLoadErr (unfinished)

The failure is a driver/segment load (`_LoadSeg`/`DrvrInstall` family).
The .Sony DCE pointer was fixed by the ROM window; the remaining
failure is likely another F108 device the driver init touches (the
last live probe before the Sad Mac was a word read of a device
register at logical->phys 0x500FB964 == slice offset 0x1B964, an
unmodelled F108 window between IDE @0x1A000 and IWM @0x1E000 — possibly
a DMA/PSC or the F108 "combo" control).  Modelling that device (and/or
whatever `_LoadSeg` needs) is the next lever.  Making unmapped in-slice
I/O read as 0 instead of BERR was tried and does NOT change the Sad Mac
(that 0x500FB964 probe is recoverable), so the dsLoadErr is elsewhere.

### Session N+1: the 0x1B964 lever is STALE — debunked with live tracing

Re-opened this with gdb per the "trace the register" instruction. Result:
**the F108+0x1B964 window is not the blocker — it is never touched.**

- Instrumented `q630_iotrace_read`/`write` (the unmapped-I/O BERR catch-all)
  with an unconditional `fprintf(stderr, ...)` for the whole IDE→SWIM gap
  (slice offset 0x1a180–0x1e000, a superset of 0x1B964), rebuilt, and ran
  the full boot to the Sad Mac **three separate times**: zero hits, every
  time. The `macio_alias` repeating-region path (which is how a
  logical/phys address like 0x500FB964 actually gets decoded — it reduces
  mod IO_SLICE and forwards through `address_space_ldl_be` into the same
  `mac-io` container) funnels through the exact same catch-all, so this
  isn't a routing blind spot. Whatever produced that probe in an earlier
  session must have been superseded by later fixes (Singer stub, ASC FIFO
  shim, 24-bit ROM/RAM windows) that changed the code path before ever
  reaching it. **Do not spend more time modelling an F108 register at
  +0x1B964 — reinstrument and re-verify before trusting that lead again.**
  (The instrumentation was reverted after confirming this; `git diff` is
  clean relative to the last commit.)

### Session N+1: gdb pitfall — byte-swapped registers without `set endian big`

`gdb-multiarch`'s `m68k` architecture, connected to QEMU's m68k gdbstub
over `target remote`, silently returns **byte-reversed 32-bit register
values** unless you run `set endian big` *before* `target remote`.
Symptom: `info registers`/`p $pc` show wild-looking values like
`0xfaa08b40` that don't correspond to any mapped region and don't match
what the QEMU HMP monitor's own `info registers` reports for the same
paused CPU. Diagnostic: reverse the bytes of the "garbage" value
(`0xfaa08b40` → bytes `fa a0 8b 40` → reversed `40 8b a0 fa` =
`0x408ba0fa`) and it resolves to a perfectly sane ROM address. Always
`set architecture m68k` **and** `set endian big` right after connecting,
or cross-check against HMP `info registers` (which is correct natively
and doesn't need this).

### Session N+1: what's actually at the end of the road (traced with gdb)

With the endian fix, traced the machine via `-d int` (exception/trap
log) and a conditional breakpoint (`break *0x40804bea if $sp > 0x100000`,
using the fp-based short-call convention this ROM uses — routines return
via `jmp (fp)` after the caller does `lea nextinsn,fp; jmp target`, no
stack frames) to catch the *late* invocation (as opposed to the benign
one during early machine ID) with disassembly:

- Immediately before the Sad Mac settles, the ROM **re-runs its own
  machine-identification logic**: the read-only-register verify at
  0x40804be4–0x40804c04 (write 0/-1 to 0x5FFFFFFC, confirm readback is
  unchanged, confirm the upper word is still 0xA55A) followed by the
  universal-table walk at 0x40804b86 (`ROM+0xA79BC`, exactly the table
  documented above) that matches the ID low word against `entry+0x44`.
  This **passes** in our emulation — `machine_id_read`/`write` in
  quadra630.c are unconditionally read-only/constant, so the verify
  can't fail here.
- Right after that: three repeats of the decoder-probe sequence from
  "I/O aliasing" above — VIA1-alias reads at slice #8/#124/#128 (phys
  0x50101c00, 0x50f81c00, 0x51001c00, pc=0x408046ac, the "writability
  prober") and a MEMCJR read at 0x5000e000 (pc=0x408031a8) — each a
  legitimate BERR (confirmed via `-d int`: `Access Fault(0x8)`, `ea:`
  matches). This 5-fault group repeats exactly 3 times then stops.
- The CPU then **permanently parks** inside the Singer "idle service"
  routine (`q630_singer_ops`, comment block already in the source) at
  pc≈0x408ba0f4–0x408ba0fa, with **SR = I:7 (all interrupt levels
  masked)**. Confirmed genuinely halted, not fast-looping: sampled full
  register state via HMP `info registers` (not gdb, to avoid the
  reconnect-forces-a-stop confound) twice, 5s and then 15s apart, with
  **zero register bits differing** including a live counter (D6) that
  had been changing during the earlier boot — and the `-d int` log
  stops producing new entries in lockstep. `info cpus` doesn't flag it
  "halted" (no STOP opcode), so it's a genuine tight loop that happens
  to touch no instrumented MMIO.

  **Read this as expected, not a bug**: this is what a real 68k Sad Mac
  does — identify the hardware (to know how to draw itself / what to
  report), reconfirm a few basics, then freeze forever with interrupts
  masked so nothing but a hardware reset can recover. It is the
  death-routine's own prologue+epilogue, not the cause. **The actual
  `moveq #15,d0` / dsLoadErr decision happens earlier and was not
  located this session** — it must be searched for further back (likely
  in RAM-resident driver/segment code: several PCs in the tail of the
  `-d int` A-Line trace before this cluster are in low RAM, e.g.
  0x9634/0x876c/0x8996/0x8aac/0x8b26, i.e. inside code that was already
  `_LoadSeg`ed into memory, not ROM — that's probably where to keep
  digging, with a breakpoint on the entry to the "identify machine +
  freeze" block at 0x40804b6c to catch its caller's return address on
  the stack, or a watchpoint on the Sad Mac icon's VRAM bytes to catch
  the exact drawing call and unwind from there).

Net effect on the "min" bar ("F108 combo window modelled with the driver
advancing measurably"): **not applicable as literally stated** — there
is no unmodelled F108 register in the failure path to model. No boot
regression either way (byte-for-byte confirmed Sad Mac before and after;
no source changes ended up kept, `git diff` clean).

### Session N+2: Cuda-PRAM-truncation lead (Q950 cross-reference) — TRIED, REGRESSED, REVERTED

Pursued the cross-machine lead first, per instructions: the Q950's Egret
MCU had a bug where `READ_PRAM` (cmd 2) returned only 1 byte per
exchange while a ROM helper clocked 2-4 bytes from a single exchange,
leaving stack-scratch garbage that fed a wrong ddType compare (fixed in
commit `2fa641736b`, "hw/m68k/quadra950: fix Egret READ_PRAM to
stream"). The Q630's sibling Cuda MCU has an analogous `CUDA_CMD_GET_PRAM`
(pseudo cmd 0x07) that, before this session, also only ever returned
exactly one data byte (`cuda_resp_len = 4`, r[3] only).

- **Applied the same fix** (stream up to 256 consecutive PRAM bytes,
  enlarging `cuda_resp[]` to `288 + ADB_MAX_OUT_LEN` to match): **this
  is WRONG for Q630 and REGRESSES the boot** — instead of reaching the
  dsLoadErr Sad Mac (~6-16s), the machine now hangs much earlier
  (interrupts masked, `SR I:7`, PC parked at ROM 0x408b3ba4, a
  `btst #3,%a2@; beqs self` busy-wait on VIA1 IFR bit 3/CB2) and never
  even reaches the point where the framebuffer would show anything
  (screendump stayed blank white). Confirmed via `-d int` that after
  the fix only ONE exception fires in the first ~20s (vs. the normal
  5-fault×3-repeat decoder-probe cluster), i.e. genuinely stuck very
  early, not just slow.
- **Root cause of the regression**: unlike Q950's Egret model, Q630's
  Cuda session/turnaround state machine
  (`q630_cuda_session_update`/`q630_cuda_ack_toggle`) treats "host
  closes the session while `cuda_resp_idx < cuda_resp_len`" as an
  **ACR-turnaround-staged response** (schedules an interrupt and keeps
  the session open, expecting the driver to continue reading) rather
  than a **clean early close with the remainder discarded**. The
  ROM's PRAM-rebuild scan (256 iterations, one `GET_PRAM` call per
  address, expects exactly 1 byte back per call before moving to the
  next address) always closes the session after reading only 1 byte.
  With the streamed 256-byte response staged, that early close now
  falls into the "turnaround" branch instead of a normal close, and
  the polled Cuda driver's own state machine — which released the
  handshake lines because it decided it was done, not because it
  wanted a turnaround — never gets the interrupt it's now actually
  waiting for in the way it expects, wedging the exchange forever.
  (Q950's Egret session-close path is structurally simpler — see
  `q950_egret_os_ack_toggle`/`q950_egret_os_session_update` — and
  doesn't have this "staged-response-implies-turnaround" branch, which
  is presumably why streaming `GET_PRAM`/`READ_PRAM` there was safe.)
- **Reverted in full** (`git diff` clean, confirmed byte-for-byte
  return to the baseline `0000000F/00000001` Sad Mac after rebuild).
  **Conclusion: the Cuda-PRAM-truncation lead does NOT apply to Q630
  as a drop-in fix.** If revisited, the real fix would need to also
  rework the session-close/turnaround branch in
  `q630_cuda_session_update` to distinguish "genuine early close
  mid-stream" (discard remainder, close normally) from "response
  staged, not yet touched, ACR turnaround" (the current, narrower,
  correct use of that branch) — nontrivial, not attempted this session.
- Also checked (before touching code) whether the ROM issues any
  **currently-unhandled** Cuda pseudo command that falls into the
  generic silent-ACK `default:` case (the closer structural analog of
  the actual Q950 bug — a specific command with *no* data reply at
  all, not an already-working one). Instrumented the `default:` branch
  and raised the shared log budget; a full boot-to-Sad-Mac capture
  (`/tmp/q630i_unimp.log`, 18k lines, budget never exhausted — i.e.
  this really is the complete Cuda command history for the run) shows
  the ROM does issue a handful of these: `0x22`/`0x26` (I2C-ish,
  twice/once), `0x1b` (SET_ONE_SECOND_MODE-style, once), `0x19`
  (SET_DEVICE_LIST-style, once) — but all of them happen in the first
  ~8000 (of 18000) log lines, i.e. well before the terminal Singer
  polling loop, and match what the existing notes already call out as
  "generically ACKed" pseudo commands. No smoking gun here; reverted
  this instrumentation too.

### Session N+2: dsLoadErr trigger — SysError(15,1) call site FOUND (first time), full caller chain traced

Since the PRAM lead didn't pan out, gdb-traced the actual
`SysError(dsLoadErr, 1)` call for the first time (previous sessions
only characterized the terminal freeze epilogue, never the call
itself). Method: `-gdb tcp::<port> -S`, `gdb-multiarch` with
`set architecture m68k` + `set endian big` (see the byte-swap pitfall
above), unconditional `break *0x408b98aa` (found by re-disassembling
the ROM around the known terminal-freeze address chain — see below for
how this address was derived) — this breakpoint hits **exactly once**,
cleanly, no condition needed.

- **Confirmed this IS the live `SysError(15, 1)` call**: at the
  breakpoint, `d7 = 0x0000000F` (15 = dsLoadErr) and `d6 = 0x00000001`
  (restart-required flag) — these are exactly the two hex numbers the
  Sad Mac displays (`0000000F` / `00000001`). `d0 = 1`, `a0 = 0x000b743c`
  (a RAM address — turned out to be a **red herring**, see below, not
  a driver/handle pointer), `fp = 0x4080246e` (the short-call
  "return" continuation per this ROM's `lea next,fp; jmp target` /
  `jmp fp@` calling convention, documented in the earlier session).
- **Forward trace (SysError's own tail, now fully explained, not just
  characterized)**: `0x408b98aa` allocates a 256-byte stack scratch,
  calls a helper at `0x408ba2ce` (return `0x408b98b8`), zeroes
  `d2/d6/d7`, then does a fixed (not data-dependent — `a0` is loaded
  via absolute `lea`, so this is just the ROM's usual PIC "far call"
  encoding, not a real dispatch table) tail-jump chain:
  `0x408b98be → 0x40847752` (`oriw #1792,%sr` — masks all interrupt
  levels, matches the frozen `SR I:7` state) `→ 0x40802f64`
  (`bral 0x40804b6c`) `→ 0x40804b6c`, i.e. **directly and
  unconditionally into the previously-documented "machine identify +
  freeze" epilogue** (machine-ID reverify, 3× decoder-probe cluster,
  final park in the Singer idle loop at `0x408ba0f4`). This connects
  the two halves of the picture the previous session left separate:
  the terminal freeze *is* SysError's own tail, not a distinct later
  event.
- **Backward trace (who calls SysError)**: `fp = 0x4080246e` at the
  breakpoint identifies the immediate caller precisely, because that
  address turned out to be **inside SysError's own code**, not the
  external caller: `0x4080246e: lea 0xb743c,a0; jmp pc@(0x4080246e,a0:l)`
  computes target `0x4080246e + 0xb743c = 0x408b98aa` — i.e. `a0`'s
  value at the breakpoint (`0xb743c`) is just this jump's own working
  register, **not a meaningful pointer** (the "0xDB6DB6DB..."
  repeating-bit-pattern RAM contents at that physical address are a
  coincidental leftover from the ROM's RAM-sizing test, unrelated).
  Following further back: `0x4080246e` is reached from `0x40802466`
  (`lea 0x4080246e,fp; jmp pc@(0x408024fe)`), which is reached from
  `0x4080245a` (`movel d6,d0; ...; jmp pc@(0x408024xx)`), which is
  reached from `0x40802444` (`movel d7,d0; ...`) — **this whole block
  is SysError's icon/digit-drawing routine**: it takes the error code
  (`d7`→`d0`) and restart flag (`d6`→`d0`), converts each nibble to a
  4×6 dot-matrix bit pattern via a rotate/mask loop
  (`roll #4,d0; andb #15,d5; ...`) and writes it into screen memory at
  an address computed with classic QuickDraw row/rowBytes arithmetic
  from globals at `0x824`/`0xc24`/`0x8a8` — i.e. this literally *is*
  the code painting the `0000000F` / `00000001` digits under the Sad
  Mac icon that we see in the screendump. `0x40802444` itself is
  reached via `jsr %a1@` at ROM `0x408027a4` — return address
  `0x408027a6` (confirmed: this exact word sits on top of the stack at
  the `0x408b98aa` breakpoint) — from inside the ROM's **generic
  old-style OS-trap dispatcher** (the Q630 equivalent of the Q950's
  documented `0x408099b0`-style dispatcher: builds a trap-number index
  from a per-trap `bsrs` stub table at `0x408026f0`, saves the
  original exception `SR`/`PC` to low-mem globals `0xc74`/`0xc70`
  around `0x408026d4-8`, then `jsr a1@` to the registered handler for
  that trap).
- **One level further**: the dispatcher's own last-recorded original
  trap PC, read live from low-mem global **`0xC70 = 0x40837310`**,
  disassembles to a **compiled (fp-relative / C-style), not
  hand-written) Memory-Manager-looking handle/zone routine**
  (`moveal %fp@(28),%a0; movew %a0@(2),%d1; ...` — reads a 2-byte
  field at offset 2 of a handle/zone-style structure, surrounded by
  `clrl %sp@-; dbf` block-zeroing loops typical of `NewHandle`/zone
  compaction code). This is circumstantial (0xC70 records whatever
  trap happened to run most recently through *that* dispatcher, and
  SysError itself may be invoked via a direct internal `jsr` rather
  than through this same trap path — not proven which), but it is a
  concrete, specific lead: **the last normal OS/Memory-Manager
  activity before the SysError(15,1) decision was handle/zone
  bookkeeping**, consistent with a driver-load memory allocation (or a
  handle-size / zone sanity check) failing and the loader calling
  `SysError(dsLoadErr, 1)` directly.
- **Not yet found**: the exact single instruction that first computed
  `d7 = 15` (i.e., the actual `_LoadSeg`/driver-install failure
  decision). A static search for `moveq #15,...` in the ROM disassembly
  (`/tmp/q630_full.dis`) turns up **~180 hits** across every data
  register, far too many to eyeball without dynamic narrowing. Two
  concrete next steps for a future session, in order of promise:
  1. **QEMU record/replay + gdb reverse debugging**: boot once with
     `-icount shift=N,rr=record,rrfile=/tmp/q630.rr`, then replay with
     `rr=replay` and use gdb's `reverse-stepi`/`reverse-continue` from
     the now-confirmed `break *0x408b98aa` to walk backward
     instruction-by-instruction (or use a reverse watchpoint on `d7`
     becoming `15`) until the exact `moveq`/`move.w #15,...` (or
     computed-15) instruction is found. Not attempted this session —
     setup/validation of record-replay determinism for this machine
     would take real time and this session was already deep.
  2. Alternatively, a **conditional breakpoint on `d7==15`** at
     candidate driver-install/Memory-Manager entry points (e.g. the
     boot-driver installer analog of Q950's `0x40807224`, or a
     breakpoint scanning the ~180 `moveq #15` sites filtered to ones
     inside RAM-resident, not-ROM, code first per the prior session's
     "0x9634/0x876c/0x8996/0x8aac/0x8b26" RAM-code lead) — tried one
     such unconditional-address conditional breakpoint
     (`break *0x408026a0 if $d7==15`) this session and it **did not
     complete in 100s** (conditional breakpoints under the m68k TCG
     gdbstub are extremely slow — every stop requires a full
     single-step-and-check cycle — this is why record/replay reverse
     debugging is the better tool, not more conditional breakpoints).
- **gdb/process-management pitfalls hit this session** (adding to the
  existing pitfall list): (a) gdb's default port 1234 collides across
  concurrent sessions/worktrees — use a distinct `-gdb tcp::<port>` per
  session, don't rely on `-s`. (b) A `timeout N gdb ... -batch` that
  hangs inside a blocking `continue` on a **conditional** breakpoint
  can outlast the `timeout` wrapper itself (SIGTERM doesn't always
  interrupt a blocked remote-protocol read promptly) — if a gdb batch
  run hangs past its timeout, `kill -9` the **qemu** PID (not just
  wait out gdb), then start a fresh qemu instance for the next attempt
  rather than trying to reuse/reconnect to one that may have already
  run stale past the breakpoint.

**Net result this session**: no source change kept (`git diff` clean,
confirmed identical `0000000F`/`00000001` Sad Mac before/after). The
Cuda-PRAM lead is now conclusively ruled out for Q630 (tried, causes a
worse regression, root-caused why, reverted). The dsLoadErr trigger's
call site is now precisely identified for the first time
(`SysError(15,1)` at ROM `0x408b98aa`, full forward chain into the
already-known freeze epilogue, and full backward chain to the generic
OS-trap dispatcher and a Memory-Manager-flavored last-trap PC) — the
remaining gap is the ~180-candidate `moveq #15` search for the actual
decision site, for which record/replay reverse debugging is the
identified next lever.

### Session N+3: THE ERROR IS dsBusError(1), NOT dsLoadErr(15) — a guest MMU fault on a corrupt graphics pointer 0x500FB964

Used the documented record/replay + gdb lever. This overturns the
central assumption of every prior session (that the code is
"dsLoadErr 15").

**Record/replay setup that works (reuse this):**
- Record/replay CANNOT use global `-snapshot` (blocked) and needs a
  deterministic disk (raw `if=scsi` disk DMA completion is NOT
  deterministic even under plain `-icount`; only `blkreplay` is).
- Audio is non-deterministic → force a null backend on BOTH record and
  replay: `-M quadra630,audiodev=snd0 -audiodev none,id=snd0` (without
  this, replay dies with "Missing audio out event in the replay log").
- Reverse debugging needs a snapshot-capable (qcow2) disk to store
  VM checkpoints, so convert the disk to qcow2 (had to `meson configure
  -Dtools=enabled && ninja -C build qemu-img` first — qemu-img was not
  built by the docs/tools-disabled configure) and record with an
  initial snapshot:
  ```
  build/qemu-img convert -f raw -O qcow2 <disk> /tmp/q630.qcow2
  # RECORD (runs deterministically; screendump the record itself):
  build/qemu-system-m68k -M quadra630,audiodev=snd0 -audiodev none,id=snd0 \
    -bios <rom> \
    -drive file=/tmp/q630.qcow2,format=qcow2,if=none,id=img-direct \
    -drive driver=blkreplay,if=none,image=img-direct,id=img-blkreplay \
    -device scsi-hd,drive=img-blkreplay,bus=scsi.0,scsi-id=0 \
    -serial null -serial null -display none \
    -icount shift=7,rr=record,rrfile=/tmp/q630.rr,rrsnapshot=init \
    -monitor unix:/tmp/mon.sock,server=on,wait=off
  # REPLAY (identical cmdline, rr=replay; add -gdb tcp::<port> -S):
  ```
- To TEST a source change you must RE-RECORD (a code change invalidates
  the old rr log → replay diverges).  The record run itself is
  deterministic and reaches the Sad Mac, so just screendump the record.
- gdb pitfalls: use a unique `-gdb tcp::<port>` per run (default 1234
  collides across worktrees); `reverse-stepi` from only an icount-0
  snapshot replays from the start each step (impractically slow) — do
  NOT single-step in reverse; use forward breakpoints / one
  reverse-continue instead.  gdb `monitor <cmd>` passthrough to the
  QEMU monitor works and is how the MMU/TTR state below was read.

**The derivation (all values deterministic, cross-checked across two
independent recordings):**
1. The Sad Mac drawing routine (ROM SysError body at `0x4080280e`) sets
   the two displayed longs as: `d6 = *(word)0xAF0` (DSErrCode, the REAL
   error code, drawn on the BOTTOM line) and `d7 = *(long)0x2BA`, and
   **if 0x2BA==0 it hardcodes `d7 = 15`** (`0x4080281e: moveq #15,d7`)
   before jumping to the bare-Sad-Mac path.  At the fault, live-read
   `0x2BA == 0`, so the TOP line is ALWAYS `0000000F` at this boot
   stage regardless of the actual error — it is a fixed "no
   system-error handler installed yet" class, NOT the code.  **Every
   prior session chasing "SysError 15 / dsLoadErr" was chasing a
   constant.**  (Digit layout: d7 drawn at row 24, d6 at row 36 → d7
   top, d6 bottom; verified in the drawing loop at 0x40802444/0x4080245a.)
2. The real code is DSErrCode (0xAF0) = **1**.  Confirmed by breaking at
   `0x40802786` (`movew d0,0xAF0`, the DSErrCode store) in replay: the
   dispatcher's computed `d0 = 1`.  The ROM's exception dispatcher
   (`0x408026a0`, reached via the `bsrs` stub table at `0x408026f0`
   that CPU exception vectors 2..11 point to, installed at
   `0x408025fa`) computes `d0 = (retaddr-0x408026f0)/2`; vector **2 =
   bus error** → stub 0 → index 1 → DSErrCode **1 = dsBusError**.
3. The saved register block the dispatcher writes at `0xC30`
   (`moveml d0-sp,0xC30`) and the saved fault PC at `0xC70` give the
   exact fault: **PC `0xC70 = 0x40837310`**, which executes
   `movew %a0@(2),%d1` with **`a0 = 0x500FB964`** → a word read of
   `0x500FB966`.
4. MMU state at the fault (gdb `monitor info registers`): **TCR =
   0x0000C000 → paging ENABLED (8K pages)**; **DTTR0 = 0xF900C060**
   (covers 0xF9xxxxxx = VRAM), **DTTR1 = 0x807FC040** (covers
   0x80–0xFFxxxxxx = the 24-bit tagged-RAM range); URP=0, SRP=0x01FFFA00.
   **Neither TTR covers 0x50xxxxxx**, and the page-table walk for
   0x500FB964 does not resolve (`MMUSR = 0x31, fault at 500FB966`).
   So this is a **genuine guest MMU page fault** — the CPU faults
   *before any QEMU memory region is consulted*.  **PROOF that no F108
   device modelling can fix it**: a test that made the in-slice
   unmapped window at slice offset 0x1B964 return 0/OK (instead of
   BERR) — and a live `xp/1xh 0x500FB966` confirming it now reads 0 —
   did NOT change the fault: the re-recorded run still froze with
   `0xC70=0x40837310`, `a0=0x500FB964`, and the iotrace window was
   never even entered (WINDOW0 log had zero hits).  This also finally
   EXPLAINS the old "read-as-0 didn't change the Sad Mac" note and
   **definitively kills the F108 `0x1B964` lever** (the address never
   reaches a device; it dies in the MMU).
5. It is graphics code.  The faulting routine is a QuickDraw
   region/coordinate routine (pixel-align math: `subw a5@(8); lsll d4;
   andiw #-32; asrl d4; addw a5@(8)`).  `a5@(8) = 870` (= 0x366 = the
   1152x870 screen height).  A sibling pointer in the same frame,
   `a3 = 0x00FFB964`, IS valid and points to a **GrafPort**: a Rect
   `{top=419,left=560,bottom=451,right=592}` followed by QuickDraw
   bottleneck-proc pointers (0x40801668, 0x4080136e, 0x40801D14,
   0x40800322).  So `0x500FB964` is a **corrupt pointer threaded as a
   parameter** into this routine, not a device register.
6. The corrupt pointer is a call parameter.  Frame walk from the saved
   fault fp (`a6=0x00FFB7BE`): `[a6]=0x00FFB8EA` (caller frame),
   **`[a6+4]=0x4083524A` = return address**, i.e. the faulting routine
   was called by `jsr @(1A9C)@(0)` at `0x40835244`.  The bad pointer is
   the callee's `fp@(28)`, and by counting the caller's argument pushes
   (0x4083520C..0x40835242) that maps to the **caller's `fp@(14)`**,
   which is itself a parameter to the caller — i.e. `0x500FB964` is
   passed DOWN through multiple QuickDraw calls from further up.
7. The corruption signature points at the F108 base leaking into a
   pointer: `0x500FB964 == 0x000FB964 | 0x50000000`, and
   `0x500FB964 XOR 0x00FFB964 (the valid sibling GrafPort) == 0x50F00000`
   — **exactly the F108 device base** (Singer 0x50F04000, IDE
   0x50F1A000, etc.).  So an address computation somewhere OR'd/merged
   the F108 I/O base (0x50F00000 / 0x50000000) into what should be a
   high-RAM GrafPort/PixMap pointer (~0x00FFxxxx).  (RAM at the bare
   low-24 address 0x000FB964 is uninitialised RAM-test fill 0xDB6DB6DB,
   so it is NOT simply a mis-tagged 24-bit pointer — the high bits are
   genuinely wrong, consistent with a base-address merge bug.)

**NEXT LEVER (crisp):** find where the F108 base (0x50F00000/0x50000000)
gets merged into this graphics pointer.  Two angles:
  (a) In replay, break earlier in the QuickDraw call chain (the routine
      at 0x40835244's caller, and up) and watch the parameter that
      becomes `0x500FB964` — set a memory watchpoint on the GrafPort/
      GDevice/PixMap field it is read from (screen `baseAddr` or a
      region handle) to catch the store that first forms `0x50…`.
  (b) It smells like the 24-bit-addressing / RAM-mirror tagging (finding
      6): pointers are tagged `0x80000000|addr` and RAM is mirrored at
      0x80000000, but a screen/PixMap base looks to have been tagged
      with the I/O base instead.  Compare the Q630's screen/GDevice
      baseAddr setup against **lc475** (which boots to Finder with the
      same QuickDraw ROM) — the divergence in how the onboard-video
      baseAddr is published to MacOS is the likely culprit.  The Q630
      model puts VRAM at 0xF9000000 (DTTR0 range); if MacOS's PixMap
      baseAddr should be ~0xF9xxxxxx but a base-merge produced 0x50…,
      the fix is in how the Valkyrie framebuffer base is reported /
      how the video driver's DCE/PixMap baseAddr is formed.

**Net result this session**: no source change kept (`git diff` clean;
the WINDOW0 test was reverted after proving the fault is an MMU fault,
not a region decode).  Deliverable: the blocker is now correctly
classified (dsBusError(1) guest MMU fault on corrupt graphics pointer
0x500FB964 at ROM 0x40837310) with a concrete corruption signature
(F108 base merged into a GrafPort/PixMap pointer) and a specific next
lever (trace the screen/PixMap baseAddr formation; diff against lc475).

### Session N+4: the screen/PixMap baseAddr is NOT the bug — it is correctly mapped; the fault is a *stray* QuickDraw structure pointer

Followed the N+3 lever (chase the screen baseAddr contamination) with
record/replay + gdb.  Result: the "video baseAddr contamination"
hypothesis is **DISPROVEN**, and the real bug is narrower and deeper.

Walked the guest structures live at the fault (deterministic replay,
break at the SysError dispatcher `0x40802786`, gdb `monitor`
passthrough):
- **MainDevice/TheGDevice** (lowmem 0x8A4/0xCC8) = handle `0x2118` →
  master ptr `0x80006A40` (correctly `0x80000000`-tagged → RAM 0x6A40).
  The GDevice is well-formed: gdRect `{0,0,870,1152}`, gdPMap (+0x16)
  = handle `0x2108` → PixMap `0x80006A90`.
- **Main-screen PixMap** at `0x80006A90`: `baseAddr = 0x51900080`,
  rowBytes = 0x8480 (flag|1152), bounds `{0,0,870,1152}`.  So
  ScrnBase (0x824) = the PixMap baseAddr = `0x51900080`.
- **CRUCIAL DISPROOF**: read `0x51900080` as a *virtual* (MMU-translated)
  address via `monitor x /1xw 0x51900080` → it **reads fine**
  (`0x51900000` holds real framebuffer bytes).  So MacOS's page tables
  DO map logical `0x51900000` → physical VRAM (our VRAM is at
  0xF9000000; MacOS maps the 0x519-slot window onto it), which is why
  the gray desktop / Sad Mac render.  The screen baseAddr `0x51900080`
  is **intentional and correctly mapped — NOT the bug.**  (The N+3
  "F108 base leaked into the screen baseAddr" framing was wrong.)
- By contrast the **fault pointer `0x500FB964` is genuinely unmapped**
  (`monitor x /1xw 0x500fb964` → "Cannot access memory").  So the bug
  is one specific *stray* pointer, not the screen base.

Traced where `0x51900080` comes from anyway (to rule it out fully):
watchpoint `watch *(int*)0x80006a90 if ==0x51900080` caught the write
at ROM `0x40827AB8` (`movel %d3,%a2@+`), in the Slot-Manager-driven
PixMap builder `0x40827A30`.  The formula is
`baseAddr = slotBase[slotinfo+0x2A] + minorBaseOS`, and here
`minorBaseOS = 0` so `baseAddr == slotBase == 0x51900080` verbatim from
the slot-info block (via lowmem SlotMgr array `[0x11C]=0x5FD0`).  This
is a normal Slot-Manager base and, again, MacOS maps it — not a bug.

**The actual bug — stray pointers in a QuickDraw structure:** the
faulting routine `0x40837310` gets its bad `a0` from its param
`fp@(28)`; the caller is `0x40835244` (`jsr @(1A9C)@(0)`).  In the
caller's frame the params pushed at `0x4083522E`/`0x40835232` come from
`[a3+24]` / `[a3+28]` (set at `0x408351DC`/`0x408351E2` from
`a3 = [a4]`).  Read live, the callee's param area holds **two** bad
pointers, `0x500FB964` (fp@28) and `0x500FB958` (fp@36), alongside
*valid* siblings `0x00FFB94A`, `0x00FFB944`, `0x00FFB952` and a valid
`0x00FFB964` GrafPort — i.e. a cluster of high-RAM QuickDraw pointers
where two are corrupt.  gdb `find` shows `0x500FB964` exists only on
the stack / in the SysError-saved regs (not in low RAM), so it is
*computed*, not read from a persistent low-RAM table; the source
structure `a3=[a4]` lives in the high-RAM/stack region (~0xFFBxxx),
which `find 0..0x300000` did not cover.

**Corruption signature (sharp):** each bad pointer is a valid
high-RAM QuickDraw pointer `0x00FFxxxx` with the **F108 register base
`0x50F00000`** merged in:
`0x500FB964 == 0x00FFB964 XOR 0x50F00000` (top byte `0x00→0x50`,
next nibble `0xF→0x0`), likewise `0x500FB958 == 0x00FFB958 XOR
0x50F00000`.  Both reduce (mod IO_SLICE 0x20000) to F108 **slice
offset 0x1B964/0x1B958** — i.e. the same 0x1B9xx window the very first
notes flagged, but reached as a *guest* dereference of a corrupt
pointer, not a device probe.

**Two hypotheses for the leak (next levers):**
1. **24-bit "dirty" master pointer not stripped.**  MacOS boots this
   ROM in 24-bit addressing (finding 6), where master pointers carry
   flag bits in the high byte and must be 32-bit-cleaned before use in
   the MMU-on (32-bit) world.  `0x500FB964` looks like a 24-bit ptr
   `0x0FB964` with high byte `0x50` un-stripped (and the `0xF→0x0`
   nibble shift suggests the strip/mask is partial/wrong).  lc475
   (which boots) does explicit 24-bit masking (`& 0xffffff`) in its
   host-side fb tracker; the *guest*-side equivalent (what makes
   MacOS's Memory Manager keep pointers 32-bit-clean) depends on the
   addressing-mode config we hand MacOS via PRAM byte 0x8A + the
   ROM-window enable (finding 6).  Re-audit whether the Q630 leaves
   MacOS in a half-24/half-32 state that leaves these flag bits in
   dereferenced pointers.  (Note the Q630 `q630_fb_track_mode` masks
   the *handle* chain with `& 0xffffff` but NOT the final `base`, and
   rejects `base>>20 != 0xF90` — cosmetic host-side only, but it shows
   the tag byte really is present on these pointers.)
2. **Slot/declaration-ROM address space too small.**  If these
   pointers are meant to point into the onboard-video card's
   declaration-ROM / sResource / param space (Rects, region data) at
   `0x50000000 + 0x0Fxxxx`, real HW would have MacOS map that region
   (from the decl-ROM's address info), but our synthetic slot/decl-ROM
   only covers ~1 MiB of VRAM so MacOS never maps `0x500Fxxxx` → the
   dereference faults.  Check what address range the Q630's onboard
   video declaration/slot setup advertises to the Slot Manager.

**Method notes for next session (record/replay is essential here):**
- Only the `blkreplay` record path is deterministic enough to reach
  0x40837310 (plain `-icount` diverges on disk DMA; audio must be
  `audiodev=none` on BOTH record and replay or replay dies with
  "Missing audio out event").  A code change invalidates the rr log —
  RE-RECORD to test (the record run itself reaches the Sad Mac, so
  screendump it).
- Watchpoints: gdb hardware `watch *(int*)ADDR` (optionally with an
  `if` on the value) works efficiently via the QEMU TCG watchpoint and
  is the right tool — but it watches a **virtual** address, so a write
  via a different alias (e.g. `0x80000824` vs `0x824`) won't trip a
  watch on the other.  Watch the address the ROM actually uses (for
  the PixMap baseAddr that was the DTTR1-tagged `0x80006A90`).
  Conditional *breakpoints* single-step and are far too slow here.
- To find where the *stray* pointer is first formed: re-record, then
  `find` the value `0x500FB958` across HIGH RAM (0x00F00000..0x01000000
  and the 0xFFxxxx stack window, not just low RAM) to locate the
  structure `a3`, then watch that field to catch the store, and unwind
  to the instruction that OR/XOR/adds `0x50F00000` (or fails to strip
  the 24-bit tag).

**PRECISE LOCALIZATION (the exact hand-off point):** `find 0x500FB958`
across high RAM shows the two bad pointers exist ONLY on the stack
(callee param area `0xffb7da`/`0xffb7e2`), never in a persistent
structure.  The SOURCE is a clean **6-pointer array in the
caller's-caller frame at `0x00FFB92C`**:
`00FFB964, 00FFB94A, 00FFB958, 00FFBB82, 00FFB944, 00FFB952` (ALL
valid `0x00FF…`).  The callee's param block at `0xffb7da` is that
SAME 6-entry sequence **but with entries #1 and #3 corrupted**
(`00FFB964→500FB964`, `00FFB958→500FB958`) while #2/#4/#5/#6 copied
verbatim.  So the leak is a **copy/relocate step in the caller chain
(the routines whose return addresses are `0x4083524A` / `0x40835986`
/ `0x408359AE`) that transforms exactly entries #1 and #3 of this
region/rect-pointer array by merging `0x50F00000`**, and copies the
rest untouched.  That selective transform is the single instruction /
short loop to find and fix next: disassemble those three routines
(`0x40835xxx`), find where the array at `[frame]` is walked and a
subset of entries get `0x50F00000` added/OR'd/XOR'd (or a 24-bit tag
left un-stripped only on those entries), and correct it.  This is the
tightest possible next lever — the corruption is localized to two
entries of one known array, produced by known ROM routines.

**Net result this session**: no source change (all gdb reads; `git
diff` clean, baseline Sad Mac unchanged).  Advance: DISPROVED the
screen-baseAddr hypothesis (that base is correct and mapped), and
narrowed the bug to two *stray* QuickDraw pointers
(`0x500FB964`/`0x500FB958` = `0x00FFxxxx` with the F108 base
`0x50F00000` merged) — pinpointed to **entries #1 and #3 of a
6-pointer region/rect array copied from `0x00FFB92C` to the callee's
params by the ROM routines at `0x4083524A`/`0x40835986`/`0x408359AE`**.
Most-promising next lever: disassemble those routines to find the
selective `0x50F00000` merge / un-stripped 24-bit tag on those two
entries; cross-check against the Q630 addressing-mode/PRAM setup vs
lc475 (hypothesis 1, 24-bit-dirty master pointers).

### Session N+6: flashing "?" -> Finder with working keyboard/mouse (four fixes)

Startpoint: gray desktop + centered boot-disk icon flashing "?"
(ROM scanning for a bootable disk and rejecting the SCSI volume).
Endpoint: interactive Finder.  All fixes in hw/m68k/quadra630.c,
commit "quadra630: boot MacOS 7.5.3 to the Finder".

1. **Boot scan rejected the disk: expected ddType latched from XPRAM
   0x77, not 0xF8-0xFB.**  Symptom: infinite lba-0 READ(6) loop on
   targets 0 (our HD) and 2 (auto scsi-cd), never reads lba 64
   (`-trace scsi_req_parsed_lba`).  The scan (0x40807224, entered via
   the per-SCSI-id loop 0x408071FC from 0x40801350) compares each DDM
   driver entry's ddType against `fp@(-10)`, latched ONCE from d3
   before the flashing-"?" loop at 0x40801430: trap **A07D** =
   _ReadXPRam 4 bytes @0x78 (default startup device, d3/d4) then trap
   **A084** = _ReadXPRam 2 bytes @**0x76** (handler 0x408013B0); the
   expected ddType is the LOW byte = **XPRAM 0x77**.  The lc475 note
   #13 ("OSDefault low byte at 0xF8-0xFB") is that ROM's layout — on
   THIS ROM the seeded PRAM[0xFB]=1 never reached the compare, and the
   template-wiped XPRAM cache (see the 0x8A saga) served 0 -> the scan
   demanded ddType 0 and rejected every disk.  FIX: seed PRAM[0x77]=1
   and extend q630_cuda_sync_xpram_cache to refresh cache bytes
   0x76/0x77 (ddType byte forced >=1).  After this the disk is
   accepted, the driver loads from lba 64 and MacOS boots to the
   Finder-stage dialog.  NOTE the latch is one-shot: the cache value
   must be right BEFORE the scan starts (the sync-on-every-PRAM-
   exchange hack achieves that; a mid-scan fix does nothing).
2. **ADB dead at the Finder, part 1 — Listen/Flush ACKed as
   "timeout".**  Our explicit-ADB response builder set flags 0x02 for
   ANY zero-length adb_request result; MacOS's ADB Manager reads that
   as a dead device on its Flush/Listen-R3 setup commands.  Only a
   TALK ((cmd & 0xC) == 0xC) with no data is a real ADB timeout.
3. **ADB dead at the Finder, part 2 — unsolicited packets need a PAD
   byte.**  The OS uses the ROM's int-driven Cuda driver (0x408A9BB4/
   0x408A9C3A): it stores the FIRST SR byte of any Cuda-initiated
   session at buffer[0] and parses the packet from buffer[1] (type at
   [1], flags at [2], cmd at [3]) — on real HW that first read is
   stale SR content.  Host-initiated responses get this pad naturally
   (SR still holds the last command-echo byte when the read session
   opens), which is why they always parsed; our unsol path preloaded
   resp[0] (the type) into SR, so the flags byte 0x40 landed where the
   type belongs and EVERY autopolled packet was dropped at the
   dispatch (0x408A9DB8 -> type 0x40 -> drop at 0x408A9E4A) — fully
   read ("response complete" in the log), service routines never
   called, nothing in the event queue (0x14A).  FIX: prepend a 0x00
   pad to unsol packets (s->sr = pad, packet from resp[1]).
   Diagnosis chain worth keeping: ADBBase [0xCF8] device table has
   the service routines (keyboard RAM 0x857CA, mouse ROM 0x408B6582);
   dispatch completion for unsol is the record at driverstruct[0xDE0]
   +52 with proc at +16 (0x408B2FDC) -> ADB manager 0x4080A494.
4. **Mouse cursor frozen, part 1 — stale trailing byte.**  After the
   pad fix the mouse service routine ran but each packet gained ONE
   trailing stale byte (the driver's final SR interrupt stores the SR
   before noticing /TREQ deasserted): 2-byte classic mouse data became
   count 3 = extended-mouse format, dx inflated/sign lost.  FIX: for
   unsol sessions deassert /TREQ WITH the last fed byte (the driver
   detects end-of-packet via PB TIP|TACK|/TREQ == 0x38 right after
   storing each byte, 0x408A9CAE); host-initiated responses keep the
   late deassert + final int the polled ROM drivers need
   (m->cuda_unsol flag).  A 0x00 pad-byte hack instead breaks negative
   deltas (extended format sign lives in the extension byte).
5. **Mouse cursor frozen, part 2 — no video VBL interrupt, jCrsrTask
   never ran.**  The mouse service routine only ACCUMULATES deltas in
   the CursorDevice record (+112/116/120/124 of its data area); the
   coupler into RawMouse/MTemp is **jCrsrTask [0x8EE]**, run from the
   internal-video slot VBL.  On the Q630 the ROM's slot ISR
   (0x4088BC2C, chained from the VIA2 CA1 handler via [0xD74], a1 =
   [0xCEC] = VIA2 base) merges VIA2 PA bits 0/5 with the **F108
   register 0x50F1A101**: bits 2-5 (bits 2-3 gated by enables in bits
   0-1) >>1 -> pending bits 1-4 = slots A-D (our IDE flag bit 5 ->
   slot D), and **bit 6 -> pending 0x40 -> slot table 0x40806F14
   entry 0x06 = pseudo-slot 0 = internal video**.  The slot-0 SInt
   handler (RAM 0xD8A70, installed by the video driver) clears the
   Valkyrie VBL status (+0x10C), waits for +0x108 bit 2 to drop, and
   runs the slot-0 queue -> jCrsrTask -> cursor moves.  FIX: bit 6 of
   ifr reads = (valkyrie status bit 2) AND (valkyrie +0x104 enable bit
   2, byte 0x107); 60 Hz tick + status-clear/enable writes recompute
   the shared VIA2 CA1 line (OR of IDE and VBL sources).  The
   internally stored macide-style "IDE irq enable" bit 6 must NOT leak
   into reads: before the video driver installs the slot-0 handler an
   empty slot queue on interrupt is a SysError (walker 0x40806F10
   returns 51 -> 0x40806EEA = _SysError).
6. Also fixed on the way: **GET_SET_IIC (pseudo 0x22)** now latches
   writes into a per-device register store and answers reads from it
   (device 0xDE/0xDF regs 0x02/0x0D observed, plus a bare 0x41 probe);
   the OS Cuda driver write-verifies these and previously retried in a
   loop (it settles by itself, was not the input blocker, but the
   store ends the retry churn).  And **SET_PRAM now stores all n-4
   data bytes** (the OS syncs XPRAM in 4-byte blocks; only byte 0 was
   kept before) — part of the N+5 commit.

**Shutdown-warning dialog**: appears every boot under -snapshot; both
the HFS dirty bit (MDB drAtrb bit 8 — patching it on a copy did NOT
suppress the dialog) and OS-side state are involved; not worth chasing
since the working keyboard dismisses it (`sendkey ret`).

**Verification pointers**: event queue QHdr at 0x14A (a lone stale
diskEvt parks at qHead when input is dead); RawMouse/MTemp 0x828/0x82C;
ADB table [0xCF8]; mouse accumulators = mouse service data area
+112..127.

**Session pitfalls (new)**:
- gdb-multiarch `-batch` treats `continue` as async against QEMU's
  gdbstub: the script continues past it and dies/detaches while the
  target runs.  Driving gdb through a FIFO from a SINGLE bash call
  (spawn gdb < fifo, exec 3>fifo, echo commands with sleeps) works;
  fd redirections do not survive across separate tool calls.
- `-trace esp_*` produced a 10 GB trace and filled /tmp; deleted-but-
  open trace files keep consuming tmpfs until the qemu holding them
  dies (lsof +L1 /tmp).  Kill stale qemus BY PID before deleting their
  logs.  When /tmp is full the monitor `screendump` can still write to
  a path under /workspace, and command output can be routed to a file
  and Read back.
- `pgrep -f | head -1` grabs the oldest matching PID — after several
  launches that is a STALE qemu, not the new one; the kills then hit
  the wrong process and trace-writing zombies pile up.  Filter by
  `ps -o comm=` == qemu-system-m68 and take the newest, or better keep
  the setsid+$! discipline.

## Q630 ROM (06684214) findings

1. **Machine ID**: 0x5FFFFFFC must read 0xA55A2224 (word reads = low
   word).  The universal entry list is at ROM+0xA79BC (self-relative
   offsets, entries 0x48 bytes); kind-13 entries carry the expected ID
   LOW WORD at entry+0x44 and have strap mask/val = 0 — identification
   is purely by ID word; bit 11 of the ID clear = no VIA1 PA strap
   check at all (runner code at 0x40804b86..0x40804c04).
   Family IDs: 0x2224=box 0x57, 0x2225=0x58, 0x2226=0x59, 0x2231=0x5A,
   0x2232=0x5B (Q630/LC630/P630 group, DecoderInfo 0xA8420);
   0x2250..0x2257=box 0x5C, 0x2258/A/B=box 0x5D (LC580 group,
   DecoderInfo 0xA84C4 — that one has +0x6C = 0x50F1A000 IDE and
   +0x38 = 0x50F24000).  We use 0x2224.
2. **I/O aliasing**: the kind-13 decoder probe checks VIA1 repeats at
   +0x20000 (probe of VIA1+0x1c00+0x20000 at 0x408030ea via the
   writability prober 0x408046ac); IO_SLICE is 0x20000 on the F108,
   not 0x40000 (lc475/q800 value) — identification loops forever
   otherwise.  Probes of 0x50F26000 (no RBV) BERR harmlessly.
3. **Cuda protocol is the REAL Cuda packet protocol** (NOT the Q605
   Egret-style raw-ADB framing):
   - Packets have a TYPE byte: 0=ADB, 1=pseudo; responses have a
     3-byte header [type, flags, cmd] + data.  Pseudo commands seen:
     0x26+0x22 (I2C-ish, generically ACKed), 0x01 AUTOPOLL, 0x07
     GET_PRAM (2-byte addr, returns 1 byte), 0x0C SET_PRAM, 0x03
     GET_TIME/0x09 SET_TIME.  ADB timeout flag = 0x02 in the header,
     autopoll flag = 0x40.
   - /TREQ semantics = real Cuda: LOW while the Cuda has response
     bytes pending, checked synchronously after every host TACK
     toggle (0x408b3bee); deasserted at the toggle after the last
     byte; asserted by ACR-turnaround processing when a response is
     staged.
   - THREE ROM driver styles share the engine: an early polled sync
     (TACK-only session at 0x40885718: Cuda must assert /TREQ as the
     ack; "cuda_sync" special case), a fully polled driver
     (0x408aa040/0x408b3a24: busy-waits IFR bit2, needs an SR int
     for EVERY event incl. one after each session close and one after
     the end-of-response toggle), and an int-driven request queue
     (0x408a9bb4, completion flag polled at 0x408a9b50).
   - The polled driver routinely ABANDONS responses after the header
     byte; a host ACR->output turnaround with stale response staged
     must DISCARD the stale bytes (else the next command is eaten).
   - PRAM: rebuilt via pseudo GET/SET_PRAM over the whole 256 bytes
     (bit-bang RTC engine kept but appears unused by this ROM).
4. **Video (Valkyrie-630)**: register block at 0xF9800000 but LAYOUT
   DIFFERS from the Q605 Valkyrie: monitor sense = drive/read dance
   on +0x200/+0x220 (return 6 at +0x220 reads = 13" 640x480); pixel
   clock PLL bit-banged through +0x3C0; CRTC longs at +0x124..+0x160;
   depth code at +0x20 (0/1/2/3/4 = 1/2/4/8/16bpp).  The ROM draws
   its startup UI as 1152x870 1bpp rowbytes 576 at VRAM+0x80
   REGARDLESS of sense (fb defaults set to that; MacOS geometry will
   come from the PixMap tracker).
5. **Singer sound codec at 0x50F04000** (F108 slice +0x4000): the
   sound driver writes reg/value pairs through a byte mailbox: +2
   command/status (bit0 = ready, polled at 0x408b9fec/0x408ba0f4),
   +6 data.  RAM-backed regbank + bit0-always-set on +2 reads
   satisfies it.  Failure to map it = infinite retry loop with VIA1
   PA3 toggles ("portA_write unimplemented" spam).
6. **24-bit addressing (the big one)**: MacOS boots this ROM in 24-bit
   mode (rebuilt PRAM), and the ROM sets ROMBase = 0x00800000 plus
   68040 DTTR1/ITTR1 = 0x807FC040 (transparent-translate logical
   0x80000000-0xFFFFFFFF to identical physical).  Two decodes are
   therefore required that lc475 didn't need:
   - **ROM window at physical 0x00800000**: the ROM installs Device
     Manager DCE driver pointers based at 0x00800000 (e.g. the .Sony
     DCE 0x0086C3E0).  A ROM alias is mapped there, ENABLED by the
     PrimeTime II write of 0x1B to 0x50F18200 (right after the PRAM
     addressing-mode byte 0x8A is stored) and kept disabled before
     that so the RAM test sees RAM in that range.
   - **RAM mirror at 0x80000000**: tagged handle/DCE pointers formed as
     0x80000000 | 24bit_addr (e.g. the .Sony DCE accessed as
     0x800061E0) transparent-translate to phys 0x80xxxxxx, so the low
     16 MiB of RAM is mirrored there.
   With both, the .Sony install and the tagged-pointer dereferences
   resolve; the boot then advances to the dsLoadErr Sad Mac above.
7. **ASC boot chime**: the ROM synthesises a startup chime into the ASC
   FIFOs and busy-waits on the FIFO half/empty status (+0x804) with a
   tight timeout (0x40845de0..0x40845e08); the real drain is host-real-
   time-paced and always loses under TCG -> death monitor.  Shim over
   +0x804 fast-drains the FIFOs so the status reads "played" instantly.
8. **PITFALL (cost ~2 h)**: `pgrep -f "M quadra630" | head -1` matches
   the LAUNCHING BASH WRAPPER, not qemu — every "kill" killed the
   wrong process and stale qemu instances accumulated, all appending to
   the same -D logfile and racing for the monitor socket (phantom
   BERRs from pre-fix binaries).  Use `QPID=$!` from the setsid launch,
   or `pgrep -f "qemu-system-m68k -M quadra630" | grep -v bash`.
9. **PITFALL**: a bash `python3 - <<EOF` heredoc that edits a file under
   `hw/m68k/` leaves the shell cwd there; a following `ninja -C build`
   then fails ("chdir to 'build'") and any `build/qemu-...` launches the
   STALE binary.  Always `cd /workspace/src/qemu-mac2` before build/run.

## Build/test commands

    ninja -C build qemu-system-m68k
    build/qemu-system-m68k -M quadra630 \
        -bios /workspace/files/mac-roms/quadra630.rom \
        -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
        -snapshot -serial null -serial null -display none \
        -monitor unix:/tmp/q630.sock,server=on,wait=off
    # kill by saved PID only (NEVER pkill by name); screendump via monitor
