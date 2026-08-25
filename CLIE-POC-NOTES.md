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
