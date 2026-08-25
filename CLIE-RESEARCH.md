# Emulating the m68k Sony CLIÉ (DragonBall Palm OS PDAs) in this QEMU tree

Research + implementation plan.  Author: Daniel's intern, 2026-08-25.

This tree already emulates six DragonBall Palm PDAs (`palmv`, `palmiiix`,
`palmvx`, `palmm100`, `palmm500`, `palmm515`).  This document maps what it
would take to add the **Motorola DragonBall (m68k) Sony CLIÉ** line on top
of that infrastructure.  All ROM-header facts below were verified
first-hand against real CLIÉ ROM dumps (see §7); register/codename facts
come from the Cloudpilot-emu source tree and the archived Sony developer
material cited inline.

---

## 1. Executive summary + feasibility verdict

**Verdict: clearly feasible, and Phase 1 is cheap.**  The m68k CLIÉs are
DragonBall EZ/VZ/SZ machines that reuse the exact SoC blocks this tree
already models.  The lineup splits into three tiers of effort:

* **Tier A — trivial (reuses everything we have):** the monochrome
  160×160 **S-series** (PEG-S300/S320/S360, MC68EZ328) and the
  monochrome 320×320 **T-series** (PEG-T400/T415, MC68VZ328).  These
  drive the *on-chip* DragonBall LCD controller — the T400 ROM literally
  contains the string `68VZ328 LCD Controller`, the S300 ROM contains
  `68328 LCD Controller`.  A CLIÉ machine here is a thin `PalmMachineClass`
  variant plus **no-fault stubs** for the two Sony peripherals the HAL
  pokes at boot (Memory Stick host controller, Jog Dial GPIO).  Expected
  to boot Palm OS 3.5/4.1 to the launcher with days, not weeks, of work —
  the same class of task as `palmm500` was.

* **Tier B — moderate (one new display model each):** the **color
  HiRes** CLIÉs (N600C/N610C/N700C/N710C, T600C/T625C/T650C) render
  320×320 in 8/16-bit color through a Sony companion display controller,
  analogous to the SED1376 we already model for the `palmm515`.  Plus a
  functional Memory Stick and Jog Dial.

* **Tier C — the big lift:** the **flip-and-rotate 320×480** models
  (PEG-NR70/NR70V) use the **MC68SZ328 "Super VZ"**, which is *not*
  modelled at all.  It adds an enhanced LCD controller (320×480,
  16bpp), an SDRAM controller, a USB device controller and more.  This is
  a genuinely new SoC and should be the last phase.

The ARM CLIÉs (NX/NZ/UX/TG/TH/TJ series, and the PEG-VZ90) are **out of
scope** for an m68k target — they are Intel XScale / Sony Handheld Engine
machines, not DragonBall.

**Recommended first target: PEG-S320 (or S300).**  MC68EZ328, 160×160
4-level grey, Palm OS 3.5 — functionally a `palmv` with a Sony chip
identity and two peripheral stubs.  Second target: PEG-T400 (VZ328,
320×320 mono, on-chip LCDC) to prove the HiRes silk/HR-library path on
hardware we already have.

---

## 2. Device / SoC taxonomy

### 2.1 DragonBall (m68k) CLIÉ models

Specs consolidated from Wikipedia's *Sony CLIÉ* pages, clie.info, and
Cloudpilot-emu's device list; SoC/codename confirmed from the Cloudpilot
`hardware/clie` source directory
(<https://github.com/cloudpilot-emu/cloudpilot-emu/tree/master/src/cloudpilot/emulator/hardware/clie>)
and, for four models, from direct ROM-header inspection (§7).  Cells
marked "≈" are from secondary sources and should be re-checked against a
datasheet before wiring a machine.

| Model | SoC | Clock | RAM | Display | Palm OS | MS | Jog | Audio | Cloudpilot codename |
|-------|-----|-------|-----|---------|---------|----|----|-------|---------------------|
| PEG-S300 | MC68EZ328 | 16.6 MHz | 8 MB | 160×160 mono 4-grey | 3.5 | ✓ | ✓ | PWM beep | `EzPegS300` |
| PEG-S320 | MC68EZ328 | 16.6 MHz | 8 MB | 160×160 mono 4-grey | 3.5.x | ✓ | ✓ | PWM beep | `EzPegS300` |
| PEG-S360 | MC68EZ328 | 16.6 MHz | 8 MB | 160×160 mono 16-grey ≈ | 4.1 ≈ | ✓ | ✓ | PWM beep | `EzPegS300` |
| PEG-S500C | MC68EZ328 | 16.6 MHz | 8 MB | 320×320 color ≈ (Japan) | 3.5 | ✓ | ✓ | PWM beep | `EzPegS500C` |
| PEG-N600C | MC68VZ328 | 33 MHz | 8 MB | 320×320 8-bit color | 4.0 | ✓ | ✓ | Sony DSP | `VZPegNasca` ≈ |
| PEG-N610C | MC68VZ328 | 33 MHz | 8 MB | 320×320 8-bit color | 4.1 | ✓ | ✓ | Sony DSP | `VZPegNasca` ≈ |
| PEG-N700C | MC68VZ328 | 33 MHz | 8 MB | 320×320 8-bit color | 3.5.2 | ✓ | ✓ | Sony DSP (`Hwr1859`) | `VZPegN700C` |
| PEG-N710C | MC68VZ328 | 33 MHz | 8 MB | 320×320 8-bit color | 4.1 | ✓ | ✓ | Sony DSP + MP3 | `VZPegN700C` |
| PEG-N760C | MC68VZ328 | 33 MHz | 8 MB | 320×320 16-bit color ≈ | 4.1 | ✓ | ✓ | Sony DSP + MP3 | `VZPegN700C` ≈ |
| PEG-T400 | MC68VZ328 | 33 MHz | 8 MB | 320×320 mono 16-grey | 4.1 | ✓ | ✓ | PWM beep | `VZPegVenice` |
| PEG-T415 | MC68VZ328 | 33 MHz | 8 MB | 320×320 mono 16-grey | 4.1 | ✓ | ✓ | PWM beep | `VZPegVenice` |
| PEG-T425 | MC68VZ328 | 33 MHz | 8 MB | 320×320 mono 16-grey ≈ | 4.1 | ✓ | ✓ | PWM beep | `VZPegVenice` ≈ |
| PEG-T600C | MC68VZ328 | 33 MHz | 8 MB | 320×320 16-bit color | 4.1 | ✓ | ✓ | Sony DSP ≈ | `VZPegVenice` |
| PEG-T625C | MC68VZ328 | 33 MHz | 8 MB | 320×320 16-bit color | 4.1 | ✓ | ✓ | Sony DSP ≈ | `VZPegVenice` |
| PEG-T650C | MC68VZ328 | 33 MHz | 8 MB | 320×320 16-bit color | 4.1 | ✓ | ✓ | Sony DSP + MP3 | `VZPegVenice` |
| PEG-SL10 | MC68VZ328 | 33 MHz | 8 MB | 320×320 mono 16-grey ≈ | 4.1 | ✓ | ✓ | PWM beep | `VZPegYellowStone`/`Modena` ≈ |
| PEG-SJ20 | MC68VZ328 | 33 MHz | 8 MB | 320×320 mono 16-grey ≈ | 4.1 | ✓ | ✓ | PWM beep | `VZPegYellowStone`/`Modena` ≈ |
| PEG-SJ30 | MC68VZ328 | 33 MHz | 8 MB | 320×320 color ≈ | 4.1 | ✓ | ✓ | PWM beep | `VZPegYellowStone`/`Modena` ≈ |
| PEG-SJ33 | MC68VZ328 | 33 MHz | 16 MB ≈ | 320×320 16-bit color | 4.1 | ✓ | ✓ | FM (`EmRegsFMSound`) ≈ | `VZPegYellowStone`/`Modena` ≈ |
| PEG-NR70 | **MC68SZ328** | 66 MHz | 16 MB | 320×480 16-bit color, flip | 4.1 | ✓ | ✓ + rotate | Sony DSP + MP3 | `SZNaples`/`SZRedwood` ≈ |
| PEG-NR70V | **MC68SZ328** | 66 MHz | 16 MB | 320×480 16-bit color, flip + camera | 4.1 | ✓ | ✓ + rotate | Sony DSP + MP3 | `SZRedwood` (confirmed, halID `sonyrdwd`) |

Cloudpilot codename→model mapping is **partially inferred**; the four
confirmed anchors are: `sonyvnce` (Venice) = T-series (from the T400 ROM
header, §7); `sonyrdwd` (Redwood) = NR70V (from the NR70V ROM header);
`EzPegS300` file = S-series; `VZPegN700C` file = N700C.  The exact
Nasca/Modena/YellowStone/Naples splits across N600C / SJ / SL / NR70 need
one more cross-check against `EmDevice.cpp`
(<https://github.com/cloudpilot-emu/cloudpilot-emu/blob/master/src/cloudpilot/emulator/EmDevice.cpp>)
before wiring those specific machines.

### 2.2 Out of scope (ARM CLIÉs)

PEG-NX60/NX70V/NX73V/NX80V, PEG-NZ90, PEG-UX40/UX50, PEG-TG50,
PEG-TH55, PEG-TJ25/TJ27/TJ35/TJ37, and PEG-VZ90 are **ARM** (Intel
XScale PXA / Sony CXD2230 "Handheld Engine"), running Palm OS 5.  They
share nothing with the m68k DragonBall target and are excluded.

---

## 3. What the existing DragonBall model already provides

All paths relative to `/workspace/src/qemu-amiga`.  The Palm machine glue
is `hw/m68k/palm.c`; the SoC blocks are the `hw/*/dragonball_*.c` models.

### 3.1 Reusable as-is

| Block | File | Notes |
|-------|------|-------|
| Machine framework | `hw/m68k/palm.c:114-139` (`PalmMachineClass`/`PalmMachineState`) | Parameterized by rom_base/size/load_offset/bigrom_offset, sysclk, chip_id/mask_id, gpio_ports, adc_dock_value, kbd rows, `sed1376_base`, `has_timer2`.  A CLIÉ is another set of these fields. |
| Reset / ROM load | `hw/m68k/palm.c:149-217` | Fills the ROM window with 0xff (erased flash), loads the image at `rom_load_offset`, seeds SP/PC from the big-ROM card header at `rom_base+bigrom_offset`.  Works for every CLIÉ layout in §7. |
| SCR / chip ID | `hw/misc/dragonball_scr.c` | Serves CHIPID/MASKID at `0xfffff004/6`.  EZ=0x43, VZ=0x56.  CLIÉ EZ/VZ reuse this unchanged; SZ needs a new ID (§4.1). |
| PLL | `hw/misc/dragonball_pll.c` | CLK32 toggling bit that Palm delay-loops poll.  Reused. |
| INTC | `hw/intc/dragonball_intc.c` | IVR/ICR/IMR/ISR/IPR/ILCR; 32 peripheral lines; `ISR = ~IMR & IPR`; presents only the highest active level.  Level table `dragonball_irq_levels[]` at `:21`.  Reused for EZ/VZ. |
| GPIO | `hw/gpio/dragonball_gpio.c` | 10 ports (A–G + J/K/M as blocks 7/8/9), byte-wide regs, port-D INT0-3/KB logic, tristate-deselect scanning.  This is where the Jog Dial and MS card-detect lines hang. |
| Timers | `hw/timer/dragonball_timer.c` | Timer 1 always; timer 2 (`has_timer2`) for the VZ tick at level 6 via ILCR. |
| SPI master | `hw/ssi/dragonball_spi.c` | Bit-exact SPIM; carries the ADS784x touch ADC. |
| SPI1 FIFO | `hw/ssi/dragonball_spi1.c` | VZ SPI unit 1 at `0xfffff700`; carries the SD card on the m500/m515.  (CLIÉ storage is Memory Stick, not SD — see §4.3 — but the block itself is here.) |
| UART(s) | `hw/char/dragonball_uart.c` | UART1 + UART2. |
| RTC + watchdog | `hw/rtc/dragonball_rtc.c` | Full time/alarm/watchdog. |
| PWM beeper | `hw/audio/dragonball_pwm.c` | The system beep — covers CLIÉ UI/alarm tones (§4.5). |
| Touch ADC | `hw/input/ads7843.c` | Bit-stream SPI slave, PENIRQ.  CLIÉ digitizer is the same class of part. |
| Keypad | `hw/input/palm_keypad.c` | Hard-button matrix + silkscreen pen taps. |
| Grey LCDC | `hw/display/dragonball_lcdc.c` | Scans framebuffer from RAM at LSSA; 1/2/4bpp grey with LGPMR palette; honours LPICF, LXMAX/LYMAX.  **This is exactly the block the T400 drives at 320×320.** |
| SED1376 color LCDC | `hw/display/sed1376.c` | Epson companion controller with embedded SRAM, 256-entry LUT, 8/16bpp, SwivelView.  The template for the Sony color-HiRes controller (§4.2). |

### 3.2 Grey-LCDC capability check for HiRes-mono

`hw/display/dragonball_lcdc.c` reads geometry straight from the LCDC
registers: width `= LXMAX` (`:20`, `DRAGBONBALL_LCDC_WIDTH`), height
`= LYMAX+1` (`:21`), and resizes the console to whatever the guest
programs (`dragonball_lcdc_updatefb_params`, `:123-136`).  There is **no
hard-coded 160×160** — it will already scan out 320×320 if the T-series
HAL programs LXMAX=0x140/LYMAX=0x13f.  The `framebuffer_update_display`
path (`:251-268`) computes `linewidth = bpp*width/8`, so 320-wide lines
work.  So **for mono HiRes the existing model likely needs no display
change at all** beyond confirming the T-series doesn't touch an
unmodelled register — a strong Phase-1/2 enabler.

### 3.3 Precise gap list (what CLIÉ needs that we don't have)

1. **Sony peripheral no-fault stubs at boot.**  Like the m515's USB poll
   (`hw/m68k/palm.c:571-580` comment), the Sony HALs poke a Memory Stick
   host controller and read Jog Dial / hardware-ID GPIOs during init.
   `mc->ignore_memory_transaction_failures = true` (`palm.c:451`) already
   keeps stray MMIO from bus-faulting, but any block that is *polled for a
   status bit* (as the SED1376 VNDP bit is, `sed1376.c`) must return the
   right bit or boot hangs.  **Gap: identify and stub those Sony regions.**
2. **Memory Stick host controller** (`0x10xxxxxx` chip-select region) —
   no model.  Needed functionally in Tier B, stub ("no card") for Tier A.
3. **Jog Dial** rotary encoder + Back button, and (NR70) the flip/rotate
   sensor — no model.  Wires into GPIO/IRQ.
4. **Sony color HiRes display controller** (320×320 8/16bpp) — no model;
   the SED1376 is the closest template but the Sony part differs.
5. **MC68SZ328 SoC** — entirely absent: enhanced 320×480 LCDC, SDRAM
   controller, USB device controller, new chip ID, possibly relocated
   register fields.  Whole new device family (Tier C).
6. **Sony audio DSP** (`Hwr1859*`, `EmRegsSonyDSP`) and FM sound
   (`EmRegsFMSound`) — deliberately **out of scope** (§4.5); stub to
   "not present" so the audio apps degrade gracefully.
7. **Sony USB device controller** (`EmRegsUsbCLIE`) — HotSync/USB; not
   needed to boot; stub.

---

## 4. Per-subsystem emulation design

### 4.1 MC68SZ328 "Super VZ" — the new SoC (Tier C)

Confirmed from the NR70V ROM (§7): the SZ328 keeps its on-chip register
window at **`0xfffffxxx`** (the ROM makes 1426 references to the
`0xfffff0xx` page and heavy use of `f5xx`/`f7xx`/`f8xx`), so the base does
**not** move — good news, the INTC/GPIO/timer/UART/SCR blocks can largely
be reused with a new chip ID.  What is genuinely new vs the VZ328:

* **CPU / clock:** static EZ68000 core at up to 66 MHz.  Reuse the plain
  `m68000` core as we do for EZ/VZ; set `sysclk` accordingly (the tick
  math in `dragonball_timer.c` is sysclk-driven).
* **Enhanced LCD controller:** drives the 320×480 panel at up to 16bpp,
  with the "flip-and-rotate" SwivelView-style rotation the NR70 ROM
  advertises (`Flip-and-Rotate LCD Design`, `GetLcdRotateState`,
  `InitJRotateRegister`, `320x480 pixel color screen`).  This is a
  superset of the VZ LCDC and of the SED1376 rotation logic — model it as
  a new device, cribbing the scan-out loop from `dragonball_lcdc.c` and
  the rotate/LUT handling from `sed1376.c`.
* **SDRAM controller** — new register block; for us it only needs to not
  fault and to let the RAM alias resolve (RAM is mapped directly at 0).
* **USB device controller** — the NR70 cradle is USB; stub like the m515.
* **Chip ID:** SCR must report the SZ328 ID (needs confirmation from
  `EmRegsSZ.h`; the VZ path uses 0x56, the SZ uses a distinct value the
  Redwood HAL checks — extract from Cloudpilot).
* **Register deltas source of truth:** the authoritative offset/bitfield
  map should be extracted at implementation time from two places rather
  than guessed:
  - Cloudpilot's `EmRegsSZ.h` / `EmRegsSZRedwood.cpp`
    (<https://github.com/cloudpilot-emu/cloudpilot-emu/tree/master/src/cloudpilot/emulator/hardware/clie>)
    — the `HwrM68SZ328Type` struct gives every register offset.
  - The uClinux `arch/m68k/include/asm/MC68SZ328.h` header (search GitHub
    for `MC68SZ328.h`) — a clean, commented register map with bitfields,
    plus the Freescale/Motorola `MC68SZ328UM` user's manual if a copy can
    be found (NXP archives / datasheetarchive).

Because the SZ328 keeps the `0xfffffxxx` base and reuses the INTC/GPIO
model shape, the incremental cost is dominated by the **new LCD
controller** (320×480×16bpp + rotate) rather than a from-scratch SoC.

### 4.2 Sony HiRes display (320×320 and 320×480)

Palm OS HiRes is a *software* density feature (BitmapV3 double-density +
Sony's HR library — the ROMs are full of `HRLib`, `HiResoDrawCheckOK`,
`Sony Silk Library`, `high-resolution (320x320)`).  The *hardware*
question is only "what scans the 320-wide framebuffer out to the panel",
and it splits cleanly:

* **Mono HiRes (T400/T415, and likely SL/SJ-mono):** driven by the
  **on-chip DragonBall LCDC** — the T400 ROM contains `68VZ328 LCD
  Controller`.  Our `dragonball_lcdc.c` already reads geometry from
  LXMAX/LYMAX and resizes the console (§3.2), so this should work by
  programming, needing at most a check that the 16-grey (4bpp) path and
  any T-series LCDC register (e.g. panel-interface config) are covered.
  **This is the cheapest way to get a HiRes CLIÉ on screen.**
* **Color HiRes (N600C/N700C/N710C, T600C/T650C, SJ33):** 320×320 in
  8/16-bit color is *not* the VZ LCDC.  Cloudpilot models a dedicated
  Sony LCD controller — see `EmRegsLCDCtrlT2.cpp`/`.h` in the `clie`
  directory — which is the color-HiRes scan-out engine.  Model it as a
  new QEMU display device in the mould of `sed1376.c` (embedded/paged
  display memory on a chip-select, a LUT for 8bpp, big-endian RGB565 for
  16bpp, a always-ready VNDP-style status bit).  Extract its register
  offsets from `EmRegsLCDCtrlT2.cpp`.
* **320×480 flip (NR70):** the SZ328 enhanced LCDC (§4.1), with the
  rotate registers (`InitJRotateRegister`).  Our SED1376 SwivelView code
  is a starting reference for the rotated addressing, which the m515
  notes (`PALM-NOTES.md`) flag as only partly solved — expect to finish
  the rotated-start-address arithmetic here.

Framebuffer byte order throughout is **big-endian** (m68k); 16bpp is
big-endian RGB565 as already implemented in `sed1376.c`.

### 4.3 Memory Stick slot

Sony's storage is Memory Stick behind the Palm OS **Expansion Manager /
Slot Driver** (`ExpSlotEnumerate`, `ExpSlotRegister`, `GetSlotType`,
`GetSlotVolumeInfo`, `Format Memory Stick` — all present in the CLIÉ
ROMs).  Underneath sits a **Memory Stick host controller** mapped on an
external chip select in the `0x10xxxxxx` region; the driver speaks the MS
**TPC** (Transfer Protocol Command) register interface — command/INT/
status registers plus a data FIFO — to read/write 512-byte sectors and
the card's attribute/boot blocks.  Cloudpilot implements the protocol
(see `MemoryStickStructs.h` and the MS controller EmRegs class in the
`clie` tree) and backs it with a card image.

Design, staged:
* **Tier A (boot to launcher): stub.**  Return "no card present" — the
  controller's card-detect/INT register reads as empty, `ExpSlotEnumerate`
  finds an empty slot, boot proceeds.  This is the minimum to not hang.
  Must sit on the correct chip-select address and return sane status
  bits; find that address by tracing the HAL's first accesses (the same
  technique used for the m100 keyboard rows and the m515 SED1376).
* **Tier B (functional): full model.**  A `dragonball`-style MMIO device
  implementing the TPC command set, backed by a host image via
  `-drive if=mtd`/a bespoke property, mounting as a FAT volume the way the
  m500 SD card already does (`hw/m68k/palm.c:367-406`).  MS card image
  layout (boot block + attribute + logical/physical sector map) comes from
  `MemoryStickStructs.h`.  The controller is widely identified as a
  Fujitsu-class MS host ("MB86189"-family); confirm the exact register
  file from the Cloudpilot EmRegs source before implementing.

### 4.4 Jog Dial

The Jog Dial is a rotary encoder (rotate up / rotate down) plus a press
("select") and a separate **Back** button; the NR70 adds LCD/camera
**rotate** sensors (`GetLcdRotateState`, `GetCamRotateState`).  Palm OS
reads it through Sony's `JogAssist` / silk library; at the hardware level
the encoder phases and the Back button are **GPIO input pins**, and rotation
raises a GPIO/INT interrupt — exactly the mechanism our GPIO model already
supports for the hard-button matrix and the pen line
(`hw/gpio/dragonball_gpio.c`, port-D INT logic; wiring pattern at
`hw/m68k/palm.c:415-435`).

Design:
* Add a small input device (or extend `palm_keypad.c`) that maps host keys
  to jog-up / jog-down / jog-press / back, driving the corresponding GPIO
  input lines and, for rotation, pulsing the encoder pins so the HAL's
  edge detector counts detents.
* The exact port/pin numbers are Sony-specific per board; get them from
  the Cloudpilot `EmRegsVZPeg*.cpp` `GetPortInternalValue` overrides
  (each Sony board file hard-codes its jog/hold/card-detect pins), then
  verify by tracing GPIO reads as was done for m100/m500 buttons.
* **Not required to reach the launcher** (Tier A can ship without jog);
  it is what makes the CLIÉ actually usable, so it is an early Tier B item.

### 4.5 Audio scoping

Two distinct audio paths:
* **DragonBall PWM beeper** — already modelled (`hw/audio/dragonball_pwm.c`).
  Covers Palm OS system beeps and alarms on every CLIÉ.  **In scope, done.**
* **Sony ATRAC3/MP3 playback** — a dedicated Sony audio **DSP** (the
  N700C ROM's `Hwr1859Dsp*` = the Yamaha/Sony audio coprocessor;
  Cloudpilot's `EmRegsSonyDSP`), and on some SJ models an FM synth
  (`EmRegsFMSound`).  These decode compressed audio in dedicated silicon
  with no realistic QEMU payoff.  **Out of scope.**  Model as a
  presence-stub: return "DSP not ready/not present" so the AudioPlayer /
  gMediaAudioLibrary app fails cleanly instead of hanging.  Cloudpilot
  itself stubs these — matching its behaviour is the safe target.

---

## 5. Phased implementation plan

Each phase is independently shippable and leaves a bootable machine.

### Phase 0 — scaffolding (½ day)
* Add `CONFIG_CLIE` / extend `hw/m68k/palm.c` with CLIÉ `PalmMachineClass`
  fields (Sony chip identity quirks, MS/jog stub addresses).  No new
  device files yet.
* Wire `-machine clie-s320` as an alias of the EZ path with the S-series
  ROM layout (§7).  Confirm it reaches the same "hangs waiting on a Sony
  register" point, then add no-fault handling.

### Phase 1 — first CLIÉ boots to the launcher (target: PEG-S320, MC68EZ328)
Reuses **everything**: EZ SoC blocks, 160×160 grey LCDC, ADS7843 digitizer,
hard buttons, PWM beep.
* ROM: `S300en.rom` / `PEG-S320 ROM` — base `0x10000000`, whole-flash,
  small ROM at file 0 (reset PC `0x100002a2`), big ROM at `+0x8000`
  (`rom_load_offset=0`, `bigrom_offset=0x8000`) — verified §7.
* Chip ID: EZ (0x43) unless the Sony HAL wants a Sony marker (check
  `HwrGetROMToken`/silk ID reads; adjust `dragonball_scr` if needed).
* **Stub the Memory Stick host controller** ("no card") and the **Jog
  Dial GPIO** pins so `ExpSlotEnumerate` and `JogAssist` init cleanly.
* Success criterion: Palm OS 3.5 Setup → digitizer calibration →
  **launcher**, screenshotted (as done for `palmv`/`palmm500`).
* Then add T400 (`clie-t400`): VZ328, 320×320 mono on the **on-chip
  LCDC** (§3.2/§4.2) — proves HiRes with no new display code.

### Phase 2 — color HiRes (PEG-N700C / T650C)
* New QEMU display device modelled on `sed1376.c` for the Sony color-HiRes
  controller (`EmRegsLCDCtrlT2`): paged display SRAM on a chip select,
  256-entry LUT (8bpp), big-endian RGB565 (16bpp), always-ready status.
* N700C ROM layout: base `0x10000000`, **big-ROM-only**, header at file 0,
  `rom_load_offset=0x8000`, `bigrom_offset=0x8000` (verified §7).
* Stub the Sony audio DSP ("not present").
* Success: launcher in 320×320 color.

### Phase 3 — functional Memory Stick
* Full MS host-controller model (§4.3) backed by a host image; mount a
  FAT volume via Expansion Manager.  Reuse the m500 SD-mount plumbing.
* Verify with an MS image containing a `.prc`/data, and the built-in
  "Format Memory Stick" flow.

### Phase 4 — Jog Dial (and rotate sensors)
* Input device driving the encoder/Back/press GPIO lines (§4.4).  Map to
  host keys.  Verify jog scrolls the launcher and app lists
  (`ListViewJogRotate`).

### Phase 5 — MC68SZ328 / 320×480 flip (PEG-NR70/NR70V)
* New SZ328 SoC variant: new chip ID, SDRAM-controller and USB stubs,
  and the **enhanced 320×480 16bpp LCD controller with rotate** (§4.1/§4.2).
* NR70V ROM layout: base `0x10000000`, 8 MB whole-flash, small ROM at file
  0 (reset PC `0x100002a2`), big ROM at `+0x10000`
  (`bigrom_offset=0x10000`), halID `sonyrdwd` (verified §7).
* Finish the SwivelView rotated-addressing left open on the m515.
* Success: NR70 boots to the 320×480 launcher; flip/rotate re-orients.

---

## 6. Risks / unknowns and how to de-risk

| Risk | Impact | De-risking |
|------|--------|-----------|
| Sony HAL polls an unmodelled MS/DSP/USB status bit and **hangs** (like the m515 USB poll) | Blocks Phase 1 boot | Trace the HAL's first accesses to each Sony region with temporary logging in the memory-region stub (proven technique from m100/m515); return the "idle/ready/absent" bit it wants. |
| Exact codename→model mapping (Nasca/Modena/YellowStone/Naples) uncertain | Wrong per-board GPIO pins | Read `EmDevice.cpp` + each `EmRegsVZPeg*.cpp` to pin down RAM size, hardware ID, and jog/card pins per model before wiring that machine. |
| Color-HiRes controller register map not yet extracted | Phase 2 slip | Extract offsets/bitfields from `EmRegsLCDCtrlT2.cpp` up front; model is small (sed1376-sized). |
| SZ328 register deltas (chip ID, enhanced-LCDC offsets, rotate) | Phase 5 slip | Use `EmRegsSZ.h`/`EmRegsSZRedwood.cpp` + uClinux `MC68SZ328.h` as the authoritative register map; base scan-out on existing LCDC/SED1376 code. |
| MS TPC protocol subtlety (attribute/boot blocks, logical sector map) | Phase 3 correctness | Follow `MemoryStickStructs.h` and Cloudpilot's MS EmRegs implementation exactly; validate by command trace as SD was (`PALM-NOTES.md`). |
| Digitizer/HiRes coordinate scaling differs from the 160×160 Palms | Calibration loops during Setup | The panel-inversion + calibration lessons in `PALM-NOTES.md` apply; expect per-model ADC channel/scale tweaks. |
| SZ328 66 MHz timer/tick constants | OS runs fast/slow | Set `sysclk` correctly; verify the tick rate as was done for the Palm Vx 20 MHz case. |

**No-fault safety net:** `ignore_memory_transaction_failures = true`
(`palm.c:451`) already prevents stray Sony-region accesses from
double-faulting — the residual risk is only *status-bit polling*, which is
always tractable by tracing.

---

## 7. ROM sourcing (verified first-hand)

### 7.1 The Sony developer archive — best source
Internet Archive item **`jp.sony.clie`** ("Sony Clié Emulation
Resources"): <https://archive.org/details/jp.sony.clie>.  32.5 MB, contains
Sony's own POSE builds *and* CLIÉ ROM images.  File manifest
(<https://archive.org/download/jp.sony.clie/jp.sony.clie_files.xml>),
sizes + md5 verified:

**ROMs** (each a zip containing a `.rom`):
* `Sony CLIE PEG-N600C ROM (English).zip` — 1,555,672 B — md5 `fae35a7efabed668fc39b776c7a919d5`
* `Sony CLIE PEG-N700C ROM (English).zip` — 1,514,763 B — md5 `bd85559b3a0cc29cb109b4361dafd4d7`
* `Sony CLIE PEG-N700C ROM (Japanese).zip` — 3,046,741 B — md5 `50c57044db0c7ff9dc2a8b0b0045a1f4`
* `Sony CLIE PEG-N710C ROM (English).zip` — 1,509,407 B — md5 `528b01b89be9cd39b37d3321e1d3e11c`
* `Sony CLIE PEG-S300 ROM (English).zip` — 771,843 B — md5 `16673e1569569171e57c4def5ea99479`
* `Sony CLIE PEG-S320 ROM (English).zip` — 1,346,924 B — md5 `2ba82507938a4f5c29c491b184dd8c9c`
* `Sony CLIE PEG-S360 ROM (English).zip` — 1,313,546 B — md5 `37e19750fab88db5a2499abbe88d6121`
* `Sony CLIE PEG-T400 ROM (English).zip` — 1,462,449 B — md5 `460b2a9186b3ae74e5a33bde18fde967`
* `Sony CLIE PEG-T600C ROM (English).zip` — 1,632,315 B — md5 `2a78d3b44070d5acd067041c7c3d66d4`

**Sony's own POSE / Palm Simulator builds** (register-behaviour ground
truth and skins): `Palm OS Emulator for PEG-N610C & S320`, `…N700C`,
`…NR70`, `…S300`, `…T400`, plus Palm OS Simulator 5.0/5.2 for CLIÉ.

An NR70V big-ROM dump is on the PalmDB "complete" mirror
(archive.org item `20250707_20250707_0134`, the same set used for the
existing Palm machines per `PALM-NOTES.md`): `Sony-Clie-NR70v.rom`,
8,388,608 B.

### 7.2 ROM layout — verified by header inspection
Format is the standard Palm "big ROM" card image: card header with the
`FEEDBEEF` signature at file+8, `"PalmCard"` at +0x10, and
`bigROMOffset` at header+0x68.  All m68k / **big-endian**.  Reset SP/PC
come from the big-ROM header, exactly as `palm_cpu_reset()` already does
(`hw/m68k/palm.c:149-163`).  Confirmed layouts (feed straight into
`PalmMachineClass`):

| ROM | Size | SoC | `rom_base` | small ROM @ file 0 | big ROM header | `rom_load_offset` | `bigrom_offset` | halID / LCD string |
|-----|------|-----|-----------|--------------------|----------------|-------------------|-----------------|--------------------|
| `S300en.rom` | 1.6 MB | EZ328 | 0x10000000 | PC `0x100002a2` | file `0x8000`, bigROMOffset `0x10008000` | 0 | 0x8000 | `68328 LCD Controller`; `SONY Corporation 0101` |
| `PEG-T400 …rom` | 3.125 MB | VZ328 | 0x10000000 | PC `0x100002a2` | file `0x8000`, bigROMOffset `0x10008000` | 0 | 0x8000 | `sonyvnce` (Venice); `68VZ328 LCD Controller` |
| `PEG-N700C …rom` | 3.25 MB | VZ328 | 0x10000000 | *(big-ROM-only)* header@0, PC `0x100091d0` | header@0, bigROMOffset `0x10008000` | 0x8000 | 0x8000 | `Hwr1859Dsp*`; `HRLib`; `Memory Stick` |
| `Sony-Clie-NR70v.rom` | 8 MB | **SZ328** | 0x10000000 | PC `0x100002a2` | file `0x10000`, bigROMOffset `0x10010000` | 0 | 0x10000 | `sonyrdwd` (Redwood); `320x480 pixel color`; `Flip-and-Rotate` |

Note the layouts are per-model: S300/T400/NR70V are **whole-flash** dumps
(small ROM at 0, big ROM after), whereas N700C is a **big-ROM-only** dump
(the `rom_load_offset`/`bigrom_offset` split already exists in
`PalmMachineClass` precisely to express this — cf. the m515 comment at
`hw/m68k/palm.c:571-580`).

### 7.3 Licensing reality
These are copyrighted Sony/Palm firmware images, never formally released
for redistribution; they circulate as abandonware and Sony *did* ship
the POSE ROMs to registered developers (which is what the `jp.sony.clie`
archive preserves).  Same status as the Palm ROMs already used by this
tree — usable for development/testing, not redistributable in the repo.
Keep CLIÉ ROMs alongside the existing set in `/workspace/src/palm-roms/`
(gitignored), as `PALM-NOTES.md` does.

---

## 8. Reference source index

* Existing Palm/DragonBall bring-up notes: `PALM-NOTES.md` (this tree).
* Cloudpilot-emu (POSE-derived, supports the m68k CLIÉs) — the primary
  register/GPIO/codename reference:
  <https://github.com/cloudpilot-emu/cloudpilot-emu>, directory
  `src/cloudpilot/emulator/hardware/clie/` (files: `EmRegsEzPegS300`,
  `EmRegsEzPegS500C`, `EmRegsVZPegVenice`, `EmRegsVZPegN700C`,
  `EmRegsVZPegNasca`, `EmRegsVZPegModena`, `EmRegsVZPegYellowStone`,
  `EmRegsSZNaples`, `EmRegsSZRedwood`, `EmRegsLCDCtrlT2`,
  `EmRegsSonyDSP`, `EmRegsFMSound`, `EmRegsUsbCLIE`,
  `MemoryStickStructs.h`, `EmSonyXZWithSlot.h`), plus
  `src/cloudpilot/emulator/EmDevice.cpp` for the device table.
* MC68SZ328 register map: Cloudpilot `EmRegsSZ.h`/`EmRegsSZRedwood.cpp`;
  uClinux `arch/m68k/include/asm/MC68SZ328.h`; Freescale/Motorola
  `MC68SZ328UM` user's manual (NXP/datasheet archives).
* ROM archives: `https://archive.org/details/jp.sony.clie` and archive.org
  item `20250707_20250707_0134` (PalmDB mirror).
* Model specs: Wikipedia *Sony CLIÉ* pages; clie.info.

---

## 9. Proof-of-concept status

No PoC worktree was built this session (disk is at ~2 GB free with another
agent building concurrently, and the coordinator's guidance was to
prioritise the writeup and avoid new builds).  All the parameters a PoC
needs are in §5 (Phase 1) and §7: a `clie-s320` machine is a
`palm.c` `PalmMachineClass` variant with `rom_base=0x10000000`,
`rom_load_offset=0`, `bigrom_offset=0x8000`, EZ chip ID, the 160×160 grey
LCDC, plus two no-fault stub regions (Memory Stick "no card", Jog Dial
GPIO).  That is the recommended first commit when build capacity frees up.
