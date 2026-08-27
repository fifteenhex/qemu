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
