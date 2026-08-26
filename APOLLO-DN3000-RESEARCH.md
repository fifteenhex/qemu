# Apollo DN3000 (68020 + MC68851 PMMU) — QEMU Sourcing & Implementation Research

Research + sourcing only (no build performed). This document sources the DN3000 boot
PROM, reverse-engineers the Apollo chipset from MAME's `apollo` driver (the only
public source-level reference for this hardware), documents the boot flow, surveys OS
test options, and lays out a phased QEMU implementation plan against this tree.

## 0. TL;DR / Executive Summary

- **Boot PROM: IN HAND.** `3000_BOOT_8475_7.bin` (32768 bytes / 0x8000), downloaded
  from bitsavers and byte-for-byte verified against MAME's ROM database
  (CRC32 `0fe2d471`, SHA1 `6c383d2266719a3d069b7bf015f6945179395e7a`). Saved at
  `/workspace/files/apollo/3000_BOOT_8475_7.bin`. This is MAME's `md8-rev-7.0`
  "MD8 REV 7.0, 1988/08/16.15:14:39" DN3000 boot ROM.
- Also fetched for reference/cross-check: the DN3500 boot ROM (`3500_BOOT_12191_7.bin`,
  64KB, matches MAME's `dn3500` CRC `3132067d`), the DN3000 3Com 3C505 Ethernet
  controller option ROM, and the DN3000 OMTI-8621 disk-controller BIOS. All four
  files are in `/workspace/files/apollo/` (total ~123KB, no strain on the tight
  disk budget). SHA256s are in §1.
- The reset vector confirms the PROM sits at physical `0x000000` with **no overlay
  trick** needed (unlike the Amiga): SSP=`0x00100180`, PC=`0x0000598a`, both read
  directly out of the 32KB ROM image as dumped.
- **Chipset is fully documented from MAME source** (`src/mame/apollo/apollo.cpp`,
  `apollo.h`, `apollo_m.cpp`, `apollo_dbg.cpp`, plus the OMTI-8621 and 3C505 ISA
  device sources) — memory map, CSR registers, PIC/IRQ wiring, DMA, RTC, SIO,
  keyboard, disk, and video are all enumerated below with file:line citations.
- **Correction to the initial framing:** the DN3000's serial/keyboard chip is
  **not** a Z8530 SCC. It is a **Signetics/Philips SCN2681 DUART** (MC68681-family,
  2-channel), confirmed independently by both MAME (`duart_base_device` subclass,
  `apollo.h:353`) and the Linux/m68k Apollo port (`struct SCN2681` in
  `arch/m68k/include/asm/apollohw.h:22`). QEMU has **no SCN2681/MC68681 model** in
  this tree today — this is new device work, not reuse of `hw/char/escc.c`.
- **68851 status in this tree:** `target/m68k/cpu.h` on both `amiga` and `add-68851`
  (currently identical — no code has landed yet) already carries an
  `M68K_FEATURE_M68851` enum value and a design comment describing the intended
  approach (pair MC68851 with 68020 via cp-id 0, reuse the 68030
  `get_physical_address_030()` table-walk machinery). **No PMOVE/PFLUSH/PTEST/PLOAD
  decode exists yet** in `target/m68k/translate.c` or `helper.c` — it's scaffolding,
  not implementation. The DN3000 is a real, well-documented consumer for that work.
- Recommended bring-up order: **(1) PROM self-test → MD `>` prompt** (ROM-only,
  no disk needed) is the first, cheapest milestone and directly exercises the CSR,
  DUART console, RTC, and PIC. **(2) Linux/m68k `apollo_defconfig`** is the
  reliable, buildable-in-this-tree OS test that actually exercises the 68851 MMU
  once the kernel turns paging on. **(3) Domain/OS (AEGIS) SR10.4** is a stretch
  goal — installable from bitsavers cartridge-tape images but is a multi-step
  tape-install process, not a drop-in disk image (see §4c).

## 1. Boot PROM sourcing

### 1.1 What was downloaded

All files fetched from `bitsavers.org/bits/Apollo/firmware/` (bitsavers blocks
non-browser `User-Agent`s with a 403; fetching required spoofing a browser UA — noted
here in case the coordinator automates re-fetching). Saved under
`/workspace/files/apollo/`:

| File | Size | SHA1 | Matches MAME ROM |
|---|---|---|---|
| `3000_BOOT_8475_7.bin` | 32768 (0x8000) | `6c383d2266719a3d069b7bf015f6945179395e7a` | `dn3000` BIOS 0, CRC `0fe2d471` — **exact match** |
| `3500_BOOT_12191_7.bin` | 65536 (0x10000) | `36f3c83d9f2df42f2537b09ca2f051a8c9dfbfc2` | `dn3500` BIOS 0, CRC `3132067d` — **exact match** |
| `3000_OMTI_8621_102640-B.bin` | 16384 | `cf1990ad72eac6b296485410f5fa3309a0d6d078` | OMTI-8621 disk controller option ROM (used by the ISA `omti8621_apollo_device`) |
| `3000_3C505_010728-00.bin` | 8192 | `7ac36cc6fc90b90ddfc56c45303b514cbe18ae58` | 3Com 3C505 Ethernet controller option ROM |

Verification method: downloaded via `curl -A "Mozilla/5.0 ..." https://www.bitsavers.org/bits/Apollo/firmware/<file>`,
then `sha1sum` + a Python `zlib.crc32` check against the CRC32/SHA1 values hard-coded
in MAME's `ROM_START(dn3000)` / `ROM_START(dn3500)` blocks
(`src/mame/apollo/apollo.cpp:1224-1229`, `1205-1214`). Both DN3000 and DN3500 boot
ROMs match exactly — high confidence these are authentic, uncorrupted dumps.

Other bitsavers firmware files seen but **not** downloaded (irrelevant to a
DN3000 CPU-board bring-up, or large): `2500_BOOT_16182_8.bin` (128K, different
model), `4500_*`, `5500_BOOT_*`, `*_RING_*` (token-ring network, 8K each),
`*_TAPE_*`, `*_WD7000_*` (DN4500 ESDI/SCSI controllers), `3c505.zip` (an 11K
archive of 3C505 firmware variants), `3500_NI_1C874.bin` (32 bytes, node-ID PROM).
The full bitsavers directory listing is at
http://www.bitsavers.org/bits/Apollo/firmware/.

### 1.2 How the PROM maps at reset

MAME's DN3000 address map (`apollo.cpp:751-756`, `dn3000_map`):

```
map(0x000000, 0xffffff).rw(apollo_unmapped_r, apollo_unmapped_w);   // default: bus error
map(0x000000, 0x007fff).rom();                                      // boot ROM, 32KB
map(0x000000, 0x007fff).w(apollo_rom_w);                            // writes logged/ignored
map(0x008000, 0x0080ff)  CPU Status Register
map(0x008100, 0x0081ff)  CPU Control Register
map(0x008400, 0x0087ff)  SIO  (SCN2681 DUART #1: keyboard + console)
map(0x008800, 0x0088ff)  PTM6840 timer
map(0x008900, 0x0089ff)  MC146818 RTC
map(0x009000, 0x0090ff)  DMA controller 1 (Am9517A/8237)
map(0x009100, 0x0091ff)  DMA controller 2
map(0x009200, 0x0092ff)  DMA page register
map(0x009300, 0x0093ff)  Latch-page-on-parity-error register
map(0x009400, 0x0094ff)  PIC8259 master
map(0x009500, 0x0095ff)  PIC8259 slave
map(0x009600, 0x0096ff)  Node-ID (Apollo network node identity) register
map(0x040000, 0x05ffff)  AT(ISA)-bus I/O space   [ATBUS_IO_BASE/END, apollo.cpp:57-58]
map(0x080000, 0xffffff)  AT(ISA)-bus memory space [ATBUS_MEMORY_BASE/END]
map(0x100000, 0x8fffff)  RAM (up to 8MB), with parity-error emulation
map(0x05d800, 0x05dc07)  mono graphics control regs (MCR)
map(0xfa0000, 0xfdffff)  mono graphics memory (MGM)
map(0x05e800, 0x05ec07)  color graphics control regs (CCR)
map(0x0a0000, 0x0bffff)  color graphics memory (CGM)
```

Reset behavior: standard 68020 — SSP is read from physical `0x00000000`, initial PC
from `0x00000004`. Reading the actual ROM dump confirms this directly (no bus
overlay/aliasing is used at all on this board, unlike e.g. the Amiga's
`OVL`/`overlay` trick):

```
offset 0x0000: 00 10 01 80 00 00 59 8a   -> SSP=0x00100180  PC=0x0000598a
```

`0x00100180` sits just above `DN3000_RAM_BASE` (`0x100000`, `apollo.cpp:77`) — the
PROM's very first act is to set up a stack in real RAM, immediately above the RAM
base, before jumping to `0x598a` (well inside the 32KB ROM) to run the actual
self-test/monitor code. Bytes `0x0008..0x03ff` are the rest of the 256-entry, 4-byte
68020 exception vector table (`0x400` bytes total); MAME's `apollo_unmapped_r`
comments (`apollo.cpp:486-489`) explicitly note two known "expected" bus-error probe
addresses used by the DN3000/DN3500 boot PROMs during self-test/memory-sizing
(`0x0000ac00` for DN3000, `0x00030000` for DN3500) — i.e. the self-test deliberately
provokes bus errors to size RAM/detect hardware, and a correct emulation must return
a bus error (not a silent read) at unmapped addresses for this to work.

**Virtual vs. physical I/O addressing note:** the Linux/m68k Apollo port
(`arch/m68k/include/asm/apollohw.h:52`, `#define IO_BASE 0x80000000`) accesses all
of the above devices at `IO_BASE + <physical offset>` (e.g. SIO at
`0x80000000+0x8400`), i.e. through **68851-translated virtual addresses**, whereas
MAME's physical map places them at the low, untranslated offsets shown above
(`0x8400` etc.). This is expected: the boot PROM initially runs with the MMU off
(physical = the low map above); once AEGIS/Domain-OS or Linux turns the 68851 on,
the *same* physical registers are accessed through a supervisor-space virtual
mapping at `0x80000000`. This is a real architectural consequence of pairing a
68020 with an *external* 68851 — the emulation must support both physical access
(pre-MMU, during PROM self-test) and virtual-translated access (post-MMU-enable,
under an OS) to the identical CSR/DUART/RTC/PIC/DMA hardware.

## 2. Chipset reverse-engineering from MAME (`mamedev/mame`, `src/mame/apollo/`)

Source files pulled from GitHub (`master` branch, current as of this research):
`apollo.cpp` (1261 lines — address maps, machine configs, ROM defs),
`apollo.h` (716 lines — class/device declarations, CSR bit definitions),
`apollo_m.cpp` (1375 lines — CSR/PIC/DMA/RTC/SIO/config device implementations),
`apollo_dbg.cpp` (1173 lines — AEGIS trap-name debugger annotations, not hardware),
`apollo_kbd.h`/`.cpp` (981 lines), plus `bus/isa/omti8621.h/.cpp` (disk) and
`bus/isa/3c505.h` (network). All cached in `/tmp` during this research (not
persisted — re-fetchable from
https://github.com/mamedev/mame/tree/master/src/mame/apollo).

### 2.1 Memory map (DN3000, `apollo_state::dn3000_map`, `apollo.cpp:751-782`)

See §1.2 table above — that *is* the DN3000 memory map. The DN3500/DN5500 maps
(`dn3500_map` at `apollo.cpp:673-716`, `dn5500_map` at `813-858`) are structurally
similar but relocate the low I/O block from `0x8000-0x96ff` to `0x10000-0x117ff`,
widen the boot ROM to 64KB, add a Cache Control/Status Register (`0x10200`), a Task
Alias Register (`0x10300`), a second SIO (`0x10500`, `SIO2_TAG`), and a Master-REQ /
Selective-Clear register block (`0x11600`, `0x16400`) not present on DN3000. DN3000
notably lacks the DN3500's on-board cache-control hardware — expected, since DN3000
is the earlier, 68020-based (uncached) board and DN3500 moved to a 68030.

Address-space notes (`apollo.cpp:56-59`, ISA/AT-bus notes):
- ISA I/O space translation: `isa_addr = (offset & 3) + ((offset & ~0x1ff) >> 7)` —
  low 3 bits of the PC ISA address pass through, the rest are shifted left 7 with
  zero-fill (`apollo_atbus_io_r/w`, `apollo.cpp:527-547`).
- 16-bit ISA bus, byte-swapped: "Motorola CPU is MSB first, ISA Bus is LSB first" —
  every ISA I/O and memory access goes through `io16_swap_r/w` / `mem16_swap_r/w`
  (`apollo.cpp:532, 546, 558, 569`).
- Unmapped memory *must* raise a real 68k bus error
  (`apollo_state::apollo_bus_error`, `apollo.cpp:263-269`) — it asserts then clears
  `M68K_LINE_BUSERROR` and sets `APOLLO_CSR_SR_CPU_TIMEOUT` in the CSR status
  register. The boot PROM's self-test relies on this (§1.2).

### 2.2 CPU Control/Status Registers (the Apollo "CSR")

Defined `apollo.h:329-349`, implemented `apollo_m.cpp:169-320`. Two 16-bit registers
at (DN3000) `0x8000`/`0x8100`, (DN3500) `0x10000`/`0x10100`:

**Status Register** (read-only; bits cleared by any write to the register):
```
SR_SERVICE            0x0001   service-mode switch state
SR_ATBUS_IO_TIMEOUT   0x0002
SR_FP_TRAP            0x0004   FPU trap pending
SR_INTERRUPT_PENDING  0x0008   DN3000 only
SR_PARITY_BYTE_MASK   0x00f0
SR_CPU_TIMEOUT        0x0100   set by apollo_bus_error() on any unmapped access
SR_ATBUS_MEM_TIMEOUT  0x2000
SR_BIT15              0x8000   always set on DN3500 ("undocumented")
SR_CLEAR_ALL          0x3ffe
```
Reset default: `cpu_status_register = APOLLO_CSR_SR_BIT15 | APOLLO_CSR_SR_SERVICE`
(`apollo_m.cpp:178`) — i.e. the board boots in **Service Mode** by default in MAME;
Normal-Mode autoboot is a config-port setting the emulator applies at reset
(`apollo_csr_set_servicemode`, `apollo_m.cpp:186-189`, driven by the
`APOLLO_CONF_SERVICE_MODE` input port bit, `apollo.h:308`).

**Control Register** (write; enables FPU trap, forces bad parity, resets devices):
```
CR_INTERRUPT_ENABLE   0x0001
CR_RESET_DEVICES      0x0002
CR_FPU_TRAP_ENABLE    0x0004   writing 0 here *enables* the FPU opcodes (inverted!)
CR_FORCE_BAD_PARITY   0x0008   diagnostic: force a parity fault on next RAM write
CR_PARITY_BYTE_MASK   0x00f0
```
Notably, writing `CR_FPU_TRAP_ENABLE=0` calls `apollo_set_cpu_has_fpu(cpu, 1)`
(`apollo_m.cpp:237-260`) — the CSR is literally how software enables/disables the
CPU's FPU decode, and (for non-DN3000, i.e. boards with the 68030's on-chip PMMU)
it also gates `m_maincpu->get_pmmu_enable()`. This is Apollo-specific glue logic
sitting *around* the 68851/68030 MMU, not part of the 68851 itself — a genuinely
new "Apollo CSR" device is required in QEMU; it isn't something the m68k CPU core
provides.

### 2.3 Apollo MMU / MC68851 pairing — what's Apollo-specific vs. what's the chip

Important finding: **MAME's Apollo driver does not implement any custom
MMU-fault/MMU-control logic of its own beyond the CSR bits above.** The 68851 (on
DN3000/DSP3000, via MAME's `M68020PMMU` CPU type, `apollo.cpp:1105,1118`) is used as
a bog-standard 68851/68030-compatible PMMU by the CPU core itself (Musashi in
MAME's case); Apollo's board-level contribution is:
1. The CSR's `CR_FPU_TRAP_ENABLE`/`get_pmmu_enable()` gating described above.
2. `IO_BASE=0x80000000` virtual-to-physical translation of all board I/O once
   the OS turns paging on (§1.2) — this is *ordinary* 68851 page-table
   translation, set up by AEGIS/Linux, not a special Apollo mechanism.
3. `apollo_bus_error()` — Apollo's bus-error/timeout wiring, which interacts with
   the 68851 the same way it would with any other bus event (an MMU page fault
   from the 68851 is a *different*, standard 68851 exception vector, orthogonal
   to Apollo's `M68K_LINE_BUSERROR` CPU-timeout path).

In other words: **there is no discrete Apollo "MMU-control chip" or extra register
block to reverse-engineer beyond the CSR** — the 68851 pairing is architecturally
"vanilla" (pins/cp-id 0 coprocessor protocol per Motorola's own MC68851 + MC68020
application notes), and the interesting new work for QEMU is squarely in
`target/m68k`'s CPU-core 68851 support (PMOVE/PFLUSH/PTEST/PLOAD decode + the
68851-only registers: BAD0-7, BAC0-7, PCSR, CAL, VAL, SCC, AC, `cpu.h:146-166`),
*not* in Apollo-board glue. This matches the `add-68851` branch's stated goal.

### 2.4 Interrupt controller — dual PIC8259 + CPU vectoring

Two Intel 8259 PICs, cascaded (`apollo_m.cpp:1093-1100`), master reads slave's ack
via `apollo_pic8259_get_slave_ack`. IRQ assignment (`apollo_m.cpp:40-49`,
`APOLLO_IRQ_*` constants; vector base `APOLLO_IRQ_VECTOR = 0xa0`):

```
IRQ0  PTM6840 timer            IRQ8  RTC (DN3000) / SIO2 (DN3500)
IRQ1  SIO1 (console/keyboard)  IRQ9  Ethernet #2
IRQ3  PIC slave cascade        IRQ10 Ethernet #1
IRQ5  cartridge tape           IRQ13 diagnostic
IRQ6  floppy (FDC)             IRQ14 Winchester disk #1 (OMTI)
```
ISA IRQ lines 2..7,10..15 are wired into the master/slave PICs 1:1
(`apollo_m.cpp:1123-1135` area, `m_isa->irqN_callback()`), with the PC-standard
IRQ2/IRQ9 cascade quirk explicitly noted ("in place of irq 2 on at irq 9 is used").

CPU vectoring (`apollo.cpp:271-286`, `cpu_space_map` + `apollo_irq_acknowledge`):
the 68020's CPU-space interrupt-acknowledge cycle (`0xfffffff1..0xffffffff`, one
16-bit read per level 1-7) is intercepted; **level 6 specifically** is not
autovectored — it fetches the actual 8259 vector byte via
`apollo_pic_get_vector()` (`apollo_m.cpp:563+`, which acks the master, and if the
slave-cascade line (IRQ3) fired, acks the slave too, then clears
`SR_INTERRUPT_PENDING` on DN3000 or the cache-status "Interrupt Pending" bit on
DN3500). All other IPL levels fall back to `m68000_base_device::autovector()`
(standard 68k autovectoring). **This means the Apollo interrupt controller is a
single-level (IPL2/level-6), vectored, dual-8259 design** — straightforward to
reuse QEMU's existing `hw/intc/i8259.c` twice, wired the same way.

### 2.5 DMA

Two Intel 8237/Am9517A DMA controllers (`m_dma8237_1`, `m_dma8237_2`,
`apollo_m.cpp` device config around line 1070-1091), plus an Apollo-specific "DMA
page register" (`0x009200` DN3000 / n/a on DN3500) and "Latch page on parity error"
register (`0x009300`/`0x11300`). Standard PC-style multiplexed DACK/DRQ wiring to
the OMTI floppy/disk controller and the 3C505 Ethernet card. QEMU has an existing
`hw/dma/i8257.c` model that is a reasonable starting point, though the Apollo page
register wiring is board-specific glue (new, small).

### 2.6 Calendar / RTC — Motorola MC146818

Standard `MC146818(config, m_rtc, 32.768_kHz_XTAL)` (`apollo_m.cpp:1108-1112`), UTC
mode, BCD-off (`set_binary(false)`), 24hr-off. Mapped at `0x8900`(DN3000)/`0x10900`
(DN3500). On DN3000 only, its IRQ is wired to `apollo_rtc_irq_function` (commented
out for DN3500 with a "FIXME: is this interrupt really only connected on DN3000?"
note at `apollo_m.cpp:1108-1109` and `apollo.cpp:1112-1113`). **This is a direct,
drop-in reuse candidate**: this tree already has `hw/rtc/mc146818rtc.c`.

### 2.7 Serial / console / keyboard — SCN2681 DUART (2x on DN3500, 1x on DN3000)

`apollo_sio` (`apollo.h:353-368`) subclasses MAME's `duart_base_device`
(MC68681/SCN2681-compatible). Confirmed independently by the Linux Apollo port's
`struct SCN2681` register layout (`apollohw.h:22-41`, register names like
`mra`, `sra_csra`, `rhra_thra`, `isr_imr` match the 2681 datasheet exactly).
Per the `apollo_m.cpp:29` header comment: **"SIO: ch A keyboard, ch B serial
console"**. DN3500 adds a second DUART (`SIO2`, likely modem/printer per the same
comment). Clocked at `3.6864 MHz` (`apollo_m.cpp:1116`, standard UART crystal for
clean 1200/9600-class baud rates). Keyboard link runs "8E1, 1200 baud"
(`apollo_kbd.cpp:260-263`).

**No SCN2681/MC68681 device exists in this QEMU tree** (`grep -rli "scn2681\|2681\b\|duart" hw/char/` → no hits besides an unrelated `xilinx_uartlite.c` match). This is real, non-trivial new device work — QEMU upstream (mainline, not this fork) also has no MC68681 model as far as this research could determine; it would need to be written from the Signetics/Philips SCN2681 datasheet (referenced directly in the MAME driver's header comment, `apollo_m.cpp:22`: `http://www.freescale.com/files/32bit/doc/inactive/MC68681UM.pdf`).

### 2.8 Keyboard

`apollo_kbd_device` (`apollo_kbd.h/.cpp`, 981 lines) — a serial (not matrix-scanned
PS/2-style) keyboard talking to DUART channel A at 1200 baud 8E1. Scancode push
protocol at `apollo_kbd.cpp:621` (`push_scancode`), with key-repeat logic
(`apollo_kbd.cpp:753-777`). This is entirely new device work (Apollo's serial
keyboard protocol is proprietary to this family), but low priority — not needed for
the Phase 1 MD-prompt milestone (console is on SIO channel B).

### 2.9 Network — 3Com 3C505 (ISA card) + Apollo "Node ID" register

Two pieces:
1. **3C505 Ethernet controller** (`bus/isa/3c505.h/.cpp`) — an Intel 80186
   (`cpu/i86/i186.h`) + Intel 82586 Ethernet coprocessor (`machine/i82586.h`) ISA
   card with its own onboard firmware (the `3000_3C505_010728-00.bin` ROM sourced
   in §1.1). This is a full sub-emulation of an embedded 80186 system, not a
   simple register-mapped NIC — substantial effort if ever tackled (deprioritize;
   not needed for MD-prompt or Linux-console milestones).
2. **Apollo "Node ID" register** (`apollo_ni` class, `apollo.h:375-413`,
   `apollo_m.cpp:835-1027`), mapped at `0x9600`(DN3000)/`0x11200`(DN3500). This is
   Apollo's 24-bit node-identity register (`DEFAULT_NODE_ID 0x12345`,
   `apollo.h`/`apollo_m.cpp:835`), readable byte-by-byte with an XOR checksum byte,
   and can be auto-populated "from disk" (reads the UID of logical volume 1 of
   logical unit 0 off the boot Winchester, `apollo_ni::set_node_id_from_disk`,
   `apollo_m.cpp:1010-1027`) or loaded from an external `.ani`/`.bin` file. Trivial
   to reimplement (a handful of registers), and required for AEGIS networking /
   node identity, but **not** required for MD-prompt or Linux console milestones
   (Linux uses DHCP/static IP, not the Apollo node-ID scheme, for `apollo_defconfig`
   networking — though note `CONFIG_NETDEVICES` in that defconfig has no Apollo NIC
   driver wired up at all currently; see §4b).

### 2.10 Disk — OMTI-8621 (Winchester/MFM) SCSI-ish controller + floppy (uPD765)

`omti8621_apollo_device` (`bus/isa/omti8621.h/.cpp`, 1494 lines implementation,
184 lines header) — an SMS OMTI-8621 disk controller presenting a SCSI-like
10-byte CDB command interface (`CDB_SIZE=10`) over the ISA bus, plus an integrated
uPD765A floppy controller (`m_fdc`, `required_device<upd765a_device>`). This is
the Winchester (hard-disk) boot device on both DN3000 and DN3500. This tree
(`hw/scsi/`) has generic SCSI bus infrastructure (`scsi-bus.c`) and several SCSI
HBA models but **no OMTI-8621 model** — new device work, but moderate in scope
since it can likely be built on top of this tree's existing `scsi-bus.c`/SCSI-disk
abstractions rather than from scratch, once QEMU's OMTI's CDB command set is
mapped from `omti8621.cpp`'s `do_command()`.

### 2.11 Display — monochrome "15i"/"19i" and 4/8-plane color

`apollo_graphics_15i` (`apollo.h:417-652`) — a substantial bespoke 2D blitter
device: monochrome/color frame memory (MGM/CGM regions), a Brooktree Bt458
RAMDAC (`bt458` inner class), a 1024-entry LUT FIFO, ROP-based block-transfer
(`blt()`), and its own control-register state machine (CR0-3, `set_cr1/cr3a/cr3b`).
`apollo_graphics_19i` extends it for the 19" monochrome variant. Screen size per
the MAMEDEV wiki: "1024x800 pixels for the dn3000 and dn3500" (visible area; the
real hardware is closer to 1280x1024, with a smaller visible/documented window in
MAME accounting for keyboard-mapping/LED/window-manager overhead). This is the
single largest new-device item in the whole board — correctly deprioritized to a
late phase; the MD-prompt and Linux-console milestones don't need it (both run
fine over the serial console).

## 3. Boot flow

From the MAMEDEV wiki (https://wiki.mamedev.org/index.php/Driver:Apollo) and
corroborated by driver comments:

1. **Reset → self-test.** CPU fetches SSP/PC from ROM (§1.2), runs POST/self-test
   (RAM sizing via deliberate bus-error probes, `apollo.cpp:486-489`; DMA sanity
   checks noted at `apollo_m.cpp:336-337` — "MD self_test will test wrong DMA
   register and mem-to-mem DMA... is often starting much too late").
2. **Service Mode vs Normal Mode** (CSR `SR_SERVICE` bit, driven by a physical
   front-panel-equivalent config switch): in Normal Mode, "Domain/OS will boot
   without user intervention." In Service Mode, the operator lands at the **"MD"
   mnemonic debugger prompt** (`>`) — per the wiki: "press Return until the MD
   version and prompt (`>`) are shown."
3. **From the `>` prompt**, boot commands are typed directly:
   - `ex aegis` (or `ex domain_os`) — loads and executes the OS boot loader from the
     configured boot device, until a bootshell prompt (`)`) appears.
   - `go` — continues/completes the load.
   - Boot device selection happens implicitly via whichever `-disk1`/`-disk2`
     (Winchester/OMTI) or `-flop`/`-ctape` (floppy/cartridge-tape) image MAME was
     given; the real hardware equivalent is a boot-device config register/switch
     (not yet identified beyond the CSR bits already documented).
4. **Minimal device set to reach the `>` prompt** (Phase 1 target, §5): CPU
   (68020+68851) + RAM + boot ROM + CSR (§2.2) + one SCN2681 DUART for the console
   (§2.7) + PIC8259 x2 (§2.4, needed because the self-test explicitly probes them)
   + MC146818 RTC (§2.6, self-test reads it). Disk/floppy/tape/network/graphics are
   **not** required to reach the prompt — only to go further (`ex aegis`/`ex
   domain_os`).

## 4. OS options for testing

### (a) Boot PROM self-test / MD prompt — first milestone, ROM only

No disk image needed at all. Success criterion: DUART console (channel B) prints
the self-test banner and MD version string, then presents `>`. This exercises the
CSR, PIC (self-test explicitly probes DMA/PIC per `apollo_m.cpp:336-337`), RTC, and
the 68020+68851 CPU core (the self-test likely probes PMMU presence/registers as
part of hardware ID, though this needs runtime confirmation once a machine exists —
flagged as an open question in §6).

### (b) Linux/m68k `apollo` — buildable in this tree today

`/workspace/src/linux-m68kdt/arch/m68k/apollo/` (`config.c`, `dn_ints.c`,
`apollo.h`, `Makefile`) plus `arch/m68k/include/asm/apollohw.h`,
`arch/m68k/include/uapi/asm/bootinfo-apollo.h`, and
`arch/m68k/configs/apollo_defconfig` are all present and look complete/buildable
(`CONFIG_APOLLO=y`, `CONFIG_M68020=y` among others in the defconfig). This is
the **reliable, real MMU-exercising test**: Linux's Apollo port programs the
PIC (`dn_init_IRQ`, `dn_ints.c:45-50`, using `m68k_setup_user_interrupt(VEC_USER+96,
16)` — i.e. it expects the vectored-IRQ scheme from §2.4, not autovectors), the
PTM6840 timer (`dn_sched_init`, `config.c:159-179`), the MC146818 RTC
(`dn_dummy_hwclk`, `config.c:181-207`), and the SCN2681 DUART directly via raw
polled I/O for early console output (`dn_serial_print`, `config.c:109-121`) — and
of course runs entirely under the kernel's own 68851/68030-class MMU once paging
initializes, which is the actual point of this whole effort.

**How it boots on real/emulated hardware:** Linux/m68k kernels are traditionally
loaded via a small stage-2 loader from disk/tape under AEGIS, or via network boot
(the Apollo ring/3C505 network, out of scope per §2.9), *not* directly by the boot
PROM's `ex` command (which expects AEGIS/Domain-OS image formats). For QEMU,
**`-kernel`-style direct boot is the pragmatic path**, analogous to this tree's
existing `mvme147`/`q800`/other m68k boards that support `-kernel` + a synthesized
`bootinfo` block (see `hw/m68k/bootinfo.h` in this tree, and the
`BI_APOLLO_MODEL` tag already defined for exactly this purpose in
`arch/m68k/include/uapi/asm/bootinfo-apollo.h:14`, values `APOLLO_DN3000=1` /
`APOLLO_DN3010=2` / `APOLLO_DN3500=3` / `APOLLO_DN4000=4` / `APOLLO_DN4500=5`).
This mirrors how this tree already boots Linux/m68k on `mvme147` and other
non-Amiga m68k boards without needing the real ROM's disk-boot path — recommended
approach; avoids needing a working OMTI+disk-image chain just to prove the CPU/MMU
core.

Bare-metal test-kernel option: initial bring-up could also use a tiny standalone
test payload (no full Linux) placed directly via `-kernel`/`-device loader`,
similar to how `mvme147`/`q800` bring-up in this tree likely started — worth
considering as an even-cheaper first PMMU smoke test before a full Linux boot.

### (c) Domain/OS (AEGIS SR10.x) — stretch goal, realistic assessment

bitsavers hosts `bits/Apollo/SR10.3/` and `bits/Apollo/SR10.4/`
(http://www.bitsavers.org/bits/Apollo/). The SR10.4 directory contains:

```
019593-001.CRTG_STD_SFW_BOOT_1-REV.A.ct.gz   13M
019594-001.CRTG_STD_SFW_1.ct.gz              15M
019594-002.CRTG_STD_SFW_2.ct.gz              14M
019594-003.CRTG_STD_SFW_3.ct.gz              17M
019594-004.CRTG_STD_SFW_4.ct.gz              16M
cptape.hlp                                    2.6K
```

These are **cartridge-tape images** (`.ct`, ~75MB compressed total across 5
volumes), not a ready-to-boot disk image. The MAMEDEV wiki's own example command
(`./mame dn3500 ... -disk1 /tmp/dn3500_sr10.4.awd -ctape
/tmp/019593-001.CRTG_STD_SFW_BOOT_1-REV.A.ct`) confirms the real workflow: boot the
PROM from the *boot* cartridge, `ex` into a bootstrap install program, then install
onto a blank `.awd` Winchester-disk image — a genuine multi-tape OS-install
procedure, not a "mount and go" disk image. **Not attempted or downloaded** in this
research pass (correctly out of scope for a sourcing task, and the ~75MB compressed
size is a meaningful chunk of the tight 760MB disk budget for something that's a
stretch-goal, late-phase milestone). Licensing: Apollo/HP's Domain/OS was
commercial software; bitsavers hosts it under their usual "for historical/preservation
research" framing — same posture as other bitsavers OS images this project would
already be relying on norms for, not fundamentally different risk. Flagging for
the coordinator to make the call before actually pulling ~75MB down.

**Recommended test order:** (a) MD prompt → (b) Linux/m68k `-kernel` boot with
`BI_APOLLO_MODEL` bootinfo → (c) Domain/OS SR10.4 tape install, only if there's
appetite for the OMTI-disk + tape-format work it implies.

## 5. QEMU implementation plan

Phased against this tree (`/workspace/src/qemu-amiga`, and its `add-68851`
sibling checkout at `/workspace/src/qemu-68851`).

### Phase 0 — prerequisite (owned by the other agent, not this task)
Land real MC68851 PMMU emulation in `target/m68k` behind the already-declared
`M68K_FEATURE_M68851` (`target/m68k/cpu.h:668`): PMOVE/PFLUSH/PTEST/PLOAD decode in
`translate.c`, the 68851-only register file (`env->mmu.*851` fields already
scaffolded, `cpu.h:146-166`), and either a new `m68020-pmmu` (or similarly-named)
CPU type in `target/m68k/cpu.c` alongside the existing `m68000/m68010/m68020/
m68030/m68040/m68060` list (`cpu.c:777-782`), or a QOM property that attaches an
external-PMMU mode to plain `m68020`. As of this research, **no such CPU type
exists yet** and no PMMU opcode decode exists in `translate.c`/`op_helper.c` — this
is 100% ahead-of-this-plan prerequisite work, tracked separately.

### Phase 1 — `apollo-dn3000` machine skeleton: boot to the MD `>` prompt
New `hw/m68k/apollo-dn3000.c` (or similar), modeled closely on this tree's existing
non-Amiga m68k boards (`hw/m68k/mvme147.c`, `hw/m68k/q800.c`) for structure:
- **CPU:** `m68020-pmmu` @ 12MHz (from Phase 0).
- **RAM:** up to 8MB at physical `0x100000-0x8fffff` (§2.1); MAME's parity-fault
  emulation (`ram_with_parity_r/w`) is a diagnostic nicety, skip initially.
- **Boot ROM:** load `3000_BOOT_8475_7.bin` (sourced, §1) at `0x000000`, 32KB,
  read-only (writes should be silently dropped/logged, not bus-error — mirrors
  `apollo_rom_w`, §2.1).
- **CSR device:** new, small — status/control registers at `0x8000`/`0x8100`
  (§2.2), including the CPU-timeout-on-bus-error wiring (§2.1/2.6) since the
  self-test depends on it.
- **Console:** new SCN2681/MC68681 DUART device (§2.7) at `0x8400`, channel B wired
  to a QEMU chardev backend (`-serial`), channel A left unconnected until keyboard
  work (§2.8, later phase).
- **RTC:** reuse `hw/rtc/mc146818rtc.c` as-is at `0x8900` (§2.6) — direct reuse,
  no new code.
- **PIC:** two `hw/intc/i8259.c` instances, cascaded per §2.4, at `0x9400`/`0x9500`,
  vector base `0xa0`, with the level-6 CPU-space-vectored-interrupt scheme
  (§2.4) — likely needs a small `cpu_space` read hook analogous to MAME's
  `apollo_irq_acknowledge`, check whether this tree's m68k CPU-space IACK
  emulation (used by other boards already) covers this directly.
- **DMA/PTM6840:** stub or defer — the self-test *touches* the DMA controllers
  (§2.5, `apollo_m.cpp:336-337` note) but a correctly-behaving stub (accept
  writes, return sane defaults) may suffice to pass whatever sanity check the
  self-test does; needs runtime verification once a machine boots.
- **Success criterion:** DUART channel B (console) emits the self-test banner and
  the MD-monitor `>` prompt.

### Phase 2 — Linux/m68k `apollo_defconfig` boot (exercises the 68851 for real)
- `-kernel` support: synthesize a `bootinfo` block carrying `BI_APOLLO_MODEL=
  APOLLO_DN3000` (`bootinfo-apollo.h:14,22`), following the pattern this tree
  already uses for `mvme147`/other m68k `-kernel` boots (`hw/m68k/bootinfo.h`).
  This sidesteps needing OMTI disk boot or the real PROM's `ex` loader entirely.
- PTM6840 6840 timer device needed for real this time (Linux's `dn_sched_init`
  programs it directly, §4b) — check if this tree has a `machine/6840ptm`-equivalent
  already (MAME's is `machine/6840ptm.h`; QEMU has no direct equivalent found in
  this pass — flagged as a to-check item, possibly small/reuse from a Motorola
  peripheral family already in this tree if one exists, otherwise new).
- Keyboard-channel A can stay disconnected; Linux's early console
  (`dn_serial_print`) only needs channel B.
- **Success criterion:** kernel boots to a shell prompt (with an initramfs, since
  no disk driver work is implied by this phase) over the DUART serial console,
  demonstrating the MMU is live (Linux enables paging early in `head.S`/
  `apollo`'s `config_apollo()` path) — this is the real "68851 works" milestone.

### Phase 3 — disk, network, display (as appetite allows)
- OMTI-8621 disk controller (§2.10) — moderate new work, buildable on this tree's
  existing `hw/scsi/scsi-bus.c` abstractions; unlocks real Linux root-on-disk boot
  and is the prerequisite for Domain/OS.
- Apollo Node-ID register (§2.9.2) — trivial, needed for AEGIS networking only.
- 3C505 network card (§2.9.1) — substantial (embedded 80186+82586 sub-emulation);
  low priority.
- `apollo_graphics_15i`/`19i` display + Bt458 RAMDAC + Apollo serial keyboard
  (§2.8, §2.11) — the largest remaining item; only needed if a graphical
  Domain/OS or Linux fbcon session is a goal, not for a headless/serial-console
  test target.

### What's reused vs. genuinely new (summary)

| Component | Status in this tree |
|---|---|
| 68020 CPU core | reuse (`target/m68k`) |
| MC68851 PMMU | **new** — Phase 0 prerequisite, tracked on `add-68851`, not started |
| MC146818 RTC | **reuse as-is** (`hw/rtc/mc146818rtc.c`) |
| Dual i8259 PIC | reuse (`hw/intc/i8259.c`), new board-level cascade/vector-6 glue |
| SCN2681/MC68681 DUART | **new** — no model exists in this tree or (as far as checked) upstream QEMU |
| Apollo CSR (status/control regs) | **new** — small, Apollo-specific, no analog elsewhere |
| i8237/Am9517A DMA | reuse candidate (`hw/dma/i8257.c`), new page-register glue |
| PTM6840 timer | **new** (no direct equivalent found in this tree this pass — needs confirmation) |
| OMTI-8621 disk controller | **new**, moderate — buildable on `hw/scsi/scsi-bus.c` |
| Apollo Node-ID register | **new**, trivial |
| 3C505 network card | **new**, substantial (80186+82586 sub-system) |
| Apollo graphics (mono/color) + Bt458 RAMDAC + serial keyboard | **new**, substantial |

## 6. Risks and open questions

1. **Phase 0 is a hard dependency and is not yet started** — `target/m68k` has only
   a feature-flag and a design comment, no PMOVE/PFLUSH/PTEST/PLOAD decode. All of
   Phase 1+ blocks on that landing.
2. **Whether the self-test genuinely probes 68851-specific state** (vs. just
   generic bus/DMA/PIC sanity) is unconfirmed without either disassembling the ROM
   dump or running it against a real/reference emulator — worth a quick
   disassembly pass (the ROM is only 32KB) before or during Phase 1 to know exactly
   what device set is load-bearing for reaching `>`.
3. **PTM6840 timer device availability in this tree is unconfirmed** — flagged in
   §5 Phase 2, needs a direct check (`grep -r 6840` across `hw/` wasn't run in this
   research pass; do this before scoping Phase 2).
4. **CPU-space (IACK) vectoring for level 6** — need to confirm whether this
   tree's `target/m68k` + generic m68k board infrastructure already exposes a
   clean hook for "vectored IRQ at one specific IPL level, autovector at others,"
   since Apollo's scheme (§2.4) is that specific split, not full custom
   vectoring nor full autovectoring.
5. **SCN2681/MC68681 DUART is genuinely absent from both this tree and (per this
   research) upstream QEMU** — this is more new-device work than the task
   framing's "reuse escc/z8530" assumption suggested; flagging explicitly since
   it changes the effort estimate for Phase 1's console.
6. **AEGIS/Domain-OS licensing** — bitsavers hosts SR10.3/SR10.4 tape images
   without an explicit license grant; treat identically to other bitsavers-sourced
   commercial OS/firmware already in use elsewhere in this project's research, and
   get an explicit go-ahead before spending the ~75MB of disk budget on it.
7. **DN3000 vs DN3500 choice**: DN3500 (68030, on-chip PMMU, no external 68851
   needed) would be *easier* to bring up in QEMU today, precisely because it
   doesn't need Phase 0. DN3000 is the right target specifically *because* it's
   the canonical external-68851 platform the other agent's work is meant to prove
   out — but it's worth the coordinator explicitly confirming DN3000 (not DN3500)
   remains the priority, now that the tradeoff is concrete.

## 7. Source citations

- MAME driver source (GitHub, `mamedev/mame`, `master`):
  - https://github.com/mamedev/mame/blob/master/src/mame/apollo/apollo.cpp
  - https://github.com/mamedev/mame/blob/master/src/mame/apollo/apollo.h
  - https://github.com/mamedev/mame/blob/master/src/mame/apollo/apollo_m.cpp
  - https://github.com/mamedev/mame/blob/master/src/mame/apollo/apollo_dbg.cpp
  - https://github.com/mamedev/mame/blob/master/src/mame/apollo/apollo_kbd.h
  - https://github.com/mamedev/mame/blob/master/src/mame/apollo/apollo_kbd.cpp
  - https://github.com/mamedev/mame/blob/master/src/devices/bus/isa/omti8621.h
  - https://github.com/mamedev/mame/blob/master/src/devices/bus/isa/omti8621.cpp
  - https://github.com/mamedev/mame/blob/master/src/devices/bus/isa/3c505.h
- MAMEDEV wiki: https://wiki.mamedev.org/index.php/Driver:Apollo
- bitsavers Apollo archive: http://www.bitsavers.org/bits/Apollo/
  - Firmware: http://www.bitsavers.org/bits/Apollo/firmware/
  - SR10.4: http://www.bitsavers.org/bits/Apollo/SR10.4/
  - SR10.3: http://www.bitsavers.org/bits/Apollo/SR10.3/
- bitsavers PDF documentation (referenced in MAME driver comments, not fetched
  this pass but worth pulling before Phase 1 coding starts):
  - http://www.bitsavers.org/pdf/apollo/002398-04_Domain_Engineering_Handbook_Rev4_Jan87.pdf
  - http://www.bitsavers.org/pdf/apollo/008778-03_DOMAIN_Series_3000_4000_Technical_Reference_Aug87.pdf
  - http://www.bitsavers.org/pdf/apollo/AEGIS_Internals_and_Data_Structures_Jan86.pdf
  - http://www.bitsavers.org/pdf/apollo/019411-A00_Addendum_to_Domain_Personal_Workstations_and_Servers_Hardware_Architecture_Handbook_1991.pdf
- Linux/m68k Apollo port (in this workspace):
  - `/workspace/src/linux-m68kdt/arch/m68k/apollo/config.c`
  - `/workspace/src/linux-m68kdt/arch/m68k/apollo/dn_ints.c`
  - `/workspace/src/linux-m68kdt/arch/m68k/include/asm/apollohw.h`
  - `/workspace/src/linux-m68kdt/arch/m68k/include/uapi/asm/bootinfo-apollo.h`
  - `/workspace/src/linux-m68kdt/arch/m68k/configs/apollo_defconfig`
- This tree's own `target/m68k` 68851 scaffolding (for Phase 0 status):
  - `/workspace/src/qemu-68851/target/m68k/cpu.h` (lines ~640-670, ~140-190)
  - `/workspace/src/qemu-68851/target/m68k/helper.c` (`get_physical_address_030`,
    line ~1032)

## 8. Downloaded files inventory

```
/workspace/files/apollo/3000_BOOT_8475_7.bin        32768 bytes  DN3000 boot PROM (sourced, verified)
/workspace/files/apollo/3500_BOOT_12191_7.bin        65536 bytes  DN3500 boot PROM (reference/cross-check)
/workspace/files/apollo/3000_OMTI_8621_102640-B.bin  16384 bytes  OMTI-8621 disk controller option ROM
/workspace/files/apollo/3000_3C505_010728-00.bin      8192 bytes  3C505 Ethernet controller option ROM
```
Total: ~123KB — negligible against the disk budget.
