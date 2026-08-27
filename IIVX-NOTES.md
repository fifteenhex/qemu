# Macintosh IIvx / IIvi / Performa 600 — implementation journal

Branch `add-iivx`, off the consolidated `amiga` branch. Builds to
`/tmp/iivx-build` (tmpfs) per `configure --target-list=m68k-softmmu
--disable-docs --disable-tools`; `/workspace` never used as a build dir
(63G disk was at ~950MB free at task start).

## ROM

Fetched from the Internet Archive item `mac_rom_archive_-_as_of_8-19-2011`
(55 MB zip), member `4957EB49 - MacIIvx & IIvi.ROM`:

```
curl -sSL "https://archive.org/download/mac_rom_archive_-_as_of_8-19-2011/mac_rom_archive_-_as_of_8-19-2011.zip" -o /tmp/macroms.zip
unzip -p /tmp/macroms.zip "4957EB49 - MacIIvx & IIvi.ROM" > /workspace/files/mac-roms/macIIvx.rom
rm /tmp/macroms.zip
```

Saved to `/workspace/files/mac-roms/macIIvx.rom`. Verified: 1,048,576 bytes
(1 MB), first 4 bytes `4957eb49` (the ROM's own checksum, matching the
expected filename tag exactly). Per classic Mac ROM convention, ROM offset
0 (the checksum) and offset 4 (a base-relative entry offset, `0x2a` here)
are read directly as the CPU's initial SP/PC at reset — same convention
maciisi.c/maciici.c already model.

## Research: the official memory map

Apple's own 1992/1995 Developer Note, "Macintosh IIvx (also includes
Macintosh IIvi and Performa 600)", was found and read in full (6-page
title info + ~50 content pages) via
`https://leopard-adc.pepas.com/documentation/Hardware/Developer_Notes/Macintosh_CPUs-68K_Desktop/Mac_IIvx.pdf`.
This is the single most useful thing found for this task — it gives a
**confirmed, published physical memory map** (Table 1-2), rather than the
address ranges having to be rediscovered by trial like the maciici/maciisi
predecessors evidently were (see their extensive historical comments).

Table 1-2, 32-bit mode:

| Function | Range |
|---|---|
| RAM | `$0000 0000`–`$043F FFFF` (68 MB: 4 MB soldered + 4 SIMM slots) |
| ROM | `$4000 0000`–`$403F FFFF` (1 MB, at the base, unlike IIci/IIsi's `+0x800000`) |
| I/O | `$50F0 0000`–`$50FF FFFF` |
| VRAM | `$60B0 0000`–`$60BF FFFF` (1 MB max, **dedicated**, not RAM-stolen) |
| RAM disk | `$7000 0000`–`$743F FFFF` (not implemented here) |
| NuBus | `$8000 0000`–`$EFFF FFFF` |

24-bit mode mirrors this exactly at `<<24`-truncated addresses (RAM
`$00 0000`–`$7F FFFF` = 8 MB max, ROM `$80 0000`–`$8F FFFF`, I/O
`$F0 0000`–`$FF FFFF`, VRAM `$B0 0000`–`$BF FFFF`, NuBus `$C0 0000`–
`$EF FFFF`).

Other confirmed facts used directly:

- **VASP** is the integrated controller (video + memory-map + sound +
  clock + "GLU"/VIA functions), successor to the RBV/V8 family used by
  the IIci/IIsi/LC. Apple explicitly did **not** publish VASP's internal
  register map ("never use absolute addresses to access hardware... these
  addresses are subject to change" — the note itself only gives the
  *external* physical windows, not VASP's internal decode).
- **ADB/RTC/PRAM/power**: "the same 68HC05 microcontroller first used in
  the Macintosh IIsi" — i.e. Egret, confirming maciisi.c (not maciici.c)
  is the right base to fork from.
- **Video**: dedicated VRAM (two 68-pin SIMMs, 512 KB standard / 1 MB
  max), unlike the IIci/IIsi's RAM-stolen frame buffer. Default video mode
  is 8-bit color (this model still only renders 1-bit, see below).
- **Gestalt IDs** (`gestaltMachineType`): IIvx 48, Performa 600 45, IIvi
  44 — matches `MAC_MODEL_IIVX`/`MAC_MODEL_P600`/`MAC_MODEL_IIVI` already
  in `include/standard-headers/asm-m68k/bootinfo-mac.h`.
- **Model differences** (Table 1-1): clock speed (32/16/32 MHz), RAM
  cache (32 KB/none/none), FPU (standard/optional/optional). Nothing in
  the note documents a hardware *strap* difference the ROM reads to tell
  the three models apart — plausibly it's a clock-speed timing measurement
  and/or a cache-presence probe, neither of which this icount-based
  emulation can reproduce faithfully. **Not implemented**: all three
  machine types run hardware-identically here (see below).

## What was built

`hw/m68k/maciivx.c` — forked from `hw/m68k/maciisi.c` (Egret variant, not
maciici.c's discrete-RTC variant, per the ADB finding above) via a
mechanical rename (`maciisi`→`maciivx` etc.), then corrected against the
Developer Note:

- `MACIIVX_ROM_ADDR` = `0x40000000`, `MACIIVX_ROM_SIZE` = `0x100000` (was
  `0x40800000`/`0x80000` in the template — wrong base *and* wrong size for
  this machine).
- Added a real, dedicated **VRAM** `MemoryRegion` (`memory_region_init_ram`,
  1 MB) at `0x60B00000` (32-bit) with a `0x00B00000` (24-bit) alias, and
  pointed the framebuffer device's scanout at it instead of at
  `machine->ram` — the single biggest hardware-accuracy difference from
  the RBV-family template, matching VASP's documented dedicated-VRAM
  design. The framebuffer renderer itself is still the maciici/maciisi
  placeholder (1-bit black/white only, `MACIIVX_FB_WIDTH/HEIGHT` = 640x480);
  it does not yet decode 8-bit CLUT color, since VASP's VDAC/RBV-analog
  register semantics aren't published and `vdac_regs`/`rbv_regs` are still
  store+log stubs inherited from the template.
- Added a **24-bit ROM alias** at `ADDR24_ROM_BASE` (`0x800000`), which
  the RBV-family siblings deliberately do *not* have ("the 24-bit map only
  exists once the ROM programs the PMMU"). This IIvx ROM demonstrably
  contradicts that assumption: traced with `-d unimp` (see Blocker below),
  it jumps to and executes from the 24-bit ROM window very early in boot,
  well before any PMMU setup could plausibly have happened. Without this
  alias the CPU walks off into unbacked ("ram-hole") memory reading back
  as zero-decoded quasi-NOPs forever; with it, execution reaches real ROM
  content at that address (which turned out to still not be the fix that
  gets past the blocker below, but is unambiguously the hardware-correct
  decode per Table 1-2 and was kept).
- Fixed the RAM-size bound and error message to the real 68 MB max (was
  copy-pasted "128 MiB (two 64 MiB banks)" from the IIci/IIsi template,
  which does not apply here).
- I/O slice layout (`IO_BASE 0x50000000`, `VIA1_OFS`/`SCC_OFS`/`SCSI_OFS`/
  `ASC_OFS`/`SWIM_OFS`/`VDAC_OFS`/`RBV_OFS`), VIA1/Egret transport, RBV
  register model, NCR5380 SCSI (+ pseudo-DMA + handshake apertures), SWIM,
  ASC, and NuBus wiring are all carried over **unverified** from
  maciici.c/maciisi.c beyond the confirmed `0x50F00000` I/O base itself —
  VASP's actual internal register map was never published by Apple (see
  above), so these offsets are a starting hypothesis, not a confirmed
  fact, and inline comments throughout the file flag this explicitly.
  Likewise, many comments carried over from maciisi.c cite specific ROM
  hex addresses (e.g. Egret protocol notes, RBV IRQ dispatch addresses)
  that are **only valid for the IIsi ROM binary**, not this different
  1 MB IIvx/IIvi ROM image; they were kept because they still document the
  general *protocol* correctly, but the literal addresses in those
  comments should not be trusted for this file.
- Three machine types share one `MacIIvxMachineClass`/common abstract base
  (`maciivx-common`, mirroring the `quadra950.c` sibling-class pattern),
  differing only in `mc->desc` and the (currently unused for boot
  behavior) `mac_model` Gestalt ID: `maciivx` (48), `maciivi` (44),
  `performa600` (45, per the task's "rebadged IIvx" framing). All three
  use the identical `pins_a` VIA1 strap value and are hardware-identical
  otherwise — see "Model differences" above for why.
- `hw/m68k/Kconfig`: new `MACIIVX` config block (ADB, MOS6522, ESCC, SWIM,
  ASC, OR_IRQ, FRAMEBUFFER, NCR5380, NUBUS, NUBUS_MAC8390 — same selects as
  `MACIICI`/`MACIISI`).
- `hw/m68k/meson.build`: added `maciivx.c` under `CONFIG_MACIIVX`.

`hw/m68k/maciici.c` and `hw/m68k/maciisi.c` were **not modified** —
`maciivx.c` is a fork, not a shared-code change, per the task's
"keep maciisi/maciici bit-exact" instruction.

## Build

```
mkdir -p /tmp/iivx-build && cd /tmp/iivx-build
/workspace/src/qemu-iivx/configure --target-list=m68k-softmmu --disable-docs --disable-tools
ninja qemu-system-m68k
```

Clean build, no warnings from `maciivx.c`. `qemu-system-m68k -M help`
lists `maciivx`, `maciivi`, `performa600` alongside the existing machines.

## Boot result: blocker

Boot command (MacOS 7.5.3, `/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda`,
`-snapshot` so the shared image is never modified):

```
/tmp/iivx-build/qemu-system-m68k -M maciivx \
  -bios /workspace/files/mac-roms/macIIvx.rom \
  -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
  -snapshot -serial null -serial null -display none -icount shift=7 -m 8 \
  -qmp unix:/tmp/iivx-qmp.sock,server,nowait -d unimp,guest_errors -D /tmp/iivx.log
```

**Result: machine exists, ROM loads and runs real code, but boot faults
before any video output** (screendump at `/tmp/iivx-shot.png` is blank
white — the framebuffer VRAM is never touched). This is the documented
"min" bar from the task brief; the trace below shows it get considerably
further than a bare stub before faulting.

Traced sequence (`-d unimp,guest_errors`, and a short `-d in_asm` capture
disassembled by hand against the ROM binary):

1. Reset vector (`SP`/`PC` poked into physical 0/4 from ROM offsets 0/4,
   entry = `ROM_ADDR + 0x2a` = `0x4000002a`) — ROM starts executing.
2. `io: read +0x21c00 -> BERR` — a decoder-kind identification probe,
   analogous to the "must NOT respond at +0x20000" fingerprint check
   maciici.c documents for its own family; ours correctly bus-errors,
   so identification does not appear to divert down a wrong branch here.
3. A sequence of legitimate `rbv`/`vdac`/`rbv-via2` register reads/writes,
   all serviced by the (unverified/best-guess, see above) stub register
   model with no bus errors — consistent with the ROM's board/monitor
   identification sequence completing "successfully" from its point of
   view (whether the *values* it reads are hardware-accurate for real
   VASP is the open question).
4. Control reaches a ROM-internal jump-table dispatcher at `0x40002f52`
   (`movel %fp,%d0; lea 0x400032b4,%a1; ...; jmp %a0@(0,%a2:l)`); computed
   **statically** by hand against the ROM image (Python, no emulator
   needed) — this resolves correctly to `0x40003064`, legitimate ROM code,
   so this particular dispatch is not the fault.
5. Execution enters a loop (`0x40002f72`..`0x400032b2`) whose body writes
   through pointers at offsets `+0x400` and `+0x1E00` relative to a moving
   base register — `0x1E00` is exactly the "twin routine tables" lowmem
   global address the maciisi.c/maciici.c XPRAM comments already document
   for this ROM family, so this is very likely Memory-Manager low-memory
   table setup, not device access.
6. The loop's final `jmp %fp@` (an indirect jump through a register
   reloaded from a caller-supplied continuation address each iteration)
   lands at an address computed relative to **installed RAM size**
   (observed at exactly `RAM_top - 2`: `0x007ffffe` for an 8 MB
   configuration, `0x00409ffe`-ish region for a 4 MB configuration —
   confirmed by rerunning with `-m 4`, which shifted the runaway
   proportionally). Nothing was ever written there in this emulation, so
   it reads back zero from the generic "unbacked RAM reads return 0"
   `ramio` catch-all (a mechanism maciici.c/maciisi.c also use, for
   RAM-sizing purposes, and which those machines boot fine with).
7. Zero words decode as valid-but-meaningless `ori.b #0,d0`-style
   instructions, so the CPU **executes a long runaway "NOP sled"**
   upward from that point (confirmed via the `-d in_asm` capture: PC
   advances in lockstep with the read address, 2 bytes at a time).
   - With 8 MB RAM, the sled reaches the (now-mapped, see above) 24-bit
     ROM alias at `0x800000` and decodes real ROM bytes — the ROM's own
     checksum (`0x4957`, `0xEB49`) — as opcodes, both illegal, faulting
     twice in a row (`Illegal instruction: 067c @ 800008` /
     `Illegal instruction: 4957 @ 800000`) and (with a SCSI drive
     attached) then loops pushing `0x2700`/exception-frame-shaped words
     to a descending stack pointer — a classic 68030 "fault during
     fault-handling" condition, which the real CPU spec defines as a full
     halt. QEMU's process exits shortly after (no crash/assert — the
     board simply goes idle under `-icount` with nothing left to do).
   - With 4 MB RAM the sled just continues past `0x400000` still reading
     zeros (hadn't yet reached `0x800000` within the observation window).

**Root-cause hypothesis** (not confirmed — would need either real VASP
register documentation, which Apple did not publish, or the kind of
long empirical trace-and-fix cycle the extensive historical comments in
maciici.c/maciisi.c show already went into *those* machines): step 6's
continuation address is very likely computed from a VASP-specific
register value (monitor sense, RAM-cache-presence probe, or a VASP
identification register with no RBV equivalent) that this model doesn't
implement faithfully, since `rbv_regs`/`vdac_regs` here are the
maciici/maciisi RBV stub verbatim, not real VASP semantics. The mechanism
itself (unbacked RAM reads as 0) is proven fine in general — maciici.c
and maciisi.c use the identical pattern and reportedly boot to Finder —
so the divergence is specifically in *what this ROM asks the hardware*
during this Memory-Manager setup sequence, not in the general emulation
approach.

## Session 2: past the double-fault — the power-on ROM overlay

The "double illegal-instruction fault" above was fully root-caused and
fixed.  It was NOT a VASP register-fidelity problem; it was the missing
**power-on ROM overlay**.

### What the fault actually was

Tracing backward from the runaway (gdb-free, using `-d exec,cpu
-dfilter` around the fault PC plus static disassembly of the ROM image),
the fault is StartBoot's self-relocation at ROM `0x4000398e`:

```
0x4000398e: movel a0@(112),d3   ; d3 = *(a0+0x70); a0 = ROM base table
0x40003992: subl  a2,d3         ; a2 = PC-relative ROM base
0x40003994: addal d3,a0
0x40003996: addal d3,a1
0x40003998: jmp   0x40002e38    ; -> addal d3,a4 ; jmp (0x40002e3e + d3)
```

`*(a0+0x70)` is a hardcoded **0** in the ROM image, so `d3 = 0 - a2`.
Run from the LOW power-on overlay (`a2 == 0`) this is `d3 == 0`: a NO-OP
relocation that keeps executing in place.  The bug was that the reset PC
had been forced into the HIGH ROM window, making `a2 == 0x40000000`, so
`d3 == -0x40000000` relocated everything into blank low RAM -> NOP sled
-> the double fault.  The ROM is *designed* to boot from the overlay.

### Fixes applied this session (all in `hw/m68k/maciivx.c`)

1. **Power-on ROM overlay** (Developer Note, "Power-On Overlay
   Function"): ROM mirrored at physical 0 (priority-1 alias over the RAM
   container), active at reset.  The reset vector (SP=ROM[0], PC=ROM[4]
   =0x2a) is therefore a LOW overlay entry.  Reset-handler ordering
   forced a change: `main_cpu_reset` can't `ldl_phys` the vector (the
   ROM loader's own reset handler runs later and the overlay shadows the
   low-RAM poke the IIci/IIsi templates use), so SP/PC are captured from
   the loaded image in `machine_init` and applied directly.
2. **ROM also mapped at 0x40800000** (`MACIIVX_ROM_HI_ADDR`).  The ROM's
   own hardware base-address table (ROM offset 0x3484) and its observed
   high-ROM PCs both use 0x40800000 as the canonical base -- the same
   site the IIci/IIsi use -- not the Table 1-2 0x40000000 window.  Both
   are now decoded.
3. **ASC FIFO-IRQ-status fixup** (maciivx-local, over ASC reg 0x804).
   Deep in StartBoot the ROM does a playback-paced single-byte FIFO
   drain/fill poll (ROM 0x85e00-0x85f02) waiting for the FIFO FULL/EMPTY
   and HALF_FULL bits -- a drain-timed calibration like the ROM's other
   TimeDBRA/SETUPTIMEK loops.  The shared ASC drains its FIFO from the
   audio backend on a WALL-CLOCK cadence; under `-icount` a CPU spinning
   in this tight poll runs virtual time far faster than the backend
   consumes, so the queued byte never drains and the ROM hangs forever.
   (The RBV siblings dodge this: their ROMs blast the whole boot chime
   through the FIFO in bulk -- verified by tracing maciici, which reads
   the status register only twice.)  The fixup reports every FIFO status
   bit asserted so each phase of the paced loop advances; `hw/audio/asc.c`
   is untouched.

### How far it boots now

From an immediate double-fault to **deep into StartBoot**: reset ->
overlay execution -> board/decoder identification -> RBV/VIA2/VDAC
probes -> Egret/RTC PRAM streaming -> ASC sound-hardware calibration ->
a SECOND StartBoot pass running from the high ROM window (0x4080_3xxx)
-> a serial-debugger poll loop at ROM `0x4084a0e6` (with `d7` bit17 set,
polling SCC RR0 Rx-available at 0x50f04002).  No fatal fault -- it spins
there.  Still no video (the ROM reaches the debugger before initialising
the display), so the screendump is blank white
(`/tmp/iivx-final.png`).

### Remaining blocker (well-characterised)

The overlay is left permanently ON because its documented teardown ("on
first high-ROM access, replaced by RAM") does not fit this ROM: it keeps
reading LOW addresses as ROM well into the high-ROM StartBoot pass (its
globals, `VBR`, and stack land in the `0x00046xxx` region, i.e. UNDER
the overlay), so removing the overlay early (tested: on the first
hardware write from a high-ROM PC) strands those on blank RAM and faults
immediately (`A7=0xff900040`, `VBR=0x461d0`, fault at `0xff8ffffc`) --
the same garbage-globals fault as running with no overlay at all.  Real
II-family hardware resolves this by exposing DRAM at a SEPARATE high
alias while the overlay covers low ROM (so the ROM builds its globals in
that RAM alias, then drops the overlay); the exact VASP aliasing was
never published.  The serial-debugger poll the ROM ends up in is the
downstream symptom of those RAM-shadowed globals going bad.  Modelling
the VASP overlay-time RAM alias is the next step and the last thing
between here and video/happy-mac.

(Session 2's "Remaining blocker" above -- the stranded-globals theory --
was CONFIRMED and resolved in session 3 by the DRAM pre-load; see that
section for the current state.)

## Session 3: modelling the overlay as a pre-loaded DRAM copy

Session 2 left the overlay permanently ON (a read-only ROM alias over
low RAM), which stranded StartBoot's stack/VBR/globals -- confirmed here
at ROM `0x46660` (`moveal sp@,a0`, SP=0x2600): a read-only shadow answers
that stack read with ROM bytes, so the ROM ran on garbage and dropped
into a serial-debugger poll.

### Fix: pre-load the ROM image into low DRAM (writable overlay)

Instead of shadowing DRAM with read-only ROM, the low `MACIIVX_ROM_SIZE`
bytes of DRAM are now filled with the ROM image (a single memcpy after
the bios load).  Low memory holds a WRITABLE copy of the ROM: fetches and
the reset vector see ROM content (PC-relative StartBoot runs, a2==0,
reloc no-op), while the stack/VBR/globals the ROM writes there persist
and read back correctly.  This is the coordinator's overlay/teardown
model realised without a teardown-timing hazard -- real hardware reveals
writable DRAM at 0 after the first high-ROM access; pre-loading the copy
gives the same net state from reset.  The former `rom_overlay` alias and
`overlay` bool are removed.

### Result: the ROM now runs its REAL power-on self-test

With globals coherent the boot path changes completely: the ROM no
longer executes against overlay-shadowed garbage.  It runs its genuine
POST from low RAM (0x45exx ASC test, 0x46xxx/0x49xxx phase code) -- the
ASC test at ROM 0x45eca correctly reads the EASC version (0xB0) and
iterates a FIFO drain/fill calibration (the ASC 0x804 fixup lets each
phase advance).

### Remaining blocker: POST parks in the serial diagnostic console

The ROM ends in a tight loop at ROM `0x49a00`/`0x4a0e6` with interrupts
masked (SR I:7, zero interrupts delivered) -- its MicroBug serial
console, polling the SCC (RR0 Rx, 0x50f04002) and d7 bit16 for an exit
that (masked) never comes.  Same failure class as the Classic II ROM
(same 68030 StartBoot family; cf. `hw/m68k/macclassicii.c` pins-a/POST
notes): a non-fatal POST TIMING test that can't pass under TCG makes the
phase sequencer park in the console (Classic II 0x40849b1c; IIvx 0x49a00,
reached unconditionally from ROM 0x499f6 which loads fp=0x49a00).  The
Classic II needed the right PA0/PA7 straps, a NOP of the POST-restart
branch, and ROM-checksum repair after patching; the IIvx will need the
analogous treatment plus the lc475/mac_via SETUPTIMEK pattern (let the
calibration run, stuff known-good constants at its end) -- the ASC 0x804
fixup here is a crude stand-in that makes the calibration measure a
meaningless value.  Deep, iterative ROM-bring-up (Classic II took many
passes); characterised, not completed here.

## Summary (current)

- Machine types `maciivx`, `maciivi`, `performa600` build cleanly and
  are selectable; `maciici`/`maciisi`/`hw/audio/asc.c` untouched
  (regression-checked: maciici still boots to its normal deep PCs).
- ROM (`4957EB49 - MacIIvx & IIvi.ROM`, checksum-verified,
  `/workspace/files/mac-roms/macIIvx.rom`) loads and, with the overlay
  modelled as a pre-loaded writable DRAM copy, runs its GENUINE
  power-on self-test (coherent stack/VBR/globals), reaching its serial
  diagnostic console (ROM 0x49a00) via that real POST -- no fatal fault.
  Screenshot blank (`/tmp/iivx-postoverlay.png`); not yet happy-mac/
  Finder.
- Blocker precisely identified: a Classic II-style unpassable POST
  timing-calibration test parks the phase sequencer in the MicroBug
  console; fix is the SETUPTIMEK/known-good-calibration + park-branch-NOP
  bring-up, done per this ROM's offsets.
- `maciivx.c` reuses the RBV/Egret/SCSI/SWIM/ASC/NuBus machinery from
  the siblings, corrected against the official Apple memory map (ROM
  base 0x40800000 + 0x40000000 mirror, dedicated VRAM, 24-bit ROM alias,
  68 MB RAM ceiling), this ROM's power-on-overlay boot model (pre-loaded
  DRAM copy), and the ASC FIFO-status fixup.

## Session 4: full POST passes (chime plays) via the Classic II relocation recipe

The session-3 conclusion was wrong in mechanism: it is NOT an
"unpassable timing test".  Root-caused the actual park.  The failing
POST phase is the ROM's **word-sum self-checksum** (ROM 0x46bd0): it
sums `a0`-based (a0 = ROM base = PC-relative) and compares against the
32-bit total stored at ROM[0].  With the session-3 pre-loaded DRAM copy
the ROM ran from LOW RAM (a0=0), which StartBoot had since written
(stack/globals), so the sum mismatched -> d6=0xffff -> the phase
sequencer (ROM 0x465fe: `tstl d6; bne 0x48dea`) parked in the serial
console.

### Fix: run the whole POST from clean HIGH ROM (the Classic II recipe)

Mirrored `hw/m68k/macclassicii.c`'s bring-up (its 0x34fc/0x3ae6 patches +
checksum repair), adapted to this ROM's offsets:
1. **Drop the overlay entirely.**  Low memory is now plain writable DRAM.
2. **Reset PC = HIGH** (`MACIIVX_ROM_HI_ADDR + ROM[4]`, i.e. 0x4080002a):
   execution begins in the clean high ROM window.
3. **Relocation-slot patch**: StartBoot's self-reloc (ROM 0x398e) reads
   its target base from ROM[0x34f4] (the 0x3484 decoder base table,
   +0x70), which ships as 0.  Patch it to 0x40800000 so d3 = base - a2 ==
   0 (a2 = high base) -> the reloc is a NO-OP and the ROM stays in high
   ROM.  (Without the patch, high reset PC + zero slot = d3 = -base ->
   relocate into blank low DRAM -> the original double-fault.)
4. **Checksum repair** (mirrors macclassicii.c): the 0x34f4 patch changes
   ROM bytes, so recompute the word-sum (every big-endian 16-bit word
   from offset 4 to end) and re-store it at offset 0.  This ROM uses only
   the single word-sum scheme (the 0x46bd0 routine has no byte-lane
   block), so only offset 0 is repaired.  Verified: the checksum phase's
   `eorl d4,d1; beq` now falls through (d6 not set).

The ASC FIFO-status fixup (ASC reg 0x804) is retained -- the POST's ASC
sub-tests still poll it.

### Result: POST passes, boot chime plays, framebuffer goes live

Boot now runs the ENTIRE power-on self-test from clean high ROM with no
fatal fault, all phases pass (checksum d6=0), and it proceeds to the
**boot-chime generation** (ASC FIFO waveform fill + delay loops at ROM
0x40885e78-0x40885f32, a3 = ASC).  The dedicated VRAM framebuffer is now
scanned out live: the screendump shows a dithered gray pattern
(`/tmp/iivx-postpass.png`, extrema 0..255) instead of blank white --
exactly the post-POST state the Classic II reached (uninitialised
VRAM/heap pattern; the happy-mac draw is past this point).

### New frontier: the cold-boot serial console (needs the OS-startup work)

After the chime the ROM takes its cold-boot path (ROM 0x48dea -> 0x48e14
-> 0x408499bc) and lands in the cold-boot **serial console** loop at ROM
0x4084a0e6 (SCC RR0 poll at 0x50f04002, d7 bit17 set, interrupts masked).
This is the SAME point the Classic II reached after its POST fixes: the
cold-boot console is the normal path but idles here until the ROM's
disk-boot continuation runs.  Per the Classic II "deep OS-startup
session" (macclassicii.c findings 1-6), getting past it needs the Egret
cold-start /XCVR sync, the Egret GET_PRAM/SET_PRAM/READ_MCU pseudo-command
+ XPRAM-ddType seeding (the boot-driver installer), ADB packet handling,
the kind-5 MMU map, framebuffer base, and a dedicated-VRAM/heap carve-out
-- substantial, iterative OS-startup bring-up.  That is the next frontier.

### Status after session 4

- **Full POST passes**; boot chime plays; framebuffer live (dithered
  VRAM, `/tmp/iivx-postpass.png`).  From an immediate double-fault (start
  of these sessions) to a complete ROM power-on self-test.
- Parks in the cold-boot serial console (ROM 0x4084a0e6) -- the Classic
  II post-POST state; next is the Egret/XPRAM/disk-boot OS-startup work.
- Model: no overlay; reset PC high; ROM relocation-slot patch (0x34f4 ->
  0x40800000) + word-sum checksum repair, mirroring macclassicii.c.
- Regression: only `hw/m68k/maciivx.c` touched; `maciici` re-verified
  (boots to its normal deep PCs); `maciisi`/`q800`/`hw/char/escc.c`/
  `hw/audio/asc.c` untouched.

## Session 5: OS-startup investigation -- the cold-boot serial console is the frontier

Attempted to port the Classic II OS-startup recipe (macclassicii.c +
CLASSIC030-NOTES.md).  Traced the post-chime path precisely; the blocker
is the **cold-boot serial console** (the ROM's MicroBug), which the
Classic II hit at the same point.  Findings (code experiments reverted --
they did not reach disk boot; documented for the next session):

### The console is gated by d7 bit26, and BOTH paths reach it

After POST+chime the ROM checks d7 bit26 (ROM 0x48df8: `btst #26,d7; bne
0x48e18`):
- bit26 CLEAR -> 0x408499bc -> (0x408499c8 btst #26 again) -> 0x499f6 ->
  the GetChar loop.
- bit26 SET  -> 0x48e18 -> warm-restart resume-vector check (reads
  0x58000000 for magic 0xAAAA5555; legitimately 0 on cold boot) -> 0x48e3c
  (bset #16, bset #22) -> 0x4084a094 (VIA1 T2 timer setup) -> the SAME
  GetChar loop.

The GetChar loop (0x40849a08 -> 0x4084a0e6) polls SCC RR0 (0x50f04002) for
a serial character and, with bit16/bit22 set, runs a VIA1-T2-delay +
'*'-banner (PutChar) sub-loop -- the MicroBug '*' command interpreter.
Its only software exit is a received serial byte (never delivered with
`-serial null`); interrupts are masked (SR I:7).  This is EXACTLY the
Classic II's cold-boot console (CLASSIC030-NOTES "New frontier: the
cold-boot serial-console loop at 0x40849b1c" -- "the normal cold-boot
path" whose "exit mechanism we're missing ... likely an interrupt-driven
boot step").

### Bit26 is set by a decoder-kind check the VASP path never reaches

`bset #26,d7` lives at ROM 0x46576, gated at 0x46540 by
`(a3@(64) & 0x1c) == 0x10` (a3 = a0@(32), a decoder/globals record) --
the analogue of the Classic II's PA0/bit26 strap-gate that macclassicii.c
fixes with pins-a.  This site never executes on the IIvx: the decoder-kind
identification (the `btst #28,d2` + `movesw 0x22000` FC7 bus-error probe at
ROM 0x3982/0x399c, which yields the machine capability word d0=0x773f --
the SAME value the Classic II computes) routes the IIvx down a path that
skips it.  The IIvx has NuBus (unlike the compact Classic II), so on real
hardware it should take the NuBus-machine boot branch (the IIci/IIsi
0x46494-analogue) rather than either console path -- that branch is where
the missing disk-boot continuation lives.

### Experiments tried (reverted)

- Forcing the bit26 gate to the real-boot branch (ROM 0x48dfc bne->bra +
  checksum repair): steers onto the cold-boot continuation and exercises
  the VIA-T2/SCC console setup, but lands in the same MicroBug GetChar
  loop (dead-ends on serial), so it does not reach disk boot.  Reverted
  (speculative; forcing bit26 is likely wrong for a NuBus machine).
- A genuine VIA1-T2 IFR fix (reconstruct the T2 one-shot flag from elapsed
  time on a polled IFR read, matching the real 6522 where the flag latches
  regardless of IER -- the shared mos6522 only sets it when IER.T2 is
  enabled, so the ROM's IER-disabled T2 poll spins forever).  Correct in
  isolation but only exercised via the (reverted) gate patch, so reverted
  too; re-add when the flow that needs it is reached.

### Next steps (unchanged deep OS-startup frontier)

The console exit needs the interrupt-driven cold-boot continuation, which
in turn needs the correct VASP/NuBus decoder-kind identity so the ROM
takes the disk-boot branch instead of the MicroBug console.  From there
the Classic II recipe applies in order: Egret cold-start /XCVR sync ->
GET_PRAM/SET_PRAM/READ_MCU pseudo-commands + XPRAM ddType seeding -> ADB
over the byte-handshake framing -> SCSI disk boot -> the kind-5 MMU map +
ScrnBase = dedicated VRAM 0x60B00000 + a VRAM/heap carve-out.  Substantial
multi-session work (the Classic II took a dedicated "deep OS-startup
session").  Current committed state is unchanged from session 4: full POST
passes, boot chime plays, dedicated-VRAM framebuffer live (dithered
pattern), parked in the cold-boot serial console.

## Session 6: root-cause narrowed and several prior theories DISPROVEN

Goal this session: reach Finder.  Not reached -- same wall as sessions
4/5 (cold-boot MicroBug serial console).  But the blocker is now
characterised far more precisely, and several earlier hypotheses were
tested and **disproven** with correct-endian gdb (the earlier sessions'
gdb register reads were likely corrupted by a missing `set endian big`
-- m68k register values come back byte-swapped without it; every gdb
run this session uses it, and all register values below are verified).

### Tooling correction (important for future sessions)

- gdb-multiarch MUST have `set endian big` or every `$dN`/`$aN`/`$pc`
  read is garbage (address breakpoints still work, register values do
  not).  This silently invalidated some mid-session measurements until
  caught.
- **Never use `-gdb tcp::1234`**: that port is the other agent's
  maclc550 debug stub.  Connecting to it runs THEIR frozen machine.
  Use a unique port (this session used 1276) and always check
  `/tmp/<stdout>` for "Address already in use" before trusting a run.
- QEMU monitor `xp/Nwx <addr>` (physical) works for ROM/RAM dumps.

### The exact steady state (mapped instruction-by-instruction)

Parked in the ROM's MicroBug command interpreter, an infinite loop:
`0x40849a08` (jmp GetChar `0x4084a0e6`) -> GetChar polls SCC RR0 Rx
(`0x4084a0f0: btst #0,%a3@(2)`, a3=SCC 0x50f04000) gated by `d7` bit17,
returns 0x8000 (no char) -> `0x40849a10 tstw d0; bmi 0x40849b02` ->
`0x40849b02 btst #16,%d7` (bit16 clear) `beq 0x40849b8a` ->
`0x40849b8a bra 0x40849a08`.  SR=0x2708 (I:7, interrupts masked).  The
ONLY software exits are a received serial byte (never, with
`-serial null`) or `d7` bit16 (never set in the loop).  A true infinite
loop given the register state -- so the machine was mis-routed here, it
is not "waiting" for anything deliverable.

### How the console is reached (the real path, corrected)

POST **passes** and the machine reaches the console via the NORMAL
cold-boot continuation, NOT via any failure branch:

1. POST word-sum checksum (routine `0x40846bd0`) returns **d6=0** at the
   decision `0x408465fe: tstl d6; bne 0x40848dea` (verified: d6==0 there;
   the `bne` is NOT taken).  The session-4 checksum repair is CORRECT and
   VALIDATED: ROM length field `*(0x40800040)=0x00100000`=bios_size, and
   the repaired `ROM[0]=0x49582bc9` makes the ROM's own algorithm (sum of
   BE words offset 4..len-2, XOR stored word at 0) evaluate to 0.  So the
   checksum is a solved non-issue.
2. Fall-through cold-boot -> `0x40848dea` (sets SP=0x2600) -> `0x40848df8
   btst #26,%d7` (**bit26 CLEAR**, so the session-5 "bit26 gate" is NOT
   what routes it) -> `0x408499bc` -> `0x408499cc btst #26` (clear) ->
   `0x408499ce` calls the capability routine `0x408468ba`(=`0x40802f18`)
   -> `0x408499da btst #12,%d0` with **d0=0x773f (bit12 SET)**, so it
   does NOT divert to console here either; it runs the continuation at
   `0x40845ce2` (a video/EASC-register init: 0x80808080 gray fills, a
   `cmpib #-80` 0xB0 version check) which RETURNS to `0x408499f6`, which
   then sets up and enters the MicroBug loop.  The console is the
   DELIBERATE next step after that init -- i.e. disk boot is simply never
   invoked on this path.

### Disproven hypotheses (all tested this session)

- **Not** a POST/checksum failure (d6==0 at the decision point).
- **Not** the d7 bit26 "factory test" gate (bit26 is CLEAR; the Classic
  II PA0->bit26 mechanism does not apply to this ROM).
- **Not** the d0 bit12 capability gate (bit12 is SET, d0=0x773f).
- **Not** a `pins-a` strap: swept PA=0x00,0x7d,0x7f,0xbf,0xdf,0xef,0xfe,
  0xff via `-global mos6522-maciivx.pins-a=` -- ALL still park in the
  console.
- **Not** a RAM-size mismatch: `-m 4/8/12/16/32/64/68` all park in the
  console.  (The XOR-fill over [0..0xBFFFBC] with pattern 0xb6db6db6 seen
  in `-d unimp` at pc `0x408469xx` is the RAM test filling the full
  theoretical range; its ceiling 0xBFFFBC is fixed, not RAM-derived, and
  is not the divert.)
- **Not** an unexpected exception: `-d int,guest_errors` shows only 24
  Access Faults, all at the EXPECTED presence-probe sites `0x40803982`
  (FC7 decoder-kind probe) and `0x40803124` (device-presence helper) --
  these deliberately bus-error to detect absent hardware and are handled.

### The actual root cause (machine identification / decoder kind)

`-d unimp` for a full boot shows the ROM **never touches SCSI**
(0 accesses at the 0x50f10000 NCR5380 window) and **never runs the Egret
transport** (0 shift-register/Egret packets; only 4 bit-bang RTC bytes
and the RBV/VDAC/VIA identification reads).  So the boot process and the
Egret cold-start are simply never entered.

The capability/decoder routine `0x40802f18` (reached via thunk
`0x408468ba`, which does `oriw #1792,%sr` then `lea 0xfffbc65a,%a0; jmp
%pc@(...)` = `0x40802f18`) walks a record table and, for the IIvx,
matches the record at **`0x408035b8`** (kind field `rec+18 = 0x0505`,
matched against the low word of the probe result **d2=0x70000505**),
yielding capability word **`rec+24 = 0x773f`**.  The sibling record
`0x40803b82` (kind 0x260e) does not match.  d2=0x70000505 is computed
from the FC7/`movesw` hardware probes (`0x40803982` etc.); because our
VASP is modelled as the RBV/VDAC/VIA stub carried over from
maciici/maciisi, those probes yield an **RBV-kind (0x05..) identity**,
not VASP's.  With that identity + capability 0x773f the ROM's cold-boot
does its hardware init and drops to the serial console instead of taking
the NuBus/RBV disk-boot branch.

**Conclusion:** the wall is exactly what session 5 suspected -- the ROM
mis-identifies the machine's decoder kind because VASP's real
identification registers are not modelled (Apple never published them).
To advance, the next session needs to discover, by tracing the probe
sequence around `0x40803982`/`0x40803124`/`0x40802f18`, which
register/value distinguishes VASP from RBV to this ROM, model it so the
identification produces the VASP decoder record (with an auto-boot
capability), and only THEN will the Egret cold-start + SCSI disk-boot
path be entered.  This is genuine VASP reverse-engineering, not a
one-line strap/patch (every strap/patch shortcut was tested above).

### Changes committed this session

- `hw/m68k/maciivx.c`: seed XPRAM startup bytes 0x76/0x77 (default OS
  ddType 1 = Mac SCSI) and 0x78-0x7b = 0xff (no startup-device
  preference), matching the working Egret siblings (macclassicii.c / the
  quadra700 oracle).  This is **forward-prep only** -- it is read far
  past the current blocker, so it does not change today's boot behaviour
  (re-verified: still parks in the console, no regression), but is
  required by the boot-driver installer once the disk-boot branch is
  reached.  Nothing else changed; the POST/checksum/overlay model from
  session 4 is confirmed correct and untouched.

### Furthest state

Screenshot `/tmp/iivx-furthest.png`: 640x480, the post-POST uninitialised
dithered-VRAM pattern (black/white checker), no Happy Mac -- the ROM
reaches the console before any video init.  Identical to sessions 4/5.

## Session 7: ROOT CAUSE PROVEN -- machine mis-identifies as IIci (RBV), not the Egret record; forcing the Egret record flips the branch

Per the coordinator's plan, went after the identification.  Result: the
misidentification hypothesis is **confirmed and the branch flips** when
corrected.

### The decoder record table (ROM 0x408032c8) fully mapped

The identify routine `0x40802f18` walks a record table; each record is
matched by decoder byte (`rec+19` == d2.b, here 0x05) AND
`(d1 & rec+32) == rec+36`, where **d1 = 0xefff0000** (measured;
`set endian big`).  d1's TOP byte 0xef == the VIA1 port-A straps
(pins_a); bytes 2..0 = 0xff0000.  Records (addr: kind@+18, cap@+24,
mask@+32, match@+36):

```
0x408035b8  0505  cap 0000773f  mask 56000000  match 46000000  <- MATCHED (PA&0x56==0x46)
0x408035f8  0505  cap 0000773f  mask 56000000  match 56000000
0x408036f8  0605  cap 00039807  mask 56000000  match 54000000
0x40803638  0905  cap 0000773f  mask 56000000  match 00000000
0x40803678  0a05  cap 0000773f  mask 56000000  match 06000000
0x408036b8  0706  cap 00000000  mask 56000000  match 52000000
0x40803b02  0c05  cap 04000000  mask 01265600  match 00001600  <- the EGRET record (decoder 5)
0x40803b42  0d07  cap 04000000  mask 01a65600  match 00005400  <- Egret, decoder 7 (V8/Eagle: NOT the IIvx)
0x40803738  fd04 / 0x40803778 fd05 / 0x408037b8 fd06 / 0x408037f8 fd00  (test/burn-in configs)
```

The 0505/0905/0a05/0605 records are the IIci-FAMILY (decoder-5 RBV, DISCRETE
RTC).  0c05 is the decoder-5 record that uses the **Egret** (mask/match in
the low bytes, not the PA byte).  Because the IIci records are tried first
in table order and one always matches on the PA straps (any pins_a lands
(PA&0x56) in {0x46,0x56,0x54,0x00,0x06}), the IIvx is identified as an IIci
and uses the RTC bit-bang -- so the Egret transport is never entered, the
cold boot runs its RBV/serial init and drops to the MicroBug console.

The IIvx SHOULD be **0c05** (decoder-5/RBV-compatible + Egret) -- VASP is an
RBV successor with an Egret, which fits 0c05 exactly; 0d07 is decoder-7
(V8/Eagle, the LC/ClassicII family), not the IIvx.  To select 0c05 the ROM
needs `(d1 & 0x01265600) == 0x00001600`, i.e. d1 byte2 bits 17/18/21 CLEAR
and byte1 bits 9/10/12 SET; our d1 byte2 = 0xff (idle/pulled-up) blocks it.
d1's low 24 bits are built by the identify dispatch's hardware probes
(monitor-sense walking test at `0x408030f2` reading rec+72, plus the
VIA-IER/decoder probes) -- a real VASP would drive them to the 0c05
pattern; our RBV/VDAC/VIA stub returns 0xff0000.  Pinning down each exact
d1-bit->register mapping is the remaining identification-modelling work.

### Proof the branch flips (env-gated force patch)

Added `IIVX_FORCE=<hex rom offset>` (env-gated, DEFAULT OFF -- no change to
committed boot behaviour) which disables the IIci 05-records and forces a
chosen record to match, to test the hypothesis directly.  With
`IIVX_FORCE=3b02` (the 0c05 Egret record):

- PC **escapes the MicroBug console** (was 0x40849a08; now 0x40814e08).
- The **Egret transport RUNS**: 231024 Egret accesses (was 0), 57757 VIA1
  accesses; the 40000 unbacked "ram-hole" reads DISAPPEAR (0).
- Control reaches the **Egret cold-start sync** at ROM 0x40814cc8-0x40814e46
  (a PB4//PB5 + SR byte handshake, IFR-bit2/SR-done polled at 0x40814e1a).

So the identification IS the blocker, and 0c05 is the correct target record.
This is exactly the coordinator's success signal (Egret packets > 0, off the
console path).

### Next blocker (well-scoped): the Egret cold-start protocol

With 0c05 forced, the ROM now spins in the Egret cold-start handshake
because our `maciivx_egret_*` model was grown against the WRONG
(IIci-identified) path -- the Egret was never actually exercised before, so
the model returns the no-response signature (0x00 0x00) and the ROM retries
forever.  The fix is to port `macclassicii.c`'s complete, working Egret
**pseudo-command** model (packet type byte 0x01; READ_MCU 0x02, GET_TIME
0x03, GET_PRAM 0x07, WRITE_MCU 0x08, SET_TIME 0x09, SET_PRAM 0x0c; XPRAM
streamed from via1.PRAM[]) plus its cold-start /XCVR sync -- maciivx's
current framing ("raw ADB byte, no type byte") is wrong for the real
Egret path.  Then: proper d1 modelling to select 0c05 without the force
patch, then SCSI disk boot -> Finder.

### Committed

- `hw/m68k/maciivx.c`: env-gated `IIVX_FORCE` decoder-record force patch
  (investigation tool; off by default, so default boot behaviour and the
  session-6 state are unchanged -- re-verified no regression).  gdb port
  used this session: 1279 (monitor sock /tmp/iivx-mon2.sock).

## Session 7 (cont.): Egret model ported + console root-caused to a RAM-test failure

### Ported the complete Egret model from macclassicii.c

maciivx's Egret (forked from maciisi, never exercised because the machine
had been mis-identified as an IIci that uses the RTC bit-bang) could not
service the real Egret cold-start.  Ported macclassicii.c's complete Egret:
the pseudo-command layer (type byte 0x01: READ_MCU/GET_PRAM/SET_PRAM/
WRITE_MCU/GET_TIME/SET_TIME, XPRAM streamed from via1.PRAM[]), the
cold-start /XCVR sync, and -- crucially -- the **PB3 (/XCVR) read override
in maciivx_via1_read** (report the Egret's line state, not the ROM's ORB
latch; without it the cold-start `btst #3` at 0x40814cfa spins forever).
Struct gained egret_xcvr_asserted / egret_pseudo_* / egret_mcu_mem and a
256-byte egret_resp.

Result (with IIVX_FORCE=3b02): the Egret **cold-start sync now passes** and
the ROM exchanges pseudo-commands (0x1b, 0x0e, 0x1c ...), then proceeds to
RAM sizing.  Verified NO regression on the default (un-forced IIci) path --
it still reaches the same console, no crash.

### The console is a RAM-TEST failure (record-independent)

Traced the post-chime path (0x46636 -> 0x4084664e -> `jmp 0x4084a6b0` ->
`0x4084665a bnew 0x40848dea`).  **0x4084a6b0 is the ROM RAM test**: it walks
a range list and writes/reads the 'Jade' (0x4A616465) pattern, accumulating
mismatches in d6; it returns `tstl d6` and the caller consoles on non-zero.
On our machine d6 != 0 (measured) because the test range extends beyond the
installed 8 MB into unbacked memory (ram-hole writes at 0x04000000, reads at
0x00909xxx): our ramio swallows the write and reads back 0 != 'Jade', so the
test "fails" and diverts to the MicroBug console at 0x40848dea.  This is the
SAME divert on both the IIci and the forced-Egret paths, and is independent
of decoder record / capability (both build d0=0x773f) and of `-m` size
(4/8/12/16 all fail -- the test range is not simply the -m size).

**This is the real console root cause.**  Next: dump the RAM-test range list
(a1/a5 at 0x4084a6f8) to see exactly which ranges it tests and why they
exceed installed RAM -- likely our RAM-sizing (ramio open-bus behaviour)
mis-detects size, or the ROM enumerates SIMM-bank ranges our flat RAM
doesn't match.  Making the test pass (d6==0) should let the boot continue
past 0x4084665a toward disk boot.

Committed: the full Egret port (maciivx_egret_* rewritten from
macclassicii.c) + PB3 read override.  Default boot behaviour unchanged
(re-verified).  IIVX_FORCE remains the env-gated record selector.

### CORRECTION + precise console divert (0x408466bc, a memory-integrity hash)

The "RAM test at 0x4084a6b0 fails" claim above is WRONG (verified): that
RAM test PASSES on both paths (d6==0; empty bank B at 0x04000000 is handled,
bank A sizes to 8 MB), and its `bne 0x48dea` at 0x4084665a is NOT taken.

The REAL console divert (found via `-d exec -dfilter` tracing the TBs into
0x40848dea) is at **ROM 0x408466bc**: `tstl d6; bnew 0x40848dea`.  d6 is set
by a memory-integrity XOR hash (loop at 0x408469c4, called via 0x40846950)
over a region taken from a table at 0xBFFFBC (in the VRAM-24 window):

```
table@0x00BFFFBC:  start=0x00000000  len=0x00C00000  (then 0xFFFFFFFF end)
HASH over [0x00000000 .. 0x00C00000], result d6=0xFFFFFFFF -> console
```

So the ROM hashes/tests **[0 .. 0x00C00000] (12 MB of 24-bit space)**.  Our
24-bit map there is: RAM 0-0x7FFFFF (8 MB), ROM-24 alias 0x800000-0x8FFFFF,
**GAP 0x900000-0xAFFFFF (unbacked, reads 0)**, VRAM-24 0xB00000-0xBFFFFF.
The unbacked gap (and the read-only ROM-24 region, which can't hold the
written pattern) makes the hash mismatch -> d6 != 0 -> MicroBug console.

This is the true, precise console root cause (supersedes the "cold-boot
always consoles" framing of sessions 4-6: it is this one memory-integrity
check).  Candidate fixes for the next session:
 1. The region [0x800000..0xC00000] should decode per the IIvx 24-bit map so
    the hash sees consistent memory (e.g. the gap 0x900000-0xAFFFFF may need
    to be a RAM/ROM alias, or the test range/`MemTop` that produced len=
    0xC00000 is wrong for our flat-RAM config and should be 0x800000).
 2. Determine why the table length is 0xC00000 (12 MB) not the sized 8 MB --
    likely MemTop/BAT setup from the (IIci) record's RAM+video layout; the
    correct IIvx (0c05, dedicated VRAM) layout may set it differently.
Making this hash pass (d6==0) lets the boot continue past 0x408466bc.

Verified tooling this session: gdb ports 1279/1280 (unique), monitor sock
/tmp/iivx-mon2.sock.  Both IIci (default) and forced-0c05 paths reach this
same 0x408466bc divert.

### The memory-integrity test is a 32-bit WRITE-VERIFY over [0,0xC00000]

Disassembled the d6 producer (0x40846950 fills [a0..a1] with a 24-byte
moveml pattern from ROM 0x40846a4e; 0x40846a20 reads it back, eor-compares,
ORs mismatches into d6).  So 0x408466bc's check is a **write-verify RAM test
over [0x00000000 .. 0x00C00000] (12 MB)**.  Both the IIci-default and the
forced-0c05 paths use the identical range (table (0,0xC00000) at 0xBFFFBC),
so this is NOT fixed by identification.

Root cause of the failure on our machine: this test runs expecting the low
12 MB to be contiguous writable RAM (a real IIvx has 68 MB contiguous at
0-0x43FFFFF in 32-bit mode).  Our machine has only 8 MB of RAM AND the file
maps STATIC 24-bit aliases -- ROM-24 at 0x800000-0x8FFFFF (read-only) and
VRAM-24 at 0xB00000-0xBFFFFF -- right in the middle of that 12 MB window.
So the test hits read-only ROM at 0x800000 and unbacked gap at 0x900000-
0xAFFFFF, the pattern doesn't read back, d6 != 0 -> MicroBug console.

**This is the concrete, precise blocker.**  The 24-bit aliases (added in
sessions 1-3 for the ROM's early 24-bit execution) overlap the 32-bit RAM
region and break the 32-bit memory test.  Fix directions for next session:
 1. Make the 24-bit ROM-24/VRAM-24 aliases LOWER priority than RAM (so real
    RAM wins where present), and boot with >=12 MB so [0,0xC00000] is
    backed writable RAM -- but verify this doesn't break the early 24-bit
    ROM execution that needed the 0x800000 ROM alias (it may only be needed
    pre-RAM-init).
 2. Or model the 24-bit vs 32-bit decode as mode-dependent (the classic Mac
    32-bit/24-bit switch) instead of static physical aliases, so 0x800000-
    0xBFFFFF is RAM in 32-bit mode and ROM/VRAM in 24-bit mode.
 3. Or find where the (0,0xC00000) test length is set and confirm whether a
    correct dedicated-VRAM IIvx config would shrink it to the sized RAM.

The full chain to Finder from here: fix this memory test -> continue cold
boot past 0x408466bc -> (with the 0c05 Egret record + ported Egret) the
Egret/SCSI disk-boot path -> Happy Mac -> Finder.

### CONFIRMED: the memory test IS the console; backing its region escapes it

Added env-gated `IIVX_TESTRAM` (default off): backs [0x800000..0xC00000]
with high-priority writable RAM (overriding the ROM-24/VRAM-24 aliases).
With `IIVX_FORCE=3b02 IIVX_TESTRAM=1`:

- The boot **ESCAPES the MicroBug console** -- PC leaves 0x4084axxx and
  runs in RAM (observed climbing 0x01ec... -> 0x0297... -> 0x032f...),
  with live RBV/video register access (rbv=234) and the Egret working
  (503).  So the 0x408466bc memory test IS the sole console blocker on
  this path -- backing its target region makes d6==0 and the cold boot
  continues into RAM-resident code.

- It then RUNS AWAY (PC climbs steadily through RAM, no SCSI, blank
  screen) because the crude 4 MB overlay CLOBBERS the ROM-24 (0x800000)
  and VRAM-24 (0xB00000) the post-test boot still reads: after the test
  writes its pattern over 0x800000..0xC00000, later 24-bit ROM/VRAM
  reads there return the test pattern -> garbage -> runaway.  This is
  the mode-dependent-decode conflict: the 32-bit memory test needs RAM
  at 0x800000..0xC00000, but 24-bit ROM/VRAM access needs ROM/VRAM
  there.

**Clean fix (next session):** model the 24-bit vs 32-bit decode properly
(classic Mac mode switch) so 0x800000..0xBFFFFF is RAM in 32-bit mode and
ROM(0x800000)/VRAM(0xB00000) only in 24-bit mode, and give the machine
>=12 MB so [0..0xC00000] is real RAM.  Then the memory test passes
non-destructively and the boot proceeds (Egret + 0c05 already in place)
toward SCSI disk boot / Happy Mac / Finder.

Net this session: identification root-caused + Egret ported + console
root-caused to one memory-test instruction AND proven bypassable.  Two
env-gated investigation tools (IIVX_FORCE, IIVX_TESTRAM), both default
off; default boot behaviour unchanged (re-verified: still consoles at
0x4084a0f0, no crash).

## Session 8: 0c05 + Egret + memory test made DEFAULT; new frontier = a garbage-stack crash past the console

Per the coordinator, converted the two env-gated proofs into default behaviour.

### Fix 1 (done): select the 0c05 Egret record by DEFAULT

The decoder-record force patch is now applied unconditionally (target 0x3b02
= the 0c05 Egret record); `IIVX_FORCE=<hex>` overrides the target and
`IIVX_FORCE=off` reverts to IIci.  NOTE the coordinator's "just set the PA
strap" plan is NOT achievable: traced d1's construction instruction-by-
instruction (stepi) -- d1 = 0xefff0000 is built as PA/fixed bytes then a
`<<16` (ROM 0x40803268) that ZEROES d1 byte 1, and 0c05's criterion needs
(d1 & 0x01265600)==0x00001600 i.e. byte1==0x16, unreachable for ANY PA
(verified: -global pins-a sweep leaves d1==0xefff0000, and the machine
overrides -global anyway).  The natural selector is VASP's undocumented
decoder-kind ID registers; absent those, the ROM-table patch (same class as
the existing relocation/checksum patches) is the principled stand-in.

### Fix 2 (partial): 24/32-bit decode + RAM so the memory test passes

- Default RAM raised to 32 MB (real IIvx up to 68 MB) so the ROM's 32-bit
  write-verify test over [0..0xC00000] has real contiguous DRAM backing.
- The static 24-bit ROM/VRAM/gap aliases (0x800000/0x900000/0xB00000) are
  now added ONLY when RAM < 12 MB (small-RAM shim for the test); with >=12 MB
  they are skipped because they would FRAGMENT the contiguous 32-bit DRAM the
  boot's heap/structures need.  For small RAM the 0x800000 ROM alias is a
  WRITABLE RAM copy of the (patched) ROM (init'd + memcpy'd) so it satisfies
  both the writable-DRAM test and 24-bit ROM reads.
- With this, the ROM's memory-integrity test PASSES and the boot ESCAPES the
  MicroBug console by default (Egret cold-start + pseudo-commands run, 0c05
  identified), on both the 8 MB-shim and 32 MB-contiguous paths.

### New blocker (precise): garbage-stack crash -> NOP-sled

Past the console the boot runs ~thousands of ROM instructions then CRASHES:
it reaches an `rts` at ROM 0x40848dd2 with **sp = 0xfffffffe** (a garbage/
underflowed stack) -> pops a garbage return -> jumps into unbacked/zeroed
RAM and NOP-sleds (observed target 0x01460000 with 8 MB, 0x00007fxx with
32 MB -- i.e. wherever the garbage return points).  fp=0x40848dd2,
d7=0x40800097, a5=0x4084a340 at the crash.  The 0x40848dc0-0x40848dd4 region
around that rts disassembles as odd byte ops (`orib #-6,d6`, `addxl a4@-,
a5@-`) -- likely a jump table / data the boot fell INTO via a bad computed
jump, whose rts then runs on an unset stack.  Persists across RAM size and
across NOP-ing the memory-test fill (env IIVX_SKIPMEMTEST), so it is NOT the
memory-test corruption -- it is a stack/flow issue on the post-console
cold-boot path (SCSI is still never touched, so disk boot isn't reached).

Next session: find the bad computed jump that lands in the 0x40848dc0 data/
rts region (trace the last good ROM PC before sp goes wild -- break where sp
first becomes > 0x0f000000 or an odd value), fix the flow (likely a
lowmem/vector or an a5-world/dispatch pointer left wrong by our
0c05-vs-IIci-config or VRAM/ScrnBase decode), then SCSI disk boot -> Finder.

### Status / regressions

- Default now gets PAST the console (further than sessions 4-7) but crashes
  as above; framebuffer blank white (screendump /tmp/iivx-s8.ppm).  There is
  currently NO stable-console fallback (IIVX_FORCE=off also escapes+crashes,
  because the memory backing is unconditional for small RAM).
- Guardrails kept: only hw/m68k/maciivx.c touched; maciici/maciisi/
  macclassicii and shared devices untouched.  gdb port 1279, sock
  /tmp/iivx-mon2.sock.

### Session 8 addendum: the crash is a POST memory test overlapping the active stack

Localized the garbage-stack crash precisely (correct-endian gdb dprintf).
At ROM 0x40848db0 (a cold-boot POST memory-test block) the registers are
a0=0x00000000, a1=0x00080000, sp=**0x00007fe6**, d7=0x40800097 -- i.e. the
test region is **[0 .. 0x80000]** (512 KB) and the ROM's own STACK is at
0x7fe6, INSIDE that region.  The block fills [0..0x80000] with the POST
pattern (0x6db6db6d/0xb6db6db6), overwriting its own return address on the
stack; at the closing `rts` (0x40848dd2) the longword at sp reads
**0xb6db6db6** -> PC = garbage -> NOP-sled (the 0x7fxx / 0x1460000 sleds).

So the real defect is that the ROM's stack sits inside a region its own
power-on memory test scrubs.  On real hardware the stack for this phase must
be ABOVE the tested window (or the window excludes it); our value (sp low,
~0x7fe6) comes from the ROM's stack setup on this cold-boot path and is
wrong for our memory map / MemTop.  Neutralising the moveml fill routine
0x40846950 does NOT fix it (a different fill -- the XOR scrubber at
0x408469a8 -- writes the 0xb6db6db6 pattern), and RAM size only moves the
sled target.

Next session's concrete task: find where this cold-boot phase sets SP (it
should be high -- above 0x80000 / at MemTop), and why ours is ~0x8000.
Likely MemTop or the a5-world/BAT is computed from a layout the 0c05 record
+ our (dedicated-VRAM) memory map get wrong; fixing SP (or the test window,
or MemTop) so the POST stack is outside the scrubbed region should clear the
crash, after which SCSI disk boot is the next milestone.  This is the single
remaining blocker between "boots past the console with Egret working" and
disk boot.

## Session 9: SP/MemTop root cause found; a stack fix reaches VIDEO INIT (furthest yet)

### Root cause of the post-console crash (precise)

Traced SP from reset to the crash (correct-endian gdb).  SP is 0x2600 through
0x46636, and the RAM-sizing routine (ROM 0x4084a6b0) runs on a HIGH stack
(SP=0x01FFFFBC, near the 32 MB top).  But at its tail it RELOCATES the POST
stack:

```
0x4084a75a  moveal %sp,%a0          ; a0 = sp (high)
0x4084a75c  moveal %a0@,%sp         ; sp = *(sp) = base of lowest RAM bank
0x4084a75e  addal  #0x00008000,%sp  ; sp = base + 0x8000
0x4084a764  movel  %a0,%sp@-        ; link the old (high) stack
```

`*(sp)` is the base of the lowest detected RAM bank.  On a real multi-SIMM
IIvx that base is non-zero so the POST stack lands safely; on OUR single
contiguous DRAM bank based at 0, `*(sp)==0` -> **SP = 0x00008000**, which is
INSIDE bank 0.  The very next per-bank memory scrubber (ROM 0x40848d90,
region [bank_base .. +0x80000] = [0..0x80000]) overwrites that stack with the
0xB6DB6DB6 pattern, so a later `rts` (0x40848dd2) pops a garbage return ->
NOP-sled.  **This is the SP/MemTop bug the coordinator predicted** -- our flat
single-bank map makes the ROM's own SP relocation land in the scrub window.
(MemTop 0x108 reads 0xB6DB6DB6 mid-sizing, but that is expected -- globals
aren't set until sizing finishes.)

### Fix (env IIVX_SPFIX, experimental) and how far it gets

Patched the `addal #0x8000` immediate to 0x01000000 so the relocated POST
stack lands at 16 MB (high, above every per-bank window, inside our 32 MB
DRAM), keeping the stack-switch link intact.  Result: the boot gets PAST the
per-bank crash and reaches **video/RBV initialization** -- a run of onboard
video RBV register programming (ROM 0x408479a0-0x40847a1c: RBV reg 0x13
monitor/mode writes 0x40/0xff/0x1f/0xc0) -- the FURTHEST the IIvx has booted.
It then hits a **DOUBLE MMU FAULT**: an access to **0xFEFFFFFC** (super-slot
$E, the onboard-video slot space) faults, and the fault frame write faults
again.  d2=0xdc000c05 (the 0c05 kind) and a1=0x40803b02 (0c05 record) confirm
we're on the Egret/IIvx path.

### Next blocker (clear): onboard-video slot-$E space not mapped

The IIvx's onboard video lives in NuBus super-slot $E (0xFE000000 window:
VRAM + a slot Declaration ROM).  Our machine maps the dedicated VRAM only at
0x60B00000 / 0xB00000, NOT at slot $E, so the ROM's video driver's slot-$E
access (0xFEFFFFFC) hits the A31 catch-all / faults -> double fault.  Next:
map the onboard VRAM (and a minimal slot-$E sResource/DeclROM) into the
super-slot $E space at 0xFE000000 (cf. how maciici exposes its onboard video
pseudo-slot), so the video driver initialises cleanly.  After that: Happy Mac
-> SCSI disk boot -> Finder.

The SPFIX immediate (16 MB) is a stopgap; the clean fix is to give the ROM a
memory-bank descriptor whose lowest-bank base is non-zero OR model the
soldered-4MB-plus-SIMMs multi-bank layout so the ROM's own SP relocation
lands high on its own.  Kept env-gated (it double-faults on the slot-$E issue
above); default boot behaviour unchanged (still the Session-8 sled, no abort).

## Session 10: SP fix reaches VIDEO INIT; blocker is the slot-$E onboard-video DeclROM

### Step 1 (SP relocation) -- analysed, kept as the working IIVX_SPFIX patch

Dumped the RAM-sizing descriptor the ROM relocates SP from (at ROM 0x4084a75a,
sp=0x01FFFFBC): it is `[base=0x00000000, top=0x02000000, 0xFFFFFFFF]`.  The ROM
does `sp = *(sp) + 0x8000` = base + 0x8000 = 0x8000 (single contiguous bank
based at 0).  The coordinator's "give the lowest bank a non-zero base" isn't
reachable -- RAM must start at 0 (reset vector/low globals), so the lowest
bank base is always 0.  Two fixes tried:
 - Read the descriptor TOP (`moveal %a0@(4),%sp`): BROKE the sizing loop (it
   then probed RAM up to 0x04000000 and spun on the unbacked 32-64 MB).
 - Enlarge the `+0x8000` guard to `+0x1000000` (SP lands at 16 MB): WORKS --
   keeps the sizing's stack-switch structure intact, POST stack lands above
   every per-bank scrub window.  Kept as IIVX_SPFIX (env-gated; see below).

### Result: IIVX_SPFIX reaches ONBOARD-VIDEO INIT (furthest yet)

With IIVX_SPFIX the boot clears the per-bank crash and runs the onboard-video
bring-up: RBV monitor/mode register programming (ROM 0x408479xx: RBV reg 0x11
<-0xfe, 0x12, 0x13 <-0x40/0xff/0x1f/0xc0).  Identification is solid throughout
(d2=0xdc000c05).

### Precise blocker: onboard video = NuBus slot $E, DeclROM not provided

The IIvx's onboard video lives in NuBus **standard slot $E** (0xFE000000-
0xFEFFFFFF; super-slot is 0xE0000000).  The video driver reads the slot's
Declaration ROM **format block at the top, 0xFEFFFFFC**, then follows its
directory.  We provide no slot-$E DeclROM, so:
 1. the read bus-errors, AND
 2. the per-bank POST scrubber has just filled [0..0x80000] -- which INCLUDES
    the 68k exception vectors at 0-0x400 -- so the bus-error vector is garbage
    (fault delivery lands at pc=0x00000024, executing scrubbed low RAM),
 => an infinite bus-error cascade (SP walks down ~340k frames) -> QEMU's
    m68k "DOUBLE MMU FAULT" fatal.

Experiments (env IIVX_SLOTE, reverted): backing slot $E with the VRAM, or
with zeroed RAM, does NOT help -- with no valid format block the driver
computes a garbage directory offset and scans DOWNWARD forever (fault address
walks 0xFEFFFFFC -> 0xFEEFFFFC -> 0xFDFFFFFC into slot $D).  A VALID DeclROM
(correct testPattern 0x5A932BC7, byte lanes, directory, board + video
sResources, mode table) is genuinely required.

### Next step (clear, substantial)

Provide the onboard-video slot-$E DeclROM + framebuffer, mirroring the NuBus
video DeclROM/sResource machinery in `hw/display/radius24xp.c` (and
`hw/nubus/nubus-device.c`'s DeclROM handling), pointed at the IIvx VRAM.  Map
the VRAM into slot $E's standard space and expose a minimal video sResource
(1 mode) so the Slot Manager + Display Manager accept it.  Then the SP fix can
go default-on (no more slot-$E abort), and the chain continues: video init
completes -> SCSI disk boot -> Happy Mac -> Finder.

### Status / guardrails
- Default boot behaviour UNCHANGED (stable Session-8 sled, no abort); the SP
  fix is env-gated IIVX_SPFIX because default-on would make `-M maciivx` abort
  on the slot-$E DeclROM fault above.  IIVX_SPFIX=1 reaches video init.
- Only hw/m68k/maciivx.c touched; shared mac-nubus-bridge.h / MAC_NUBUS_LAST_SLOT
  and maciici/maciisi/macclassicii untouched.  Ports 1279/1280, sock
  /tmp/iivx-mon2.sock.  Not touched: cc-build/hp300-build/mvme16x-build.

## Session 11: slot-$E onboard-video DeclROM PROVIDED (correct + verified); Session 10's blocker diagnosis CORRECTED to a VIA-timer interrupt storm

### Step 0 result: the DeclROM is ROM-embedded, aliased into slot $E (approach 1)

Searched the IIvx ROM for the DeclROM testPattern `0x5A932BC7`: found at ROM
offsets 0xc80, 0x2318, 0x5f44, and **0xffffa** (6 bytes from the end of the
1 MB ROM).  The 0xffffa one is a real Apple **format block** at the very top of
the system ROM (last 20 bytes, ending at 0xFFFFF):

```
dirOffset=0x00FFE8C8  length=0x174c  crc=0x32a481a7  romRev=0x01
format=0x01  testPattern=0x5A932BC7  reserved=0x00  byteLanes=0x0F
```

`dirOffset` low 24 bits sign-extend to **-0x1738**, pointing from the format
block (ROM off 0xFFFEC) back to ROM off **0xFE8B4** = `0x100000 - length` = the
DeclROM base.  So the ROM carries a complete, self-contained onboard-video
DeclROM in its top 0x174c bytes, designed to be **top-aligned in a slot space**
(exactly the SuperMario/RBV "internal built-in-video slot ROM" pattern the
coordinator described).  Parsing the sResource directory (base 0xFE8B4):
board sResource (id 1) + video sResources (id 0x82/0x83/0x86/0x8a/0x8b/0x8e/
0xb4, VRAM-size/mode variants), strings "Macintosh Z Built-In Video",
".Display_Video_Apple_Brazil" (Brazil = the IIvx video codename),
"&CPU_68030_\_MacIIFamily", gamma tables.  This is a **single-machine**
DeclROM, NOT the multi-driver "whole family DeclROM" the RBV siblings
(maciici.c ~line 1899) warn against aliasing -- so aliasing it into slot $E is
safe here.

The video sResource's frame-buffer siting: **MinorBaseOS = 0x00000000**,
MinorLength = 0xC0000, and mode 0x80's VPBlock has **vpBaseOffset = 0** -- i.e.
the frame buffer is at the slot's own base **0xFE000000** (640x480 / 512x384
modes, up to 8bpp).

maciici comparison: the IIci does NOT alias into slot $E -- it registers the
motherboard DeclROM as pseudo-slot 0 (its ROM's onboard video works that way).
The IIvx ROM is different: its Brazil video driver expects the DeclROM in slot
$E standard space.  So "mirror maciici" does not literally apply; the correct
IIvx behaviour is the ROM-alias.

### Step 1 implemented (in maciivx.c only)

- `rom_slotE`: alias the whole 1 MB system ROM at **0xFEF00000** (top-aligned:
  ROM off 0xFFFFF -> 0xFEFFFFFF), priority 1 over the NuBus bridge (which maps
  slots 9-E at priority -1).  Verified on a paused machine: 0xFEFFFFFA reads
  `5A 93 2B C7`, 0xFEFFFFEC reads `0x00FFE8C8`, directory backed at 0xFEFFE8B4.
- `vram_slotE`: alias the dedicated VRAM (also at 0x60B00000) into the bottom
  of slot $E at **0xFE000000** (MinorBaseOS 0), priority 1, so the Brazil
  driver's pixel writes land.

Both are correct and forward-looking; the default boot (Session-8 sled) is
unaffected (re-verified: no abort, no regression).

### Step 2/3: BLOCKED -- and Session 10's blocker was MISDIAGNOSED

Making IIVX_SPFIX default-on still DOUBLE-FAULTS, at the SAME point as Session
10, and the slot-$E DeclROM aliases do NOT change it.  Instrumented slot $E
with a full access-logging region (forwarding to the real ROM/VRAM backing):
**there are ZERO reads anywhere in slot $E.**  The DeclROM is never consulted.
Every slot-$E access is a *write* -- exception-frame pushes at pc=0x24/0x28,
i.e. the runaway stack descending through slot $E from ~0xFF000000.  So
Session 10's "video driver reads the DeclROM at 0xFEFFFFFC -> bus error" was
WRONG: 0xFEFFFFFC was an exception-frame push onto the runaway stack, not a
DeclROM fetch.

The REAL blocker (traced with `-d int` + ROM disassembly): with IIVX_SPFIX the
boot clears the per-bank scrub and reaches the ROM's **VIA1-timer interrupt
calibration** at ROM 0x40847248 (a2 = VIA1 base a0@(8); registers 0x200 apart:
0x1600=ACR, 0x1A00=IFR, 0x1C00=IER, 0x1E00=ORA).  It installs a Level-1 handler
at 0x408473bc (VBR+0x64), programs VIA1 T1/T2, enables interrupts (SR I:0), and
counts CA1/T1/T2 interrupts (IFR bits 1/6/5) over a fixed delay loop --
expecting exact counts (d3==10, d4 in [128,208], d5==1) to derive a timing
constant.  Under icount the VIA1 T1/T2 interrupts **re-assert every
instruction** (the handler writes IFR 0x1A00 to clear bits 1/5/6, but the timer
immediately re-fires): a Level-1 storm (observed IFR=0x20 T2, then 0x62, then
0x44).  ~180+ storm interrupts later a downstream FC7 machine-ID probe at
0x40803982 bus-errors -- and because the per-bank scrub wiped the low-RAM
exception vectors (0-0x400) with the 0x6DB6DB6D pattern, the bus-error vector at
0x8 is garbage -> the handler jumps to 0x6db6db6d -> Address Error -> infinite
frame-push cascade down through 16 MB and slot $E -> **DOUBLE MMU FAULT**.

This is the known-hard "POST/SETUPTIMEK timing calibration under icount" area
(cf. Sessions 3-5 and macclassicii.c).  It is genuinely a different problem
from the coordinator's stated slot-$E DeclROM blocker (which is now solved and
forward-ready).  Disabling the 60Hz VBL timer does NOT help (the storm is
VIA1 T1/T2, not CA1).

### Next steps (well-scoped)

The path to Finder now needs the VIA1-timer calibration to not storm under
icount, one of:
 1. Make the mos6522 VIA1 T1/T2 one-shot IFR flags fire the expected finite
    number of times per the ROM's delay loop (the Session-5 "VIA1-T2 IFR
    reconstruction" idea, applied to this calibration) -- ideally maciivx-local
    (a via1 read/write shim) without touching shared mos6522.c.
 2. Or stuff the calibration's result (the SETUPTIMEK/known-good-constant
    pattern lc475/mac_via use) and NOP the interrupt-driven measurement, then
    repair the ROM checksum -- the same class as the existing relocation/record
    patches.
 3. Independently, restore the low-RAM bus-error vector after the per-bank
    scrub (or land the SPFIX stack + keep vectors intact) so a stray bus error
    is non-fatal rather than a cascade.
Once the storm clears, the boot should reach the video DeclROM (now provided)
-> onboard video up -> SCSI disk boot -> Happy Mac -> Finder.

### Status / guardrails
- `hw/m68k/maciivx.c` only: added `rom_slotE` (ROM alias into slot $E at
  0xFEF00000) and `vram_slotE` (VRAM alias at 0xFE000000), both verified.  All
  session debug scaffolding removed.  Default boot unchanged (stable Session-8
  sled, no abort -- re-verified; screendump /tmp/iivx-s11.png blank white, the
  sled never reaches video).  IIVX_SPFIX stays env-gated (default-on aborts on
  the VIA-timer storm above).  Shared mac-nubus-bridge.h / MAC_NUBUS_LAST_SLOT
  and maciici/maciisi/macclassicii untouched.  Not touched:
  cc-build/hp300-build/mvme16x-build.

## Session 12: VIA-timer POST storm bypassed + exception vectors preserved -> DOUBLE MMU FAULT ELIMINATED; SPFIX default-on; boot reaches the cold-boot console via the full POST+Egret path

Went after the Session-11 real blocker (the VIA1-timer calibration storm) using
the coordinator's proven sibling recipe (maclc550.c SETUPTIMEK-class bypass +
vector safety net).  BOTH sub-blockers are now fixed and the machine no longer
faults.

### Fix 1: VIA1-timer POST diagnostic bypass (test 0x0C01, ROM 0x4084722e)

Traced the storm precisely.  Past the per-bank scrub the ROM's POST sequencer
(dispatcher at 0x408493fe / 0x40849460, test-ID markers 0x0D01/0x0D02/0x0C01
written to a5@(24)) runs hardware diagnostics that each return a pass/fail code
in d6.  Test 0x0C01 (entry 0x4084722e, body 0x40847248) programs VIA1 T1/T2,
installs a Level-1 handler at 0x408473bc, enables interrupts, and COUNTS
CA1/T1/T2 IRQs (into d3/d4/d5) over a fixed dbra delay loop, requiring exact
ratios (d3==10, d4 in [128,208], d5==1) to pass.  a2 = VIA1 base a0@(8), regs
0x200 apart (0x1600=ACR, 0x1A00=IFR, 0x1C00=IER).  Under icount the VIA1 T1/T2
interrupts re-assert every instruction -> a Level-1 storm and a garbage ratio.
Fix (mirrors macclassicii.c / maclc550.c NOP-the-calibration): patch the test
entry to `moveq #0,%d6; jmp %fp@` (report pass without the interrupt-driven
measurement).  The test is self-contained (its setup helper 0x40847086 saves
VIA state and restores it), so skipping it leaves the VIA untouched.

### Fix 2: exception-vector preservation across the POST scrub

The storm was NOT the fatal event -- with it bypassed the boot still
DOUBLE-FAULTED.  Root-caused with a low-vector write-logger: the POST
write-verify fill/scrub routine (0x40846950, called by both the [0..0xC00000]
integrity test at 0x408466b4 and the per-bank scrubber at 0x40848dc2) fills
[a0..a1] with the 0x6DB6DB6D pattern.  For bank 0 a0==0, so it OVERWRITES the
68k vector table at 0-0x3FF -- including the bus-error (0x8) and address-error
(0xC) vectors the ROM had just installed at 0x408468ae -- and never restores
them.  On a real multi-bank IIvx the lowest bank base is non-zero so the
vectors survive; on our single bank based at 0 the post-scrub FC7 machine-ID
probe (0x40803982 `movesw 0x22000` with SFC=7, a deliberate presence probe)
bus-errors through the scrubbed vector 0x8 (== 0x6DB6DB6D), the CPU jumps to
that odd address -> Address Error -> an infinite Access-Fault cascade down the
stack -> DOUBLE MMU FAULT.  (This is what Session 10 misread as a slot-$E
DeclROM read: 0xFEFFFFFC was the runaway stack, not a fetch.)  Fix (coordinator
step 3): clamp the fill routine's start address a0 up to 0x2000 so the low 8 KB
vector page is never scrubbed.  The routine keys its fill/eor-scramble/verify
all off a0 (re-derived at 0x408469a8), so clamping a0 once at entry is
self-consistent and the verify still passes.  Implemented as a short thunk in
free ROM padding at 0x4084ac3c, branched to from the routine's prologue.  ROM
checksum repaired after all patches (unchanged mechanism).

### Result: no more crash; SPFIX default-on; boot reaches the cold-boot console

With Fixes 1+2 (and the Session-11 slot-$E DeclROM), IIVX_SPFIX is now
DEFAULT-ON (IIVX_NOSPFIX disables).  `-M maciivx` with no env vars runs the
ENTIRE POST -> boot chime path -> Egret cold-start + pseudo-command layer
(GET/SET_PRAM, READ_MCU streaming ~168 XPRAM bytes, verified 1333 Egret
accesses) -> Toolbox A-line traps -> cold-boot, with NO DOUBLE MMU FAULT
(verified: default boot idles, no abort; exception count dropped from ~516000
cascade faults to ~460, all handled device probes).  This is dramatically
further than every prior session (which faulted here).

### Remaining frontier (unchanged from Sessions 5-7): the cold-boot MicroBug console

The boot idles at ROM 0x4084a0f0 (SR I:7), the ROM's cold-boot serial console.
Disassembled the path definitively: cold-boot 0x40848dea -> (btst #26,d7 clear)
0x408499bc -> capability routine 0x408468ba (d0 bit12 set) -> video/EASC init
0x40845ce2 -> RETURNS to 0x408499f6 -> console setup (0x40849f84/0x40849fb2:
programs the SCC console, sets d7 bit17, VIA1 config) -> GetChar loop
0x4084a0e6/0x4084a0f0.  This cold-boot routine ALWAYS ends in the console (both
d7-bit26 and d0-bit12 branches lead here); SCSI is never touched (0 accesses),
so disk boot is not invoked on this path.  This is exactly the Session 5-7
"cold-boot serial console" wall: on a NuBus machine the disk-boot continuation
is interrupt-driven, but the console runs at I:7.  Crossing it is the deep
OS-startup work: the interrupt-driven cold-boot/boot-device continuation, the
Egret READ_MCU/GET_PRAM boot-config data (our XPRAM ddType seed is read but the
boot device is never selected), and likely the correct NuBus disk-boot branch
routing.  Furthest screenshot /tmp/iivx-s12.png (blank 640x480 -- the console
is reached before any Happy-Mac video draw), same visual as prior sessions but
now via the full crash-free POST+Egret path.

### Status / guardrails
- Only hw/m68k/maciivx.c touched.  Two new ROM patches (VIA-timer test bypass
  at 0x4722e; vector-preserve thunk at 0x4ac3c + entry redirect at 0x46956),
  both covered by the existing checksum repair; SPFIX flipped default-on.  All
  session debug scaffolding removed.  Default `-M maciivx` boots without env
  vars and no abort (re-verified).  Shared mac-nubus-bridge.h /
  MAC_NUBUS_LAST_SLOT and maciici/maciisi/macclassicii untouched.  Ports
  1279/1280, sock /tmp/iivx-mon2.sock.  Not touched:
  cc-build/hp300-build/mvme16x-build.

## Session 13: cold-boot path map CORRECTED; console root-caused to the case-5 decoder dispatch; XPRAM 'RBBI'/'SCBI' signature reads discovered; disk-boot branch proven unreachable for the kind-5 identity (maciici Rosetta diff)

Goal: reach Finder by diffing against the working RBV sibling maciici.  Not
reached (same MicroBug wall), but the divergence is now mapped far more
precisely than Session 12, and two inherited path claims were DISPROVEN.

### maciici reference (the Rosetta stone) -- confirmed to Finder

`-M maciici -bios macIIci.rom` with the SAME disk
(`HD0-OpenRetroSCSI-7.5.3.hda`, `-snapshot`) boots to the Finder desktop
(menu bar, "not shut down properly" dialog) in ~45s -- screenshot
`/tmp/iivx-maciici-finder.png`.  PC sampling shows maciici at t~10s in ROM
`0x4080b578` at **SR I:0 (interrupts ENABLED)**, and by t~14s executing
RAM-resident OS code (`0x0002ca06`): its StartBoot hands off to the disk
boot with interrupts on.  The IIvx, by contrast, idles at **I:7** in ROM
forever.

### CORRECTION 1: the console is NOT reached via 0x40848dea

Session 12's path (`0x40848dea -> btst#26,d7 -> 0x408499bc -> console`) is
WRONG for the current build: a breakpoint at `0x40848dea` never fires in
280s.  `0x40848dea` is actually the POST **error handler** (target of many
`bne 0x40848dea` test-fail branches at 0x46600/0x4665a/0x466bc/0x49244/...).

### The REAL live default path (gdb-traced, `set architecture m68k:68030`)

Idle state: `PC=0x4084a0f0 SR=0x2708 (I:7)`, `D2=0x70000c05`,
`A7(USP)=0x70000c05`, `VBR=0x003ffe8e`.  The path that gets there:

1. POST passes -> the **cold-boot case dispatch at `0x408464a0`**: it reads
   `d2 = USP` (= `0x70000c05`, built by helper `0x40846788` from the
   capability word d0=0x773f: low word 0x0c05 = decoder kind, top bits from
   d0 bits 1/12/13) and switches on the **low byte = 0x05** via a
   `cmpib #3/#6/#5/#7/#14,d2` chain (`0x408464b0..0x408464fe`).  Low byte
   0x05 -> **case 5** (`0x408464e6`).
2. Case 5 tests **VIA1 port-A bit0**: `bclr #0,VIA1_DDRA(0x50f00600)` (make
   PA0 input); `btst #0,VIA1_ORA(0x50f01e00)` at `0x408464f2`.  a2=0x50f00000
   (=a0@(8), the VIA base), so this reads `pins_a & 1` = `0xef & 1` = **1**.
   -> `bne 0x408465a8` (PA0=1 path).
3. `0x408465a8` -> CACR setup -> cap -> `0x408465c4`: `(d1 & 0x0e) == 6`?
   d1=0x126 -> yes -> computed jump to **`0x40814c4c`** = the **Egret
   cold-start** (send-byte `0x40814cc8`, a1=VIA base, ORB bits 5/4/3 =
   /SYS_SESSION//VIA_FULL//XCVR).  It sends pseudo-commands 0x01/0x1b/0x1c
   (the Session 7/12 Egret layer -- runs cleanly), returns to `0x408465da`,
   sends 0x1c, then -> `0x40846bd0` **checksum** (passes, d6=0) ->
   `0x4084a6b0` **RAM test** -> cap -> video/EASC init `0x40845ce2` ->
   (computed jump) `0x408499bc` -> `0x408499f6` -> **MicroBug console
   `0x4084a0f0`**.

SCSI is never touched; **StartBoot (`0x4080bxxx`, where maciici boots) is
never reached** (breakpoint at 0x4080b578 never fires).  So the IIvx ROM's
kind-5 cold-boot runs the Egret + a second POST-ish pass and then drops into
the interactive `*` serial debugger -- it never invokes the disk boot.

### DISCOVERY: XPRAM 'RBBI'/'SCBI' signature reads gate the case dispatch

The case-3/5/6/7 branches all converge on `0x40846576`, which sets d7 bit26
then reads a **4-byte configuration signature** via a fetcher `0x4084a3a0`.
The fetcher is gated by decoder-record byte `rec+0x1c`: `(rec+0x1c & 0x70)`
== 0x20 selects an Egret-SR read; ELSE (our case -- `rec+0x1c == 0x00` for
ALL records 0x35b8/0x3b02/0x3b42) it uses the **RTC/PRAM bit-bang** alt path
`0x4084a4de` (ORB bit2 = vRTCEnb, command `0xb8|...`).  So the signature is
read from **XPRAM via the 343-0042 protocol our via1.PRAM[] already serves**.

Proven by seeding identity markers: the RBBI fetch (d3=0x7c00fc) builds
`d4 = [PRAM[0xfc],PRAM[0xfd],PRAM[0xfe],PRAM[0xff]]` and compares to
**'RBBI' = 0x52424249**; the SCBI fetch (case 14, d3=0x7800f8) reads
PRAM[0xf8..0xfb] vs **'SCBI' = 0x53434249** (both immediates live in the ROM
at 0x46526/0x4659a).

### EXPERIMENT (reverted): RBBI is a DIAGNOSTIC branch, not disk boot

With `pins_a=0xee` (PA0=0 -> take the `0x40846576` fetch branch; 0xee still
satisfies the earlier `(PA&0x56)==0x46`) AND `PRAM[0xfc..0xff]='RBBI'`, the
ROM **escapes the MicroBug console** (PC leaves 0x4084a0f0) and reaches
`0x40848f80` -> RAM test -> table-fill -> a SECOND RBBI fetch (0x40848fe2/
0x40849014, which then WRITES XPRAM via 0x4084a222) -> and finally an
**infinite blink loop `0x40849228`** (`eorib #8,a3@(16)` toggling a
video/RBV register forever, I:7, `a5@(14)==0`).  So the 'RBBI' XPRAM
signature triggers a **RAM/burn-in diagnostic**, NOT disk boot.  Reverted
(pins_a back to 0xef, no PRAM seed) -- it is a dead end, and default boot is
unchanged (re-verified: console at 0x4084a0f0, no regression;
`/tmp/iivx-s13.png`).

### Conclusion: the kind-5 identity has NO reachable disk-boot path

Every branch of the `0x408464a0` case dispatch our kind-5 (0x0c05/0x0505)
identity can take funnels to either the MicroBug console (PA0=1 -> Egret ->
2nd-POST -> console) or a diagnostic blink (PA0=0 + XPRAM signature).  The
ONLY branch with a clean `bset #26,d7` + StartBoot-style continuation is
**case 14 (d2 low byte 0x0e)**, reached via `0x40846536` (gated by
`XPRAM[0xf8..0xfb]=='SCBI'` AND `(a0@(32)@(64) & 0x1c)==0x10`) -- but the
kind-5 identity never produces d2 low byte 0x0e.  This matches the
Session 6/7 root cause (VASP's decoder-kind identification registers, which
Apple never published, are not modelled, so the cap routine `0x40802f18`
yields an RBV-family kind-5 identity), now pinned to the EXACT dispatch and
the EXACT XPRAM/PA0 gates.

Whether maciici avoids all this because its (different-binary) IIci ROM
lacks this `cmpib #3/#5/#6/#7/#14,d2` dispatch entirely (confirmed absent in
macIIci.rom by byte search) -- its cold-boot invokes StartBoot directly at
I:0.  So "mirror a maciici value" does not apply verbatim: the IIvx ROM's
cold-boot is structurally different and demands the kind-14 (or another)
identity our VASP stub can't synthesize.

### Concrete plan for next session (in priority order)

1. **Find which decoder kind routes to StartBoot.**  Extend the existing
   `IIVX_FORCE` infra to also force the USP/d2 low byte at `0x408464a0`
   (or the kind word the helper `0x40846788` builds).  For each case
   (3/5/6/7/14) trace whether the continuation reaches `0x4080bxxx`
   (StartBoot) vs console/blink.  Case 14's `0x40846536->0x40846576` sets
   d7 bit26 -- follow it past the RBBI fetch to confirm it can reach
   StartBoot when 'SCBI' matches and `(a0@(32)@(64)&0x1c)==0x10`.
2. **If case 14 reaches StartBoot:** (a) force d2 low byte 0x0e (record
   kind low byte, or a targeted patch at the dispatch), (b) seed
   `XPRAM[0xf8..0xfb]='SCBI'` (0x53 0x43 0x42 0x49), (c) set the
   `a0@(32)@(64)` decoder-record field so `&0x1c==0x10` (identify a0@(32)
   -- it is the decoder/globals record from the cap routine -- and which
   byte/where it is seeded).  Verify d7 bit26 set + StartBoot reached.
3. **Interrupts:** StartBoot must run at I:0 (maciici does).  The console is
   entered at I:7; once StartBoot is reached, confirm the ROM lowers the
   mask (it should, as part of the boot process) -- if not, the boot-device
   scan (task suspect d) needs the VIA/Egret/RBV interrupt wiring live.
4. From StartBoot: SCSI disk boot (NCR5380 model already present) -> Happy
   Mac (slot-$E DeclROM already provided) -> System -> Finder; expect the
   XPRAM ddType / boot-driver-installer work already staged
   (XPRAM 0x76/0x77/0x78 seeds, READ_MCU streaming).

### Status / guardrails

- No code change committed this session (all experiments reverted); default
  `-M maciivx` unchanged -- boots crash-free to the MicroBug console
  (`PC=0x4084a0f0 I:7`), screenshot `/tmp/iivx-s13.png`.  maciici reference
  to Finder: `/tmp/iivx-maciici-finder.png`.
- Only investigation; `maciici`/`maciisi`/`macclassicii`/shared devices
  untouched.  Build `/tmp/iivx-build` only.  gdb needs
  `set architecture m68k:68030` (NOT just `set endian big`) or reads return
  "Truncated register 16".  Ports 1279/1280, sock /tmp/iivx-mon2.sock.

## Session 14: BROKE THE CONSOLE WALL -> SCSI DISK BOOT, System runs (headless).  Root-caused the whole cold-boot chain; three IIvx-local fixes committed.

The Session 4-13 MicroBug wall is GONE.  `-M maciivx` now runs the full POST,
StartBoot, the Toolbox, enables interrupts, boots off the SCSI disk, and runs
the loaded System in RAM.  Remaining gap: the on-board video (slot-$E Brazil)
driver does not point QuickDraw at the VRAM, so it runs headless (blank
screen).  Four discoveries, three committed fixes:

### 0. Case sweep (coordinator's decisive experiment) -- case 14 DISPROVEN

Forced the `d2` low byte at the cold-boot case dispatch `0x408464a0` (fixed
`0x408464b0`, after USP is reloaded) and swept cases 3/5/6/7/14 with the
XPRAM 'SCBI'/'RBBI' signatures seeded: NONE reaches StartBoot.  5/6/7/14 ->
Egret cold-start -> console; 3 (with RBBI) -> diagnostic blink `0x40849228`.
So the `0x408464a0` dispatch is NOT the boot lever (it is a hardware-config
sub-routine); the real boot path is elsewhere.

### 1. The REAL console cause: POST phase-table device self-tests fail

After the Egret cold-start the cold-boot reaches a table-driven POST phase
sequencer (`0x408466f8`; phase table `0x40846908`, ids 0x84-0x97) and, once
all phases pass, jumps to **StartBoot at `0x408000b8`** (via `0x4084677c`,
`jmp pc@ + 0xfffb993a`).  Four phases FAIL because our device stubs don't
reproduce their register-level self-test behaviour:
  - **0x88** (`0x408473fc`): NCR5380 SCSI register self-test (a0@(32)=
    0x50f10000: ICR reg1 <- 0x80/0x10/8/4/2 read-back, bus-status reg4/5),
  - **0x8b/0x8c/0x8d** (`0x408478d4`/`0x40847a44`/`0x40847d32`): SCC/ASC/
    VIA2-RBV register walks.
Each returns d6!=0 -> the soft-fail handler `0x40848efa` routes to the
MicroBug console (`0x40849f84`).  **FIX (commit a09bef36e2):** patch the
soft-fail handler's first test to `rts` so unmodelled-device POST diagnostics
are non-fatal (phases still run in full; only the verdict is ignored).  POST
completes -> StartBoot reached -> Toolbox runs (A-line traps).

### 2. StartBoot fault: VASP config register 0x5FFFFFFC unmapped

StartBoot (`0x4084ab2a`) `_SwapMMUMode`s to 32-bit and reads a 3-bit
machine/video config field at **0x5FFFFFFC** (-> lowmem 0xCB3), which was
unmapped (gap between I/O 0x50Fxxxxx and VRAM 0x60B00000) -> bus error ->
StartBoot stalls back to console.  **FIX (commit 179f88d34d):** back the
0x54000000-0x5FFFFFFF decode returning 0.  With this, StartBoot completes:
**Level-1 (Egret ADB) and Level-2 (RBV) interrupts fire at I:0** and the
Toolbox runs extensively (exception count 459 -> 4267).

### 3. dsBadSlotInt (SysError 51): slot-$E VBL with no handler

With interrupts live the RBV level-2 slot dispatcher (`0x40806eaa`) fires for
the on-board-video slot-$E VBL, finds a NULL handler slot, loads `moveq #51`
(`0x40806f10`) and calls `_SysError(51)` = **dsBadSlotInt** -> console
(verified: d0=0x33 at the A9C9 site `0x40806eea`; the -d int log ends with
pc=0x40806eea just before the console).  The OS enabled slot-$E in RSIER
before its VBL handler was installed.  **FIX (commit 2cbad0c0e6):** exclude
slot-$E (bit6) from the level-2 CPU summary in `maciivx_rbv_update_irq`
(`& 0x3f`; SIFR bit6 still pollable) -- matching the "VBL missed if masked is
safe" model already documented there.

### RESULT: SCSI DISK BOOT

With all three, `-M maciivx` boots off the SCSI disk: the System loads and
**executes in RAM at I:0** -- verified PC cycling through RAM + ROM Toolbox
(0x0002eb8a, 0x0077e160, 0x007a6928, 0x4080e01c, ...) in a live event loop,
exceptions in the thousands, Egret ADB + RBV interrupts serviced.  This is
the goal signal (off the console, running the disk System) -- dramatically
past the Session 4-13 wall.

### Remaining gap: on-board video runs headless (the last mile to Finder)

The System runs but the screen stays blank: QuickDraw's **ScrnBase (lowmem
0x824) = 0x000072b0** (low RAM), NOT the dedicated VRAM (0x60B00000 / slot-$E
0xFE000000), and the VRAM reads all-zero.  The slot-$E "Brazil" onboard-video
driver (DeclROM provided in Session 11) is not establishing the VRAM frame
buffer / gDevice, so QuickDraw draws nowhere our framebuffer scans out.
Screenshots blank (`/tmp/iivx-long.png`).  This is the onboard-video /
Slot-Manager frontier (Session 10/11 territory), now REACHABLE for the first
time because the OS actually boots.  Next session:
  1. Trace the Slot Manager / Display Manager reading the slot-$E DeclROM
     (0xFEFxxxxx) during OS boot and loading `.Display_Video_Apple_Brazil`;
     confirm PrimaryInit runs and where it sets the PixMap baseAddr /
     ScrnBase.  (Session 11 saw ZERO slot-$E reads because boot never got
     here; that should now change.)
  2. Fix whatever prevents the driver from setting ScrnBase = the VRAM slot
     base (0xFE000000, our vram_slotE alias) -- likely a DeclROM sResource /
     mode-table / VDAC-depth detail, or the 8-bit-vs-1-bit renderer (our fb
     is 1-bit; the IIvx default is 8-bit CLUT, so even a drawn desktop of
     index-0 pixels reads as blank -- the renderer may need CLUT/depth).
  3. Re-examine the slot-$E VBL: excluding it (fix 3) unblocks boot but the
     VBL-driven cursor/Vertical-Retrace tasks won't run; once the video
     driver installs its handler, assert slot-$E again (or gate the assert on
     handler presence) so the desktop is interactive.

### Status / guardrails

- Three fixes, all `hw/m68k/maciivx.c` only (soft-fail handler rts; VASP
  config-gap decode; slot-$E L2 exclusion), each covered by the existing
  checksum repair where a ROM patch.  Commits a09bef36e2, 179f88d34d,
  2cbad0c0e6.  maciici re-confirmed to Finder on the same disk this session
  (`/tmp/iivx-maciici-finder.png`); maciisi/macclassicii/shared devices
  untouched.  Build `/tmp/iivx-build` only; gdb needs
  `set architecture m68k:68030`; ports 1279/1280, sock /tmp/iivx-mon2.sock.

### Session 14 addendum: onboard-video investigation (the last mile)

Traced QuickDraw's screen while the System runs: **ScrnBase (lowmem 0x824) =
0x000072b0**, and MainDevice (0x8a4) -> GDHandle 0x2124 -> GDevice 0x80007338
(24-bit tagged -> 0x7338) -> gdPMap 0x2114 -> PixMap 0x80007388 whose
**baseAddr = 0x72b0** -- i.e. the OS built its main screen as a 1-bit software
bitmap in LOW RAM, not at the dedicated VRAM (0x60B00000 / slot-$E
0xFE000000).  So the slot-$E "Brazil" video driver is not creating a hardware
gDevice at the VRAM; the Slot/Display Manager fell back to a RAM screen our
framebuffer never scans.  (Pointing the fb at RAM 0x72b0 as a 640x480x1 image
rendered solid, inconclusive -- the fallback screen's depth/rowBytes/base are
not our fb's assumption; env knob IIVX_FBSCR was used for this and reverted.)
Concrete next step: break during OS boot on the first slot-$E DeclROM read
(0xFEFxxxxx) to confirm the Slot Manager enumerates it and runs the video
sResource's PrimaryInit; find why it doesn't install a gDevice with
baseAddr=0xFE000000 (likely a DeclROM sResource/mode-table/VDAC-depth detail),
then the desktop lands in VRAM and the fb (which may also need 8-bit CLUT
rendering) shows Finder.
