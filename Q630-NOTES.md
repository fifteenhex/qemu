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
- [ ] **BLOCKED: Sad Mac `0000000F / 00000001` = dsLoadErr (SysError
      15), a driver/segment LOAD failure during late startup.**  Does
      NOT reach the Finder.  Video renders the Sad Mac fine (framebuffer
      works).  IDE controller is present and enumerates an attached
      disk (verified via `info qtree` / `info block`).

## FINAL STATE (honest)

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

**Update (see "Session N+1" under "Suspected next step" below):** the
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
