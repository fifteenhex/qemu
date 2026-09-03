# Booting real SunOS 4.1.x on the QEMU `sun3x` (Sun 3/80) — recon (Phase 0)

Goal: boot the **original vendor OS, SunOS 4.1.1 (sun3x kernel)** on QEMU
`-M sun3x`, to a serial (`ttya`/zs) shell, to validate the 68030 on-chip PMMU
under real Sun Unix. Today `sun3x` only boots Linux via a *synthesised* minimal
PROM + RAM initramfs (`sun3x.c`, `sun3x-NOTES.md`).

## 1. OS / versions confirmed

* The Sun 3/80 is a **sun3x** machine: 68030 @ 20 MHz, 68882 FPU, the 68030's
  *on-chip* PMMU (NOT the discrete Sun-3 segment/page MMU of the 68020 sun3
  models). Kernel architecture name = `sun3x`.
* **Last SunOS for Sun-3/3x = SunOS 4.1.1 (and the 4.1.1_U1 update).** Sun-2
  dropped after 4.0.3; Sun-3/3x supported through 4.1.1_U1. So **SunOS 4.1.1 /
  4.1.1_U1 sun3x** is the correct, last target. (4.1.2+ are SPARC-only.)
* Install media ships **both** a `sun3` (68020, Sun-MMU) and a `sun3x` (68030,
  PMMU) kernel. The 3/80 boot command selects the arch:
  `b sd(0,30,4)` = sun3, `b sd(0,30,3)` = **sun3x** (the one we want).
* Alternatives (not the task, noted for completeness): NetBSD/sun3 covers the
  sun3x port; OpenBSD/sun3 too. But the task is the *vendor* OS = SunOS 4.1.1.

## 2. Real Sun 3/80 boot PROM — SOURCED, DOWNLOADED, HASHED (small)

The 3/80 PROM is a **128 KiB (131072-byte)** image. Source: Sun3 Archive
`sun3arc.org/ROMs/3_80/`. All four revisions pulled to
`/tmp/sun3x-sunos-roms/` and decoded to raw binary (v2.9.2 ships raw; the 3.0.x
ship as Intel-HEX — decoded here with a small script):

| Rev    | gz URL (sun3arc.org/ROMs/3_80/) | raw size | raw SHA1 |
|--------|----------------------------------|----------|----------|
| 2.9.2  | sun3_80_v2.9.2.gz  | 131072 | `7ecd4a0d0988c1d1d53fd79ac16c8456ed73ace1` (already raw) |
| 3.0    | sun3_80_v3.0.gz    | 131072 | `1e045b6f542aaf7808d6567c28a9e734a8c5d815` (from hex) |
| 3.0.2  | sun3_80_v3.0.2.gz  | 131072 | `830187dfe58e65289533717a797d2c42da86ac4e` (from hex) |
| **3.0.3** | **sun3_80_v3.0.3.gz** | **131072** | **`e4be2dcbb29fc5c60ed9d838ab241c634fdd24e5`** (from hex) |

Confirmed genuine — v3.0.3 strings include *"Sun-3/80 Boot PROM Selftest
(Rev. 3.0.3)"*, a monitor prompt, a test/scope-loop menu, `_exit_boot:
disabling boot state.`, and P4 framebuffer probe messages.

**Recommended: rev 3.0.3** (part 524-1041-07, "12rev51"): it is the only rev
that boots from SCSI disk even with dead NVRAM, and boots QIC-150 + CD-ROM.
(2.9.2 & 3.0.2 can't boot QIC-150; 3.0 self-test flags a memory error.)

Note `_exit_boot: disabling boot state.` — like the 3/60, the 3/80 has a
post-reset **boot state** that redirects (supervisor) fetches to the EPROM until
cleared. Reset PC/SSP come from the PROM. This is exactly the mechanism
`hw/m68k/sun3.c` models for the 3/60 (68020) — but the 3/80 is a 68030, so the
redirect couples to the PMMU / transparent-translation instead of the discrete
Sun-MMU.

### Important premise correction re: TME

The task assumed *"TME boots SunOS on emulated Sun-3x and documents which PROM
images work."* **This is not accurate.** TME's *documented, working* SunOS
4.1.1 path is the **Sun 3/150 / 3/160 — a `sun3` (68020, Carrera VME board,
`sun3-carrera-rev-3.0.bin`)**, NOT a `sun3x`/3/80. There is **no ready TME 3/80
(sun3x) config**. So there is no upstream emulator we can crib a 3/80 memory map
from; the 3/80 hardware contract has to be reverse-engineered from the PROM
binary itself + NetBSD/OpenBSD `sun3x` sources (`sys/arch/sun3/sun3x/`).

## 3. SunOS 4.1.1 sun3x install media — SOURCED, sized (NOT downloaded)

**No ready-to-boot pre-installed sun3x disk image was found.** SunOS on sun3x
requires an **install from tape/miniroot onto a SCSI disk**. Media (Sun3
Archive `sun3arc.org/BootTapes/Sun3x/`, plus the phabrics turnkey repo which is
sun3-only but structurally identical):

* `miniroot_sun3x` — the miniroot install filesystem, **~7 MB**
  (sun3 equiv = 7,168,000 B). Boots the installer.
* `munix_sun3x` — standalone install kernel, **~0.7 MB**.
* `munixfs_sun3x` — **~1.6 MB**.
* Distribution `sun3_*.tar.Z` set (kvm/install/networking/… — the actual OS
  filesystem to unpack onto the target disk): **~60–80 MB compressed** total
  (installed system ≈ 100–200 MB). The `sun3_*.xdrtoc` files describe QIC-24 /
  QIC-150 / CD-ROM / Exabyte tape layouts.
* `Sun_4.1.1U1.tar.gz` (4.1.1_U1 update) = 6,072,872 B.

Turnkey reference (sun3, not sun3x): `github.com/phabrics/Run-Sun3-SunOS-4.1.1`
— gives the exact tape-load / `dd` disk-create / partition / install recipe we
would adapt for sun3x (swap `sun3` → `sun3x` kernels, `b sd(0,30,3)`).

**Disk budget for Phase 1+ (to download when greenlit):**
* PROM: 128 KB — already have (`/tmp/sun3x-sunos-roms`, 1.9 MB w/ all revs).
* Install media (miniroot + munix + tar.Z set): **~80 MB** → /tmp.
* Target SCSI disk image to install onto: **~300 MB** raw (or qcow2 to shrink).
* → **~380 MB** of new image data, plus the m68k QEMU build in tmpfs
  (`/tmp/sun3x-sunos-build`, ~1–1.5 GB for an m68k-softmmu-only build).

## 4. QEMU gaps vs current `hw/m68k/sun3x.c`

Current `sun3x.c` is a **minimal Linux shim**: it synthesises a fake romvec +
identity page table + tiny zs putchar/getchar stubs, enters the Linux ELF
directly at reset (MMU off), absorbs *all* unknown I/O (no bus errors), and
models only intreg, one zs, NVRAM/clock, a scratch IOMMU region. SunOS instead
runs the **real PROM** at reset and drives far more hardware. Gaps:

**(a) Real PROM window + reset — the big one.**
Replace the synthesised romvec path with: map the 128 KB PROM image (as ROM),
and make CPU **reset vector come from the PROM** (SSP+PC at the PROM's base) in
a post-reset *boot state* that redirects fetches to the EPROM until the PROM
clears it (`_exit_boot`). Need the 3/80 physical map: where the PROM decodes at
reset, its runtime mapped address, and how boot-state interacts with the 68030
PMMU/TT registers. `hw/m68k/sun3.c` (3/60) is the structural template
(`bios_name`, `memory_region_init_rom`, boot-state fetch redirect, EEPROM,
IDPROM, bus-error-on-unmapped for PROM probing) — but re-cast for the 68030.
The current sun3x "absorb everything, never bus-error" policy is **incompatible
with a real PROM self-test**, which sizes memory and probes devices via bus
errors — this likely needs real bus-error semantics on unmapped space.

**(b) NCR5380 SCSI + disk.** The tree already has a full `hw/scsi/ncr5380.c`
device (`TYPE_NCR5380`) and `hw/m68k/sun3.c` wires an "si" NCR5380 (at 3/60
obio `0x140000`, ipl 2). Reuse `ncr5380.c` for the 3/80's on-board "si" SCSI,
at the **3/80** address with its **DVMA** path (the sun3x `si` does DVMA through
the IOMMU, unlike the simpler 3/60). Attach a SunOS disk via `-drive`; PROM `b
sd(0,30,3)` reads the SunOS `disklabel`/boot block → loads `boot` → sun3x
kernel. Need the 3/80 `si` register/DVMA addresses (NetBSD `sys/arch/sun3/dev/
si_*` + `sun3x` obio map).

**(c) Sun-3x registers SunOS exercises that Linux didn't.** Proper interrupt
register, the **DVMA/IOMMU** (currently just scratch RAM — SunOS SCSI/enet DVMA
needs it functional), memory sizing via bus error, the P4 framebuffer probe
(can be a stubbed absent-device bus-error so PROM/SunOS fall back to `ttya`
headless), Ethernet (Am7990 `le` — optional; SunOS boots multiuser without
net). The MMU context/TT usage is the 68030 PMMU (already exercised by
hp300/Apollo) — that's the validation target, expected mostly-OK.

**(d) Clock/TOD.** SunOS reads the 3/80 TOD (Mostek/Intersil-class clock in
NVRAM at `0x64000000+0xf8`, already RAM-backed) and an interval timer for
`hz`. Current model has a 100 Hz level-5 tick; SunOS's timer expectations
(clock chip programming, level-5 vs level-7 NMI) need checking against the real
PROM/kernel.

## Feasibility verdict (Phase 0)

**Feasible but LARGE — materially bigger than the Linux sun3x work**, because
SunOS needs the *real* PROM + real SCSI/DVMA + faithful bus-error/probe
semantics, and there is **no existing sun3x emulator (TME included) to crib the
3/80 memory map from** — it must be reverse-engineered from the PROM binary and
NetBSD/OpenBSD `sun3x` sources. Scope is comparable to what `hw/m68k/sun3.c` did
for the 3/60, but for the 68030 + SCSI-disk install.

Suggested milestones / gate:
1. **PROM self-test → monitor `>` on ttya** (map PROM, reset-into-PROM,
   boot-state, zs, NVRAM/IDPROM, bus-error-on-unmapped). This alone proves the
   PROM+console+reset path and is the right first gate.
2. NCR5380 `si` + DVMA; PROM `b sd(0,30,3)` reaches the SunOS boot block.
3. SunOS `sun3x` kernel boots to single-user on ttya (the PMMU validation).
4. (Optional) full miniroot install to a fresh disk / multiuser.

**Effort estimate:** milestone 1 ≈ 1 focused day of PROM-selftest iteration;
milestones 2–3 ≈ several more days (SCSI/DVMA + disklabel/boot-block + kernel
MMU bring-up). Real risk of PROM walls (checksum/rev, memory-size probe,
missing device probes) before we ever reach the kernel.

---

# PHASE 1 — real PROM bring-up (implemented in `sun3x.c`, SunOS mode)

SunOS mode is entered when a boot PROM image is supplied with **`-bios`**; with
no `-bios` the machine is byte-for-byte the merged Linux shim (verified: Linux
`sun3x` still boots to a shell, CPU/MMU 68030). Run it with:

```
qemu-system-m68k -M sun3x -m 16M -bios /path/sun3_80_v3.0.3.bin \
    -serial <chardev> -display none
```

The genuine **rev 3.0.3** PROM (128 KiB, SHA1 e4be2dcb…) runs its complete
power-on self-test with **every subtest PASSing**, sizes and initialises RAM,
and reaches the monitor's console/boot-countdown handshake:

```
Sun-3/80 Boot PROM Selftest (Rev. 3.0.3)
System Enable Register Read Test {pass}   PROM Checksum Test {pass}
TOD [Clock/Calender] Test {pass}          I/O Mapper RAM ...{pass x3}
<Memory Size = 0x00000010 MB>
Memory Address Test {pass}   Memory Read/Write Byte Alignment {pass x2}
Bus Error Register Test {pass}
Level 1/2/3 Interrupt Test {pass x3}      Parity Memory Test {pass}
...
<Selftest aborted, Initializing the Monitor>
<Initializing Main Memory... 0x00000010 Megabytes Initialized>
```

## What the real PROM needed (all reverse-engineered from the ROM + NetBSD)

Physical map confirmed from NetBSD `sys/arch/sun3/sun3x/obio.h` (Type-1 space
0x58000000..0x7bffffff): IOMMU 0x60000000, ENABLEREG 0x61000000, BUSERRREG
0x61000400, DIAGREG 0x61000800, MEMREG 0x61001000, INTERREG 0x61001400,
ZS_KBD_MS 0x62000000, **ZS_TTY_AB 0x62002000** (the console — ttya is channel
A at +4/+6), EEPROM 0x64000000 (IDPROM2 +0x7d8, **Mostek clock +0x7f8**),
LANCE 0x65002000, **EMULEX_SCSI 0x66000000 + DMA 0x66001000** (3/80),
FDC 0x6e000000, PRINTER 0x6f00003c.

1. **Reset / PROM window.** Reset SSP/PC come from the ROM vector (offset 0/4 =
   0xfef60c00 / 0xfefe0228); the ROM decodes at **0xfefe0000** (top 128 KiB) and
   is **also aliased at 0x63000000** (the cold-start path rebases its data/vector
   references there — VBR := 0x63000000, TC sentinel read from 0x63007860). The
   0xfef00000..0xfefe0000 window below the ROM is the monitor's private
   scratch/stack/VBR RAM. MMU stays off (the "pmovefd" loads a TC sentinel with
   E=0), so everything runs at physical addresses.
2. **Bus errors are real.** No catch-all background: unmapped space faults. A
   lowest-priority "bus timeout" region latches the **TIMEOUT bit (0x20)** in a
   modelled **bus error register (0x61000400)** and raises a 68030 access fault
   — the PROM's Bus Error Register test reads nonexistent 0x40000000 and
   requires TIMEOUT to latch. QEMU's `m68k_cpu_transaction_failed` already
   builds the format-A frame + `bus_error_suppress` completion.
3. **Memory sizing = floating bus.** Absent memory banks must **read back all
   ones (0xffffffff)**, not 0: the PROM writes a pattern and treats 0xffffffff
   as "bank empty"; returning 0 was misread as *"Bank is partially filled"* and
   dropped it into an interactive diagnostic. With all-ones it prints the
   correct `Memory Size = 0x10 MB`.
4. **I/O mapper (DVMA) RAM = 8 KiB** (2048 PTEs; the PROM walks it to
   0x60001ffc) — modelled as scratch RAM, enough for the RAM-cell self-tests.
5. **Interrupt register soft interrupts.** Bit 0 = master enable; bits 1/2/3 =
   software interrupt at autovector **level 1/2/3** (the PROM writes 0x03/0x05/
   0x09 and requires the matching level to fire). Bit 5 = the periodic level-5
   clock. The zs is wired to autovector **level 6**.
6. **OBIO absorb + misc.** A low-priority absorb over the whole Type-1 window so
   the self-test's pokes of LANCE/SCSI/FDC/printer/cache-tag registers don't
   fault before the PROM installs its probe handler; real registers (buserr,
   intreg, iommu, zs, eeprom) overlay it. Framebuffer (OBMEM) left absent so the
   PROM stays on ttya.

## Current walls (short of the interactive `>` monitor prompt)

There are two independent paths to the monitor, each with one remaining gap:

**(A) Full self-test → monitor.** Every subtest passes up to the deep parity
test. `Parity Memory Test` and `Parity Memory Forced Error Test` pass (the
memory error/control register at 0x61001000 is modelled as a read/write-back
byte), but the next stage forces bad parity and requires a **level-7 (NMI)
parity-error interrupt** plus the parity error register reading **0x07** —
i.e. faithful parity-error injection on a tagged RAM write. Not yet modelled:
```
Parity Memory Forced Error Test {pass}
 error1: no parity error (level 7) interrupt occurred when bad parity forced.
 error3: Bad Parity Memory Error Reg: exp=0x00000007, obs=0x00000000
```

**(B) Aborted self-test → monitor (`q`/`Esc`).** This skips parity and reaches
`Initializing Main Memory... Megabytes Initialized`, then enters the monitor's
console / boot-countdown handshake described below.

After `Megabytes Initialized` the monitor enters its **console / boot-countdown
handshake** (~0xfefe3038–0xfefe306c) and spins: it waits for either a tick
counter in scratch RAM (`0xfef720c6`, read via `a5`) to advance ~1000 ticks
(the autoboot/console-select countdown) **or** a flag `*(0xfef72013)` to change
from 0xde (set by the zs receive path). At this point the monitor has written
the interrupt register back to **0** (master interrupts disabled).

The counter is advanced by the monitor's **level-7 (NMI) clock handler at
`0xfefe04fe`**, which the monitor installs at **VBR+0x7c** (VBR = its resident
table `0xfef60c00`, so the vector is at `0xfef60c7c`; confirmed = `0xfefe04fe`).
The handler reads a clock-pending register (`0xfef06010`), and *only if
non-zero* increments `0xfef720c6` by 10 and acks the clock by pulsing bit 7 of
`0xfef0b400` (`andb #0x7f` / `orb #0x80`).  Modelled so far (SunOS mode): a
`clk_pending` latch set each 100 Hz tick, a level-7 line gated on the clock
handler being installed at `0xfef60c7c`, and `0xfef06010`/`0xfef0b400`
status/ack stubs.  A first cut got the counter to advance by 10 (one NMI
delivered) — proving the mechanism — before hitting the wall below.

### PIVOTAL FINDING — the monitor runs with the 68030 MMU ON

The clock (and console handshake) modelling is blocked by this: **the monitor
enables the 68030 MMU** and its `0xfef0xxxx` addresses (VBR/vectors, the clock
registers `0xfef06010`/`0xfef0b400`, and the tick counter) are **virtual**,
mapping through the monitor's page tables — most likely onto the relocation RAM
at `0x63000000` and the real OBIO device space.  Evidence: reading `0xfef60c7c`
**through the CPU/MMU** (gdb) returns the installed handler `0xfefe04fe`, while
reading the *physical* address (our `promram` RAM pointer) returns 0 — i.e. the
monitor's write went to the MMU-mapped physical page, not physical
`0xfef60c7c`.  Consequently our *physical*-address device overlays
(`sun3x.clkstat`/`sun3x.clkack` at `0xfef06010`/`0xfef0b400`) and the physical
vector read are on the wrong side of the MMU and never see the monitor's
accesses, so the level-7 clock never actually fires.

### RESOLUTION — MMU-aware clock: the `>` prompt IS reached

Walking the **live 68030 page tables** with `m68k_cpu_get_phys_addr_debug()`
resolved where the monitor maps its virtual clock addresses once its page tables
are up:

| monitor virtual | physical it maps to | what |
|-----------------|---------------------|------|
| `0xfef06010`    | **`0x64000810`**    | clock-pending status |
| `0xfef0b400`    | **`0x61002800`**    | clock ack |
| `0xfef720c6`    | top of RAM (`~0xffc0c6`) | tick counter (plain RAM) |
| `0xfef60c7c`    | top of RAM (`~0xffec7c`) | level-7 vector |
| `0xfefe04fe`    | `0x630004fe`        | handler (runs from the reloc RAM) |

So the clock device belongs at **physical `0x64000810`/`0x61002800`** (on the
monitor's side of the MMU), and the clock is the **level-7 (NMI) auto-vector**.
Modelled (SunOS mode): `clkstat`/`clkack` at those physical addresses; a
`clk_enabled` latch set once the monitor's page tables map `0xfef06010` through
to our device (keeps it inert during the self-test); and — crucially — the
level-7 line is re-armed as a fresh **NMI edge every tick** from `sun3x_tick()`
(drop-then-raise), since the handler's ack goes through the monitor's MMU and
cannot be relied on.

With this the genuine PROM **reaches the interactive `>` monitor prompt** on
ttya, printing its full banner ("Sun Workstation, Model Sun-3/80 Series / ROM
Rev 3.0.3, 16MB memory installed / Ethernet address … / Host ID 42000000"),
attempting auto-boot (which correctly bus-errors, no boot device yet), and
dropping to `>`.  See `sun3x-SUNOS-transcript.txt`.

**RESOLVED — deterministic `>`.** The non-determinism was a QEMU 68030/68851 PMMU core bug (found by the PMMU audit): PTEST ignored the `An` descriptor-address writeback that the PROM's page-table read/modify API (`ptestr #7,<va>,N,%a0`) relies on, so every monitor page-map update went to a garbage physical page.  With that fix (merged from `amiga`, plus PMOVEFD TLB-flush / ATC root-tagging / interrupt-mask fixes) and a board-level NMI vector-gate (deliver the level-7 edge only once the monitor's `[VBR+0x7c]` vector holds a handler — closes the pre-install double-fault window), the genuine PROM reaches `>` **deterministically: 10/10 runs, 0 crashes, ~1.5s after aborting the self-test**, ending "No bootable devices found" -> `>` (no boot device modelled yet = M2).

**Milestone-1 status: reached `>` (demonstrated).** Genuine PROM + reset + real
bus errors + full self-test (all subtests pass) + memory init + monitor
self-relocation + bidirectional ttya console + the interactive monitor prompt
all work, exercising the 68030 PMMU under the real Sun firmware.  Next:
deterministic clock (above), then Milestone 2 (NCR5380 `si` at 0x66000000 +
DVMA, `b sd(0,30,3)`, SunOS install).

## Milestone 2 progress — si/NCR5380 register map RESOLVED; DMA still open

The PROM's `b sd(0,30,3)` SCSI driver (transfer core at 0xfeff0c00, command
state machine at 0xfeff0d10 / 0xfeff1072, arbitration at 0xfeff0fa2) was traced
against the emulated bus.  Findings:

**Register map (validated empirically, committed).** The Sun 3/80 "si" board
wires the NCR5380's 8 registers **4 bytes apart (reg-shift 2) with reg0 (data)
at physical 0x66000008**, not 0x66000000:

    0x66000000..07  si control regs (low)
    0x66000008      NCR5380 reg0  (CSD / ODR)       <- CDB out, data in
    0x6600000c      NCR5380 reg1  (ICR)             <- SEL/ACK/ATN handshake
    0x66000010      NCR5380 reg2  (MR)              <- 0x03 = ARBITRATE|DMA_MODE
    0x66000014      NCR5380 reg3  (TCR)
    0x66000018      NCR5380 reg4  (CSB, cur status) <- phase read (BSY|REQ)
    0x6600001c      NCR5380 reg5  (BSR)             <- phase/DRQ read
    0x66000020/24   si scratch/dma-control regs     <- read-back regs
    0x6600002c      si scratch                      (read-back, tolerant)
    0x66001000      si CSR / DMA block

The driver only uses NCR5380 reg0..5 directly; it reads phase from CSB(reg4)
and BSR(reg5).  With the 5380 mapped at +0x08 (reclaiming 0x20/0x24 at higher
priority) the driver drives it with correct semantics and its si register
self-test (0xfeff0c00) passes.

**si CSR bits (ground truth, NetBSD sun3 `sireg.h`).** NOTE: several earlier
values in this file were wrong.  Correct bits:

    0x8000 DMA_ACTIVE  0x4000 DMA_CONFLICT  0x2000 DMA_BUS_ERR  0x1000 ID
    0x0800 FIFO_FULL   0x0400 FIFO_EMPTY    0x0200 SBC_IP       0x0100 DMA_IP
    0x0020 BPCON(VME)  0x0010 DMA_EN(VME)   0x0008 SEND         0x0004 INTR_EN
    0x0002 FIFO_RES    0x0001 SCSI_RES

The command state machine (0xfeff1072 prologue) gates progress on
`(si_csr & 0x403) == 1` (bit0 set, bits 1 & 10 clear).  Forcing bit0 set lets
it advance from the arbitration wait into reading the 5380 phase and cycling
the probe commands (TEST-UNIT-READY 0x00, START/STOP 0x1b, INQUIRY 0x12).

**Open sub-layer (the wall).** Selection does not complete: CSB(reg4) reads 0
(no target BSY) because
  1. the board uses **non-standard DMA-assisted arbitration/selection** — the
     driver never writes an ODR target bitmask, so QEMU's Mac-style
     ncr5380_select (triggered on ICR SEL+DATA) never fires; and
  2. the sun3/80 **OBIO** si routes DMA through an **Am9516 UDC** DMA
     controller (`udc_data`/`udc_addr` regs), a separate chip that is not yet
     modelled, plus the IOMMU DVMA translator (0x60000000, 2048 PTEs).

To finish M2: model the Am9516 UDC + si CSR SBC_IP/DMA_IP interrupt reflection,
teach the NCR5380 model the Sun arbitration/selection completion, add the
IOMMU DVMA translator, then drive INQUIRY/READ -> boot block -> miniroot ->
shell.  Ground-truth sources (NetBSD `sys/arch/sun3/dev/si.c`, `si_obio.c`,
`sireg.h`, and the Am9516 `sys/arch/sun3/dev/*udc*`) are the reference.

## si SCSI register-level contract (full RE of PROM driver 0xfeff1072)

Ground truth from disassembly. BP=0x66000000 (5380 base, reg0=data@+0x08),
DP=0x66001000 (si DMA/CSR). POLLED driver, no interrupts (INTR_EN never set).

Register access map (writes match stock 5380 at BP+0x08+4*reg):
  BP+0x00 = DMA count low byte    | BP+0x04 = DMA count high byte
  BP+0x08 reg0 ODR(w)/CSD(r) data (CDB written here 6x FIFO-style pre-select)
  BP+0x0c reg1 ICR   | BP+0x10 reg2 MR(w) / PHASE(r)&7
  BP+0x14 reg3 TCR(w)/ BSR(r, PHASE_MATCH 0x08)  | BP+0x18 reg4 CSB(r)
  BP+0x1c reg5 BSR/StartDMASend  | BP+0x20/0x24 si scratch/ctrl
  DP+0x00 si_csr(16b) | DP+0x04 dma_addr(32b, HW auto-increments) | DP+0x08 unused

si_csr forward-progress gate (state-machine prologue, every call):
  proceed iff (si_csr & 0x0403)==1  => bit0(0x0001)=1, bit1(0x0002)=0,
  FIFO_EMPTY(0x0400)=0. Else: FIFO_EMPTY set -> spin<=500000 or err8;
  bit1 set -> err7; bit0 clear -> err21.

SELECT->COMMAND->DMA-READ->STATUS sequence:
 A. build CDB: write 6 CDB bytes to reg0(0x08); a5@0=16; reg3 TCR=0x98;
    reg2 MR=struct[7] (arb id, low bits=target); reg1 ICR=0x41; enter 0x20000000
    poll loop calling 0xfeff1072.
 B. selection: driver does NOT assert ICR SEL(0x04); relies on MR/ICR/TCR
    preload + si HW.  On success bus reaches CSB&7 / BSR PHASE_MATCH(0x08).
 C. arm DMA (0xfeff0e04): dma_addr(DP+0x04)=buf; count lo->BP+0x00, hi->BP+0x04;
    reg1 ICR=0x80; READ: si_csr|=0x0200 & ~0x0100; WRITE: si_csr|=0x0300;
    a5@0=18; reg1 ICR=0x90.  (does NOT use DMA_EN/SEND, nor DP+0x08.)
 D. data+complete (0xfeff0e6e): poll si_csr SBC_IP(0x0200); clear it;
    poll FIFO_EMPTY(0x0400) clear (data present) else err8; poll DMA_IP(0x0100),
    if set si_csr|=0x0040 then spin<=500 for 0x0040 clear else err19;
    si_csr &= ~0x0300; read dma_addr; bytes = dma_addr-start (HW incremented).
 E. status/msg (0x12b0): REQ handshake via reg1 ICR=0x10; read STATUS=reg0,
    MESSAGE=reg0; ICR=0x12 then 0x00; msg 0x00 (CMD COMPLETE) -> a5@0=32 -> done.
    status&0x1e nonzero -> err20.

Phase dispatch: reads reg2(BP+0x10)&7 -> {0,1:state24; 3:DATA/DMA handler
0x1278; 7:STATUS/MSG 0x12b0; else err5}.  BSR dispatch on reg3(BP+0x14).
=> MODEL must surface live bus-phase in low bits read at BP+0x10 (reg2) and
BSR at BP+0x14 (reg3): the read roles are shifted vs a stock 5380.

## M2 boot blocker — data-in DMA byte-count (root cause pinned)

The `b sd(0,30,3)` boot reaches the label read but stops before the READ.
Exact chain (PROM sd label-read routine 0xfeff16b0):
  - issues INQUIRY (0xfeff16b4, cmd 0x12) via 0xfeff178c;
  - **checks the returned byte count d7 > 0** (`blew 0xfeff15cc` @0xfeff16c0);
  - only then issues the block READ (cmd 8) @0xfeff1706, whose data is
    validated as a Sun disklabel (magic 0xDABE @508) by 0xfefeeeba.
The transfer's returned byte count is the **dma_addr delta** (HW auto-
increment, contract step D).  For INQUIRY that delta is 0, so d7<=0 and the
READ is never issued -> "No label found" / "Device not found".

Why the delta is 0: the state machine's DMA-arm 0xfeff0e04 (state 24 ->
writes dma_addr@0x66001004 and si_csr|=0x0200) is **gated on a2@(24) (the DMA
byte count) being non-zero**; for the INQUIRY it is 0, so the arm is skipped
(observed: only si_csr=0x0080/0x0000 written, no dma_addr, no si_csr=0x0200,
no reg0 data reads).  So no data is DMA'd, dma_addr never increments, count=0.

Likely cause: the "complete the command at selection" shortcut in
ncr5380_select_target (sun-mode) executes the SCSI command immediately and
lands the bus in the DATA phase, so the PROM's per-transfer count field
a2@(24) is never established the way its COMMAND-phase / transfer-setup path
(0xfeff0c00 / 0xfeff178c) expects.  FIX DIRECTION: do not complete at
selection; keep the bus in COMMAND phase after select, let the state machine
run its transfer-setup + DMA-arm (0xfeff0e04) so a2@(24)/dma_addr are set,
then execute the command and run sun3x_si_dma_run on the si_csr arm so the
data lands in DVMA memory and dma_addr advances by the transfer length (the
byte count the boot checks).  With that, INQUIRY returns >0, the READ fires,
block 0 is read, and the disklabel (built by sun3x-SUNOS-mkdisk.py) is found.

INQUIRY period-authenticity was tested and is NOT the gate: even with
`-device scsi-hd,scsi_version=2,vendor=SUN,product=SUN0207` (SCSI-2, Sun
vendor) the READ still does not fire — the block is the byte-count above.

## M2 RESOLVED — the disk READ fires; ufsboot + the sun3x kernel now load

Commit `sun3x/SunOS: fix si data-in DMA so the disk READ fires`.  Two coupled
bugs in the si data-in handshake were fixed (both sun-mode gated):

1. `sun_phase_bits()` returned 3 ("transfer complete") for the DATA phase up
   front, so the driver finalised the transfer without ever arming DMA.  Now
   it returns 1 (DATA IN) until the si engine has drained the bytes
   (`sun_dma_done`), then 3 — so the arm at 0xfeff0e04 actually fires and
   dma_addr advances by the transfer length.

2. The DMA-run trigger in `sun3x_sireg_write` (0x66001000) keyed off a
   **rising edge** of SBC_IP (0x200).  si_csr is never cleanly cleared to 0
   between commands (its bits are *synthesised* on read: the 0x1000 read
   returns `ID|1|(si_csr&0x300)`), so a stale SBC_IP left set after INQUIRY
   swallowed the READ's arm edge and the READ's DMA never ran — dma_addr stayed
   at the buffer base, byte count came back <=0, and the PROM aborted at
   0xfeff1710 (`ble 0xfeff1722` -> returns -1 -> `sd: Device not found`).
   *Observed with SIDMA tracing:* INQUIRY's DMA ran (moved 36 -> 0xfff00024)
   but the READ logged `SICSR old=0x301 new=0x301` (no edge) and never ran.
   Fix: gate the run on `!sun_dma_done` (cleared per-command by
   `ncr5380_do_command`) instead of the SBC_IP edge, so the engine runs exactly
   once per data phase regardless of stale si_csr bits.

Verified end to end (`b sd(0,30,3)` / auto-boot, deterministic): INQUIRY
returns 36 -> READ(6) cmd 0x08 block 0 -> disklabel magic 0xDABE validates
(0xfefeeeba) -> `Boot: sd(0,18,0)` -> ufsboot reads the UFS miniroot in 8 KiB
blocks and loads the ~698 KiB sun3x kernel via **79 READ(6) transfers** (LBA
0..~6560) -> the kernel executes.  Transcript: `sun3x-SUNOS-transcript-M3.txt`.

Boot disk: `python3 hw/m68k/sun3x-SUNOS-mkdisk.py /tmp/sunos-media/miniroot_sun3x
/tmp/sunos-boot.img` (miniroot from sun3arc.org/BootTapes/Sun3x/miniroot_sun3x;
UFS superblock magic 0x011954 @8192, bootblk `4efa010a` @block 1).

## M3 — the next blockers (kernel bring-up), precisely pinned

After ufsboot hands off, the sun3x kernel (a.out OMAGIC, entry **0xf8004000**)
runs and stops here (repeatable):

    Boot: sd(0,18,0)
    Error: bad idprom format (0 s.b. 1)   (x4)
    bdevvp: bad open
    input ether device name ( ie... le... gn... ec... ):

Two distinct, ordered blockers:

1. **IDPROM (first, likely the gate).**  The kernel reads its IDPROM `id_format`
   byte as 0 (wants 1).  Confirmed by tracing (temporary logged-IO over the
   0x64000000 NVRAM *and* the whole 0x58000000..0x7c000000 OBIO absorb, filtered
   to non-PROM PCs): **the kernel never reads physical 0x640007D8** (the sun3x
   IDPROM/Mostek address — matches NetBSD `sun3x/obio.h` OBIO_IDPROM2, and our
   `sun3x_build_nvram` already puts a valid format=1 checksummed IDPROM there;
   the *PROM* reads it fine, hence the correct banner ethernet/hostid).  So the
   kernel gets the IDPROM from **RAM / the PROM romvec**, not the hardware, and
   that source is zero.  NEXT: find the SunOS sun3x romvec IDPROM export (the
   `struct sunromvec` idprom/`v_idprom` field the kernel copies at startup) and
   make our real-PROM handoff / that RAM structure hold the valid IDPROM.  The
   garbage ether-device names ("...4278059160") are downstream of this.

2. **Root open / kernel si driver (second, bigger).**  `bdevvp: bad open` = the
   kernel could not open its root block device, then it falls back to prompting
   for a net device.  SCSI tracing shows **zero kernel-side SCSI**: every one of
   the 85 selects / 79 reads is the PROM/ufsboot polled driver issuing READ(6)
   (cmd 0x08); the kernel issues **no** command (no READ(10)/0x28, no si
   register access) before it bails.  So it dies at (1)/root-config *before* its
   own driver runs.  Once (1) is fixed and the kernel tries to mount root off
   sd, expect the real work: the SunOS `si` driver is **interrupt-driven** and
   DMAs through the **Am9516 UDC**, unlike the PROM's polled/pseudo-DMA path our
   `hw/m68k/sun3x.c` models — INTR_EN/SBC_IP/DMA_IP interrupt reflection and the
   UDC will need modelling for the kernel driver to complete a transfer.

Reproduce: build to /tmp/sun3x-build, then
`qemu-system-m68k -M sun3x -m 16M -bios .../sun3_80_v3.0.3.bin -drive
file=/tmp/sunos-boot.img,format=raw,if=scsi,bus=0,unit=3 -display none -serial
<chr>`; send ESC to abort selftest; auto-boot proceeds to the above.

## M3 blocker #1 RESOLVED — IDPROM: sun3-probe must bus-error (commit follows)

The "bad idprom format" chain was NOT a romvec/RAM issue — it is the standalone
boot's (ufsboot, loaded at 0x00200000) IDPROM reader.  Full RE (gdb: halt at the
wall, dump 0x200000..0x220000, disassemble):

* IDPROM reader = `0x211a68`.  It first tries the **sun3** IDPROM at virtual
  **0xfef1cc00**, guarding a *word* read with a temporary bus-error vector
  (`0x211ab0`: swap `vbr@8`, `movew (a0),d0`, restore; returns -1 on fault).
  Only if that read **faults** does it fall back (`0x211a86`) to the **sun3x**
  IDPROM at virtual **0xfef047d8**.
* The monitor's page tables map `0xfef047d8 -> physical 0x640007d8` (our
  MK48T02 IDPROM; gdb read there = `01 42 08 00 20 11 22 33 … 6b`, valid) and
  `0xfef1cc00 -> physical 0x61001800`.
* Our broad `enareg` absorb (0x61000000..0x61002000) was ACKing 0x61001800
  (read 0, no fault), so the probe "succeeded" and the boot used the sun3
  location -> `id_format = 0` -> "bad idprom format (0 s.b. 1)".

FIX (in `sun3x_init`, SunOS mode): overlay an `sun3x_busfault_ops` region
("idprobe-hole") at physical **0x61001800** (size 0x800), so the sun3 probe
bus-errors exactly like a real 3/80 and the boot falls back to the valid sun3x
IDPROM.  Verified safe: the PROM self-test never touches 0x61001800 (only
ufsboot's probe does — pc 0x211ad2 word + pc 0x211a9a byte-copy), and the real
registers in that block are at +0x400 / +0x1000 / +0x1400.

Result (deterministic): the 4x "bad idprom format" **and** the "bdevvp: bad
open" / "input ether device name" fallback are **gone**; auto-boot now prints
just `Boot: sd(0,18,0)` and proceeds into the real boot path.

## M3 blocker #2 (now exposed) — ufsboot si transfer wedges (STOP point)

With the IDPROM valid, the boot no longer bails; it loads the kernel (same 79
READ(6)) and then **spins forever at PC 0x213abe**:

    0x213ab6: moveb %a5@(8),%d7        ; a5 = 0x0031e000 (RAM si "softc")
    0x213aba: tstb  %a5@(28)           ; softc[0x1c] == 0x10 (never changes)
    0x213abe: bnes  0x213ab6           ; loop

This is inside ufsboot's **own copy of the PROM si driver** — the state machine
`0x2139c4` and the DMA-arm `0x213772` are byte-identical to the PROM's
`0xfeff1072` / `0xfeff0e04`.  a5 (=`arg@(8)` = 0x31e000) is the driver's softc;
a3 (=`arg@(12)` = 0x66001000) is the live si_csr.  The spin waits for
`softc[0x1c]` to clear, which only the driver's reset/complete path `0x2139aa`
(`clrb %a5@(28)`) does on a *successful* transfer completion.  The transfer
never completes in our model, so it never clears.  softc snapshot at the wall:
`[0x0c]=40 [0x10]=07 [0x14]=08 [0x18]=6c [0x1c]=10 [0x20]=07 [0x24]=04`.

### ROOT CAUSE FOUND — reg5 (5380 BSR) IRQ latch is dual-purpose (genuinely deep)

Fully REd via gdb (halt at spin, read the driver's pointers/HW):

* a5 (=`chan@(8)` = **0x31e000**) is NOT RAM — it is a **virtual alias of the
  5380 register file** (maps to physical 0x66000000, reg0 @+0x08, reg-shift 2).
  So `a5@(0x1c)` is a **live read of reg5 = the 5380 BSR**, and `a5@(0x08)` is
  reg0.  a3 (=`chan@(12)` = 0x31f000) aliases the si DMA/CSR block (0x66001000).
* The wedge loop is therefore `do { read reg0; } while (reg5 != 0);` — the
  driver polls the BSR and waits for it to go **0** (bus idle) before it reads
  STATUS/MESSAGE and completes the command (`0x213c84` `tstb reg5; bne` ->
  `0x213aaa`; `cmpib #1,reg5; bls` = read next byte if <=1, else spin).
* Live values at the wall: `reg5 val=0x10, s->bsr=0x18, phase=PHASE_ST(12),
  dev=1`.  The 0x10 is **BSR_IRQ** — which our model sets at selection
  (`ncr5380_select_target`) and bus-free and never clears (the polled driver
  **never reads reg7/RPI** — confirmed: no `moveb aN@(36),dX` in the si driver).
  So reg5 stays 0x10 and the spin never ends.

**Why it's not a one-liner (the conflict):** reg5/BSR_IRQ is *overloaded* by the
driver.  Right after arbitration it needs reg5 **non-zero** (BSR_IRQ) to mean
"selected / interrupt pending" — the working M2 probe path depends on this
(masking BSR_IRQ, or clearing it at `enter_status`, makes selection/TUR fail
-> `sd: Device not found`, verified by regression).  But at a command's
STATUS-completion it needs reg5 **zero**.  On real hardware the 5380 IRQ latch
is cleared by reading reg7 (RPI); the sun si *polled* driver never does, so the
sun si board must present reg5 as a **live, self-clearing bus signal** (IRQ
de-asserted once the initiator has serviced the phase) rather than the latched
5380 semantics we model.  Getting this right means reworking sun-mode reg5 to
track live bus/phase state (set on a fresh phase-change REQ, clear once
serviced) *without* regressing the M2 selection-detection — a real change, not
a tweak.  Tried and reverted (all break M2): global BSR_IRQ mask in reg5 reads;
clear in `ncr5380_sun_to_status`; clear in `ncr5380_enter_status`.

NEXT agent: model sun-mode reg5 as live state.  Sketch: keep BSR_IRQ asserted
from selection only until the driver has *consumed* the selection/phase event
(e.g. clear it when the CDB/data phase begins, or when the driver first reads
reg0/reg2 in the new phase), and re-assert on each subsequent REQ/phase-change,
so it reads non-zero exactly while a phase is pending service and 0 when idle.
Validate against BOTH the M2 probe (`b sd(0,30,3)` must still find the disk +
read the label) AND the ufsboot post-load transfer (0x213abe must stop
spinning).  Everything up to here is committed and deterministic.

### RESOLVED — reg5 modelled as the live target /REQ (both paths pass)

The precise distinguisher (found by disassembling the PROM's status handler):
* PROM status/msg handler `0xfeff12da` waits for **`(reg5 & 0x1f) != 0`** = "a
  bus event / byte is pending" (masking BSR_IRQ there hangs the probe at its
  500000-count timeout -> `sd: Device not found`).
* ufsboot's blind status/msg loop `0x213aba` waits for **`reg5 == 0`** = "bus
  idle" before completing.
Both are satisfied by making reg5 bit4 (IRQ) track the **live target /REQ**
(`CSB_REQ`): asserted while a phase byte is waiting, de-asserted once taken.

Two coupled changes in `hw/scsi/ncr5380.c` (both sun-mode gated, committed):
1. reg5 (R_BSR) read: derive BSR_IRQ from `CSB_REQ`, not the stored latch.
2. reg0 (R_CSD) read in STATUS/MESSAGE **when ACK is de-asserted** (the ufsboot
   blind path; the PROM asserts ICR ACK so its handshake path is untouched):
   take the byte, drop /REQ, and step the phase STATUS -> MESSAGE IN -> bus
   free.  This drops /REQ so reg5 goes 0 and the ufsboot spin ends.

The key insight that unlocked it: the two spins were BOTH `reg5` polls but for
opposite conditions, and the PROM uses the ICR-ACK handshake while ufsboot
reads reg0 blind — so gate the reg0-advance on `!ACK` to serve ufsboot without
disturbing the PROM.

Validated deterministic, BOTH paths:
* M2 probe: `b sd(0,30,3)` -> 3xTUR + 2xSTART + 2xINQUIRY(36) + **79xREAD(6)**,
  disklabel 0xDABE, ufsboot loads the ~698 KiB kernel.
* ufsboot post-load spin at 0x213abe **clears**; auto-boot advances past
  `Boot: sd(0,18,0)`.

## M3 blocker #3 (now exposed) — `bdevvp: bad open` (device open fails)

With the spin cleared, auto-boot reaches (transcript `sun3x-SUNOS-transcript-M3c.txt`):

    Boot: sd(0,18,0)
    bdevvp: bad open
    input ether device name ( ie... le... gn... ec... ):

`bdevvp` (ufsboot 0x20b00a) calls the block-device open dispatch (0x20228a ->
driver open 0x203584), which returns **-1**, so it prints "bdevvp: bad open"
and falls back to prompting for a net device.

### FULLY REd — it is NOT a dev_t/target mismatch; it is the sd-open TUR

Chased the whole chain in gdb:
* dev_t 0x730 (major 7 = sd).  The sd driver's own open is **0x210770** (devsw
  name "sd" @0x2185ee, open ptr @+10 = 0x210770).  It reads a target/lun table
  at **0x2185d0** (`target = byte>>3, lun = byte&7`): entry = 0x18 -> **target
  3, lun 0**, which *matches* our attached disk.  So the target mapping is
  correct -- the coordinator's dev_t/unit-vs-target hypothesis is ruled out.
* The sd open identifies the device via **0x2123a0** (4 checks: 0x212b18,
  0x2123f0, 0x213e20, 0x213520; returns 0 if any passes).  Traced: the 4th
  check 0x213520 passes the *first* time (d0=0) but fails (d0=-1) the *second*,
  and the flip is caused by the SCSI probe **0x2109f0(cmd 0 = TEST UNIT READY)
  returning -1** in between.
* The TUR's SCSI actually **completes GOOD**: trace shows
  `ncr5380_command 0x00` -> `ncr5380_complete status 0x00`.  But ufsboot's
  transfer wrapper 0x2109f0 runs its state machine (a5@(32) = **0x2139c4**, the
  same one whose reg5 spin we just fixed) in a loop, and that state machine
  **returns d0 = -1** (gdb at 0x210c3c: `SM ret d0=-1 a4@0(state)=4 a4@1(err)=0`
  -- no driver error code set), so the loop treats it as failure
  (0x210c3c `blt 0x210c86`) and 0x2109f0 returns -1.

### Root cause / precise next step

The reg5 fix converted the *hang* (spin at 0x213abe) into a *clean completion
that returns the wrong value* for a no-data command: the blind reg0 STATUS/MSG
advance drains status+message and tears the bus down to **bus-free (dev=NULL)**,
after which the state machine's next `reg2` read returns **phase 0** (our
`sun_phase_bits` returns 0 when `!s->dev`), and the phase-0 path yields the -1
that 0x2109f0 rejects.  For the ufsboot *post-load* TUR this -1 is tolerated
(no one checks it) so the boot advances; for the *sd-open* identification TUR
it is checked, so the device is declared unopenable.

### DEEPER — the -1 is the state machine's *retry loop after bus-free* (err5)

Single-stepped/breakpoint-traced the sd-open TUR through 0x2139c4 (gdb, new
build).  The actual sequence for the failing TUR:

    SUCCESS-path 0x213c8c (reg5==0 -> state 27)        <- command DID complete
    EXIT-path    0x213d7c  d0=27  a4@1(err)=5          <- next call: err5
    FINISH       0x2138e4  called
    WRAP         0x210c3c  smret d0=-1                 <- 0x2109f0 returns -1

So the command completes on the first state-machine call (success, state 27),
but the transfer wrapper 0x2109f0 polls the state machine a FIXED number of
times (TEST UNIT READY: 4), and a *trailing* call -- run after the bus has gone
free -- reads phase 0 and drives the phase-0 retry counter to overflow
(0x213ac6 -> 0x213bc6 sets a4@(1)=**5** "unexpected/retry-exhausted") ->
0x213d7c -> finish -> **returns -1**.  And the bus-free is produced by the
DRIVER's own final message ACK: the success path 0x213c8c writes
`ICR = ASSERT_ACK` (reg1=0x10), which through ncr5380_ack/ack_release steps
MESSAGE IN -> bus free (dev=NULL) -- so we can't prevent it from the blind-read
side.

Three fixes tried this round, all REVERTED (none opened dev_t 0x730):
* (a) "command-complete terminal": keep `dev` set + phase MESSAGE IN + CSB=0 in
  the blind MI read.  -> reached SUCCESS (state 27) but the driver's ICR-ACK
  release still tore the bus down, so the trailing call still hit err5.
* (b) minimal "drop /REQ only, let the ICR-ACK path do bus-free": still
  bdevvp: bad open (same err5).
* (c) si_csr bit0 = `s->scsi.dev` (so the state machine's benign early-exit
  `si_csr & 3 == 0` at 0x2139de fires after bus-free): **regressed the boot**
  (didn't even reach `Boot: sd`) -- the register self-test / arbitration reads
  si_csr with bit0 expected 1 while NO device is selected.  So bit0 is NOT
  simply "device selected".

REFINED next step (the si_csr-bit0 lever is a DEAD END: the PROM register
self-test and the arbitration prologue both read si_csr with bit0 expected 1
while NO device is selected, so bit0 can't be gated on busy/dev -- that is what
regressed fix (c)).  The workable lever is the **phase** the trailing calls
observe.  The state machine's phase jump table (0x213d16) is benign only for
phase 3 (STATUS) and phase 7 (MESSAGE IN); phase 0/1/2/4/5/6 all fall into the
retry/err5 path.  A trailing call in state 27 (command complete) reading phase
**7** re-takes the success branch (0x213c9a -> 0x213c24 -> reg5==0 -> 0x213c8c),
returning >= 0.  So: after a Sun blind completion, present reg2 == 7 (MESSAGE
IN) for the *trailing* calls instead of 0.  Add a `sun_cmd_complete` flag set
when the blind MI byte is taken / the command reaches bus-free, make
`sun_phase_bits` return 7 when (`!dev && sun_cmd_complete`), and clear the flag
on the next selection (MR ARBITRATE).  The hard part is not regressing the M2
PROM probe (0xfeff1072), whose own trailing calls must stay benign too -- verify
the PROM path's post-completion reg2 expectation before committing.  Validate
ALL THREE every iteration: M2 probe (`b sd(0,30,3)` -> 79xREAD, disklabel),
post-load spin cleared, AND 0x2109f0(TUR) >= 0 so bdevvp opens dev_t 0x730 ->
root mounts.  Still the polled ufsboot si driver (no Am9516 yet).

**Root did NOT mount this round.**  The reg5 live-/REQ task is complete and
committed (both paths validated); bdevvp is the next, precisely-pinned wall.

## M3 blocker #3 RESOLVED — bdevvp opens, ROOT MOUNTS, SunOS 4.1.1 kernel runs

The `bdevvp: bad open` wall is **cleared**.  Two coupled fixes were needed; with
both, auto-boot mounts the UFS miniroot as root and executes the genuine SunOS
4.1.1 sun3x kernel (`vmunix`) to memory init.  Console now reaches:

    Boot: sd(0,18,0)
    root on sd6a fstype 4.2                 <- ROOT MOUNTED
    Boot: vmunix
    Size: 564040+134024+41672 bytes          <- real kernel loaded off the miniroot
    SunOS Release 4.1.1 (MINIROOT) #1: Fri Oct 12 17:49:27 PDT 1990
    mem = 16384K (0x1000000)   avail mem = 15777792
    INVALID FORMAT CODE IN ID PROM           <- NEW wall (kernel IDPROM reader)

### Fix 1 (ncr5380.c, sun-mode) — the trailing-completion state walk

The precise mechanism of the sd-open TUR returning -1 was fully REd from the
ufsboot state machine (0x2139c4) disassembly (dumped live 0x200000..0x220000,
objdump m68k:68030).  After a no-data command completes, the transfer routine
(0x213666) loops calling the state machine until the state var `a4@0` reaches
**0 (done)** — NOT until it merely "succeeds".  Reaching 0 needs the machine to
walk state **24 -> 27 -> 32 -> 0**, and each step is gated on the reg2/reg3/reg0
the trailing calls observe:

* reg2 (phase) must read **7** (MESSAGE IN) on every trailing call, else the
  phase-0 retry path exhausts -> err5 -> -1.  (`sun_cmd_complete` gates this;
  it is set ONLY by the ufsboot blind reg0 bus-free path, so the PROM probe's
  ACK-handshake teardown never sets it and the PROM path stays benign.)
* The phase-match dispatcher (0x213d36) then routes on **reg3**: 0x08 -> the
  *phase* dispatcher (0x213d00), 0x20 -> the *BSR* dispatcher (0x213b50).
  State 27 (read the final MESSAGE byte, advance to 32) needs the phase
  dispatcher -> reg3 == **0x08**; state 32 (complete to 0) needs the BSR
  dispatcher (its only state-32 case, 0x213b50 -> 0x213b30 -> state 0) ->
  reg3 == **0x20**.  A *fixed* reg3 fails: 0x20 err5s state 27; 0x08 oscillates
  27<->32 forever (a 0x10000000-count timeout, observed as a silent hang).
* reg0 at 0x213c24 (state-27 message read) must be **0x00** (COMMAND COMPLETE)
  so the message dispatch settles into state 32 with no error.

Modelled with two flags (both cleared on selection / reset):
`sun_cmd_complete` (set at the blind reg0 bus-free) and `sun_msg_taken` (set
when the trailing reg0 message byte is taken).  In the trailing window
(`!dev && sun_cmd_complete`): reg2 -> 7; reg0 -> 0x00 (and sets `sun_msg_taken`);
reg3 -> 0x08 while `!sun_msg_taken`, then 0x20.  This walks 24->27->32->0 and the
transfer returns a non-negative byte count.  Verified: the sd-open now runs its
full identify (TUR, START/STOP, TUR, INQUIRY, READ-label) — all 5 transfers
return >= 0 (0/0/0/36/512), the label READ delivers 512 bytes with magic 0xDABE
and a valid XOR checksum.

### Fix 2 (sun3x-SUNOS-mkdisk.py) — disklabel partition size is read as 16-bit

With the SCSI path fully working, the sd-open then failed in the disklabel
partition geometry (traced to sd-open 0x210770 -> 0x211b50 label-validate OK,
then partition-size check 0x21095c).  The SunOS standalone/kernel sd driver
computes a partition's byte size as **`(dkl_nblk & 0xffff) << 9`** — it reads
`num_sectors` as a *16-bit* field (0x21094e `andil #0xffff`).  The old mkdisk
sized every partition to the whole 256 MiB disk (524288 sectors = 0x80000),
which truncates to **0** -> partition size 0 -> open fails.  Fix: size partition
'a' (index 0, the root) to the miniroot rounded up to a cylinder boundary and
clamped to 16 bits (14336 sectors = 0x3800), keeping magic 0xDABE + XOR==0.
With that, partition 'a' opens and root mounts.

### Validation (all three gates + root, deterministic, clean build)

1. M2 PROM probe: `b sd(0,30,3)` -> probe + **79x READ(6)**, disklabel 0xDABE
   validates, ufsboot loads.  PASS (boot reaches `Boot: sd(0,18,0)`).
2. ufsboot post-load spin (0x213abe) stays cleared.  PASS (boot advances to
   root mount, no hang).
3. 0x2109f0 sd-open transfers all return >= 0 -> `bdevvp` opens dev_t 0x730 ->
   **`root on sd6a fstype 4.2`**.  PASS — ROOT MOUNTED.

The PROM probe path (0xfeff1072) is untouched: `sun_cmd_complete`/`sun_msg_taken`
are set only by the ufsboot blind reg0 path, never by the PROM's ACK-handshake
teardown, so the PROM's trailing calls still read phase 0 (benign) and reg3 0x20.

Reproduce: `python3 hw/m68k/sun3x-SUNOS-mkdisk.py /tmp/sunos-media/miniroot_sun3x
/tmp/sunos-boot.img`, build to /tmp/sunos-build, then
`qemu-system-m68k -M sun3x -m 16M -bios .../sun3_80_v3.0.3.bin -drive
file=/tmp/sunos-boot.img,format=raw,if=scsi,bus=0,unit=3 -display none -serial
<chr> -icount shift=6`; send ESC to abort selftest; auto-boot mounts root.

## M4 (now exposed) — kernel IDPROM machine-type reader

After root mount + `mem = 16384K`, the sun3x `vmunix` stops at:

    INVALID FORMAT TYPE IN ID PROM
    DEFAULTING MACHINE TYPE TO SUN3X_470
    ... (banner, mem) ...
    INVALID FORMAT CODE IN ID PROM        <- halts here

This is the **kernel's** IDPROM reader (distinct from the earlier ufsboot IDPROM
issue, which was resolved by the sun3-probe bus-error hole).  Our IDPROM at
physical 0x640007D8 (MK48T02) holds a valid format=1, machtype=0x42
(SM_SUN3X|SM_3_80 = Sun-3/80), Sun OUI ethaddr, XOR checksum — the PROM reads it
correctly (banner Host ID 42000000, ethernet 8:0:20:11:22:33) and ufsboot
accepts it.  But `vmunix` reads its IDPROM/machine-type through its OWN MMU
context and rejects the format ("INVALID FORMAT TYPE"), defaults the machine to
SUN3X_470 (0x41, the Sun-3/470), then hits "INVALID FORMAT CODE" and stops.
NEXT: RE where the sun3x kernel copies/reads the IDPROM (the `sunromvec`
`v_idprom` export vs. a hardware read at its virtual mapping of 0x640007D8) and
what format/machtype byte it requires; likely needs the real-PROM handoff to
export a kernel-visible IDPROM (or the machtype byte adjusted) so the kernel
identifies the Sun-3/80 instead of defaulting.  This is a vmunix-level RE task,
materially separate from the (now-complete) si/ncr5380 bring-up.

## M4 RESOLVED — kernel IDPROM: bus-error the sun3-probe -> Sun-3/80 recognized

After root mount, the SunOS 4.1.1 `vmunix` kernel rejected its own IDPROM:

    INVALID FORMAT TYPE IN ID PROM
    DEFAULTING MACHINE TYPE TO SUN3X_470
    ... mem = 16384K / avail mem = 15777792 ...
    INVALID FORMAT CODE IN ID PROM   -> double-fault to PROM 0xfefe0502 (QEMU SIGABRT)

Fully REd from a live kernel dump (gdb hbreak-free: attach once the kernel is
running — on "SunOS Release" — and `dump binary memory 0xf8004000 0xf80c0000`;
note the PROM clock/countdown is disrupted if gdb attaches during the PROM
phase, and QEMU cpu_aborts a few ms after the second IDPROM message, so dump
during the window between kernel start and the abort).  vmunix a.out is OMAGIC,
text 0xf8004000 (no symtab); map file_off -> vaddr = 0xf8004000 + off.

* The machine-type checker (0xf805e5ba) and the format checker (0xf805e550)
  both call the IDPROM reader **0xf8061842**, which *probes* the sun3 discrete
  IDPROM at virtual **0xfedf8c00** with a bus-error-guarded peek (0xf8059d92):
  peek OK -> mode 1 (read 32 bytes from 0xfedf8c00); peek faults -> mode 2
  (read from virtual **0xfedfa7d8**).
* gva2gpa (via `monitor gva2gpa` over the gdbstub): 0xfedf8c00 -> physical
  **0x61000c00**; 0xfedfa7d8 -> physical **0x640007d8** (our valid MK48T02).
  Reading them live: 0xfedf8c00 = all zeros (our enareg absorb ACKs it), so the
  kernel took mode 1, read format byte 0 -> rejected.  0xfedfa7d8 = our IDPROM
  `01 42 08 00 20 11 22 33 ... 6b` (format=1, machtype 0x42 = Sun-3/80).
* The machine-type checker accepts machtype **0x41 (Sun-3/470)** or **0x42
  (Sun-3/80)** *if* format==1; our 0x42 would pass — the whole failure was that
  the reader returned the wrong (sun3) IDPROM.

FIX (sun3x.c, SunOS mode): add a second `sun3x_busfault_ops` bus-error hole
`idprobe_hole2` at physical **0x61000c00** (0x400, the free gap between DIAGREG
+0x800 and MEMREG +0x1000), mirroring the ufsboot `idprobe_hole` at 0x61001800.
The kernel's sun3 probe now faults like a real 3/80 -> mode 2 -> valid sun3x
IDPROM.  Result: the two INVALID messages and the DEFAULTING/double-fault are
gone; the kernel prints the ethernet address and enters device autoconfig:

    Ethernet address = 8:0:20:11:22:33
    sm0 at obio 0x66000000 pri 2
    st0..st3 at sm0 slave 32/40/24/16 ; sr0 at sm0 slave 48

Root still mounts (`root on sd6a fstype 4.2`).  Committed.

## M5 (now exposed) — kernel "sm" SCSI driver hangs during bus probe

After the SCSI target declarations, the kernel hangs (deterministic).  Live PC
samples land across the si driver (0xf80735c4, 0xf8074dc4, 0xf807390e ...) — it
is *looping* in the kernel's polled probe state machine, NOT waiting on an
interrupt.  Key structural finding: the kernel probe state machine (0xf8073586)
is **byte-for-byte the same** polled si state machine as the PROM/ufsboot one
(si_csr `&3` gate, an `a4@0/@1/@2` state/err/retry struct — here the pointer at
0xf80a6f92 -> struct 0xf80b8910, err codes err18/err19/err9/err12, the phase
dbcc-search dispatch at 0xf8074b16).  So the reg2/reg3/reg0 trailing-completion
model already in ncr5380.c should drive it too — **once the registers are
mapped where the kernel looks.**

ROOT CAUSE (pinned, not yet fixed): the kernel reads si_csr and gets **0x0**,
which fails the `(si_csr & 3) != 0` gate (0xf80735c2) -> err19 -> the caller
retries forever.  Our si_csr never reads 0 (it returns `ID|1|...`).  The kernel
reads si_csr through a pointer (0xf80b8920) = virtual **0xff003000**, and
`monitor gva2gpa`:

    virt 0xff002000 (si 5380 base) -> phys 0x66000100   (NOT 0x66000000)
    virt 0xff002008 (5380 reg0)    -> phys 0x66000108
    virt 0xff003000 (si_csr)       -> phys 0x66001100   (NOT 0x66001000)

i.e. the kernel maps the OBIO "si" device base to physical **0x66000100**, a
consistent **+0x100** vs the PROM's (and our model's) 0x66000000/0x66001000.
Our `sun3x_sireg_read` serves si_csr only at offset 0x1000, so offset 0x1100
hits `default -> 0`.  The IDPROM mapping (0xfedfa7d8 -> 0x640007d8) was exact,
so this +0x100 is specific to the si mapping, not a global PMMU offset — either
the sun3/80 onboard "sm" register block genuinely sits 0x100 into the OBIO
window (and the PROM "si" driver used 0x66000000 as a decode alias), or a
residual 68030 page-descriptor low-bits quirk for this particular OBIO PTE.

NEXT (M5, the effort the earlier notes predicted): RE the sun3/80 **"sm"**
onboard-SCSI register map (NetBSD/OpenBSD `sys/arch/sun3/dev/` sm/si + the 3/80
obio) to learn why the kernel's device base is +0x100 and the exact register
layout the "sm" driver expects, then either serve the si registers at the
kernel's offsets (alias/extend `sun3x_sireg_ops` + the NCR5380 mapping to
0x66000100/0x66001100) or fix the PTE translation.  Because the kernel probe
state machine is identical to ufsboot's, that alone may let the existing
ncr5380 sun-mode model complete the probe; the driver may then switch to its
interrupt-driven/Am9516-UDC path for normal I/O (INTR_EN/SBC_IP/DMA_IP
reflection + the UDC still unmodelled).  Hang PC 0xf80735c4; state machine
0xf8073586; si_csr ptr 0xf80b8920 = virt 0xff003000 = phys 0x66001100.

### M5 progress — the +0x100 si register mapping is fixed (err19 -> err2)

Implemented the +0x100 mapping (sun3x.c, SunOS mode): alias the NCR5380 at
0x66000108 (`scsi_a`, distinct from the PROM's 0x66000008 so no conflict) and
fold the kernel's DMA/CSR offsets (0x100/0x104 and 0x1100/0x1104/0x1108) onto
the base-0 handlers via `sun3x_si_fold()`.  A broad 0x2000 alias was tried
first and REGRESSED the PROM boot (it overlaps the PROM's si_csr at 0x66001000
-> "le: cannot initialize / No bootable devices found") — only the specific
kernel offsets may be served.  Verified: the kernel now reads si_csr = **0x1001**
(virtual 0xff003000; was 0x0), and its probe state-machine error advances from
**err19** (`si_csr & 3 == 0`) to **err2**.  No regression: PROM boot + root
mount + device autoconfig (sm0, st0-3, sr0) all unchanged.

The remaining M5 wall is now the kernel probe's **selection** step: state=0,
err=2, set at 0xf8074bfe (the selection handler, reached from the beqs at
0xf8074bac; err2 then calls 0xf8072c46).  The kernel autoconfig probes SCSI
targets 4/5/3/2/6 (st0-3 slaves 32/40/24/16, sr0 slave 48); target 3 is our
disk.  err2 is most likely "selection timeout / target not present" for the
empty targets — the kernel's selection/timeout handshake (how it expects
si_csr/CSB to behave on a selection that does/doesn't win BSY) differs from what
the sun-mode ncr5380 model presents to the polled ufsboot path, so the probe
loop wedges.  NEXT: RE the kernel "sm" selection handler (0xf80749xx..0xf8074cxx)
and the ncr5380 selection/timeout presentation for both present (target 3) and
absent targets; then the kernel may switch to its interrupt-driven/Am9516-UDC
path (INTR_EN/SBC_IP/DMA_IP still unmodelled).  Hang loop 0xf8074b16 (the phase
dbcc-search); err2 site 0xf8074bfe.

### M5 diagnosis (after the +0x100 fix) — kernel probe selects NO target

Confirmed a true wedge (no progress after 185 s), not slow probing.  gdb at the
hang (unique ports, self-reaped pid — never sweep qemu by name): PC 0xf8074dc4,
the request `a2` = 0xf80b5208 with `a2@0 = 6` (probing SCSI target 6 = sr0
slave 48), state struct 0xf80b8910: state=0 err=2 retry=0, and the target-found
bitmask `a0@(11) = 0x00`.  si_csr (virt 0xff003000) = 0x1001 (correct).

Key finding: `found11 == 0` means the kernel selected **no** target at all —
including **target 3, our attached disk** — even though the SAME polled state
machine selects target 3 fine for ufsboot (root mounts).  So the kernel "sm"
probe's selection/command sequence does not complete to the probe-success
**state 28** (set at 0xf8073c7a via the per-target found-bit test 0xf8073c5c;
err2 at 0xf8074bfe when state != 28) with our ncr5380 sun-mode model, and the
probe loop wedges on the last target (6).

Since the state-machine *code* is identical to ufsboot's and the registers now
map (si_csr=0x1001), the divergence is in the higher-level "sm" probe setup: a
different CDB/command than ufsboot's READ (likely TEST-UNIT-READY/INQUIRY with
an IDENTIFY message-out and/or a selection-with-ATN), and/or the kernel driver's
interrupt-driven completion (the notes' predicted INTR_EN/SBC_IP/DMA_IP + Am9516
UDC path) which our polled model does not service.  NEXT: instrument/trace the
exact register writes the kernel issues during a single target-3 selection
(does ncr5380_select_target/do_command even fire? is a message-out/IDENTIFY
phase expected? is it waiting on a selection-complete interrupt?), then extend
the sun-mode ncr5380 model to complete the kernel probe.  This is the
interrupt-driven "sm" driver bring-up the earlier notes scoped as the big M-final
task; the register mapping (this commit) is the prerequisite now in place.

### M5 — traced the kernel sm selection; fixed the trigger; hit the probe-completion boundary

Traced the exact register sequence the SunOS "sm" driver issues for a target
selection (per-reg write log gated to kernel PCs >= 0xf8000000, self-reaped
QEMU, unique ports).  For one probe it does, in order:

    reg3 (TCR)=0x98, reg4 (SER)=0, reg5=0            (0xf80751f6..)
    reg0 (ODR) = CDB byte  x6  (all 0x00 = TUR)      (0xf8073084, FIFO staged)
    reg2 (MR)  = target id                           (0xf80730b6)
    reg3 (TCR) = 0x98                                (0xf80730ba)
    reg1 (ICR) = 0x41                                (0xf8073178, launch)

This is byte-identical to the PROM/ufsboot "si" sequence.  The IDENTIFY message
(built at 0xf8073040..0xf807304e as 0xC0|lun) is SKIPPED for this probe (no ATN,
ICR has no 0x02), so sun_fifo holds a clean 6-byte CDB — no MESSAGE-OUT phase to
model.

FIX (committed): the kernel encodes the TARGET ID in `reg2 (MR) & 7`; the
"arbitrate" bit (0x01) is merely bit 0 of that id, set only for ODD targets.
Our sun-mode hook fired selection on MR_ARBITRATE, so it worked for the
PROM/ufsboot (target 3, odd) but never for EVEN targets — and the kernel probes
all ids.  Now selection fires whenever a CDB is staged (`sun_fifo_count > 0`)
and `!dev`, target = `MR & 7`, regardless of bit 0.  Verified the kernel
selection now fires for even targets (traced `SELECT id=6`).  No regression
(root mounts, autoconfig intact).  NOTE this is *latent* for the current single
disk at target 3 (odd, already selectable) — its value is enabling even-target
selection, a prerequisite for a full probe scan.

REMAINING BOUNDARY (banked here): the kernel probe still wedges.  It runs a
multi-channel poll loop (0xf8073522: for each channel with `si_csr & 3`, call
the state machine 0xf8073586) and a state machine byte-identical to ufsboot's.
For an ABSENT target the model returns dev=NULL / CSB=0 / phase=0 / reg3(TCR)=
0x20, and the state machine's reg3=0x20 dispatch (0xf8074afc search -> 0xf8073976
-> state-16 handler 0xf8073ab4) sets **err3**, while other paths set err2
(0xf8074bfe, state!=28) and the 30-count phase-timeout sets err17 (0xf8073966).
Observed at the wedge: state=0, err=2, retry=0, looping in the poll reads
(0xf80735e2/f4, 0xf807361e, 0xf807390a) — the absent-target probe never cleanly
"times out and advances to the next id", so the scan never reaches the present
target 3, and found-bitmask a0@(11) stays 0.  The err2 handler (0xf8072c46) is
only a diagnostic printf, so the retry lives higher up, and the whole
completion path is entangled with the driver's interrupt-driven design
(INTR_EN/SBC_IP/DMA_IP reflection + the Am9516 UDC) that our polled sun-mode
ncr5380 model does not implement.  Getting the probe to complete cleanly for
both absent ids (proper selection-timeout -> advance) and the present target 3
(selection -> IDENTIFY?/CDB -> data/status -> state 28) is the interrupt-driven
"sm" driver bring-up — a bounded-but-non-trivial next task, NOT a small tweak.

Landmarks for the next iteration:
  poll loop            0xf8073522   (per-channel, gates on si_csr & 3)
  state machine        0xf8073586   (== ufsboot 0x2139c4, +0x100 registers)
  reg3=0x20 dispatch   0xf8074afc -> 0xf8073976 -> 0xf8073ab4 (state16 -> err3)
  state!=28 -> err2    0xf8074bfe ;  30-count timeout -> err17  0xf8073966
  probe-success state  28  (set 0xf8073c7a) ; found bitmask  a0@(11)
  err2 diag printf     0xf8072c46
  si_csr / 5380 base   virt 0xff003000 / 0xff002000 = phys 0x66001100 / 0x66000100

## M5 RESOLVED — kernel SCSI probe completes; autoconfig reaches sd config

The kernel "sm" probe wedge is crossed.  Root cause (traced with per-register
kernel-PC logging, self-reaped QEMU, unique ports): the kernel completion is
**polled on si_csr bit0 as a "bus/command busy" flag**, NOT interrupt-driven
(the kernel never writes INTR_EN 0x0004).  Its state-machine inner loop
(0xf8074dc2) re-runs the body WHILE bit0==1 and exits when bit0==0; the entry
gate (0xf80735c2, si_csr & 3) and poll loop (0xf8073548) also need bit0==1.  We
hardwired bit0=1, so the loop never exited.

Fix (committed): present si_csr bit0 = 1 only while a device is selected (a
command in flight), gated to kernel PCs (0xf8000000..0xf9000000) so the
PROM/ufsboot keep bit0=1 (their self-test/arbitration read bit0 expecting 1 with
no device -- the old bit0=dev dead-end).  Also: fire selection on a staged CDB
rather than the MR "arbitrate" bit (that bit is just bit 0 of the target id, set
only for odd targets), and use scsi_cdb_length() for the CDB length (the sd
driver pads reg0 to 16 bytes).  Result:

    sm0 at obio 0x66000000 ; st0-3 ; sr0 ; sd0 sd1 sd2 sd3 sd4 sd6
      (sd6 = slave 24 = target 3 = the attached disk)

The probe now scans ALL targets, absent ids read bit0=0 (clean no-device),
present target 3 holds bit0=1 through its command and reaches probe-success
state 28 -> found-bitmask bit set -> sd6 configured.  root still mounts
(root on sd6a).  All prior gates intact.

## M6 (banked boundary) — sd device-open completion needs ASYNC completion

After sd config the kernel opens sd6 and issues a TEST-UNIT-READY, which wedges
in the STATUS phase.  Fully traced:
* The kernel status handler (0xf8073f60) reads the GOOD status, sets state=25,
  writes ICR=0x10 (ACK at 0xf8073f84) and **HOLDS ACK** (does not release it),
  then re-runs its state machine expecting the target already in MESSAGE IN.
  Our model only advanced STATUS->MESSAGE on ACK *release* (which the kernel
  never does), so CSB_REQ stayed clear (reg3=0x20) and it spun.
* Making the kernel's ACK-assert advance the bus (STATUS->MESSAGE IN, deliver
  the 0x00 COMMAND-COMPLETE message, then bus-free at the MESSAGE-IN ACK, all
  gated to kernel PCs) DOES complete the command -- traced: phase advances
  12->28, the kernel reads the message (reg0=0x00), asserts ICR=0x12 (ACK|ATN),
  dev clears, only ~11 register ops total (no spin).

BUT that completion then **crashes the kernel** (deterministic): a stack
overflow at IPL 7 (fault at f808dffc, A7=f808e000, abort to the PROM
double-fault handler 0xfefe0502).  Root cause: our model completes commands
**synchronously/instantly**, so the kernel's completion callback (0xf8074d6e)
re-issues the next command inline, which completes instantly again, recursing
until the ISP stack overflows.  On real hardware the sm completion is
**asynchronous** -- the command posts an interrupt, the callback returns, and
the next command starts from a fresh context.

So the sd-open (and all subsequent multi-command sequences: READ CAPACITY, the
disklabel/VTOC READ, root remount) need an **asynchronous, interrupt-driven sm
completion** model: assert the si/5380 completion interrupt on command-complete
(and reflect SBC_IP/DMA_IP + the Am9516 UDC for data transfers), let the kernel
ISR advance the state machine and post the callback, so completions do not
recurse.  This is the interrupt-driven "sm" driver the earlier notes scoped as
the large sub-boundary; the polled-probe path (bit0 busy) got us cleanly to sd
config, but device I/O needs the async path.  Banked here with the reverted
ACK-advance experiment documented (it advances the command but must be paired
with async completion to avoid the recursion crash).

Landmarks: status handler 0xf8073f60 (ACK 0xf8073f84, held); completion callback
0xf8074d6e; bit0 poll 0xf8074dc2; crash = ISP overflow at IPL 7 -> PROM
0xfefe0502.  The ACK-advance + CDB-length + selection + bit0 changes are the
prerequisites already in place.
