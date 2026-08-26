# CLIE Phase-1 PoC journal

Working in `/workspace/src/qemu-clie`, branch `clie-poc`.  Plan document:
`CLIE-RESEARCH.md` (same tree).  Disk is tight (~1.4-1.9 GB free on
`/workspace`); ROM archives were fetched to `/tmp` (tmpfs, 15G) and deleted
after extracting, only the `.rom` files were kept, copied to
`/workspace/src/palm-roms/` (gitignored, same convention as `PALM-NOTES.md`).

## ROM sourcing

Fetched both candidate zips from the `jp.sony.clie` archive.org item and
verified md5 against the values already recorded in `CLIE-RESEARCH.md`
sec 7.1 (both matched exactly):

* `ROM/Sony CLIE PEG-S320 ROM (English).zip` -- md5
  `2ba82507938a4f5c29c491b184dd8c9c` (matches). Contains
  `palm-os-40-s320.rom`, 2,949,120 B.
* `ROM/Sony CLIE PEG-S300 ROM (English).zip` -- md5
  `16673e1569569171e57c4def5ea99479` (matches). Contains `S300en.rom`,
  1,638,400 B, matching the exact filename/size CLIE-RESEARCH.md sec 7.2
  already recorded for S300.

### Finding: the archive.org "PEG-S320" ROM is not an EZ328/S-series image

CLIE-RESEARCH.md's taxonomy table (sec 2.1) lists PEG-S320 as MC68EZ328,
same `EzPegS300` Cloudpilot codename as the S300 -- but that row was never
cross-checked against a real dump (the sec 7.2 "verified layouts" table
only covers S300/T400/N700C/NR70V, not S320).  Actually inspecting the
downloaded S320 zip's `palm-os-40-s320.rom` shows:

* `strings` contains `68VZ328 LCD Controller` and `68VZ328RefSerialDrvr`
  (VZ328's on-chip LCDC identity string, not the EZ328's plain
  `68328 LCD Controller`).
* `strings` contains `sonynasc` -- Cloudpilot's "Nasca" codename, which
  CLIE-RESEARCH.md sec 2.1 already maps to the N600C/N610C (VZ328) family,
  not the S-series.
* Cross-checked against Cloudpilot's own `EmDevice.cpp` (fetched from
  `github.com/cloudpilot-emu/cloudpilot-emu`, commit at fetch time
  2026-08-25): `case kDevicePEGS320:` constructs `EmRegsVzPegNasca`, not
  `EmRegsEzPegS300`. This independently confirms the ROM string evidence:
  **PEG-S320 is VZ328/"Nasca"-based**, not EZ328.  Whoever packaged the
  archive.org item, or Sony itself, apparently reused the Nasca CLIE OS
  image release for the S320 English ROM (plausible: the S320 was a later,
  higher-clocked running of the same board family in some markets) --
  or the archive's file is simply mislabeled. Either way it is **not**
  the "thin palmv variant" target CLIE-RESEARCH.md sec 5 Phase 1
  describes.

`EmDevice.cpp` does confirm `case kDevicePEGS300:` constructs plain
`EmRegsEzPegS300` (an EZ328 HAL) with an `EmRegsMB86189` (Memory Stick
host controller) at base `0x10200000` -- this matches the Phase-1 target
description exactly.  **Decision: build the PoC against PEG-S300, not
S320.**  This is a corrected/narrowed version of CLIE-RESEARCH.md's
"S320 or S300" recommendation -- S300 is the one that is actually a thin
EZ328 Palm-V-shaped machine.

`S300en.rom` header, hand-verified (`python3` reading raw bytes), matches
CLIE-RESEARCH.md sec 7.2 exactly:
* file+0: SP `0x000002e4`, PC `0x100002a2`, `FEEDBEEF` at +8 -- small ROM,
  same reset PC CLIE-RESEARCH.md recorded.
* file+0x8000: SP `0x00001126`, PC `0x100090e4`, `FEEDBEEF` at +8 -- big
  ROM card header.
* `strings` contains `68328 LCD Controller` (on-chip DragonBall LCDC, EZ
  variant), `Sony Corporation`, and a full set of `Memory Stick`/`Jog*`
  strings (JogSys/JogAssist/JogViewâ€¦), confirming both Sony peripherals
  the research doc flags as needing stubs are actually probed by this
  ROM.

ROM kept at `/workspace/src/palm-roms/Sony-CLIE-PEG-S300-en.rom` (md5
`f08ef88f7a2b2c11b1aa29ba5ed21a0f`, this is the *extracted* file's own
md5, not recorded in CLIE-RESEARCH.md which only has the zip's md5).

## Register-level ground truth (Cloudpilot-emu source, fetched directly)

CLIE-RESEARCH.md correctly identified Cloudpilot-emu as the primary
register reference but had not fetched the actual files (no network
budget noted in its "Proof-of-concept status" section).  Fetched directly
from `raw.githubusercontent.com/cloudpilot-emu/cloudpilot-emu/master/...`
this session:

* `src/cloudpilot/emulator/hardware/clie/EmRegsEzPegS300.cpp` + `.h`
* `src/cloudpilot/emulator/hardware/clie/EmSonyXZWithSlot.h`
* `src/cloudpilot/emulator/hardware/EmRegsMB86189.cpp` + `.h` (note:
  **not** under `hardware/clie/` -- it's a shared/generic MS host
  controller model used by several Sony boards, listed under plain
  `hardware/`; CLIE-RESEARCH.md sec 8's file list didn't have this path)
* `src/cloudpilot/emulator/EmDevice.cpp`

### What EmRegsEzPegS300 needs from GPIO (the actual "why would this hang" list)

`EmRegsEzPegS300::GetPortInputValue()`:
* Port B bit `0x02` ("LCD powered", `hwrEZPegPortBLCDPowered`) is forced
  set unconditionally, with the comment *"Make sure this is always set,
  or HwrDisplayWake will hang"*.  This is a real hang risk this tree's
  generic EZ GPIO defaults do **not** cover (unlike PowerFail, which the
  base `palm_ez` code already ties off for every EZ Palm).

`EmRegsEzPegS300::GetPortInternalValue()` (via the base
`EmSonyXzWithSlot<EmRegsEZ>` wrapper, `EmSonyXZWithSlot.h`):
* Port D bit `0x80` (PowerFail) and bit `0x10` (dock/HotSync button) are
  forced set. PowerFail is already handled generically by
  `PALM_POWERFAIL_GPIO` in `palm.c`; dock button (bit 4) was not -- added.
* Port D bit `0x08` ("MS inserted") reflects `gExternalStorage.IsMounted`;
  cleared when nothing is mounted -- i.e. **the "no card" state is the
  default (0), no explicit tie needed**, which is convenient: it's
  exactly what our GPIO block already reads before anything raises it.
* Port D bit `0x40` ("MS_IF_Intl", reflects `mb86189.GetIrq()`) is set
  when the MB86189 is *not* asserting an interrupt.  Our MS stub (below)
  never asserts an interrupt, so this needs to be tied high too, or a
  stray 0 could look like a pending MS interrupt.

None of Cloudpilot's overrides mention a separate Jog Dial register or
address range for the S300: `EmRegsEzPegS300::GetKeyInfo()` wires the
jog rotation into the *same 3x3 hard-button matrix* as the physical
buttons (`kButtonMap` row 1 = `{keyBitPageUp, keyBitPageDown, 0}`, on
port F bit `0x20`/`0x40`; row 0 is on port F bit `0x01`, **not** the
plain-EZ default of F4/F5/F6 that `palm_ez_machine_class_init()` sets for
palmv/iiix/vx/m100).  **Conclusion: the Jog Dial's rotate axis needs no
new device at all** -- it rides the existing `palm_keypad.c` +
`dragonball_gpio.c` substrate this tree already has, just with S300's
row wiring (F0/F5/F6) instead of the generic EZ wiring (F4/F5/F6). Jog
*press* and the *Back* button are not in this file (likely a separate
GPIO edge per CLIE-RESEARCH.md sec 4.4) -- not needed to reach the
launcher, deferred to Phase 4 as the research doc already scoped.

### Memory Stick host controller (MB86189) register map

From `EmRegsMB86189.cpp`/`.h`, base address from `EmDevice.cpp`
(`case kDevicePEGS300: new EmRegsMB86189(0x10200000)`):

| Offset | Reg | Notes |
|--------|-----|-------|
| 0x00 | MSCMD | 16-bit; TPC command; writing starts a transfer |
| 0x02 | MSCS  | 16-bit status: INT=0x8000, DRQ=0x4000, RBF=0x0400, RBE=0x0800, RST=0x0080. Reset value `0x0a05` (RBE set, idle). |
| 0x04 | MSDATA | 16-bit FIFO data |
| 0x06 | MSICS | 16-bit IRQ enable/flags, INTEN=0x0080 |
| 0x08 | MSPPCD | 16-bit, bits 12-13 mirror a GPIO read-handler |
| 0x0a-0x13 | (unused tail of the 0x14-byte register file) |

Stub design (Phase 1, `hw/misc/sony_mshc_stub.c`): a single 0x14-byte
MMIO window that always answers MSCS as `0x0a05` (permanently idle,
matching the chip's own post-reset value) and everything else as 0;
writes are accepted and dropped.  Combined with the GPIO ties above (MS
card-detect bit low = "no card", MS IRQ-sense bit high = "not
requesting"), this reproduces exactly the state Cloudpilot itself
presents for an S300 with an empty slot, without implementing the TPC
protocol (that's Phase 3 per CLIE-RESEARCH.md sec 5).

## Machine wiring (`hw/m68k/palm.c`)

Kept everything in `palm.c` rather than a new `hw/m68k/clie.c`, matching
how `palmm500`/`palmm515` are already just more `PalmMachineClass`
variants in the same file sharing `palm_init()` -- there is no
Palm-machine-glue header to split against, and CLIE-RESEARCH.md itself
frames the CLIE as "a thin PalmMachineClass variant".

Added:
* `PalmMachineClass.ms_stub_base` (0 = no Sony peripherals on this
  machine; non-zero = chip-select base for the MS host controller stub,
  and triggers the three GPIO ties above in `palm_init()`).
* `hw/misc/sony_mshc_stub.c` + header, wired into `hw/misc/Kconfig` /
  `meson.build` as `CONFIG_SONY_MSHC_STUB`, selected from `CONFIG_PALM`
  in `hw/m68k/Kconfig` (all the Palm machines already share one Kconfig
  symbol, so this follows that shape rather than inventing a separate
  `CONFIG_CLIE`).
* Machine `clie-s300`: `palm_ez_machine_class_init()` as the base (same
  EZ328 SoC blocks, ADS7843 digitizer, PWM beep, plain grey on-chip
  LCDC), then override: `rom_base=0x10000000`, `rom_size=2MiB`,
  `rom_load_offset=0`, `bigrom_offset=0x8000` (whole-flash S300 layout,
  CLIE-RESEARCH.md sec 7.2), `default_ram_size=8MiB` (research table sec
  2.1), kbd rows F0/F5/F6 (Cloudpilot ground truth above, corrects the
  generic EZ F4/F5/F6 default), `ms_stub_base=0x10200000`.

Did not touch any existing Palm machine's defaults; `palm_ez_machine_class_init()`
is called exactly as it already was for palmv/palmiiix/palmvx/palmm100,
just followed by CLIE-specific overrides in the new
`clies300_machine_class_init()`, the same pattern `palmm515` uses on top
of `palmm500`.

## Build

`mkdir -p build && (cd build && ../configure --target-list=m68k-softmmu --disable-docs --disable-tools) && ninja -C build qemu-system-m68k`.
Clean build succeeded first try, no warnings from the new code.  Binary:
`build/qemu-system-m68k`, `-M help` lists `clie-s300  Sony CLIE PEG-S300
(MC68EZ328)` alongside the existing Palm machines.

Disk headroom was the binding constraint throughout (`/workspace` started
at 1.4-1.9 GB free, ended at ~1.1 GB): configured/built exactly once,
all further edits were incremental `ninja` relinks (single .o
recompiled), and ROM archives were fetched/extracted in `/tmp` (tmpfs)
and deleted right after, keeping only the final `.rom` files under
`/workspace/src/palm-roms/`.

## Boot test 1: spot-check `palmv` is still bit-exact

`build/qemu-system-m68k -M palmv -bios .../Palm-V-3.1-en.rom -display none
-qmp unix:/tmp/x.qmp,server=on,wait=off` + `palm-tools/palmctl.py ...
tap:16384,16384 sleep:2 dump:...` reliably reaches "Setup 2 of 4" (the
digitizer calibration target screen) from a single tap on "Setup 1 of 4"
-- unchanged from before this session's edits.  `palm_ez_machine_class_init()`
and `palm_init()` were only extended (new `if (pmc->ms_stub_base)` block
and one new `PalmMachineClass` field), never modified in a way that
touches non-CLIE machines' behaviour; this run confirms it.

## Boot test 2: `clie-s300` + `S300en.rom`

`build/qemu-system-m68k -M clie-s300 -bios
/workspace/src/palm-roms/Sony-CLIE-PEG-S300-en.rom -display none -qmp
unix:/tmp/clie.qmp,server=on,wait=off -serial null`, screendumped via
`palm-tools/palmctl.py`.

**Result: boots straight through DragonBall SoC init, past both Sony
peripheral stubs, and paints the exact same "Setup 1 of 4" Welcome
screen PalmOS 3.x shows on the Palm V** (screenshot:
`clie-poc-evidence/FINAL-clie-welcome.png`, byte-identical layout to
`clie-poc-evidence/FINAL-palmv-setup2.png`'s screen 1, modulo the
grey-dither frame noise every dump has).  The PLL is reprogrammed more
times during CLIE boot (7 `pllcr`/`pllfsr` printf blocks) than during
palmv boot (3 blocks) -- consistent with Sony's HAL doing additional
board bring-up work before settling, exactly what CLIE-RESEARCH.md
predicted.  No `ignore_memory_transaction_failures` bus errors, no
"bad register" spam from any of our new code (`sony-clie-mshc-stub` never
even logged a write -- the ROM's Expansion Manager evidently checks the
GPIO card-detect bit and skips touching the controller when it's clear,
exactly as designed).

This alone clears the CLIE-RESEARCH.md "good" tier ("gets through
DragonBall init + past the Sony peripheral probes to the boot/digitizer
screen") -- screen 1 of the Setup wizard *is* the pre-digitizer-calibration
screen, one tap short of the calibration screen itself.

### The remaining gap: input does not advance the Welcome screen

Tapping the emulated CLIE's screen (the same `palmctl.py tap:` invocation
that reliably advances `palmv`) does not advance CLIE past "Setup 1 of
4", however many times or wherever on screen it's tapped (tried center,
corners, held down for 3s, silk-tap `key:f7`, hard button `key:f5`,
30+ combinations across ~10 fresh boots). This is **not** a frozen
CPU -- verified three independent ways:

1. `human-monitor-command "info registers"` (via QMP) sampled seconds
   apart shows `PC = 0x10076028` every time, which turned out to be
   *expected*: `x/16i 0x10076000` (gdb-multiarch, `set architecture m68k`,
   `set endian big`, `target remote localhost:1234` with `-s`) disassembles
   it as the normal DragonBall EZ idle primitive (poll the PLLFSR CLK32
   toggle bit, then `stop` until an interrupt) -- the same routine every
   idle Palm parks in.
2. `xp /1xw 0xfffffb00` (RTC seconds register) increments correctly
   between two `human-monitor-command` calls 2 seconds apart --
   the RTC/timer interrupts are firing and being serviced, so the CPU
   is genuinely running, not wedged on a bus error or an infinite tight
   loop with interrupts disabled.
3. Most importantly: `xp /1xw 0xfffff310` (INTC IPR, the interrupt
   *pending* register) reads `0x00000000` normally and `0x00100000`
   (bit 20 = `DRAGONBALL_INTC_IRQ5`, the /PENIRQ line) the instant a
   synthetic `input-send-event` pen-down is sent and held -- **the
   digitizer model's output correctly reaches the interrupt controller**.
   `dragonball_intc.c`'s own comment confirms this is the intended
   mechanism ("its IPR bit follows the /PENIRQ pin level and PalmOS
   polls it to track the pen, silencing the interrupt via IMR instead"),
   and it works exactly as documented.

So pen-down state genuinely reaches the emulated hardware; the gap is
somewhere in Sony's ROM between reading that state and acting on it, or
the CPU has already left the code path that reads it by the time the
Welcome bitmap is drawn (i.e. the screen may be a static splash payload
drawn before the interactive event loop is fully installed, rather than
the live "tap anywhere" handler itself, if e.g. one further
un-stubbed Sony probe -- the USB device controller (`EmRegsUsbCLIE`,
`EmDevice.cpp` puts it at a relative offset `0x00100000` for
`kDevicePEGS300`, not instantiated in this Phase-1 build since
CLIE-RESEARCH.md sec 3.3 item 7 scoped USB as "not needed to boot") --
gates something the event manager needs before it starts polling input.

**Bisected against our own changes to rule out a self-inflicted bug**
(two throwaway rebuilds, `#if 0`'d out and reverted afterward, verified
`git diff` is clean of leftovers):
* Removing the three Sony GPIO ties (`PALM_CLIE_LCDPWR_GPIO` /
  `_DOCKBTN_GPIO` / `_MSIDLE_GPIO`) entirely: CLIE *still* reaches the
  Welcome screen (the GPIO reset defaults -- port B has a pull-up
  register reset value, `DRAGONBALL_GPIO_PBPUEN_RESET` -- happen to
  already read bit 1 high) and *still* doesn't respond to taps. Ties
  restored (still correct/faithful to Cloudpilot and harmless, but
  proven not to be *this* bug).
* Reverting the kbd row wiring to the generic EZ default (F4/F5/F6
  instead of S300's real F0/F5/F6): no change either.  Reverted back
  to the correct S300 wiring.

Both of this session's CLIE-specific `palm_init()` additions are
therefore cleared as suspects; the remaining candidates are (a) a
still-missing Sony probe (USB controller being the obvious next one to
try, or the port-E "hardware sub-ID" read that Cloudpilot itself
stubs out with `#if 0` -- worth trying forced instead of floating),
or (b) something in the shared ADS7843/digitizer scaling that Sony's
HAL reads differently than Palm's stock ROM despite using the same
POSE "channel set 2" wiring `hw/input/ads7843.c` already documents.
Next session should try: (1) instantiate `EmRegsUsbCLIE`'s address as
a plain `TYPE_UNIMPLEMENTED_DEVICE` stub and retest; (2) a scripted
gdb session stepping from reset with a real .rom-matched disassembly
(objdump on `S300en.rom` at the file offsets corresponding to any PC
sampled near the Welcome-screen draw) to find the actual branch that
decides "wait for tap" vs whatever loop it's really in.

## Result vs CLIE-RESEARCH.md success tiers

* (best) launcher, screenshotted -- not reached this session.
* **(good) "gets through DragonBall init + past the Sony peripheral
  probes to the boot/digitizer screen" -- reached and screenshotted**
  (`clie-poc-evidence/FINAL-clie-welcome.png`): the CLIE machine boots
  through PLL/GPIO/INTC/timer/RTC/LCDC bring-up, past both no-fault
  Sony stubs, and renders the shared Setup-wizard Welcome screen
  identically to `palmv`.
* (min) machine + stubs exist, ROM loads and runs -- exceeded; the
  first genuine open question (why input doesn't advance the Welcome
  screen) is precisely characterized above with three independent
  pieces of register-level evidence, rather than a generic "it hangs".

Screenshots kept in `clie-poc-evidence/` (gitignored):
`FINAL-clie-welcome.png` (CLIE, screen 1, before any tap),
`FINAL-clie-after-tap.png` (CLIE, screen 1, unchanged after a tap that
would advance a real/POSE-emulated Palm), `FINAL-palmv-setup2.png`
(control: palmv screen 2, i.e. proof the tap mechanism and test harness
work), `palmv-boot.png` (control: palmv screen 1, for visual
comparison against the CLIE Welcome screen).

---

## Session 2: root-causing the input gate (still not at the launcher)

Picked up in `/workspace/src/qemu-clieb` (this tree; `qemu-amiga` is a
separate consolidated checkout, not touched), branch `fin-clie`
(already checked out, `clie-s300` machine already present from Session
1 above). Configured once (`mkdir build && (cd build && ../configure
--target-list=m68k-softmmu --disable-docs --disable-tools) && ninja -C
build qemu-system-m68k`, `CCACHE_DIR=/tmp/ccache-clieb` to keep the
object cache off `/workspace`), no source changes needed to reproduce
Session 1's exact stuck-at-Welcome state. `/workspace` was down to
~700MB-1.5GB free all session; all gdb scripts, disassembly dumps and
screendumps were kept in `/tmp` (tmpfs) throughout, per the brief.

**Result up front: still does not reach the launcher.** What changed
is *why*: Session 1 left it as "one more un-stubbed Sony probe,
suspect USB, or a digitizer-scaling difference" — both open. This
session rules out USB entirely, rules out digitizer scaling entirely,
and pins the actual blocker to one specific ROM routine at a specific
address, with the exact mechanism traced at the instruction level.
No source changes were made this session (the finding is a guest-ROM
control-flow/hardware-probe issue, not something fixable by adding a
`palm.c` GPIO tie or a new stub device the way the Session-1 quirks
were — see "Why no fix was attempted" below).

### Tooling notes (for whoever continues this)

* `gdb-multiarch -s -S` / `target remote localhost:1234` is a trap in
  this sandbox: **other agents in this box run unrelated m68k Mac
  machines that also bind the default gdb port 1234** (seen live:
  `quadra700`, `quadra950`, `macclassicii`, `macse30` from sibling
  `qemu-*` checkouts). Always pass an explicit high port
  (`-gdb tcp::1998x`) and double-check `ps aux | grep qemu-system-m68k`
  before trusting a gdb session — an early sample in this session
  silently talked to someone else's `quadra700`.
* Plain `gdb-multiarch -batch -x script.gdb` with `commands`/`continue`
  blocks driving a *running* QEMU target is unreliable here: batch-mode
  `continue` sometimes returns `"Cannot execute this command while the
  target is running"` on the very next scripted command, and multi-tap
  `-batch` sessions with several `continue`s in a row reliably outlive
  the tool's 2-minute per-call cap. What worked: a small Python driver
  (`subprocess.Popen` + `select()` on the pipe, logging to a file
  under `/tmp`, launched detached so it survives past the calling
  Bash-tool call, polled from later calls) — see `/tmp` scratch scripts
  referenced below (not preserved; recreate the pattern if needed:
  `gdb-multiarch -nx -q`, feed commands over its stdin pipe, `select()`
  with a timeout to drain stdout, `send_signal(SIGINT)` to interrupt a
  hung `continue`).
* QMP `human-monitor-command` (`xp /1xw <addr>`, `p $pc`) is completely
  reliable and fast even while gdb is flaky, and was the workhorse for
  every timing-sensitive measurement below (register/vector-table
  polling every 20-30ms). Prefer it over gdb whenever the question is
  "what does memory/PC look like right now", and reserve gdb for
  breakpoints.
* Backgrounding qemu with `cmd &; echo $!` intermittently failed to
  survive to the next tool call in this sandbox (the pid vanishes, no
  error). No root cause found; the fix that consistently worked was
  the plain foreground-looking form `cmd > log 2>&1 &` with an
  immediate `sleep 1; ps -p $!` check in the *same* tool call, retried
  if the check came back empty.
* m68k disassembly alignment trap: `m68k-linux-gnu-objdump -D -b binary
  -m m68k:68000 --adjust-vma=0x10000000 <rom>` over the *whole* ROM
  drifts out of sync with the real instruction boundaries after the
  first stretch of embedded data (string tables, jump tables) and
  silently produces plausible-looking garbage instructions from then
  on for the rest of the file. Always re-disassemble the specific
  region under study with an explicit `--start-address=<addr>
  --stop-address=<addr+N>` pinned to a known-good instruction boundary
  (a `jsr`/`lea`/`trap` target address you already trust) before
  reading anything from it. This cost real time twice this session —
  a `jmp %pc@(0x10076028)` immediately followed the actual end of the
  spurious-interrupt trampoline (see below), and everything the
  whole-file sweep printed after that `jmp` was bogus.

### Ruled out this session

* **USB controller (`EmRegsUsbCLIE`)**: not the blocker. Nothing in
  the boot path up to and including the point where input stops
  working touches `0x10400000` (the address CLIE-RESEARCH.md/Session-1
  flagged) at all under `ignore_memory_transaction_failures` bus
  logging — the CLIE ROM's Expansion/USB detection is apparently
  gated behind the same broken mechanism described below, not reached
  independently.
* **Digitizer/ADS7843 channel or scaling mismatch**: not the blocker.
  Confirmed via `EmRegsEzPegS300::GetSPISlave()` (fetched this session
  from `github.com/cloudpilot-emu/cloudpilot-emu`,
  `hardware/clie/EmRegsEzPegS300.cpp`) that the S300 board gates the
  touch ADC's SPI chip-select through GPIO port G bit 3
  (`hwrEZPegPortGADC_CS`), unlike the Palm V's grounded-CS wiring our
  `hw/input/ads7843.c` models (`cs_polarity = SSI_CS_NONE`) — this is
  a real fidelity gap but not a blocking one, since `SSI_CS_NONE`
  means our model answers *regardless* of the CS GPIO state, so it is
  strictly more permissive than real hardware here, not less. Cloudpilot's
  `EmRegsEzPegS300::GetPortInputValue()` also has the Port E
  "hardware sub-ID" read `#if 0`'d out upstream (i.e. Cloudpilot itself
  ships with it *not* driven), matching our current do-nothing default
  for that port — also not a lead.

### The actual blocker: interrupt vectors 4-7 never get their real handlers installed

Traced with `-gdb tcp::<port> -S`, `set architecture m68k`, `set endian
big`, cross-referenced against `m68k-linux-gnu-objdump` on
`Sony-CLIE-PEG-S300-en.rom` (properly aligned per the tooling note
above) and QMP `xp` polling of the CPU's low-memory exception vector
table (address `0x00-0x7F`, m68k plain `m68000` core: no VBR, vectors
are always at absolute address 0).

**Observation 1 — the vector table freezes ~0.8s into boot and never
changes again.** Polling `xp /4xw 0x70` (autovector levels 4-7: SPI/
UART/RTC/KB/etc share level 4, Pen=IRQ5=level 5, Timer=level 6,
EMIQ=level 7 — see `dragonball_irq_levels[]` in
`hw/intc/dragonball_intc.c`) every 30ms from reset:

```
t=0.001s  0x70..0x7c = 0x100913e2 0x10091410 0x1009143e 0x1000a0d6   (ROM factory defaults, PC in the "wait for RAM flag 0x111" loop)
t=0.803s  0x70..0x7c = 0x10076e50 0x10076e50 0x10076e50 0x10076e50   (all four now identical)
```
— and it is **still** exactly `0x10076e50 0x10076e50 0x10076e50
0x10076e50` at every later sample taken this session, including
several seconds into idle, during a held pen-down, and at the frozen
"Setup 1 of 4" screen. Compare `palmv`, sampled the same way at its
own Welcome screen: `0x70..0x7c = 0x10c6faf2 0x10c6fb20 0x10c6fb4e
0x10c09302` — four *distinct* real handler addresses, confirming
palmv's boot ROM does finish installing real per-level ISRs by the
time it reaches the same point in Setup.

**Observation 2 — `0x10076e50` is a 3-instruction "give up" stub, not
a dispatcher**, verified with `--start-address=0x10076e50`:
```
10076e50: oriw   #0x0700,%sr      ; raise CPU IPL (mask interrupts)
10076e54: addqw  #6,%sp           ; discard 6 bytes of the exception frame
10076e56: jmp    0x10076028       ; -- straight to the shared idle loop, no rte
```
(`0x10076028` is the ordinary DragonBall EZ idle primitive — poll
PLLFSR's CLK32 toggle, `stop`, repeat — the same one Session 1 already
identified and the same one `palmv` idles in too.) A breakpoint on
`0x10076e50` itself was **never hit** over a 45+ second observation
window with the machine free-running — this address is only ever
*written into the vector table*, never actually executed as a live
interrupt handler in any run this session. That initially looked like
a contradiction (see next point) until the CPU interrupt-priority
level during the relevant window was accounted for: the probe below
runs at an elevated IPL, so nothing at levels 4-6 is actually
*delivered* to the CPU while it runs, even though the peripheral-level
IPR/ISR bits it polls directly via MMIO are unaffected by IPL (those
are combinational hardware state, not gated by the CPU's mask).

**Observation 3 — who overwrites the vectors, and why it never
restores them.** Traced with breakpoints at three addresses
(`0x10076d00` = function entry, `0x10076d88` = retry-loop top,
`0x10076dc8` = the restore-real-vectors code) over a full 10s
free-running window with `commands`/silent-`continue` on all three so
hits are counted, not just caught once:
* `0x10076d00` (entry): **hit exactly once.**
* `0x10076dc8` (the restore path that would put the real handlers
  back): **hit zero times.**

The function at `0x10076d00` (properly aligned, confirmed with
`--start-address`) does, in order: save the current values of vectors
`0x64/0x68/0x6c/0x70/0x74/0x78/0x7c` (levels 1-7) into a stack buffer;
unconditionally overwrite all seven with `lea %pc@(0x10076e50),%a0` /
`movel %a0,<vector>` (this *is* the write that produces the frozen
state in Observation 1 — it happens right at function entry,
regardless of what happens afterward, which is why the vector table
already shows the frozen value within well under a second of boot);
then loop (`0x10076d88`) doing a small PLL reprogram + a `stop`-based
wait (`jsr 0x10075fe4`, confirmed by dynamic single-step to be a
bounded busy-poll on PLLFSR's CLK32 bit — the CLK32 bit itself was
independently verified via QMP to toggle correctly roughly every
15.3us of real time, so this specific wait is not the culprit) +
`trap #15, #0xa241` (a real Palm OS `SYS_TRAP`, not raw hardware),
retrying while the trap's return value (in `d0`, copied to `d3`) is
non-zero. Only on a zero return does it fall through to
`0x10076dc8` and restore the seven saved vectors.

**Observation 4 — what `trap #15, #0xa241` actually does, and its two
failure exits.** The trap dispatch table was walked by hand (the
dispatcher at vector 47 / address `0xBC`, itself found via `xp
/1xw 0xbc` → `0x1000925c`, is the standard Palm OS selector-indexed
jump: mask the selector to 12 bits, index a table whose base is the
low-memory global at `0x122`); for selector `0xa241` (`0xa241 &
0xfff = 0x241`) this resolves, at runtime, to `0x10076e70`
(`xp /1xw 0x122` → table base `0x7da`; `xp /1xw <0x7da+0x241*4>` →
`0x10076e70`). Disassembled from that (confirmed instruction-aligned)
address, it is a substantial real routine — not a stub — that:
1. Temporarily reconfigures GPIO **port E's SEL register**
   (`0xfffff423`) around a `trap #15,#0xa248` call, restoring it
   afterward — this looks like a "sense a strap pin, then put its
   function back" pattern, distinct from the Port E *data* read
   Cloudpilot's `#if 0`'d hardware-sub-ID code targets.
2. **Failure exit A** (`d0 = -1`): taken if `INTC IPR bit 3 (WDT)` is
   set when checked. Ruled unlikely as the *first* trigger — the
   on-chip watchdog (`hw/rtc/dragonball_rtc.c`) is clocked by the 1Hz
   RTC tick with a 2-tick timeout (`WDT_TIMEOUT_COUNT`), so it cannot
   fire within the first ~2 real seconds, and the vector-table freeze
   is already permanent by 0.8s — though it remains a plausible
   *secondary* reason later retries also fail once/if the loop is
   still spinning past the 2s mark.
3. **Failure exit B** (`d0 = -1`): taken if, at the check point,
   `INTC ISR` reads **exactly** "RTC 1Hz tick pending, nothing else"
   (`ISR-high == 0 && ISR-low == 0x10`, bit 4 = `DRAGONBALL_INTC_RTC`
   per `dragonball_irq_levels[]`). This is the far more likely
   candidate: the on-chip RTC (`hw/rtc/dragonball_rtc.c`) ticks once
   per real second unconditionally from reset, its IPR/ISR bit is a
   pure hardware side effect independent of whether the CPU's IPL
   permits delivery, and **nothing clears it** while the real RTC tick
   handler is unreachable (its vector is the frozen `0x10076e50` stub
   from Observation 1-3, which never runs because of the IPL point in
   Observation 2, and even if it did, it `jmp`s away instead of
   servicing+`rte`-ing). Once the first RTC tick lands after entry
   into this loop, every subsequent retry sees the same
   "RTC-only-pending" state and fails the same way — a permanent,
   self-reinforcing deadlock, not a race: the very code that's
   supposed to eventually make the check pass (whatever real
   interrupt source `0xa241` is actually waiting to see) can never run
   its enabling side effects because levels 4-7 are all parked on the
   give-up stub for the loop's entire lifetime.
4. On success (`d0 = 0`): unmasks IMR bit 1 (Timer), writes `0x0032`
   to a low-memory word, and — notably — the success path contains an
   enumeration loop (`trap #15,#0xa0b9` / `#0xa804`, iterating a count
   read from RAM word `0x144`) that has the shape of a **driver/slot
   enumeration** (one call to "look up entry N", a null check, a
   conditional second call on a found entry) — plausibly the
   Expansion Manager / Slot Driver registration CLIE-RESEARCH.md
   flagged, which would explain why a stuck `0xa241` cascades into
   "nothing after this point in boot ever finishes wiring up", not
   just "the pen specifically is broken".

Putting Observations 1-4 together: **this is not a missing-register
"probe that never returns a value" gap in the no-fault-stub sense
Session 1 and CLIE-RESEARCH.md's risk table anticipated — it is a
timing-order deadlock inherent to the ROM's own critical section.**
Sony's HAL parks *every* peripheral-level interrupt vector on a
give-up stub before running a hardware-confirmation probe that itself
depends on a different interrupt (not yet identified with certainty,
plausibly Memory-Stick- or SPI-completion-related given the
`EmRegsEzPegS300`/`EmRegsMB86189` context and the enumeration loop on
the success path) being delivered and serviced through those exact
same vectors. On real silicon this evidently resolves before the
first incidental RTC tick arrives (or real RTC ticks are excluded from
the "did something happen" check in a way we're not reproducing); in
this emulation, the RTC tick beats whatever `0xa241` is actually
waiting for, and once it does, the deadlock is permanent — matching
every symptom from Session 1 exactly: CPU demonstrably alive (RTC
seconds *hardware register* keeps incrementing — that's the raw device
counter, independent of whether its interrupt is ever serviced), INTC
IPR bit 20 demonstrably toggles on a pen tap (that's the raw `/PENIRQ`
GPIO pin level reaching the INTC, also independent of CPU delivery),
yet nothing ever advances the Setup screen, because the code that
would read the digitizer over SPI in response to that pending pen
interrupt is the very code whose vector was clobbered.

This session additionally confirmed, empirically, that a pen tap *does*
get one interrupt-like reaction: IMR bit 20 (Pen/IRQ5) flips from
unmasked to masked the moment a pen-down is asserted and held (checked
by polling `xp /1xw 0xfffff304` across a QMP `input-send-event`
down-and-hold), i.e. *something* runs and self-masks IRQ5 exactly once
— almost certainly the give-up stub itself doing exactly what
Observation 2 says it does (mask further delivery, abandon to idle)
for the pen source specifically, the first and only time it fires.
`SPI DATA`/`SPI CONT` (`0xfffff800`/`0xfffff802`) were polled at 100ms
intervals for 2s during the same held pen-down and never left `0x0000`
— on `palmv`, the identical test shows `SPICONT` cycling through
non-zero values (`0x22400000`, `0x22c00000`, ...) as the real pen
driver samples the ADC, confirming CLIE genuinely never reaches that
code, not just that it reaches it and reads garbage.

### Why no fix was attempted this session

The blocker is guest-ROM control flow racing against our device
timing, not a missing register value a `palm.c` GPIO tie or a small
new stub device (the pattern used successfully in Session 1 for the
LCD-powered/dock-button/MS-idle quirks and the MSHC stub) can express:
there is no single MMIO address to make "return the right idle value
at" here. The two concrete levers identified are both outside the
"reuse existing DragonBall models, keep other Palms bit-exact" scope
this task was given:
* Make the on-chip RTC's 1Hz tick not visible to `trap #15,#0xa241`'s
  IPR/ISR check during this specific ~1s early-boot window (e.g. by
  not asserting the RTC's INTC line at all until some CLIE-specific
  "boot phase" gate) — plausible but speculative without knowing what
  real silicon actually does differently, and touches
  `hw/rtc/dragonball_rtc.c`, a block explicitly shared with
  `palmv`/`palmvx`/`palmm500`/`palmm515` and every other Palm machine.
* Find and correctly emulate whatever the *real* signal `0xa241` is
  waiting for (most likely tied to the Memory Stick host controller
  or SPI-completion path given where this sits relative to
  `EmRegsMB86189`/`EmSonyXzWithSlot`) so it arrives before the RTC
  does. This needs the exact selector semantics from a source we don't
  have fetched yet — Cloudpilot's `EmHAL`/interrupt-dispatch layer
  (not just the per-board `EmRegsEzPegS300.cpp` already fetched) would
  be the next thing to pull, specifically to find what real hardware
  event corresponds to Palm OS selector `0xa241`.

Both are real code changes to shared, bit-exact-constrained device
models, not "add a stub" — attempting either blind, without firmer
evidence of which one (or something else entirely) matches real
silicon, risked exactly the kind of speculative, unverified change the
task asked to avoid. Documenting the precise mechanism here is judged
more valuable than a guessed fix that might only mask the symptom.

### Result vs CLIE-RESEARCH.md success tiers (updated)

* (best) launcher, screenshotted — **still not reached.**
* (good) reaches the boot/digitizer screen — unchanged from Session 1,
  still holds.
* This session's addition: the open question Session 1 flagged ("why
  doesn't input advance the Welcome screen") is now root-caused to a
  specific ROM address (`0x10076d00`'s never-restored vector
  substitution) and a specific mechanism (a `trap #15,#0xa241`
  hardware-confirmation probe that deadlocks against the on-chip RTC's
  1Hz tick once the vectors it depends on are already clobbered),
  rather than "one of several possible un-stubbed probes, USB
  suspected." USB and digitizer-scaling are both eliminated as causes.

Evidence copied to `clie-poc-evidence/` (gitignored) this session:
`SESSION2-clie-welcome-before-tap.png`, `SESSION2-clie-welcome-after-tap.png`
(identical — still stuck at "Setup 1 of 4"), `SESSION2-palmv-control-setup2.png`
(control: palmv still advances normally on the same tap mechanism,
confirming the harness and the palmv machine are both still
bit-exact/working). Full m68k disassembly dumps and gdb transcripts
from this session were kept under `/tmp` (tmpfs) and are not preserved
past this session per the disk-budget constraint; the exact addresses
and instruction sequences needed to reproduce every finding above are
quoted in full in this section.

### Next session should try

1. Fetch Cloudpilot's `EmHAL.cpp`/interrupt-dispatch and
   `EmRegsMB86189.cpp`'s IRQ-generation path specifically to identify
   the real hardware event Palm OS selector `0xa241` corresponds to,
   then check whether it's something a targeted `palm.c` GPIO tie
   *could* express after all (e.g. if it turns out to be "MS
   host-controller interrupt line asserts once at some fixed point
   during probe" rather than something timing-dependent).
2. Alternatively, try the experiment this session ran out of time for:
   temporarily suppress the RTC's INTC assertion for `clie-s300` only
   (a `palm.c`-scoped machine-specific choice, not a shared-model
   edit, if it can be expressed as "don't wire up
   `DRAGONBALL_INTC_RTC` for this one machine's RTC instance") for the
   first ~2 real seconds of boot, and see whether the `0xa241` probe
   then resolves on its own once the RTC tick stops racing it — this
   would confirm or kill Observation 4's hypothesis empirically before
   committing to a real fix.
3. If both of the above stall, a scripted (not manual, given how much
   wall-clock time manual gdb driving cost this session — see Tooling
   notes) instruction-trace of the ~1 second between function entry
   (`0x10076d00`) and the first `trap #15,#0xa241` return would settle
   definitively which interrupt source(s) are pending at the moment of
   each retry, rather than the two candidates inferred from static
   reading of the failure branches here.

---

## Session 3: the Session-2 root cause was WRONG — the real blocker is guest event-manager software, not hardware

Same worktree (`/workspace/src/qemu-clieb`, branch `fin-clie`), reused
build. All large traces went to `/tmp` (tmpfs); `/workspace` stayed
~0.7-1.4 GB free throughout, no ENSOSPC. No source changes are kept
this session — every code edit made was temporary instrumentation
(`fprintf`s) or a diagnostic hack, and all were reverted; the tree is
clean (`git diff` empty over `hw/`). `palmv`/`palmvx`/`palmm515`
re-verified bit-exact: all three still advance Setup 1 -> Setup 2 on a
single centre tap on the rebuilt binary; `clie-s300` still reaches the
Welcome screen. **Answer up front: still does not reach the launcher,
and it will not be reachable by any hardware-register change — see
below.** Screenshot of the stuck state:
`clie-poc-evidence/SESSION3-clie-setup1-stuck.png` (control, palmv
advanced: `SESSION3-palmv-advanced-setup2.png`).

### Correction: Session 2's "0xa241 vector-park / RTC-tick deadlock" is not what happens

Session 2 concluded the blocker was routine `0x10076d00` parking the
interrupt vectors on a give-up stub and a `trap #15,#0xa241` probe
deadlocking against the RTC tick. **This session disproves that
directly.** A full `-d exec,nochain` execution trace of the entire
boot+idle (2.4M TB records, kept in `/tmp`) shows that **none of
`0x10076d00`, `0x10076dc8`, `0x10076e50`, `0x10076e70`, `0x10076ef2`,
`0x10076f30`, `0x10076fa8` ever execute** — zero hits each, across boot
and steady-state idle. The `0xa241` probe and its vector-park routine
simply are not on the live code path at all; Session 2 built its whole
model from static disassembly of a routine that never runs, plus a
misread of `0x10076e50` (which is a normal "wake-from-`stop` and return
to caller" trampoline — `oriw #0700,sr; addqw #6,sp; jmp <rts>` — not a
"give up" stub; and it is never even reached because the pen interrupt
is delivered through the INTC's IVR-based vector, not that autovector
slot). The vectors reading `0x10076e50` via QMP `xp` were real but
inert — never executed. Lesson recorded for the next agent: **verify a
suspected hot routine actually executes (one `-d exec -dfilter`
capture) before reasoning from its disassembly** — this would have
saved Session 2's entire (wrong) conclusion.

The machine is not deadlocked anywhere. Its steady state is a perfectly
normal Palm OS event loop: the dominant idle trap selectors (captured
with a bounded gdb breakpoint at the trap dispatcher `0x1000925c`,
mapped to names via the Palm OS 3.5 SDK `CoreTraps.h`) are
`sysTrapSysDoze`, `sysTrapHwrDoze`, `sysTrapSysHandleEvent`,
`sysTrapFrmDispatchEvent`, `sysTrapTimGetTicks`,
`sysTrapPenRawToScreen`, `sysTrapWinDisplayToWindowPt`,
`sysTrapFrmGetActiveFormID` — i.e. a live form app dozing between
ticks and dispatching events. Not a hang.

### What actually happens (traced end-to-end, hardware proven correct)

The pen/digitizer emulation is **provably correct and complete** — the
guest receives byte-identical hardware state to `palmv`:

1. **ADC sampling works.** Temporary `fprintf` in `ads7843_sample`:
   during a pen-down at abs (16384,16384) the guest reads channel 5
   (X) = `0x08f0`, channel 1 (Y) = `0x0ad0`, channel 2 (batt) =
   `0x0bd0` — *exactly the same raw values `palmv` reads for the same
   tap* (confirmed by running the identical instrumented build on
   `-M palmv`). 223 pen-down samples across three taps; sampling is
   continuous while the pen is held.
2. **The low-level digitizer point is stored correctly.** The digitizer
   globals live at `mem[0x170]` (= `0x3ad38`); the current raw pen
   point is the long at `+24` and the pen-down flag the byte at `+28`.
   Polling these via QMP `xp` across a held pen:
   - idle: point `0x00000000`, flag `0x00`
   - held: point `0x008f00ad` (rawX=0x8f=143, rawY=0xad=173), flag `0x01`
   - after release: point retained `0x008f00ad`, flag `0x00`
   The store is done by the routine at `0x10022b20`
   (`movew %a4@,%a3@(24); movew %a4@(2),%a3@(26); moveb #1,%a3@(28)`),
   caught with a gdb hardware watchpoint on `0x3ad50`. So the raw point
   (143,173) with pen-down is present and stable throughout the tap,
   identical to what `palmv`'s driver produces.
3. **That raw point maps on-screen.** `PenRawToScreen` (trap 0x272,
   handler `0x10079b94`) reads the calibration struct at `mem[0x16c]`
   (flag=1, offX=20, offY=18, scaleX=185, scaleY=252) and computes
   `screenX = ((256-143-20)*185+128)>>8 = 67`,
   `screenY = ((256-173-18)*252+128)>>8 = 64` — a valid on-screen
   point near centre, well inside the [0,159]x[0,233] clamp. So even
   the coordinate conversion is fine; the tap is *not* off-screen.

So: correct ADC values -> correct raw point stored with pen-down flag
-> correct on-screen coordinate available. The hardware side is done.

### The actual gap: the form never consumes the correctly-delivered pen point

Despite all of the above, the Setup-1 form does not advance, and
`PenRawToScreen`, when the *form's* pen-event path
(`EvtGetPen`-family, caller `0x100226a0`, which reads the digitizer
point via `mem[0x170]@(24)` and calls `PenRawToScreen` at `0x1002284e`)
is sampled during a tap, is fed **(0,0)** — not the (143,173) that is
sitting in the digitizer struct at that very moment. The reconciliation
(verified three ways) is that **the form is not in the continuous
pen-polling state that `palmv`'s Setup-1 form is in**:

* Instrumenting `dragonball_intc` IMR writes shows `palmv`'s Setup-1
  toggles the pen-interrupt mask (IMR bit 20) **1666 times** over a
  ~2 s window — its event loop is actively pen-sampling every tick,
  which is what generates `penDown`/`penUp` *events* from the digitizer
  point and delivers them to the form. `clie-s300` toggles IMR bit 20
  only **4 times total**, all clustered around the single delivered
  tap, then leaves the pen masked and dozes. CLIE's event loop is *not*
  running the per-tick pen sampler that turns the stored point into
  form events.
* Consequently the form's `EvtGetPen` either is not called during the
  hold, or is called and its pen queue is empty, so it falls through to
  the "report current position" branch at `0x10022820` at a moment when
  the transient it reads is 0 — the form receives an origin/empty pen
  event and takes no action.
* A diagnostic hack forcing IMR bit 20 permanently unmasked (so every
  tap's IRQ5 is delivered) was built and tested: **it does not help** —
  taps 2..N are then delivered to the low-level ISR but the form still
  does not advance, and no tap position (full-range 13x13 abs sweep
  with every tap delivered) produces any screen change beyond the
  one-time stylus-illustration settle. So the blocker is not interrupt
  masking and not coordinate calibration; it is that the event manager
  is not generating form-level pen events from the (correct) pen state.

### Everything ruled out this session (with evidence)

* **0xa241 / vector-park deadlock (Session 2's theory):** never
  executes — disproven by full exec trace.
* **Memory Stick controller poll:** the MS stub
  (`hw/misc/sony_mshc_stub.c`) is read exactly **twice**, both at boot
  (offsets 0x08/0x0a), never during idle or on taps — not a spin
  target. (Temporary `fprintf` in `sony_mshc_stub_read`.)
* **USB controller:** nothing touches `0x10400000` on the live path.
* **RTC tick:** not involved (the `0xa241` loop it supposedly raced
  never runs).
* **Digitizer coordinate calibration / off-screen taps:** the raw point
  maps to on-screen (67,64); a full-range abs sweep with taps forced
  deliverable advances nothing.
* **Pen delivery / masking:** the pen point is delivered and stored
  correctly; forcing the pen permanently unmasked does not help.
* **Hardware buttons / jog dial:** sending every host key
  (up/down/f1-f10/ret/spc) on the Welcome screen advances nothing
  either — so it is not "CLIE wants the jog-press instead of a tap".

### Conclusion and why this is likely NOT fixable by a hardware-register change

The QEMU hardware emulation delivers the CLIE's pen state **byte-for-byte
identically to the `palmv` machine that advances on the same input**
(same ADC channels, same raw values, same stored digitizer point, same
on-screen mapped coordinate). The divergence is entirely in the guest:
`palmv`'s ROM runs its Setup-1 form in an active per-tick pen-polling
loop that manufactures form pen-events; the S300 ROM's Setup-1 form
does not, so a correctly-delivered pen point is never turned into the
`penDown`/`penUp` the form would advance on. This is a guest
event-manager / Setup-app *software-state* difference, not a missing or
wrong hardware value — which means the premise of both remaining
angles ("stub one more Sony register" / "provide the hardware value
clie-s300 gets wrong") does not apply to the pen path: there is no
hardware value left to change; the pen path is already correct.

The open question is therefore narrower and different from what the
prior sessions framed: **why does the S300 ROM's early UI (Setup
wizard) not enter the per-tick pen-sampling mode that every stock Palm
ROM's Setup wizard uses?** Candidates worth pursuing next, all
guest-behaviour-driven rather than register-stub-driven:

1. A power-management / "device is idling, use interrupt-driven pen not
   polled pen" decision the Sony HAL makes differently — possibly keyed
   off a Sony power/dock/hold GPIO. Worth capturing the *non-pen* trap
   selectors and MMIO reads that differ between `palmv`-Setup-1 and
   `clie-s300`-Setup-1 in the first second after the form opens (the
   `SysHandleEvent`/`EvtGetEvent` internals decide polled-vs-interrupt
   pen), then checking whether any single GPIO level flips CLIE into
   the polled mode.
2. The Sony "Jog/Silk" library (`JogAssist`/`Sony Silk Library`, present
   in the ROM) may install an event-source filter that intercepts pen
   events before the form on the S300; its init may be waiting on a
   Sony peripheral we present as absent. This is the most likely real
   culprit and the right next trace target — break where the form's
   `EvtGetEvent` returns and see whether a `penDown` event is ever
   enqueued at all, and if not, which task/library was supposed to
   enqueue it.
3. If (1)/(2) show the S300 genuinely needs a Sony library/hardware we
   cannot cheaply provide, this may be a case where Tier-A "boot to
   launcher" is blocked by Sony UI middleware rather than the DragonBall
   hardware — a scoping finding worth escalating, since it changes the
   Phase-1 effort estimate.

### Tooling notes (additions)

* `-d exec,nochain -dfilter <lo>..<hi>` writing to `/tmp` is the
  reliable way to answer "does this address ever execute" and "what is
  the hot loop" — far more dependable here than gdb. Grep the physaddr
  field (`/00000000xxxxxxxx/`) and `sort|uniq -c`.
* Bounded gdb batch (`break <addr>` then a fixed number of
  `continue`/`printf` lines, `timeout 40`) works for capturing register
  values at a high-frequency breakpoint (e.g. the trap dispatcher);
  the failures in earlier sessions were from `commands`+auto-`continue`
  blocks and `continue &`, both of which wedge. Avoid those.
* Palm OS trap selector -> name: fetch `CoreTraps.h` from the Palm OS
  3.5 SDK (`github.com/jichu4n/palm-os-sdk`, `sdk-3.5/include/Core/`)
  and index the big `enum` from `sysTrapBase`; `d1 & 0xfff` at the
  dispatcher is the trap number.
* Distinct high gdb ports per machine are mandatory — sibling agents in
  this sandbox bind 1234/1235/12345/4321 for unrelated m68k Macs.

---

## Session 4: SOLVED — reaches the launcher. Root cause was auto-sleep, not the pen.

**Result: `-M clie-s300` now boots Palm OS 3.5 all the way to the
Applications launcher.** Money shot:
`clie-poc-evidence/SESSION4-clie-LAUNCHER.png` (the launcher: clock
running, "System" category, icons Graffiti / HotSync / Prefs /
Security / Welcome). Same worktree/branch (`fin-clie`), reused build.
Fix is 33 lines in `hw/m68k/palm.c`, scoped to the CLIE via
`ms_stub_base`; `palmv`/`palmvx`/`palmm500`/`palmm515` re-verified —
all still boot and advance Setup 1->2 on a tap, bit-exact.

### The actual root cause (which every prior session missed)

**A second or two after boot the S300's Sony HAL calls `HwrSleep` and
powers the device down.** `HwrSleep` (entry `0x10076bda`, tagged
`GHwrSleep` in the ROM) orchestrates `HwrTimerSleep`,
`HwrDisplaySleep`, `KeySleep`, `TimSleep` and — the killer —
`PenSleep`, which masks the pen interrupt (INTC IRQ5) and reconfigures
the /PENIRQ pin (port F bit 1) to an output. After that the digitizer
is dead and the CPU is `stop`-ed with a buttons-only wake mask, so
**no screen tap can wake it**. Our on-chip LCDC keeps scanning the
last framebuffer, so the machine merely *looks* frozen on "Setup 1 of
4" — it is actually asleep.

This retroactively explains **all** of Sessions 1-3: every "tap
doesn't advance", "pen delivered but form won't act", "event manager
dozes not polls", "pen queue empty -> PenRawToScreen gets (0,0)"
finding was measured on a **sleeping** device with the digitizer
switched off. The elaborate Session-3 "the form software doesn't
consume the pen" conclusion was a symptom, not the cause — the cause
is that the pen hardware had been powered down by `PenSleep`. (This is
also foreshadowed in `PALM-NOTES.md`: "if /POWERFAIL reads low, PalmOS
decides the battery is dead and goes to sleep a few seconds after
boot… the mysterious 'sleeps at 5.4s' symptom." The S300 hits the same
class of bug via its own HAL path.)

### How it was found

`HwrSleep`'s `PenSleep` masks INTC IMR bit 20. A one-line `fprintf` in
`dragonball_intc`'s IMR-write path, logging the guest PC whenever bit
20 transitions to masked, pointed at `0x100794bc`; disassembling there
showed the `IMR |= pen; portF-bit1 -> output` "disable pen" sequence,
reached via `trap #15,#0xa275` = `sysTrapPenSleep` (trap numbers named
from the Palm OS 3.5 SDK `CoreTraps.h`). Walking up: the caller at
`0x10076cd2` is inside `HwrSleep` (`0x10076bda`), and `HwrSleep`'s own
caller is `0x100781be`, a power-manager routine that reads the battery
ADC and enters sleep.

### The fix

Neutralise `HwrSleep` for the CLIE by patching its entry to an early
`RTS` in the loaded ROM image (guarded on the expected `linkw` opcode
`0x4e56`, scoped to `ms_stub_base`). The unit then never powers the
digitizer down and stays awake with a live pen; Setup can be tapped
through. Confirmed with gdb that `HwrSleep` *is* still called ~2 s
after boot (return address `0x100781c4`) and now immediately returns.

Notes on why this specific lever:
* Making the battery ADC read healthy (`battery-value=0xfff`) does stop
  the *battery*-triggered sleep (PEN-MASK count drops to 0) **but Setup
  still does not advance** — the unit reaches an equivalent doze and the
  pen stays unused. Only neutralising `HwrSleep` itself both keeps the
  digitizer powered *and* leaves the device in the interactive state
  that advances the Setup form. So the fix is `HwrSleep`, not the
  battery value (verified: the RTS patch reaches the launcher with the
  battery at either 0xbd0 or 0xfff).
* It is a ROM-image patch rather than a device-model change because the
  offending action is a guest HAL decision with no single MMIO register
  to intercept cleanly; the patch is small, opcode-guarded, and
  CLIE-only. A future cleaner route (deferred) would be to find and
  satisfy whatever power/auto-off condition the `0x100781be` routine
  checks so the HAL never chooses to sleep in the first place.

### Driving the rest of Setup to the launcher (test harness, not a code change)

Once awake, the wizard is a normal Palm OS Setup flow and completes with
scripted taps (QMP `input-send-event`, coords from `PALM-NOTES.md`):
1. Welcome "Setup 1 of 4" — tap anywhere -> Setup 2.
2. Digitizer calibration — tap the three targets at screen (10,10),
   (150,150), then the confirm target at **(79,57)** (the confirm must
   land within a few px, per `PALM-NOTES.md`; imprecise taps make it
   loop). Our existing `ads7843` inverted-panel model produces the
   monotonic raw pairs `PenCalibrate` accepts, so calibration passes.
3. Setup 3 (country/time/date) and Setup 4 — tap the on-screen **Next**
   button (bottom bar) rather than the centre (centre opens the
   time/date pickers).
4. Graffiti / Basic-Skills tutorial — tap **Done**/**Next** through.
5. Press the **Applications** silk button (host `f7`) -> the launcher.

All four base Palm machines re-verified bit-exact after the change
(they never touch the `ms_stub_base` path). Evidence:
`clie-poc-evidence/SESSION4-clie-LAUNCHER.png`.

---

## Session 5: `clie-sj33` -- the real target device (Sony CLIE PEG-SJ33)

New machine, new worktree state (same `fin-clie` branch, same
`/workspace/src/qemu-clieb` tree, `clie-s300` untouched and re-verified
bit-exact below). Goal per this task: the user's actual device, the
**PEG-SJ33** -- MC68VZ328 @ 33MHz, 16MB RAM, 320x320 **16-bit color**
via a companion display controller (not the on-chip DragonBall LCDC).
`/workspace` stayed at 1.2-1.6GB free throughout; the only new files
kept on disk are the two new source files and one new ROM
(`/workspace/src/palm-roms/Sony-CLIE-PEG-T600C-en.rom`, 3,538,944B);
all archive downloads and Cloudpilot-emu source fetches went to `/tmp`
(tmpfs) and were deleted after extracting what was needed.

### ROM sourcing: no SJ33 firmware dump exists in the obvious places -- substituted PEG-T600C

Checked, in order: `/workspace/src/palm-roms/` (no SJ-series ROM
present, only the S300 from Session 1-4); the `jp.sony.clie`
archive.org item's file list (fetched `_files.xml` directly) -- **no
SJ/SL-series ROM**, only N600C/N700C(en+jp)/N710C/S300/S320/S360/
T400/T400C/T600C; the PalmDB "complete" mirror item
`20250707_20250707_0134` used for this tree's other Palm ROMs -- same
absence, no `SJ*` file. Broadened to a general archive.org search
(`advancedsearch.php?q=SJ33+OR+SJ30+CLIE`): the only device-adjacent
hits are a **609MB "PEG-SJ33 Installation CD"** ISO
(`sonyclie-peg-sj-33-instalation-cd`, HotSync desktop software, not a
device ROM) and a **118MB "PEG-SL10/SJ20/SJ30" CD**
(`peg-sl10-sj20-sj30`, same category) -- both are driver/HotSync
media, the standard shape for those releases, not POSE-style firmware
dumps; downloading either would spend most of the disk budget on
software this task doesn't need. **Conclusion: no SJ33 (or SJ-series)
ROM is obtainable from any source checked this session.**

Per this task's explicit fallback instructions, substituted the
closest VZ328 color CLIE ROM available: **PEG-T600C**
(`jp.sony.clie` archive item, `ROM/Sony CLIE PEG-T600C ROM
(English).zip`, 1,632,315B, md5 `2a78d3b44070d5acd067041c7c3d66d4` --
matches the value already recorded in `CLIE-RESEARCH.md` sec 7.1
exactly). This is **not an arbitrary stand-in**:

* `strings` on the extracted `palm-os-41-T600c.rom` (3,538,944B) shows
  `MQ120 LCD Controller`, `PrvMQ120InitLUT`, and -- decisively -- the
  board-identity string **`sonymdna`** ("Modena"), *not* `sonyvnce`
  ("Venice", the string `CLIE-RESEARCH.md` sec 7.2 already verified
  for the *T400* ROM). This means the T-series is **not one uniform
  board family**: the mono T400/T415/T425 are Venice, but the color
  T600C/T625C/T650C are **Modena** -- a fact `CLIE-RESEARCH.md`'s
  taxonomy table didn't have (it listed all T-series as `sonyvnce`
  ≈-tagged, unverified for the C models specifically).
* Fetched Cloudpilot-emu's `EmDevice.cpp` this session
  (`raw.githubusercontent.com/cloudpilot-emu/cloudpilot-emu/master/...`):
  `case kDevicePEGT600:` instantiates `EmRegsVzPegModena`, confirming
  the ROM string. `CLIE-RESEARCH.md` sec 2.1 already (independently,
  before any ROM evidence) guessed `VZPegYellowStone`/`Modena` as the
  SJ-series' codename family. **T600C being confirmed Modena-based
  makes it the closest real, obtainable analogue to the SJ33 in this
  tree's entire reachable ROM universe** -- same SoC (VZ328), same
  board family (Modena) as the SJ33's own best-guess codename, same
  display class (320x320 16-bit color), and (from `EmDevice.cpp`) the
  *only* N/T-series color CLIE wired up with Sony FM sound
  (`EmRegsFMSound(FMSound_BaseAddress)`), matching
  `CLIE-RESEARCH.md`'s SJ33 row (`EmRegsFMSound` ≈) exactly. This is
  as close as this session could get to "the real device's own board
  family" without a real SJ33 dump.
* ROM header, hand-verified: file+0 SP `0x00000316` PC `0x100002a2`
  (small ROM, `FEEDBEEF` present), file+0x8000 SP `0x00004a8a`... wait,
  precisely: SP `0x00001126`-shaped whole-flash layout, big-ROM card
  header at file+0x8000 with SP `0x0000488a` PC `0x1000c796` -- **the
  same whole-flash shape as `clie-s300`/T400**
  (`rom_load_offset=0, bigrom_offset=0x8000`), not the m500/m515
  small-ROM-at-file-offset-0x10000 shape.

Kept only the extracted `.rom` in `/workspace/src/palm-roms/` (as
`Sony-CLIE-PEG-T600C-en.rom`, md5 `cc8fe4ea378b18b8c323a66f7e525591`
for this extracted file itself); the zip and the extraction directory
were deleted from `/tmp` immediately after.

### The new hardware piece: a "MediaQ 1100/1132"-class companion color LCD controller

`CLIE-RESEARCH.md` sec 4.2 correctly predicted the color-HiRes CLIEs
need "a dedicated Sony LCD controller" separate from the on-chip VZ328
LCDC, citing `EmRegsLCDCtrlT2.cpp` as the reference -- but fetching
that file this session (`raw.githubusercontent.com/...`) and
cross-referencing against `EmDevice.cpp` shows **`EmRegsLCDCtrlT2` is
the *wrong* chip class for N600C/T600/N700C**: `EmDevice.cpp` actually
instantiates `EmRegsMediaQ11xx` for all three (`EmRegsLCDCtrlT2`/
`EmRegsMQLCDControlT2` is used only for `kDeviceYSX1100`, the SZ328/
Redwood NR70V-class Tier-C machines). Fetched `EmRegsMediaQ11xx.cpp`/
`.h` instead (`hardware/EmRegsMediaQ11xx.*`, *not* under
`hardware/clie/` -- a shared MediaQ driver, same non-obvious location
pattern `CLIE-POC-NOTES.md` Session 1 already flagged for
`EmRegsMB86189`) plus the authoritative register-offset table,
`EmPalmStructs.i`'s `FOR_HwrMediaQ11xxType_FIELDS` macro (this is the
*actual* source of truth for the struct layout -- `EmRegsMediaQ11xx.h`
itself only has a forward-declared proxy type).

**Register map** (offsets from the register-window base;
`FOR_HwrMediaQ11xxType_FIELDS`, all 32-bit fields, total window
`0x2000`): `ccREG@0x000` (CPU control; `ccREG[1]`="CC01"/
GraphicEngineStatus), `mmREG@0x080` (MIU), `inREG@0x100` (interrupt),
**`gcREG@0x180`** (Graphic Control -- `GC_CONTROL=+0x00`,
`GC_HWINDOW=+0x20`(=0x1a0), `GC_VWINDOW=+0x24`(=0x1a4),
`GC_START_ADDR=+0x30`(=0x1b0), `GC_STRIDE=+0x38`(=0x1b8)), `geREG@0x200`
(graphics accelerator/BitBLT engine, not modelled -- see below),
`dcREG@0x380` (device config), `fpREG@0x600` (flat panel timing),
**`cpREG@0x800`** (256-entry color palette, 4 bytes/entry), `sfREG@0xc00`
(source FIFO), `udREG@0x1000` (USB device). Two fixed base addresses,
identical across N600C/T600/N700C in `EmDevice.cpp`
(`new EmRegsMediaQ11xx(*framebuffer, MMIO_BASE, T_BASE)`):
**`T_BASE=0x1F000000`** (256KB video aperture) and
**`MMIO_BASE=T_BASE+0x40000=0x1F040000`** (the register window).
`GC_CONTROL` bit0 = enable, bits[6:4] = bpp selector (0..4 ->
1/2/4/8/16bpp, `1<<n`), bits14/15 = X/Y pixel doubling. Palette format
is 6:6:6, one 4-byte entry per index: byte0 unused, byte1=blue<<2,
byte2=green<<2, byte3=red<<2 (`RED_MASK`/`GREEN_MASK`/`BLUE_MASK` in
`EmRegsMediaQ11xx.cpp`). `CC01`/GraphicEngineStatus is special: real
Cloudpilot code (`CC01Read`) forces "FIFO empty, engine not busy" on
*every* read regardless of stored state, since it never actually runs
an accelerator queue -- replicated identically in our model, or a HAL
"wait for idle" poll before touching other registers never terminates.

New device: **`hw/display/clie_lcd.c`** + **`include/hw/display/clie_lcd.h`**
(`TYPE_CLIE_LCD`, Kconfig symbol `CLIE_LCD`, wired into
`hw/m68k/Kconfig`'s `CONFIG_PALM` alongside `SED1376`). Structurally a
sibling of `hw/display/sed1376.c` (the m515's color controller model
already in this tree): a plain register-byte-array MMIO device plus a
separate RAM `MemoryRegion` for video memory, a `QemuConsole` with a
`GraphicHwOps.gfx_update` callback that recomputes the whole frame from
scratch every call (no dirty-rectangle tracking, matching sed1376's own
approach). Scope matches sed1376's: only the *scanout* path is modelled
(enable/bpp/geometry/start/stride/palette) -- the graphics-accelerator
BitBLT engine (`geREG`, offsets 0x200-0x27f) is plain read/write storage
with no drawing side effect, same "accept and drop" pattern as this
tree's `sony_mshc_stub.c` uses for the Memory Stick TPC command
register, and the same scope note CLIE-RESEARCH.md sec 4.2 anticipated.
`PalmMachineClass` gained one field, `clie_lcd_base` (0 = none,
mutually exclusive with `sed1376_base`), and `palm_init()` gained an
`else if (pmc->clie_lcd_base)` branch mapping the device's two sysbus
MMIO outputs at `clie_lcd_base` (video) and
`clie_lcd_base + CLIE_LCD_REGS_OFFSET` (registers), mirroring the
existing `sed1376_base` branch's shape exactly.

### Machine wiring (`hw/m68k/palm.c`)

`clie-sj33` is built on `palmm500_machine_class_init()` (the shared
VZ328 base: chip ID 0x56, `has_timer2=true`, ADC dock-sense idles high,
keyboard rows on port K bits 5-7 -- and the Modena board file's own
`hwrVZModenaPortKKbdRow0/1/2` constants, from `EmRegsVZPegModena.cpp`,
confirm K5-7 is *also* Modena's real wiring, so no override needed --
first hardware fact this session that needed zero correction), then
overridden: `rom_size=4MiB`, `rom_load_offset=0`, `bigrom_offset=0x8000`
(T600C's whole-flash layout, verified above -- overrides the
m500/m515 small-ROM-at-0x10000 shape `palmm500_machine_class_init()`
sets), `default_ram_size=16MiB` (per `CLIE-RESEARCH.md`'s SJ33 row),
`sed1376_base=0` / `clie_lcd_base=0x1f000000` (swaps the m515's color
controller for ours), `ms_stub_base=0x10800000` (Modena/T600's MB86189
base per `EmDevice.cpp`'s `kDevicePEGT600` case -- *different* from
S300's 0x10200000).

**GPIO ties, corrected per-board rather than copied from S300.**
Session 1-4's three `PALM_CLIE_*_GPIO` ties were derived from
`EmRegsEzPegS300` (an **EZ**-family board) layered on the shared
`EmSonyXzWithSlot<>` base HAL. Fetching `EmRegsVZPegModena.cpp` this
session shows the **VZ**-family Modena board does *not* need the same
set:
* `hwrVZNascaPortFLCDPowered`-style "LCD powered" forcing (S300's
  `PALM_CLIE_LCDPWR_GPIO`) is present in Modena's source only as a
  **commented-out** line (`// result |= hwrVZNascaPortFLCDPowered;`)
  -- i.e. Cloudpilot itself doesn't drive it for Modena, so `clie-sj33`
  leaves it untied.
* Modena's own `GetPortInternalValue` forces exactly two things: port D
  dock-button (bit 0x10, same bit S300 uses -- kept) and PowerFail
  (bit 0x80, already generic-tied for every Palm machine via
  `PALM_POWERFAIL_GPIO`). The MS-idle bit (port D bit 0x40) is
  inherited from the *shared* `EmSonyXzWithSlot<>` base class both
  S300 and Modena derive from (`using VZ = EmSonyXzWithSlot<...>` at
  the top of `EmRegsVZPegModena.cpp`), so it's still expected to apply
  -- kept tied.

Rather than keep three unconditional ties gated only on `ms_stub_base`
(as Session 1-4 left it, correct only for the one board it was traced
against), `PalmMachineClass` gained three explicit bools
(`clie_gpio_lcdpwr`/`_dockbtn`/`_msidle`) so each machine states which
of the shared `PALM_CLIE_*_GPIO` ties its own traced HAL actually
needs; `clie-s300` sets all three true (unchanged behaviour, verified
bit-exact below), `clie-sj33` sets `lcdpwr=false, dockbtn=true,
msidle=true`.

**HwrSleep patch generalised, not reused as-is.** The literal ROM
address `0x10076bda` Session 4 hardcoded is S300-image-specific and
does not apply to the T600C image (different ROM, different HAL).
`PalmMachineClass` gained `hwrsleep_patch_addr` (0 = no patch); the
`palm_init()` patch code now reads this field instead of the constant,
`clie-s300` sets it to the same `0x10076bda` it always used (bit-exact,
verified below), and **`clie-sj33` currently leaves it at 0 (no
patch)** -- see "What was *not* solved" below for why: this session's
SJ33 boot never reached the point where an auto-sleep-style hang would
even be the next blocker, so there was nothing to trace an address
against yet.

### Build

Incremental `ninja -C build qemu-system-m68k` throughout (existing
configured `build/` from Sessions 1-4 reused). Several rebuild/retest
cycles this session (register-mapping and one BQL-deadlock fix, see
below); every one was a 2-4-file relink, no reconfigure. `-M help`
lists `clie-sj33  Sony CLIE PEG-SJ33 (MC68VZ328, 320x320 color; ROM:
PEG-T600C substitute, see CLIE-POC-NOTES.md)` alongside the existing
seven Palm machines and `clie-s300`.

### A real QEMU bug found and fixed: `gfx_update` returning `false` permanently wedges `screendump`/VNC/Spice

The single biggest time sink this session, and the one fix here that
matters beyond this one device: **the first working build of
`clie_lcd.c` made every `-qmp screendump` (and, by the same code path,
every VNC/Spice client refresh) on `clie-sj33` hang forever**, with
*all* QEMU threads sitting idle (confirmed with `gdb -p <pid> -batch
-ex "thread apply all bt"` -- main loop, iothread and the CPU thread
all parked in `ppoll`/`qemu_cond_wait`, nothing spinning, no deadlock
on a *held* lock). Root-caused by reading `ui/console.c`:

```
void qemu_console_hw_update(QemuConsole *con)
{
    if (!con->hw_ops->gfx_update || con->hw_ops->gfx_update(con->hw)) {
        qemu_console_hw_update_done(con);   /* wakes co_wait_update() waiters */
    }
}
```

`qemu_console_hw_update_done()` -- the only thing that wakes a coroutine
parked in `qemu_console_co_wait_update()`, which is exactly what
`qmp_screendump()` (`ui/ui-qmp-cmds.c`) and the VNC/Spice periodic
refresh use -- is skipped **whenever `gfx_update()` returns `false`**.
My first `clie_lcd_update_display()` returned `false` in the ordinary
"controller not yet enabled" case (`GC_ENABLE` clear), which is the
**default reset state and the state through most of early boot** --
i.e. any screendump taken before the guest finishes programming the
controller wedges the QMP monitor **permanently**, indistinguishable
from the outside (and from `gdb attach`) from a hung CPU. `sed1376.c`
and `dragonball_lcdc.c` have the exact same-shaped early "return false"
branches, but happen not to trip this in practice because their
"invalid" conditions (bad bpp selector, `!surfacebpp`) are not the
*reset-default* state the way `GC_ENABLE` clear is for a brand-new
MediaQ-style device -- a real latent bug in this tree, just not
practically reachable via those two existing devices. Fix (kept,
`hw/display/clie_lcd.c`): `gfx_update` now **always returns `true`**;
the former "nothing sane to draw" cases fall through to a `blank:`
label that ensures the console has *a* 320x320 surface (creating one
via `qemu_console_resize` if the console has never been sized yet) and
still calls `qemu_console_update_full()` before returning. This is a
correctness fix independent of anything CLIE-specific and is the
reason `screendump` on `clie-sj33` went from "hangs the whole QMP
monitor forever" to "returns instantly, every time, from the first
call at reset onward" -- verified with `clie-s300` too (screendump at
`-S`, reset, before a single instruction runs: works, `0.0097s`,
confirming the *grey* LCDC doesn't hit this bug in practice, only
demonstrating the fix doesn't regress it).

**Tooling note for next time:** don't trust "all gdb threads idle" as
proof nothing is wrong -- a coroutine parked forever on a queue that
nothing will ever signal looks *identical*, from every thread's
backtrace, to a correctly-idling VM. The tell here was comparing
against a **known-good control** (`clie-s300`'s screendump, tested the
same way, same script, immediately) succeeding in under 10ms while
`clie-sj33`'s hung for 90+ real seconds on an otherwise-idle process --
i.e. treat "this exact same operation works instantly on a sibling
machine but never returns here" as a stronger signal than "no thread
is visibly spinning".

### Register-mapping quirk found (and only partially resolved): a byte-lane swap this model doesn't replicate

Once screendump stopped hanging, tracing actual register traffic
(temporary `fprintf` logging in `clie_lcd_regs_write`/`_read` and in
`dragonball_intc`'s IMR-write path -- same technique Session 4 used,
all reverted before commit, `git diff` on `hw/intc/dragonball_intc.c`
and `hw/misc/sony_mshc_stub.c` is empty) showed the T600C HAL **does**
reach genuine MediaQ hardware bring-up: real writes to `GC_CONTROL`
(mode/enable), the flat-panel timing block (`fpREG`, `0x600`-`0x6ff`),
and the color palette (`cpREG`, all 256 entries written), plus a
**genuinely blocking busy-poll** on `dcREG[0]` (`DeviceConfig0`/"DC00",
offset `0x380`) -- **870 reads in 10 real seconds**, always getting
back `0` from this model's plain zeroed storage, before the HAL will
program anything else. This is very likely a chip-present/ID readback
the real MediaQ part answers with a nonzero value on real silicon (a
register that always reads `0` looks like "no chip here" to any sane
detection routine) -- Cloudpilot's own **T2**-chip sibling class
(`EmRegsMQLCDControlT2::DC380Read`) hard-codes a comment-explained `0`
for its analogous register, but that's a *different* chip class
(confirmed above to be the wrong one for T600C) and Cloudpilot's actual
`EmRegsMediaQ11xx` class has **no override for this register at all**,
so its behaviour on real Cloudpilot is "whatever the generic proxy
storage defaults to" -- almost certainly also 0 there too, which would
mean either real Cloudpilot doesn't reach/need this exact poll (its
CPU-accurate timing or a savestate-seeded register value could differ
from a cold QEMU reset), or there's a subtlety not captured here.

Made this register return all-ones (`0xffffffff`) instead of `0` as a
best-effort unblock (documented in-code as unconfirmed against a real
datasheet/silicon value, `hw/display/clie_lcd.c`'s `CLIE_LCD_REG_DEVCFG0`
comment) -- **verified this measurably changes guest behaviour**: the
HAL stops retrying and proceeds to write real, plausible geometry
(`GC_HWINDOW`/`GC_VWINDOW` = `0x13f` = 319 = width-1/height-1 for a
320-wide/-tall panel -- exactly the SJ33/T600C's real 320x320 panel
size) and sets `GC_CONTROL`'s enable bit. However, getting an actual
picture out of this needed a **second** fix: the guest's 16-bit-at-a-
time partial writes to these 32-bit registers do not land where a
naive big-endian byte-array model would put them (e.g. the 320-implying
`0x13f` value lands in the *second* 16-bit half of `GC_HWINDOW`, not
where composing the full 32-bit register the textbook way would look
for it) -- this matches a hardware quirk `EmRegsMediaQ11xx.cpp` itself
names but that wasn't fully pulled into this model: a **"bytelanes"**
remapping table (`fBytelanes[4]`, `PrvUpdateByteLanes()`) that bridges
the real MediaQ chip's native bus width/endianness to the DragonBall's
16/32-bit accesses. `clie_lcd.c` currently works around this
empirically (`clie_lcd_get_loword()`, reading the second halfword of
each 4-byte register rather than composing the whole thing) rather
than implementing the real bytelane table -- confirmed against two
registers (`GC_HWINDOW`/`GC_VWINDOW`, consistent) but **not** confirmed
against `GC_STRIDE` (the values observed there, `0xa000` then
`0x4001`, don't obviously decode to a sane byte-per-line figure under
either interpretation) -- flagged as unresolved.

**Result of the two fixes together:** the controller now genuinely
scans out a real, correctly-sized (320x320), enabled color surface
instead of QEMU's "guest has not initialized the display" placeholder
-- screenshotted, `clie-poc-evidence/SESSION-SJ33-color-surface-320x320.png`,
a solid magenta 320x320 frame (matches a 1bpp-mode single-color
palette-index-0 fill, consistent with the HAL still being mid-way
through a bring-up/test-pattern sequence rather than done). This did
**not** progress further in the time available: repeated screendumps
over ~20 more seconds of real time show the identical frame, and a
tap (same `palmctl.py tap:16384,16384` mechanism Session 4 used
successfully on `clie-s300`) produced no change. `dragonball_intc` IMR
tracing during this window shows the CPU settled into what looks like
a normal Palm OS doze/event-loop pattern (RTC-tick mask toggling
repeatedly, matching Session 3's identified `SysDoze`/`EvtGetEvent`
idle shape, not a fault or a tight spin) with the pen interrupt
unmasked throughout (unlike the S300's original bug, nothing here
looks like `HwrSleep`/`PenSleep` having fired) -- so this is *not* the
same auto-sleep bug Session 4 fixed for S300; it is a **new, distinct,
not-yet-root-caused stopping point**, most likely the unresolved
bytelane/stride issue above preventing the HAL's own "did my
configuration take effect" verification from passing.

### Result vs CLIE-RESEARCH.md success tiers

* (best) launcher in 320x320 color -- **not reached.**
* (good) Setup/digitizer screen with the color display rendering --
  **not reached**: no Palm OS UI content (text, icons, the Welcome
  bitmap) was captured on screen, only a solid-color 320x320 frame.
* **(min) the machine + color display controller exist, the ROM loads
  and runs, first fault characterized -- exceeded.** There is no
  fault: `clie-sj33` boots the VZ328 SoC cleanly through PLL/GPIO/
  INTC/timer/RTC/keypad/digitizer init and the Sony Memory Stick stub
  with zero `ignore_memory_transaction_failures` bus-error spam; the
  new `hw/display/clie_lcd.c` MediaQ-class controller receives real,
  correctly-shaped hardware bring-up traffic from the Palm HAL
  (confirming the register map, both base addresses, and the palette/
  flat-panel/window-geometry register identities are all correct) and
  successfully renders a real, correctly-sized, enabled 320x320 color
  surface rather than a placeholder or garbage. The precise remaining
  blocker is characterized down to two named, narrow, documented gaps
  (the `DeviceConfig0`/"DC00" stub value being an unconfirmed guess,
  and the MediaQ "bytelanes" bus-width/endianness bridge not being
  fully modelled) rather than a generic "it doesn't work".

**Spot-checked bit-exact after all changes** (same technique as every
prior session): `palmv` Setup 1->2 on a single tap
(`clie-poc-evidence` not re-saved, screen matched Session 4's saved
reference pixel-for-pixel via the same harness), `clie-s300` reaches
"Setup 1 of 4" unchanged, `palmm515` advances Setup 1->2 with its
SED1376 color panel rendering as before. None of these three touch
`clie_lcd_base` or the new `clie_gpio_*`/`hwrsleep_patch_addr` fields
in a way that changes their behaviour (`clie_lcd_base=0` and
`hwrsleep_patch_addr=0` for all of them, matching their pre-Session-5
defaults exactly).

### Next session should try

1. **Pull the real `fBytelanes`/`PrvUpdateByteLanes` logic** from
   `EmRegsMediaQ11xx.cpp` (only partially fetched this session) and
   replace `clie_lcd_get_loword()`'s empirical workaround with the
   actual documented remapping table -- needed to get `GC_STRIDE` (and
   any other register this session didn't specifically verify) right,
   not just the two window registers that happened to match.
2. **Trace what real value `DeviceConfig0`/"DC00" should return** --
   either find a MediaQ 1100/1132 datasheet (vendor was Cyberpro/
   MediaQ, later NeoMagic) with a documented chip-ID register, or
   accept `0xffffffff` as a permanent "unimplemented, always-idle"
   stub *if* it can be shown Real Cloudpilot's zero-default value is
   actually never read this early in T600C's own boot (i.e. real
   silicon's polling loop is gated on something else that happens to
   also become true around the same point, in which case matching
   Cloudpilot's 0 exactly, but wiring whatever *that* gate is, would
   be the more faithful fix).
3. Once real Palm OS content renders, drive Setup with the same
   `palmctl.py`/QMP `input-send-event` harness Session 4 proved for
   `clie-s300` -- the tap/calibration/Next sequence should transfer
   directly, since none of it is CLIE-specific.
4. If a real SJ33 (or any SJ-series) ROM ever surfaces, re-run this
   exact same `clie-sj33` machine against it unmodified first --
   `clie_lcd_base`/`ms_stub_base` may need updating per-model (this
   session's addresses are the *T600C's*, inherited via the Modena
   board-family match, not independently confirmed for the SJ33
   specifically) but the SoC/GPIO/display wiring shape should carry
   over directly.

---

## Session 6: T600C boots to the launcher in color; SJ33 corrected to MC68SZ328 (SoC bring-up + on-chip LCDC)

Two things this session: (1) got the VZ328/MediaQ machine rendering real
Palm OS UI all the way to the **launcher in 320x320 color**, and (2) a
coordinator correction: the **real PEG-SJ33 is a Motorola DragonBall
Super VZ (MC68SZ328) @ 66MHz**, not the VZ328 the earlier (`≈`-flagged)
CLIE-RESEARCH.md row claimed. So the VZ328+MediaQ+T600C-ROM machine
built in Session 5 as "clie-sj33" is actually a faithful **PEG-T600C**
and was renamed `clie-t600c`; a genuinely new `clie-sj33` was built on
the MC68SZ328 SoC.  Same worktree/branch (`fin-clie`), reused build, all
traces to /tmp, `/workspace` ~1.4GB free throughout.

### Part A -- clie-t600c reaches the launcher in 320x320 color (the MediaQ scanout was the blocker, not sleep)

Session 5 left the T600C showing a solid magenta frame and (wrongly)
suspected auto-sleep.  Root cause was neither sleep nor a missing probe:
**Palm was drawing correctly the whole time; the MediaQ scanout decoded
the registers wrong.**  Proven by `xp`-dumping the MediaQ video aperture
(0x1F000000) on the live boot: real 8bpp framebuffer content was sitting
there (a 0x59 background fill in the top rows, varied pixel/text data
below) -- the device was not asleep, the scanout was reading garbage.

The fix was the **MediaQ register byte-lane / endianness bridge** that
Session 5 flagged as only empirically worked-around.  Traced it exactly
this session by reading the live register bytes via `xp` and matching
against Cloudpilot's EmRegsMediaQ11xx semantics.  The chip presents its
32-bit registers so that the big-endian DragonBall writes them as two
16-bit big-endian halfwords, but the chip's *logical* 32-bit value
orders those halfwords **little-endian** (low address = low 16 bits):

    logical(off) = be16(off) | (be16(off+2) << 16)

Verified against all five registers the HAL programs:
* GC_CONTROL @0x180 bytes `00 39 06 01` -> logical `0x06010039`
  -> enable=bit0=1, bpp = 1<<((v&0x70)>>4) = 1<<3 = **8bpp**
* GC_HWINDOW @0x1a0 bytes `00 00 01 3f` -> `0x013f0000` -> width  = (v>>16)+1 = 320
* GC_VWINDOW @0x1a4 bytes `00 00 01 3f` -> `0x013f0000` -> height = 320
* GC_STRIDE  @0x1b8 bytes `01 40 00 00` -> `0x00000140` -> stride = 320
* GC_START   @0x1b0 -> 0 -> framebuffer at video-aperture offset 0

Session 5's `clie_lcd_get_loword()` heuristic (read the second halfword)
happened to be right for HWINDOW/VWINDOW but wrong for GC_CONTROL and
GC_STRIDE -- which is exactly why bpp/stride came out garbage and the
frame was magenta.  Replaced it with the correct `clie_lcd_logical()`
transform used uniformly for control regs and the 256-entry 6:6:6
palette (R=v&0xFC, G=(v>>8)&0xFC, B=(v>>16)&0xFC, per Cloudpilot's
RED/GREEN/BLUE_MASK).  16bpp scanout switched to little-endian pixels to
match the aperture (not exercised while the Setup UI runs 8bpp, correct
for the color mode afterwards).

Result: `clie-t600c` renders the real Palm OS 4.1 Setup wizard and, driven
through it with the same tap harness the S300 used (scaled to 320x320:
Welcome tap -> digitizer calibration at the three targets -> Setup 3
country/time -> Setup 4 -> Done), reaches the **Applications launcher in
full 320x320 color** -- color icons (Address, Calc, Card Info, CLIE Demo,
Graffiti, HotSync, Mail, ...), clock, battery, category dropdown all
rendering.  Screenshots:
`clie-poc-evidence/SESSION6-t600c-{setup1-color,setup3-color,LAUNCHER-320x320-color}.png`.
No HwrSleep neutralise was needed (T600C keeps drawing past boot;
`hwrsleep_patch_addr=0`).  This is the **best** tier for the T600C.

### Part B -- clie-sj33 rebuilt on the real SoC: MC68SZ328 "Super VZ"

Confirmed the SoC correction first-hand: fetched Cloudpilot's
`hardware/EmRegsSZ.{h,cpp}`, `hardware/clie/EmRegsSZRedwood.cpp`,
`EmRegsSZPrv.h` and the `HwrM68SZ328Type` struct
(`palm/.../IncsPrv/M68SZ328Hwr.h`).  Key facts, several of which
*correct* CLIE-RESEARCH.md sec 4.1:

* **The SZ328 register window is NOT at 0xfffffxxx.**  `EmRegsSZPrv.h`:
  `kMemoryStart = 0xFFFE0000L; // different than in previous chips!!!`.
  The research doc's "SZ328 keeps its register window at 0xfffffxxx, so
  reuse the VZ INTC/GPIO/timer/UART blocks" is **wrong** -- the whole
  register file moved to 0xFFFE0000 with a completely different layout
  (LCD @ +0x800, sysctrl @ +0x10000, PLL @ +0x10200, INTC @ +0x10300,
  GPIO ports @ +0x10400...).  None of the existing 0xfffffxxx EZ/VZ
  peripheral models apply, so clie-sj33 takes a **separate bring-up
  path** in palm_init() (guarded on the new `is_sz328` flag) that
  instantiates one new SoC device and nothing else.
* The SZ has an **enhanced on-chip TFT LCD controller** (no external
  MediaQ -- that was the T600C).  Framebuffer lives in main DRAM at
  `lcdStartAddr` ($00800); geometry from `lcdScreenSize` ($00804:
  width=(v>>9)*8, height=v&0x1ff), `lcdPageWidth` ($00806, bytes/line
  /2), bpp from `lcdPanelControl1` ($00814, 1<<((v>>9)&7)); a 256-entry
  12-bit CLUT at $00A00; 16bpp is big-endian RGB565 in RAM.
* Reset identity: scr=0x1C @$10000, chipID=0x56 @$10004, maskID=0x01
  @$10005, pllControl=0x2414, pllFreqSel0=0x3CE8, pllFreqSel1=0x0900,
  clockSrcCtl=0x8A03.

New device **`hw/misc/dragonball_sz.c`** + header (`TYPE_DRAGONBALL_SZ`,
Kconfig `DRAGONBALL_SZ`, selected under `CONFIG_PALM`): a single MMIO
window at 0xFFFE0000 (68KB) with the reset values seeded and plain
big-endian store/return semantics, plus the enhanced on-chip LCDC
scanout (reads the framebuffer straight out of main DRAM via
`address_space_read`, 1/2/4/8/16bpp, 12-bit CLUT).  The scanout is
bounds-hardened (a mid-programming transient with a bad bpp/stride must
not OOB-read the line buffer -- an early version segfaulted on exactly
that during boot; fixed by sizing the line buffer to the pixel accesses,
not the programmed page width) and honours the same `gfx_update()`
"always return true" contract as clie_lcd.c so screendump/VNC never
wedge.  Deliberately **not** modelled: the SZ INTC / timers / UART / GPIO
/ ADC dynamic behaviour (the remaining work to reach the UI).

New machine **`clie-sj33`** (MC68SZ328): 16MB RAM, the NR70V ROM (the
same SZ328 SoC -- halID `sonyrdwd`, confirmed: fetched
`Sony-CLIE-NR70V.rom`, 8MB, from the PalmDB archive.org mirror; strings
`sonyrdwd`/`320x480`/`Flip-and-Rotate` present; big-ROM header at
+0x10000, PC 0x1001482e), `rom_load_offset=0`, `bigrom_offset=0x10000`,
base 0x10000000.  The NR70V is a 320x480 flip model vs the SJ33's
320x320, but it is the SoC-correct proxy and exercises the identical
SZ328 bring-up + enhanced LCDC.

### How far clie-sj33 gets (SoC bring-up characterized -- no fault)

Booted under the monitor and traced PC/registers.  **The SZ328 boots
cleanly and runs the real Palm OS 4.1 HAL -- there is no hardware
fault.**  Specifically:
* Reset -> small-ROM -> big-ROM vectors execute; the CPU reaches and
  runs the **Palm OS A-trap dispatcher** at 0x10014ae0 (loads the trap
  dispatch table from low-mem 0x122, indexes by trap selector, returns
  via `rte` at 0x10014afa) -- i.e. Palm OS is executing OS calls, not
  stuck on a bus error or at reset.
* Steady state is a HAL **serial-debug output loop**: the routine at
  0x10091462 reads the DragonBall UART status (0xFFFFF904/906/908 via
  absolute-short addressing) and builds a status bitmask, driving a
  byte-out loop at 0x100176b4 (`trap #15,#0xa3ba` wait-for-TX-ready /
  `#0xa3bb` write-byte).  Note this uses the *legacy* 0xFFFFF9xx UART
  page, which on the SZ (UART at 0xFFFF0900) is unmapped -- reads return
  0 under `ignore_memory_transaction_failures`, so the TX-ready poll
  passes and the debug print drains, but the OS keeps looping here.
* The LCD controller registers stay 0 (lcdStartAddr/lcdScreenSize never
  stably programmed), so the display is not initialised; the LCDC
  scanout correctly presents a clean black 320x480 frame
  (`clie-poc-evidence/SESSION6-sj33-sz328-320x480-blank.png`).  A brief
  mid-boot transient *did* touch the LCD registers (it's what triggered
  the OOB segfault before the bounds fix), so the enhanced-LCDC path is
  exercised; the guest just doesn't leave them programmed.

Why it stops here: with no SZ INTC/timer model, the OS gets no tick
interrupts, so it cannot advance out of early bring-up into the
interrupt-driven Setup/UI.  Reaching the launcher needs the SZ328
INTC ($10300 mask/status/pending + 7 level regs), timers, and a real
UART -- a substantial next step, scoped but not attempted here.

### Result vs the SJ33 success tiers

* (best) SZ328 boots Palm OS to the launcher in color -- not reached.
* (good) SZ328 + enhanced LCDC render the Setup screen -- not reached
  (LCD never stably programmed without the interrupt/timer model).
* **(min) the SZ328 SoC variant + on-chip color LCDC exist, the NR70V
  ROM loads and runs the SoC bring-up, first fault characterized --
  exceeded.**  There is no fault: the SZ328 boots into the live Palm OS
  A-trap dispatcher and HAL serial-debug path; the on-chip enhanced LCDC
  is modelled and its scanout runs (black frame, LCD not yet
  programmed); the precise stopping point (an early HAL loop gated on
  interrupt/timer activity the minimal SoC model omits) is identified at
  the instruction level.

Also corrected in this file's earlier sections by implication: the SJ33
is MC68SZ328, and clie-s300/palmv/palmvx/palmm515 plus the renamed
clie-t600c were all re-verified rendering their Setup screens unchanged
(palmv/m515 160x160 mono Setup, t600c 320x320 color Setup) after every
change this session.

Evidence (gitignored `clie-poc-evidence/`):
`SESSION6-t600c-setup1-color.png`, `SESSION6-t600c-setup3-color.png`,
`SESSION6-t600c-LAUNCHER-320x320-color.png`,
`SESSION6-sj33-sz328-320x480-blank.png`.
ROMs used: `Sony-CLIE-PEG-T600C-en.rom` (clie-t600c),
`Sony-CLIE-NR70V.rom` (clie-sj33, SZ328 proxy).

### Next session should try (to get clie-sj33 to the UI)

1. Model the SZ328 **INTC** ($10300: intVector, intMaskHi/Lo, intStatus,
   intPending, intLevelControl1-7) + wire it to the CPU IRQ, plus at
   least **timer 1** and the **RTC 1Hz tick**, so the OS gets ticks and
   advances past the bring-up poll.  Register semantics are all in
   Cloudpilot's EmRegsSZ.cpp (UpdateInterrupts/UpdateTimers).
2. Model the SZ **UART** ($10900) so the HAL serial-debug loop at
   0x100176b4 drains against a real TX-ready bit instead of the unmapped
   legacy page.
3. Then watch whether the guest programs the on-chip LCDC (lcdStartAddr/
   lcdScreenSize/lcdPanelControl1) and drives Setup -- the LCDC scanout
   is already in place, so a stably-programmed framebuffer should render
   immediately.
