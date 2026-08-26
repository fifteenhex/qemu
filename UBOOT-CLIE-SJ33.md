# Bootstrapping U-Boot on the Sony CLIÉ PEG-SJ33

Research + design document. No code was built for this task (disk-constrained,
concurrent builders); everything below is grounded in this tree's existing
`CLIE-RESEARCH.md`, the `oldmac-rebase` U-Boot m68k port, the `linux-m68kdt`
nommu kernel tree, and primary-source DragonBall/U-Boot documentation cited
inline.

---

## 1. Executive summary + recommended path

**The target:** PEG-SJ33 = Motorola **MC68VZ328 "DragonBall VZ"**, an
**MC68EC000 core** (plain 68000 instruction set, no MMU, **no VBR** — the
exception vector table is hard-wired at physical address 0) clocked at
33 MHz, 16 MB RAM, NOR flash at `0x10000000`, 320×320 16-bit color via a
**Sony companion display controller** (not the on-chip DragonBall LCDC),
Palm OS 4.1. Cloudpilot codename `VZPegYellowStone`/`Modena` (per
`CLIE-RESEARCH.md` §2.1, unconfirmed against `EmDevice.cpp`).

**Verdict: U-Boot does not exist for this CPU family today, porting it is a
real but bounded project (roughly SPL-sized: a new `arch/m68k` CPU class +
~4 drivers + a board file), and the right place to do all of it first is
**QEMU**, not the physical device.** Concretely:

1. **U-Boot's m68k support is ColdFire-only upstream.** The `oldmac-rebase`
   tree already proves this by *adding* a from-scratch classic-680x0 CPU
   class (`arch/m68k/cpu/m680x0/`, `M680x0` Kconfig entry) that upstream
   U-Boot does not have (§4.1). That new code targets 68030/68040 (full
   MMU CPUs, `movec`/`pmove`, copyback or writethrough caches). The
   DragonBall's 68000-core/no-MMU/no-cache profile is **architecturally
   simpler** than what `oldmac` already built (§4.2) — there is no MMU
   teardown, no TT/DTT registers, and likely no cache to flush at all
   (the plain 68000 has no CACR). The hard parts are elsewhere: GCC/GAS
   `-mcpu=68000` code-generation correctness for U-Boot's `-fPIC` GOT
   relocation model (§4.4, risk), and four new from-scratch MMIO drivers
   (UART, timer, NOR flash, optionally the Sony color LCDC).
2. **On real hardware, the DragonBall mask ROM has a genuine UART
   "Bootstrap Mode"** (Ch. 17 of the `MC68VZ328UM`) that downloads and
   executes code over a UART with no flash content required at all — in
   principle the ideal way to get *anything* (a JTAG-free U-Boot SPL)
   running on bare SJ33 silicon. But entry requires a hardware mode-select
   condition sampled at reset that reference boards expose via a DIP
   switch; **whether Sony wired that condition to anything user-accessible
   on the SJ33 PCB is unknown and must be checked with a multimeter/logic
   analyzer against the primary MC68VZ328UM PDF before it can be relied
   upon** (§3.1, open question).
3. **The historical precedent for running foreign code on a DragonBall
   Palm — including full uClinux — is not a boot-time mechanism at all.**
   It is **PalmLoader**, a Palm OS `.prc` application that runs *under*
   Palm OS, escapes the emulated-68k sandbox via `PceNativeCall`, copies a
   kernel image (chunked into ≤64 KB Palm database records) into RAM, and
   jumps to it directly — i.e. Palm OS itself is the bootstrap loader
   (§3.2). This is the lowest-risk way to get *code* running on a real
   SJ33 today (no flash reflash, no hardware access needed, fully
   reversible), but it cannot host a *bootloader* in the conventional
   sense (nothing runs before Palm OS) — see §3.2 for exactly what it can
   and cannot substitute for.
4. **Reflashing the Palm OS ROM with U-Boot is the only way to make U-Boot
   the actual first thing that runs on real SJ33 hardware**, and it is
   high risk (§3.3): no confirmed in-tree flash-write path, no known
   external recovery path (no confirmed bootstrap-mode fallback — see
   point 2), and the SJ33 ROM's exact big-ROM layout is not yet verified
   in this tree (§2.3).
5. **QEMU is therefore where "bootstrap U-Boot on the SJ33" should
   actually happen first**, and it is cheap: this tree already emulates
   the entire non-display DragonBall VZ SoC (INTC, GPIO, timer, UART, PLL,
   SCR, RTC, PWM — `hw/*/dragonball_*.c`), already has a working
   680x0-class U-Boot port to crib from (`oldmac-rebase`), and — critically
   — QEMU's `palm_cpu_reset()` **already bypasses real chip-select
   emulation** (`hw/m68k/palm.c:181-195`), so a U-Boot image needs no
   chip-select-programming code at all to boot under QEMU, even though
   real silicon does (§3.1, §5).

**Recommended path, phased (detail in §5):**

| Phase | What | Where |
|---|---|---|
| 0 | New `clie-sj33` (or generic `dragonball-vz328`) QEMU machine variant: VZ328 SoC (already modelled) + a raw-binary boot mode that loads a flat image at `rom_base` and reads SP/PC from its first two longwords (not the Palm big-ROM card-header convention) | QEMU, this tree |
| 1 | New `arch/m68k` CPU class `m68000` in a U-Boot fork (sibling of `oldmac-rebase`'s `m680x0`): reset vectors at RAM 0, no MMU/cache code, `-mcpu=68000` build | U-Boot fork |
| 2 | Board port `board/sony/clie/`: UART console driver, PIT/timer driver, and enough SCR/GPIO poke-and-ignore to reach a prompt — **serial console only, no framebuffer** | U-Boot fork, boots under the Phase-0 QEMU machine |
| 3 | `bootelf`/raw-image boot of the `linux-m68kdt` nommu kernel from U-Boot, proving the same "load to RAM, set SP/PC, jump" contract PalmLoader already uses (§3.2, §4.3) | QEMU |
| 4 | Real hardware: attempt UART Bootstrap Mode first (§3.1) to load Phase-2 U-Boot into RAM with **zero flash writes** and validate the drivers against real silicon; only after that succeeds, consider a NOR reflash to make U-Boot the persistent first-stage loader | Real SJ33, high risk, last |

Reusing an existing DragonBall/uClinux bootloader (PalmLoader, or a
hand-written UART-bootstrap-mode stub) is the **pragmatic alternative** to
a full U-Boot port, and is explicitly not mutually exclusive — §4.5 gives
an honest recommendation: build the U-Boot port (it is genuinely not much
harder than what `oldmac` already did, and the user has stated interest in
U-Boot specifically), but use PalmLoader as the **delivery vehicle** for
getting the first U-Boot build onto real hardware RAM, rather than trying
to reflash on day one.

---

## 2. SJ33 hardware / boot / flash facts

### 2.1 SoC identity and address map

From `CLIE-RESEARCH.md` §2.1/§3.1 and confirmed against the DragonBall
register map (uClinux's `MC68328.h`
<https://github.com/spotify/linux/blob/master/arch/m68k/include/asm/MC68328.h>,
cross-checked against this tree's `hw/m68k/palm.c` `PALM_MMIO_*` constants,
which match exactly):

| Block | Base | Source |
|---|---|---|
| SCR (System Control Register, chip/mask ID) | `0xfffff000` | `MC68328.h`; `hw/m68k/palm.c:77` |
| PLL (PLLCR/PLLFSR) | `0xfffff200` | `MC68328.h`; `palm.c:78` |
| Chip-select group A (CSA0-3) | `0xfffff110`–`0xfffff11c` | `MC68328.h` |
| Chip-select group B (CSB0-3) | `0xfffff120`–`0xfffff12c` | `MC68328.h` |
| Chip-select groups C/D (CSC/CSD) | `0xfffff130`–`0xfffff14c` | `MC68328.h` |
| INTC (ICR/IMR/ISR/IPR/IVR) | `0xfffff300` | `MC68328.h`; `palm.c:79` |
| GPIO ports A–M | `0xfffff400`+ | `MC68328.h`; `palm.c:80` |
| PWM | `0xfffff500` | `palm.c:81` |
| Timer 1 / Timer 2 | `0xfffff600` / `0xfffff610` | `palm.c:82-83` |
| SPI1 (VZ) | `0xfffff700` | `palm.c:108` |
| SPI master | `0xfffff800` | `palm.c:84` |
| UART1 / UART2 | `0xfffff900` / `0xfffff910` | `MC68328.h`; `palm.c:85-86` |
| LCDC (on-chip, mono/grey — **not** what SJ33 uses for its color panel) | `0xfffffa00` | `MC68328.h`; `palm.c:87` |
| RTC | `0xfffffb00` | `palm.c:88` |

All internal registers live in the fixed `0xfffff000`–`0xffffffff` window,
independent of chip-select programming (this is the DragonBall's "EMU/IO
space", always decoded regardless of CS state).

NOR flash / ROM: `rom_base = 0x10000000` (the address the CLIE-family
big-ROM cards are conventionally mapped to once boot software reprograms
the chip selects — `CLIE-RESEARCH.md` §7.2 confirms this for the T400 and
N700C ROM dumps, the closest confirmed VZ328/Modena-family relatives to
the SJ33). RAM sits at `0x00000000` in the final memory map.

### 2.2 The boot sequence — the CSA0-to-RAM-remap two-stage dance

All 68000-family CPUs fetch the initial SP (long at address 0) and PC
(long at address 4) directly from physical memory at reset — this predates
VBR, and the DragonBall's MC68EC000 core has **no VBR at all**
(confirmed: DragonBall's Interrupt Vector Register, `IVR` at `0xfffff304`,
only relocates the *upper 5 bits* of autovectored peripheral interrupt
numbers — it is not a general vector-base relocation register; genuine VBR
was added in the 68010 and is absent from 68000/68EC000). This means the
**exception vector table (1 KB, 256 × 4-byte vectors) must always live at
physical address 0** for any OS or bootloader running on this chip,
DragonBall or not.

DragonBall's answer to "how does flash get to address 0 for the reset
fetch, when RAM also needs to be at 0 afterwards for a writable vector
table" is the documented **two-stage chip-select remap**, described for
the sibling MC68328/EZ328 in the Flameman/m105 write-up
(<https://elinux.org/Flameman/m105>) and in the `MC68EZ328UM`
(<https://www.nxp.com/docs/en/reference-manual/MC68EZ328UM.pdf>) System
Memory Map / Chip-Selects chapter:

1. **Out of reset**, chip-select group A's CSA0 is configured by hardware
   default to answer the **entire address space**. The boot flash is
   wired to CSA0, so the CPU's SP/PC fetch at address 0/4 is satisfied by
   the flash, wherever in the physical map the flash device actually sits
   — flash aliases through every address, including 0.
2. **Early boot code** (running out of that aliased flash) reprograms
   CSA0's base/mask registers (`0xfffff110`) to their final, narrow
   address range (`0x10000000` on the CLIÉ family), consistent with the
   PC it is already executing at (the alias must include the final
   address or the CPU "won't notice the change", per the Flameman
   write-up).
3. Boot code then programs the RAM chip-select group to claim address
   `0x00000000`, at which point RAM — not flash — answers vector fetches,
   and the OS/bootloader can install its own vector table there.

**This tree's QEMU model deliberately does not emulate this dance.**
`palm_cpu_reset()` (`hw/m68k/palm.c:181-195`) reads SP/PC directly out of
the ROM image at `rom_base + bigrom_offset` (the Palm "big-ROM card
header" convention — see §2.3) rather than modelling CSA0/RAM
chip-select registers at all; the comment at `palm.c:186-191` says so
explicitly: *"On silicon, CSA0 answers the whole address space out of
reset so the reset vectors are fetched from the flash; the big-ROM card
header carries them. Load them by hand instead of modelling that
aliasing."* No `hw/*/dragonball_*.c` file implements CSA/CSB/CSC/CSD
chip-select registers at all — accesses to `0xfffff110`-`0xfffff14c` on
this tree's Palm/CLIÉ machines simply hit unclaimed MMIO
(`ignore_memory_transaction_failures = true`, `palm.c:451`, per
`CLIE-RESEARCH.md` §6, so they read-as-zero/write-ignored rather than
faulting).

This has a direct, useful consequence for U-Boot bring-up (§5, Phase 0):
**a U-Boot image under this QEMU model does not need to actually
reprogram chip selects to boot** — the machine model already places RAM
at 0 and ROM at `rom_base` unconditionally. Real hardware does need the
two-stage dance; QEMU sidesteps it exactly the way it sidesteps Mac
NuBus/VIA slot-timing on `oldmac`.

### 2.3 ROM / flash layout — what's confirmed for SJ33 vs. what isn't

`CLIE-RESEARCH.md` §7.2 confirms the "Palm big ROM" card layout (a card
header carrying the `FEEDBEEF` signature + `"PalmCard"` string + a
`bigROMOffset` field at header+0x68, from which reset SP/PC are read) for
four *other* ROM dumps: `S300en.rom` (EZ328), `PEG-T400` (VZ328,
`sonyvnce`/Venice), `PEG-N700C` (VZ328, `VZPegN700C`), and
`Sony-Clie-NR70v.rom` (SZ328, `sonyrdwd`/Redwood). **No SJ33 ROM dump has
been header-inspected in this tree yet** — `CLIE-RESEARCH.md`'s SJ33 row
(§2.1) is entirely marked "≈" (inferred), including its Cloudpilot
codename (`VZPegYellowStone`/`Modena`), its RAM size (16 MB), and its
audio path.

Given the family precedent — both confirmed VZ328 layouts (T400, N700C)
use `bigrom_offset = 0x8000` despite differing in whether the small ROM
occupies file offset 0 (T400: whole-flash dump, `rom_load_offset = 0`) or
is absent (N700C: big-ROM-only dump, `rom_load_offset = 0x8000`) — the
SJ33 is very likely one of these same two shapes, base `0x10000000`,
`bigrom_offset = 0x8000`. **This must be verified by inspecting an actual
SJ33 ROM dump's header before wiring a `clie-sj33` machine or targeting
real SJ33 flash**, exactly the technique `CLIE-RESEARCH.md` §7.2 already
used for the other four models. No SJ33 ROM has been sourced via the
`jp.sony.clie` archive.org item per that document either — this is a
prerequisite research step, not something this report can resolve without
a ROM in hand.

### 2.4 Display path (relevant to what U-Boot needs to drive, if anything)

Per `CLIE-RESEARCH.md` §2.1 and §4.2, the SJ33's 320×320 16-bit color
panel is **not** driven by the on-chip DragonBall LCDC at `0xfffffa00` —
that block only does grey/mono HiRes on the T-series. The SJ33's color
path goes through a **Sony companion display controller**
(`EmRegsLCDCtrlT2` in Cloudpilot), a completely separate, currently
unmodelled device on a chip-select region. This means:

* A U-Boot **serial console** on this device is nearly free once the
  DragonBall UART driver exists — reuses `hw/char/dragonball_uart.c`
  (already modelled) unchanged.
* A U-Boot **video console** would require porting the Sony LCDC
  register set from scratch on both the QEMU side (a new display device,
  `CLIE-RESEARCH.md` §4.2, Tier B/Phase 2 work — not done yet) and the
  U-Boot driver side. **Recommendation: skip video entirely for the
  U-Boot port; serial console only**, matching the priorities of this
  report's phased plan (§5) and consistent with `oldmac`'s own bring-up
  order (serial first, video was added later as commit
  `board: apple: oldmac: add framebuffer video console` well after the
  SCSI/serial boot chain worked, per `REBASE-REPORT.md`'s commit log).

---

## 3. Bootstrap-mechanism survey

### 3.1 DragonBall VZ328 UART Bootstrap Mode (mask-ROM serial download)

**This is real and documented**, not folklore. The `MC68VZ328UM`
("MC68VZ328 Integrated Processor User's Manual", NXP/Freescale/Motorola,
Rev. 0, 02/2000) has a dedicated **Chapter 17, "Bootstrap Mode"**
(<https://www.nxp.com/docs/en/user-guide/MC68VZ328UM.pdf>, mirrored at
<https://docs.rs-online.com/6394/0900766b801cf944.pdf>; a summary is also
excerpted at
<https://www.manualslib.com/manual/106699/Motorola-Mc68vz328.html?page=307>).
What it does and how, per that chapter:

* **Entry:** the chip is placed into Bootstrap Mode by a hardware
  mode-select condition sampled at reset, rather than by anything a
  program running on the chip can request. On the reference
  **M68VZ328ADS** development board this is DIP switch **S2-8**, held on
  through a `RESET` press (`M68VZ328ADSUM`,
  <https://www.nxp.com.cn/docs/en/reference-manual/MC68VZ328ADSUM.pdf>).
  **The exact silicon-level pin/condition this corresponds to (which pin,
  what level, sampled on which reset edge) is documented in the same
  manual chapter but was not extracted verbatim in this pass** — reading
  the primary PDF chapter directly is a required next step before relying
  on this path (§6, open question). It is plausible, given other DragonBall
  chips of this era, that the condition is tied to one of the boot-related
  pins (e.g. a bus-width/`DTACK`-class pin) that consumer products
  typically hard-strap for normal operation and do **not** expose on a
  header — this must be checked against a real SJ33 board/schematic
  before assuming it is reachable without desoldering anything.
* **Protocol:** once in Bootstrap Mode, the chip listens on **UART1 or
  UART2** (auto-detected). The **first character received is not data**
  — it is used purely to let the chip measure the incoming baud rate and
  thereby determine whether the reference crystal is 32.768 kHz or
  38.4 kHz, and which UART is in use.
* **Data format:** a proprietary "bootstrap record" framing, not raw
  binary or S-records directly — Motorola shipped a DOS tool, **STOB.EXE**
  ("S-record TO Bootstrap"), to convert a Motorola S-record file into the
  bootstrap-record stream the mask ROM expects.
* **Capability:** bootstrap mode can (a) write arbitrary values to any
  on-chip register (registers are memory-mapped, so "download to memory"
  covers register init too — addressable up to `0xFFFFC0`-class offsets
  per the manual text found), (b) download a program/data block to system
  RAM, and (c) **execute** downloaded code — i.e. this is a genuine
  "load and jump" primitive implemented in mask ROM, requiring no flash
  content and no working OS at all.
* **Errata caveat:** the MC68VZ328 chip errata document
  (<https://www.nxp.com/docs/en/errata/MC68VZ328CE.pdf>) notes at least
  one EMU/bootstrap-adjacent quirk (some systems cannot start in EMU mode
  because that signal is reserved for emulation/debug-monitor hardware) —
  worth re-checking against the specific SJ33 mask/revision before
  hardware attempts.

**Why this matters for U-Boot:** if the mode-select condition *is*
reachable on the SJ33 (even via a bodge wire to a test point), Bootstrap
Mode is the **cleanest real-hardware bring-up path for a from-scratch
U-Boot port** — it lets you download and run a U-Boot SPL/binary in RAM
repeatedly, with zero flash writes, zero risk of bricking, and a fast
edit-flash-reset-retry-free iteration loop, exactly analogous to how a
JTAG debugger would be used on a board with one. This should be
investigated (schematic/continuity trace on a real SJ33) **before** any
flash-reflash attempt (§3.3).

### 3.2 The uClinux-on-Palm precedent: PalmLoader

This is the actual, historically-used mechanism for running foreign
kernel code (uClinux) on DragonBall Palm devices, and it is **not** a
boot-time/mask-ROM mechanism — it runs entirely *after* Palm OS has
already booted:

* **What it is:** `PalmLoader` is a Palm OS `.prc` application,
  distributed with uClinux for 68k DragonBall Palms (Palm Linux
  Environment project, <https://palm-linux.sourceforge.net/>; also listed
  on PalmDB, <https://palmdb.net/app/uclinux>). Original authors credited
  as Jeff Dionne and David McCullough (Lineo); build `uClinuxPalm.prc`
  combined PalmLoader with a uClinux 2.0.38 kernel + minimal filesystem.
* **How it works (mechanism, from the SourceForge project page and
  corroborating summaries — the loader's own source was not directly
  located in this pass, see below):**
  1. The kernel image (plus filesystem, e.g. `romfs`) is shipped as one or
     more Palm OS **database records**, because a single Palm OS database
     cannot exceed **64 KB** — the kernel image is split into ≤64 KB
     chunks at build/packaging time and reassembled by the loader at
     runtime.
  2. On launch, `PalmLoader` reassembles the chunks into a contiguous
     buffer in RAM.
  3. It then must **escape the Palm OS 68k execution sandbox**: Palm OS
     application code that targets ARM-based (PalmOS 5, "Handheld Engine")
     devices runs under 68k emulation (PACE), so getting native 68k
     control requires `PceNativeCall` — on m68k DragonBall Palm OS this
     specific API is not needed the same way (the CPU already *is* 68k
     natively), but the same class of "drop out of the OS/managed
     environment into raw code" step is what's referenced across the
     writeups; on the classic DragonBall devices this is effectively
     achieved by taking full control of the processor (masking interrupts,
     disabling the OS's own vector table use) rather than by PACE — treat
     the exact mechanism as **needing verification against loader source**
     before relying on details here.
  4. The reassembled image is jumped into directly: **read SP from the
     image's first long, PC from its second** (the loader must interpret
     the raw kernel image's own embedded vector table — exactly the
     `e_vectors` table this tree's `linux-m68kdt/arch/m68k/68000/romvec.S`
     builds: `.long CONFIG_RAMBASE+CONFIG_RAMSIZE-4, _start, buserr, trap,
     ...` — confirmed present in that file, §4.3), set A7/PC accordingly,
     and transfer control. This is a **software** "load to RAM, read
     SP/PC from offset 0/4, jump" contract — the exact same contract
     `bootelf`/ELF-entry booting uses in `oldmac` (`m68k: hand off to a
     Linux/m68k kernel from bootelf`, per `REBASE-REPORT.md`'s commit
     log), just without the ELF header.
  5. A serial terminal at **9600 baud** connected to the cradle/HotSync
     port is the only I/O — the classic instructions are: HotSync the
     `.prc`, disable HotSync, wire up a serial terminal, launch
     PalmLoader, watch kernel boot messages, land at either a shell prompt
     or a fatal exception (per the SourceForge/PalmDB usage
     instructions).
* **A device-specific example exists for a VZ328 machine close to the
  SJ33's silicon:** a developer (Rui He) ported/rebuilt PalmLoader +
  uClinux 2.0.39 for the **Palm m500** (also MC68VZ328, 33 MHz — same CPU
  family, different DRAM technology (SDRAM vs. EDO) and UART wiring
  (m500 cradle on UART2, IR on UART1) than the earlier EZ328 devices);
  this is documented as `uClinux_Palm_m500.prc` on the Palm Linux
  Environment site. This is strong precedent that the same technique
  transfers to VZ328-class Sony devices (the SJ33 is VZ328-family too),
  modulo re-deriving the exact RAM/UART wiring for the SJ33 the way Rui He
  did for the m500.
* **Source code:** the PalmLoader stub's own source was **not located** in
  this pass — the SourceForge project page references "a CVS of
  PalmLoader coming soon" (dated 2001) and a "palm-linux CVS on
  sourceforge", but this pass did not reach working source. **Recommended
  next step:** check the SourceForge CVS/SVN history directly (via
  SourceForge's legacy CVS web viewer or an `rsync`/`cvs` checkout of
  `palm-linux`), and separately check `rhuitl/uClinux` on GitHub
  (<https://github.com/rhuitl/uClinux>, described as "uClinux plus my own
  patches, based on uClinux-dist-20110810") for a `user/` or `vendor/`
  tree containing a PalmLoader stub, before writing one from scratch.

**What PalmLoader can and cannot substitute for, relative to "bootstrap
U-Boot":**

* **Can:** be the delivery vehicle to get a from-scratch **U-Boot binary**
  (or a raw kernel) into RAM and running on real SJ33 hardware, with
  Palm OS still resident in flash, fully reversible, no hardware access
  needed beyond a serial cable. This is a legitimate **Phase 4** vehicle
  (§5) for validating a U-Boot port's drivers against real silicon before
  committing to a flash reflash.
* **Cannot:** replace U-Boot as an actual first-stage bootloader — nothing
  runs "before" Palm OS in this scheme; Palm OS's own ROM boot (which
  U-Boot is not involved in at all) has already completed by the time
  PalmLoader/any loaded payload gets control. If the goal is "U-Boot is
  what the SJ33 boots into at power-on," only §3.3 (reflash) achieves
  that.

### 3.3 Flash reflashing (replacing/prepending the Palm OS ROM)

The only way to make U-Boot the actual first code the SJ33 executes at
power-on is to write it into the NOR flash at `0x10000000`, either
replacing the Palm OS ROM outright or prepending a small stub that jumps
to Palm OS if not overridden (dual-boot style, analogous to how the
`oldmac` SPL chooses between a payload and a fallback).

Risks, concretely:

* **No confirmed in-flash-write path exists yet in this tree or in any
  source consulted.** Cloudpilot/POSE emulate the ROM as a file, not as
  writable NOR with a documented flash-command sequence for the SJ33's
  actual flash part (part number not identified in this pass — would need
  extraction from a teardown or from Sony service documentation).
* **No confirmed hardware recovery path.** If Bootstrap Mode (§3.1) turns
  out to be reachable on the SJ33, reflashing is comparatively low-risk
  (a bad flash write is recoverable by reloading a good image via
  Bootstrap Mode). **If Bootstrap Mode is not reachable, reflashing is a
  one-way door with no known recovery mechanism** short of chip-off flash
  reprogramming — this must be resolved (§3.1) before any reflash is
  attempted on a real (presumably irreplaceable) SJ33.
* **ROM layout for SJ33 is not yet confirmed** (§2.3) — writing anything
  to flash before that is settled risks corrupting a device with a
  layout assumption borrowed from a sibling model that turns out to be
  wrong.
* **Licensing/preservation note**, inherited from `CLIE-RESEARCH.md` §7.3:
  the existing Palm OS ROM is Sony/Palm copyrighted firmware, abandonware
  in practice — dumping it *before* any reflash attempt (so it can be
  restored) is both a preservation good and the obvious safety net.

**Recommendation: do not attempt reflashing until (a) Bootstrap Mode
reachability is confirmed as a recovery path, (b) the SJ33's specific ROM
layout is confirmed by header inspection of a real dump, and (c) the
U-Boot port has been fully validated under both QEMU (Phase 0-3) and via
the PalmLoader RAM-load path (Phase 4) first.**

### 3.4 HotSync / debugger-based code download

No evidence was found of a HotSync-conduit-based *native* code-download
mechanism beyond installing `.prc` applications (which is exactly the
PalmLoader vehicle, §3.2) — HotSync's own protocol is a database
sync/backup protocol, not a code-execution channel. No JTAG/BDM header is
documented as present/exposed on the SJ33 in any source consulted in this
pass (unlike, say, development boards); this would need to be checked
against a teardown. Not pursued further as a distinct mechanism — it
collapses into either "PalmLoader" (§3.2) or "physical hardware access,
in which case Bootstrap Mode (§3.1) is the documented, chip-native path"
in practice.

---

## 4. U-Boot-on-VZ328 feasibility + concrete porting plan

### 4.1 Confirmed: U-Boot upstream is ColdFire-only for m68k

Upstream U-Boot's `doc/arch/m68k.rst` documents support for "nearly the
entire range of ColdFire processors" and lists only ColdFire families
(MCF5272, MCF5282, MCF5445x, etc.) — no classic 68000/68328/DragonBall
entry exists (<https://github.com/u-boot/u-boot/blob/master/doc/arch/m68k.rst>).
This is corroborated directly by this tree's own `oldmac-rebase` fork:
its `arch/m68k/Kconfig` (`/workspace/src/oldmac-rebase/arch/m68k/Kconfig`)
has to **add** a brand-new `config M680x0` entry ("classic Motorola 68000
family") alongside the pre-existing `MCF52x2`/`MCF523x`/etc. ColdFire
entries, and a brand-new `arch/m68k/cpu/m680x0/` directory
(`cpu.c`, `start.S`, credited to a 2025 contributor, Kuan-Wei Chiu) that
does not exist upstream. **There is no DragonBall/68000/68328 support in
either upstream U-Boot or this fork today** — a DragonBall port is a new
CPU class, full stop, whether built as a sibling of `m680x0` or from
scratch.

### 4.2 What `oldmac`'s `m680x0` class already solved, and why DragonBall is a subset of it

Reading `arch/m68k/cpu/m680x0/cpu.c` (full file inspected) and
`arch/m68k/cpu/m680x0/start.S`, the 68030/68040 port had to solve, in
order of difficulty:

1. **MMU teardown / transparent translation** — 68040 `movec %dttN`,
   68030 `pmove %tt0/%tt1`, both CPU-detected at runtime via a weak
   `m68k_is_68040()` hook (`cpu.c:60-63`), per `board/apple/oldmac/PORTING.md`'s
   68030-vs-68040 section.
2. **Cache maintenance** — 68040 copyback caches need `cpusha` (push +
   invalidate); 68030 writethrough caches only need a CACR clear
   (`cpu.c:71-88`).
3. **PIE/GOT relocation** — `relocate_code()` (`cpu.c:97-171`) copies the
   monitor to its final high-RAM address and walks `.rela.dyn`
   (`R_68K_32`/`R_68K_RELATIVE`/`R_68K_JMP_SLOT`) to fix up the GOT
   (`%a5`), compiled with `-fPIC`.
4. **Vector table + SPL entry** (`start.S:1-80`) — a static 1 KB vector
   table with `_fault` stubs for everything except reset/SP, `#0x2700`
   into `%sr` to mask interrupts, GD allocation, conditional BSS clear in
   SPL builds.

**The DragonBall MC68EC000 core needs (1) and (2) not at all**: it has no
MMU (nothing to tear down, no TT/DTT registers exist), and the plain
68000/68EC000 core has **no on-chip cache and no CACR** — `flush_cache()`/
`flush_dcache_range()`/`icache_*`/`dcache_*` can all be **empty stubs**,
strictly simpler than even the 68030 path `oldmac` already wrote. Item
(3), the PIE/GOT relocation model, is the one piece that needs explicit
verification rather than being assumed to "just work" on `-mcpu=68000`
(§4.4, flagged as a risk, not a known blocker — GCC's default `-fPIC`
m68k model is GOT/register-indirect via `%a5`, which does not obviously
require 68020+ addressing modes; the compiler-support caveat that *does*
apply to 68000 is `-mpcrel`, a different, non-default relocation model —
see §4.4). Item (4) — vector table at 0, mask interrupts, SPL entry — is
essentially copy-paste from `start.S`, since a DragonBall reset-vector
table (SP + PC + fault stubs, `.fill` out to 0x400) is exactly what
`linux-m68kdt/arch/m68k/68000/romvec.S` already builds for the same core
family (confirmed in this pass, §2.2): `.long
CONFIG_RAMBASE+CONFIG_RAMSIZE-4, _start, buserr, trap, trap, trap, ...`.

### 4.3 Concrete porting plan

Modelled directly on `oldmac`'s file layout (`board/apple/oldmac/`,
`arch/m68k/cpu/m680x0/`, `configs/oldmac_defconfig`):

**New arch code — `arch/m68k/cpu/m68000/`** (sibling of `m680x0`):
- `start.S`: 1 KB vector table (SP/PC + `_fault`×~46, matching the 68000's
  smaller vector set vs. 68030/040's — no MMU/cache-fault vectors to wire
  since there's no MMU/cache), `#0x2700` to `%sr`, GD alloc, BSS clear —
  copy `m680x0/start.S`'s shape, drop nothing CPU-specific since there's
  nothing CPU-specific to drop *in* (68000 has fewer optional vectors, not
  more).
- `cpu.c`: `print_cpuinfo()` → `"CPU: M68VZ328 (DragonBall VZ)\n"`;
  `m68k_flush_caches()`/`flush_cache()`/`flush_dcache_range()`/
  `icache_*`/`dcache_*` → empty stubs (no cache exists); `relocate_code()`
  → reuse `oldmac`'s verbatim (the GOT-relocation loop is CPU-generic,
  not 68040/68030-specific — it is already written against the ELF
  relocation *types*, not the CPU).
- New `arch/m68k/Kconfig` entry: `config M68000` under the `M680x0`-style
  "classic 68000 family" umbrella (or its own top-level choice item,
  mirroring how the Linux `arch/m68k/Kconfig.cpu` distinguishes
  `M68000`/`M68EZ328`/`M68VZ328`/`M68SZ328` from `M68030`/`M68040`).

**New board dir — `board/sony/clie/`** (or `board/motorola/dragonball/`
if kept CPU-generic and specialized later, matching how `oldmac`'s
`board/apple/oldmac/` already spans four Mac models via one model table):
- `clie.c`: SCR chip-ID read (`0xfffff004`/`0xfffff006`, EZ=0x43, VZ=0x56
  per `CLIE-RESEARCH.md` §3.1), RAM-size probe or hard-code 16 MB pending
  §2.3 confirmation, minimal GPIO pokes to keep the Sony HAL-adjacent
  peripherals quiet (not applicable to U-Boot itself — U-Boot doesn't run
  Palm OS's HAL — but *is* relevant if this board also needs to coexist
  with reflashed-alongside Palm OS, or needs to leave hardware in a known
  state before `bootelf`-ing into `linux-m68kdt`).
- `Kconfig` + `MAINTAINERS`, matching `oldmac`'s pattern.
- CS/RAM bring-up code implementing §2.2's two-stage remap **only for the
  real-hardware target**; the QEMU target (§5, Phase 0) does not need it
  since `palm_cpu_reset()` already places RAM/ROM correctly.

**New drivers** (`drivers/serial/`, `drivers/timer/`, `drivers/mtd/`):
1. **UART console driver** — DragonBall UART1/2 at `0xfffff900`/
   `0xfffff910` (USTCNT/UBAUD/URX/UTX per `MC68328.h`, confirmed offsets
   in §2.1's table). This is the single most important driver: everything
   else in the phased plan is validated over this console. Directly
   analogous in scope to `oldmac`'s `serial_scc.c` (Zilog 8530) — this is
   simpler (no clock-generator dance, DragonBall UART is a straightforward
   memory-mapped 16550-adjacent register set).
2. **Timer/PIT driver** — DragonBall Timer 1 at `0xfffff600`
   (`CONFIG_TIMER`, matching how `oldmac`'s `via_timer.c` wraps VIA1 for
   the `UCLASS_TIMER` U-Boot API). Needed for `udelay()`/timeouts, not
   strictly for boot-to-prompt but for anything beyond the bare minimum.
3. **NOR flash driver** — only needed if the flash-write path (§3.3) is
   ever pursued; **not needed** for Phases 0-4 of the phased plan (§5),
   which never write flash. CFI-style if the SJ33's part turns out to be
   a standard CFI NOR (needs confirming — DragonBall boards commonly use
   AMD/Intel CFI parts per the `MC68EZ328UM`'s example memory map, §2.1).
4. **GPIO driver** — only needed if U-Boot itself must read Jog
   Dial/buttons; not required to reach a serial prompt.
5. *(Explicitly out of scope, §2.4)*: the Sony color LCDC driver.

**`configs/clie_defconfig`** — modelled on `oldmac_defconfig`: `CONFIG_M68K=y`,
new `CONFIG_M68000=y` (or equivalent), `CONFIG_TEXT_BASE=0x00000000` (RAM
base, matching `oldmac`'s choice and consistent with §2.2's "vector table
must be at 0" constraint), `CONFIG_DM_SERIAL=y` + the new UART driver,
`CONFIG_TIMER=y` + the new timer driver, boot command using `bootelf`
against a `linux-m68kdt` kernel once that's the target (§5 Phase 3) rather
than SCSI/FAT (there's no block device on this path at all — flash-image
or serial-loaded RAM images only, at least initially).

### 4.4 Risk: GCC/GAS `-mcpu=68000` code generation correctness

`oldmac`'s `relocate_code()` and the whole U-Boot proper build rely on
`-fPIC`, producing `.rela.dyn` entries U-Boot applies itself at runtime
(§4.2 point 3). GCC's default `-fPIC` model for m68k is GOT-indirection
through a register (`%a5`) — this is a data-driven scheme, not an
addressing-mode trick, so there is no a priori reason it requires 68020+.
The specific documented limitation is narrower and different: `-mpcrel`
(direct PC-relative addressing, bypassing the GOT) **requires 68020+ and
currently only supports `-fpic`, not `-fPIC`**
(<https://gcc.gnu.org/onlinedocs/gcc/M680x0-Options.html>) — but U-Boot's
existing scheme does not use `-mpcrel`. **Action before writing new
arch code: build a trivial `-mcpu=68000 -fPIC` U-Boot-shaped translation
unit and `objdump -d` it**, exactly as `PORTING.md`'s own methodology did
for the 68030 case ("disassembling the current `-mcpu=68040` `u-boot`
binary shows the only 68040-only instructions are 3 `cpusha` and 5
`movec`... ordinary compiled C contains none") — confirm no 68010+-only
opcodes leak into ordinary compiled C under `-mcpu=68000` before assuming
the port is purely a matter of writing `cpu.c`/`start.S`. This is a
half-day verification task, not a structural blocker, but it is the one
item in this plan that could force `-mcpu=68010`-baseline code generation
(compatible with the "68000 emulation core" the DragonBall's EC000
implements — need to confirm EC000 accepts 68010 encodings, it likely
does since it's described as a superset silicon variant) rather than true
`-mcpu=68000`, which would still be entirely workable.

### 4.5 Alternative bootloaders — comparison and recommendation

Realistic alternatives to a from-scratch U-Boot port, and why this report
still recommends U-Boot:

| Option | Pros | Cons |
|---|---|---|
| **PalmLoader-style RAM loader** (write a tiny custom stub, `.prc`, or bare-metal Bootstrap-Mode payload that just loads-and-jumps a Linux kernel directly) | Smallest possible code; matches the historical uClinux-Palm precedent exactly (§3.2); fastest to a booted `linux-m68kdt` kernel | Not U-Boot — no `env`, no `bootm`/`bootelf` command shell, no USB/network/filesystem boot menu, nothing reusable for a *second* payload without editing the loader; doesn't advance the user's stated interest in U-Boot specifically |
| **`colilo`-class ColdFire uClinux bootloaders** | N/A — **not applicable**: `colilo` and similar (`dBUG`, u-boot-v1's ColdFire support) all target **ColdFire**, not classic 68000/DragonBall; there is no direct "uCbootstrap" project found in this pass that targets the 68000-core DragonBall specifically (uClinux-dist's DragonBall support boots via PalmLoader or via XIP-in-flash + `romvec.S`, not via a distinct third-party bootloader binary) | Wrong CPU family; would need the same from-scratch DragonBall arch work U-Boot needs anyway, with none of U-Boot's existing driver-model/command infrastructure to build on |
| **Full U-Boot port** (this report's recommendation) | Reuses `oldmac`'s proven 680x0-family patterns (relocation, SPL framework, driver model) almost directly (§4.2); gives `env`, `bootm`/`bootelf`, a command shell, and a real QEMU-verifiable regression target (§5) exactly like `oldmac` has for the Quadra 800; matches the user's demonstrated preference (they already built and are extending the `oldmac` U-Boot port) | New CPU class + 3-4 new drivers is real work; nothing to build on for the *DragonBall-specific* pieces since no prior U-Boot DragonBall port exists anywhere to crib from (unlike `oldmac`, which could at least look at how Linux/m68k Mac support shaped its driver choices) |

**Recommendation: build the U-Boot port, but treat §3.2 (PalmLoader) as
the Phase-4 hardware-validation *vehicle* for it, not as a competing
deliverable.** The DragonBall arch/driver work is genuinely smaller than
what `oldmac` already shipped (§4.2), the QEMU side is nearly free (§5
Phase 0 reuses six already-modelled SoC blocks), and it directly serves
the user's actual documented interest (an m68k U-Boot builder who already
has a working 680x0 port and asked specifically about *their* PEG-SJ33).
A bespoke PalmLoader-only stub would be faster to a booted kernel by a
few days, but throws away everything U-Boot gives for free (env,
scriptable boot, a second target OS later) and does not build toward
anything reusable.

---

## 5. Phased plan: QEMU first, then real hardware

### Phase 0 — `clie-sj33` QEMU machine variant with a raw-boot mode (cheap)

- Add a `PalmMachineClass` variant (`CLIE-RESEARCH.md` §3.1's existing
  parameterization: `rom_base`, `sysclk`, `chip_id=0x56` (VZ), `gpio_ports`,
  `has_timer2`) — this is almost entirely data, no new device files,
  exactly as `CLIE-RESEARCH.md` §5 Phase 0 already scoped for Palm-OS
  CLIÉ bring-up generally.
- **New:** a "raw firmware" boot mode alongside the existing Palm
  big-ROM-card reset convention (`palm_cpu_reset()`, `palm.c:181-195`):
  when the `-bios`/firmware image is a bare U-Boot binary (no `FEEDBEEF`
  card header), read SP/PC from **offset 0/4 of the image itself**
  (standard embedded convention — exactly what `arch/m68k/cpu/m68000/
  start.S`'s own `_vectors` table will contain, §4.3) rather than from
  `rom_base + bigrom_offset`. This can be a machine property
  (`-machine clie-sj33,firmware-mode=uboot`) or simply a second machine
  type: `dragonball-vz328-raw` for bring-up, `clie-sj33` for Palm OS.
- Confirm boot reaches U-Boot's `start.S` entry with no chip-select code
  needed at all (§2.2's key QEMU-vs-hardware simplification).

### Phase 1 — U-Boot boots to a serial prompt under QEMU

- `arch/m68k/cpu/m68000/` (§4.3) + `board/sony/clie/` (or a
  `board/motorola/dragonball/` shared base) + the UART driver (§4.3 item
  1) + timer driver (§4.3 item 2).
- Success criterion, directly analogous to `REBASE-REPORT.md`'s own
  `oldmac` q800 boot-log evidence: a U-Boot banner + `=>` prompt over the
  QEMU serial console, e.g.
  ```
  U-Boot SPL 20XX... (DragonBall VZ328)
  U-Boot 20XX...
  CPU:   M68VZ328 (DragonBall VZ)
  Board: Sony CLIE PEG-SJ33
  DRAM:  16 MiB
  =>
  ```
- This is the point at which "U-Boot boots on the CLIÉ SJ33" first
  becomes literally true, in emulation.

### Phase 2 — boot a `linux-m68kdt` nommu kernel via U-Boot

- Use `bootelf` (if `linux-m68kdt`'s build produces an ELF `vmlinux`) or a
  raw-image `go`/custom command reading SP/PC from the image's own
  `romvec.S`-style header (§2.2, §3.2) — this is the exact "load to RAM,
  read SP/PC from offset 0/4, jump" contract PalmLoader already
  implements for uClinux on real Palm hardware, so U-Boot doing it too is
  low-risk and reuses `oldmac`'s own precedent (`m68k: hand off to a
  Linux/m68k kernel from bootelf`).
- Confirms the whole point of having U-Boot here at all: it's a real
  bootloader for a real (if research-stage) kernel target, not just a
  monitor prompt.
- This is also where the user's other in-flight nommu work (GCC unaligned
  access on 68000, nommu FLAT userspace) gets its first real boot
  vehicle on this specific SoC.

### Phase 3 — QEMU regression harness

- Wire a `clie-sj33` (or `dragonball-vz328`) boot smoke test into
  whatever this tree already uses for the `oldmac`/q800 regression check
  (`REBASE-REPORT.md` §2's "QEMU q800 smoke test" pattern) — timeout-bounded
  boot-to-prompt check, so future DragonBall driver changes can't
  silently break the boot chain, exactly as PORTING.md flags the Quadra
  800 as "the regression guard" for the 680x0 CPU work.

### Phase 4 — real SJ33 hardware, via PalmLoader first

- Before touching flash: attempt to load the Phase-1 U-Boot SPL/binary
  into real SJ33 RAM via **either** DragonBall Bootstrap Mode (§3.1, if
  confirmed reachable) **or** a PalmLoader-style `.prc` (§3.2, always
  available, no hardware risk) and validate the UART/timer/SCR drivers
  against real silicon, iterating without ever writing flash.
- Only after the drivers are validated on real hardware via Phase 4's
  RAM-load path should a persistent NOR reflash (§3.3) even be
  considered, and only with a full ROM backup and a confirmed recovery
  path in hand.

---

## 6. Risks + open questions

| # | Risk/question | Why it matters | How to resolve |
|---|---|---|---|
| 1 | Whether Bootstrap Mode's hardware entry condition is reachable on a stock SJ33 PCB | Determines whether real-hardware bring-up (Phase 4) has a safe, repeatable RAM-load path or only the one-way PalmLoader/reflash options | Read `MC68VZ328UM` Ch. 17 in full for the exact pin/level; trace the SJ33 board/schematic (teardown) for that signal |
| 2 | SJ33's exact big-ROM layout (`rom_load_offset`/`bigrom_offset`, whole-flash vs. big-ROM-only) is unconfirmed | Any QEMU `clie-sj33` Palm-OS machine, and any real-hardware reflash, needs this exactly right | Source an actual SJ33 ROM dump (`jp.sony.clie` archive.org item per `CLIE-RESEARCH.md` §7.1) and header-inspect it per §7.2's method |
| 3 | `-fPIC`/`-mcpu=68000` code-generation correctness for U-Boot's relocation model | Could force a `-mcpu=68010` baseline instead of true 68000, or (worst case) a different relocation strategy | Build-and-disassemble check (§4.4), half a day |
| 4 | SJ33 RAM size (16 MB, marked "≈" in `CLIE-RESEARCH.md`) and exact Cloudpilot codename/GPIO pin map (`VZPegYellowStone`/`Modena`, also "≈") are unconfirmed | Affects `CONFIG_SYS_SDRAM_SIZE`-equivalent sizing and any GPIO-poking board code | Cross-check Cloudpilot's `EmDevice.cpp` + `EmRegsVZPegYellowStone.cpp`/`EmRegsVZPegModena.cpp`, per `CLIE-RESEARCH.md` §6's own open item |
| 5 | No confirmed NOR flash part number/CFI compliance for SJ33 | Blocks §3.3 (reflash) and the optional NOR driver (§4.3 item 3) entirely until known | Teardown / Sony service documentation; not needed for Phases 0-4 |
| 6 | PalmLoader's actual source was not recovered in this pass | Currently only the *usage* and *general mechanism* are sourced (§3.2), not the exact escape-the-emulator/jump code | Pull `palm-linux` SourceForge CVS history directly; check `rhuitl/uClinux` GitHub mirror's `user`/`vendor` trees |
| 7 | No confirmed in-flash-write path/tooling exists for this specific NOR part | Blocks §3.3 until resolved | Depends on risk #5 |
| 8 | UART1-vs-UART2 cradle wiring for the SJ-series (VZPegYellowStone/Modena) is unconfirmed | Determines which UART the U-Boot console driver should default to | Trace `EmRegsVZPegYellowStone.cpp`/`EmRegsVZPegModena.cpp` `GetPortInternalValue`, the same technique `CLIE-RESEARCH.md` §4.4 already prescribes for Jog Dial pins |
| 9 | Whether the DragonBall's MC68EC000 core accepts pure `-mcpu=68000` GAS encodings or needs `-mcpu=68010`-superset assumptions in hand-written `.S` (e.g. `MOVEC`-family instructions used nowhere here, but worth a sanity check) | Low risk given §4.2's analysis (no MMU/cache code needed at all), but should be confirmed alongside risk #3 | Same build-and-disassemble check |

---

## Source index

- This tree: `CLIE-RESEARCH.md` (device taxonomy, ROM layout, register
  addresses); `hw/m68k/palm.c` (reset/ROM-load logic, MMIO base
  addresses); `hw/*/dragonball_*.c` (modelled SoC blocks).
- `oldmac-rebase` tree: `board/apple/oldmac/PORTING.md` (68030-vs-68040
  CPU-conditional design, the direct template for a 68000 CPU class);
  `REBASE-REPORT.md` (commit-by-commit history/boot-log evidence for the
  existing 680x0 U-Boot port); `arch/m68k/cpu/m680x0/{cpu.c,start.S}`;
  `arch/m68k/Kconfig`; `configs/oldmac_defconfig`.
- `linux-m68kdt` tree: `arch/m68k/68000/romvec.S` (confirmed DragonBall/
  68000-core vector-table + entry convention); `arch/m68k/configs/
  mc68ez328_defconfig` (confirms this fork's own DragonBall/nommu support
  exists as `CONFIG_M68KDT`/`CONFIG_DRAGONBALL_TIMER`/`CONFIG_DRAGONBALL_INTC`).
- DragonBall/VZ328 primary docs: `MC68VZ328UM.pdf`
  (<https://www.nxp.com/docs/en/user-guide/MC68VZ328UM.pdf>, mirror
  <https://docs.rs-online.com/6394/0900766b801cf944.pdf>) — Ch. 17
  Bootstrap Mode; `MC68EZ328UM.pdf`
  (<https://www.nxp.com/docs/en/reference-manual/MC68EZ328UM.pdf>) —
  System Memory Map / Chip-Selects; `M68VZ328ADSUM.pdf`
  (<https://www.nxp.com.cn/docs/en/reference-manual/MC68VZ328ADSUM.pdf>)
  — ADS board Bootstrap Mode DIP-switch usage; `MC68VZ328CE.pdf`
  (<https://www.nxp.com/docs/en/errata/MC68VZ328CE.pdf>) — chip errata;
  summary excerpt
  <https://www.manualslib.com/manual/106699/Motorola-Mc68vz328.html?page=307>.
- Register map: `MC68328.h`
  (<https://github.com/spotify/linux/blob/master/arch/m68k/include/asm/MC68328.h>).
- Two-stage CSA0-to-RAM boot remap: eLinux Flameman/m105
  (<https://elinux.org/Flameman/m105>).
- uClinux-on-Palm / PalmLoader: Palm Linux Environment
  (<https://palm-linux.sourceforge.net/>); PalmDB uClinux page
  (<https://palmdb.net/app/uclinux>); `rhuitl/uClinux` GitHub mirror
  (<https://github.com/rhuitl/uClinux>).
- U-Boot m68k scope: `doc/arch/m68k.rst`
  (<https://github.com/u-boot/u-boot/blob/master/doc/arch/m68k.rst>).
- GCC m68k PIC/relocation model: M680x0 Options
  (<https://gcc.gnu.org/onlinedocs/gcc/M680x0-Options.html>).
- Cloudpilot-emu (CLIÉ register/model ground truth, per
  `CLIE-RESEARCH.md` §8): <https://github.com/cloudpilot-emu/cloudpilot-emu>.
