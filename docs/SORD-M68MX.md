# SORD M68MX — Machine Description and Hardware Reference

*A preservation / machine-description document for the SORD M68MX, a
Japanese MC68000 CP/M-68K workstation of the mid-1980s, and its QEMU
emulation (`qemu-system-m68k -M sord-m68mx`).*

This document is self-contained. It is written from two authoritative
sources and cross-checked between them:

* `SORD-M68MX-NOTES.md` — the accumulated reverse-engineering journal.
* `hw/m68k/sord_m68mx.c` — the QEMU machine implementation (authoritative
  for exact addresses and register behaviour).

Every address and behaviour below has been checked against the emulator
source. Where the hardware is only partially understood, or where the
emulation deliberately simplifies or approximates, this is stated
explicitly rather than guessed. To our knowledge this is the first
emulation of the SORD M68MX.

---

## 1. Overview and history

The **SORD M68MX** was built by **SORD Computer Corporation** (Japan),
with a boot ROM dated **1985** ("M68MX BOOT ROM REVISION 01E", and an
in-ROM banner "(C) COPYRIGHT 1985, SORD"). It is a **Motorola MC68000**
business/engineering workstation running **CP/M-68K**, with:

* a memory-mapped **80×25 text console** driven by an HD6845 CRTC
  (the CP/M BIOS ships its own "SORD CRT" console driver that writes the
  video RAM directly — the console is the display, not a serial line);
* a full **kanji / JIS** software stack (kanji CP/M utilities, fonts,
  dictionaries; the boot ROM itself has both an ASCII and a
  Japanese/kanji message set);
* an optional **SGX bitmap graphics board** ("SORD Graphics eXtension"),
  640×500 in 16 colours, with a resident CP/M driver (`SGX.SYS`) and a
  `trap #1` graphics API;
* SORD's integrated application suite **LEONIS** (launched from the
  startup batch via `MENUMAN LEONIS.MDT`), the **GEDIT** "G-Editor"
  graphics editor, and various communications and kanji tools;
* an **intelligent ("smart") keyboard** and a **graphics locator**
  ("puck" / direction pad) as the two human-input devices.

The machine boots CP/M-68K from 8-inch floppies. The two surviving disk
images used here (`M68MX_1.imd` / `M68MX_2.imd`, from OldComputers.es)
carry the system, utilities, kanji tools and the SGX/LEONIS graphics
suite.

**Emulation status in one line:** CP/M-68K boots to the `A>` prompt and
is fully interactive over the modelled keyboard; with the SGX board,
smart keyboard and locator enabled, the GEDIT editor runs and draws real
application pixels onto the 640×500 graphics plane. See §10 for the
honest list of gaps.

---

## 2. CPU and reset

| Item | Value |
|---|---|
| CPU | Motorola **MC68000** (QEMU `m68000`; the only valid CPU type for this machine) |
| Reset SSP | first ROM longword, `*(0xE00000)` (big-endian) |
| Reset PC | second ROM longword, `*(0xE00004)` |
| Address space | 24-bit (16 MB); I/O lives in the `0xE00000`–`0xE8xxxx` region |
| Default RAM | 1 MB (`sord.ram`), mapped at `0x000000` |

At reset the 68000 fetches its initial supervisor stack pointer and
program counter from the first two longwords of the boot ROM. The
emulator reads these from the loaded ROM image at machine-init time and
installs them on every reset (`sord_machine_reset`). The ROM's own
initial SP is `0x10000` (it therefore requires at least 64 KB of DRAM).

**Boot device selection is strap-driven** (see §4): the ROM reads a
configuration strap register and decides whether to boot from floppy /
hard disk, drop into the Sunbug monitor, or run a serial S-record
loader. The default modelled strap (`0x82`) selects **floppy boot**.

**Bus-error probing.** The ROM sizes DRAM and probes for optional
devices by touching addresses and catching bus errors. On real hardware
these are ordinary 68000 group-0 bus-error exceptions; the ROM's
vector-8 handler does `tas 0x0; addal #8,sp; rte` and relies on the
14-byte group-0 stack frame plus a two-NOP "skid pad" after each probe.
This required a fix in QEMU's m68k core to push the classic group-0
frame (with PC = faulting instruction) and to let the re-run access
complete as an unassigned access instead of re-faulting. This is a CPU
core detail, not part of the machine model, but it is why unmapped
regions must genuinely bus-error (see the HD/SCC probes in §3).

---

## 3. Memory map

The MC68000 sees RAM at the bottom of the address space and the ROM plus
all I/O near the top. The table below is the complete map as modelled;
sizes are the mapped region sizes in the emulator.

| Address | Size | Region / device | Description |
|---|---|---|---|
| `0x000000` | 1 MB (default) | **DRAM** (`sord.ram`) | Main memory; sized by the ROM via bus errors. Vector table + ROM scratch live here. Everything unmapped bus-errors on purpose. |
| `0x020000` | — | **Floppy boot load address** | ROM loads the boot sectors here, checks the `"M68MX-ID"` signature at `0x20004`, then `jmp 0x20000`. |
| `0x0DFFF0` | — | **Expansion ROM probe** | Guarded read of an 8-byte `"M68MX-ID"` signature; unmapped ⇒ treated as absent. |
| `0xE00000` | 16 KB (`0x4000`) | **Boot ROM** (`sord.rom`) | Reset SSP/PC in the first 8 bytes. Word checksum of `0..0x1ECE` must sum to zero. Contains POST, the Sunbug monitor, the S-record loader, the ASCII+kanji message sets, the keysym table, and `trap` vector stubs. |
| `0xE10000` | 16 KB region (`sord.vram`) | **Text VRAM** | 80×25 cells, **one longword per cell**: attribute word then character word. Text occupies `0xE10000`–`0xE11FFF` (8 KB). Cleared at boot to `0x0007_0020` (attr 0x0007, char 0x20 = space). |
| `0xE12001, 03, … , 1F` | (within VRAM region) | **Display-type / kanji ID bytes** | 16 odd bytes. **All-zero ⇒ Japanese/kanji message mode** (ROM sets the "bit 9" display flag); any non-zero byte ⇒ ASCII message set. The emulator writes `0xFF` to `0xE12001` at reset, so it **defaults to ASCII messages**. |
| `0xE20201 + 2·ch` | 2 bytes/ch | **DMA channel address** | Custom 4-channel DMA. Two byte-writes per channel (lo then mid); a per-channel flip-flop sequences them. FDC = channel 1, HD = channel 3. |
| `0xE20209` | 1 | **DMA control** | bit0 = go/master, bits1–4 = per-channel enable (`1<<ch`), bit5 = direction. |
| `0xE20211 + 2·ch` | 2 bytes/ch | **DMA channel count** | Two byte-writes (lo then hi). Remaining bytes = `(value & 0x3FFF)+1`; the top two bits of the 16-bit value encode direction (`0x4000` = device→memory, `0x8000` = memory→device). |
| `0xE20281 / 0xE20283` | 1 / 1 | **µPD765 / i8272 FDC** | Main Status Register (`0x281`) and data port (`0x283`). Completion is delivered on **interrupt vector 0x45**. See §7. |
| `0xE20301` | 1 | **Addressable latch** (LS259-style) | Written byte: bits2–0 = bit address, bit3 = data value; read-back returns the 8 latched bits. Known bits: bit1 = speed/density, bit2 = motor/ready, bit5 = write precompensation, bit7 = RTC hold. Boot init sequence `01 0A 03 0C 05 06 07`. |
| `0xE20305/307/30F` | 1 | Aux latch ports | Written during init; modelled as no-ops. |
| `0xE2030B / 0xE2030D` | 1 / 1 | **RTC** (MSM5832-style) | Nibble address / data registers 0–12, gated by latch bit7. Not battery-backed in the model: data reads as **0**. |
| `0xE20381` | 1 | **SGX plane READ-select** | Write `0..3`: which of the 4 bit-planes is read back through the `0xE40000` window. (Decode is shared with the interrupt-controller region — see the note below and §10.) |
| `0xE203A1` | 1 | **SGX plane WRITE-mask** | Write bits0–3 (= plane 1/2/4/8): which planes receive writes to `0xE40000`. |
| `0xE203C1 / 0xE203E1` | 1 | **Interrupt controller** | `0xE203E1` programs the vector base (`0x40`). Sources: keyboard (0x42), FDC (0x45), HD (0x46). Modelled as a stub that ORs pending sources and delivers the lowest-numbered vector at IPL 2. |
| `0xE20781` | 1 (read) | **Strap register** | Configuration straps; boot-device select. See §4. Default modelled value `0x82` (floppy). |
| `0xE207A0 / 0xE207A1` | 2 | Strap latch enable(?) | Word `0xA1A1` written at boot; modelled as no-op. |
| `0xE207C1 + n` | 1 | **DMA page registers** | Address bits 23–16 per channel; channel 1 at `+0`, channel 3 at `+2`. |
| `0xE30001 / 0xE30003` | 1 / 1 | **HD6845 CRTC** | Address / data registers. 80×25; cursor position read/written via R14/R15. See §5. |
| `0xE30081`–`0xE3008F` | — | movep-programmed timer/counter(?) | Programmed with `movep` (`0x82, 0x4EF1, 7, 0xEE`); function unknown; modelled as no-op. |
| `0xE30141 / 0xE30143` | 1 / 1 | **MC6850 ACIA #1** | The **graphics locator** ("puck") port (also the ROM/BIOS "aux/mouse" probe). See §6. |
| `0xE30161 / 0xE30163` | 1 / 1 | **MC6850 ACIA #0** | The **console / smart keyboard** port; also the Sunbug console and the S-record loader line. See §6. |
| `0xE40000` | 64 KB (`0x10000`) | **SGX graphics plane window** | One 64 KB window onto the graphics VRAM; the board is **4 bit-planes banked** through this single window (read plane via `0xE20381`, write mask via `0xE203A1`). Also the ROM's 64 KB RAM-present probe target (unbanked default keeps that probe working). |
| `0xE50000, 02, … , 1E` | 32 B (`0x20`) | **SGX palette** | 16 colour registers, one byte at each even word offset. Boot ROM + driver load an identity ramp 0..15. |
| `0xE50001` | 1 (read) | **SGX board-present** | bit7 = board present, **active-low** (`btst #7,0xE50001`; 0 ⇒ present). Modelled as present (returns 0x00). |
| `0xE80005` | — | **HD controller probe** | Guarded read; left **unmapped** so the probe bus-errors ⇒ "Hard Disk is not connected". |
| `0xE80101 / 0xE80105` | — | **Z8530 SCC probe** | Guarded read; left **unmapped** ⇒ reported absent. |

**Note on `0xE20381` / `0xE203A1`.** The early memory-map table in the
notes labelled the block `0xE20381/3A1/3C1/3E1` as "interrupt
controller". Reverse-engineering the SGX driver later showed that
`0xE20381` and `0xE203A1` are actually the **graphics plane
read-select** and **write-mask**; only `0xE203C1` / `0xE203E1` are the
interrupt controller. The emulator implements the graphics meaning. In
this model the interrupt controller delivers vectors directly and does
not use those two ports, so the shared decode is harmless — but if
interrupt masking is ever needed this overlap should be re-checked
against real hardware.

**POST test-flag bits** (`d7` during power-on self test): bit0 RAM OK,
bit1 HD controller present, bit5 `0xE40000` 64 KB RAM present, bit6 SCC
present, bit8 expansion ROM at `0xDFFFF0` present, bit9 Japanese display
mode.

---

## 4. Boot / strap sequence

### 4.1 POST (power-on self test)

1. Status-register / CPU-register test, then ROM checksum (word sum of
   `0..0x1ECE` must be zero).
2. Initialise the DMA controller, the HD6845 CRTC, and the addressable
   latch (`01 0A 03 0C 05 06 07`); clear the text VRAM to spaces.
3. Guarded device probes: HD controller (`0xE80005`), SCC
   (`0xE80101/105`), expansion ROM (`0xDFFFF0`); test the optional 64 KB
   RAM bank at `0xE40000` with an `0xAA55` readback; size DRAM upward
   from `0x400` until a bus error.
4. Clear the screen, read the strap register, and branch on boot device.

### 4.2 Strap register semantics (`0xE20781`, read)

The strap byte is read and interpreted by the ROM (the emulator simply
returns the configured byte; the decode below is the ROM's):

* **Boot device** = `(~strap) >> 6 & 3`:
  `1` = Floppy, `2` = Hard Disk, `3` = first active device.
* If that field is `0`, the raw **bit 5** decides: set ⇒ enter the
  **Sunbug monitor** (on ACIA0); clear ⇒ run the **serial S-record
  loader** on ACIA0 (a `*` prompt).
* Low-nibble sanity: bit1 must be set, bit2 must be clear, and the value
  must not be `0xF` — otherwise the ROM takes a "Strap ID Error" path.

Modelled strap values:

| `strap=` | Result |
|---|---|
| `0x82` (default) | Boot from **floppy** |
| `0xE2` | **Sunbug monitor** on serial ("M68MX Sunbug Ver.01A / (C) Copyright SORD ,1985") |
| `0xC2` | **S-record loader** on ACIA0 (`*` prompt) |

### 4.3 Floppy boot and CP/M-68K bring-up

For a floppy boot the ROM:

1. Issues `SENSE DRIVE STATUS` to units 0–3 until one reports READY
   (ST3 bit5).
2. Detects density: `RECALIBRATE`×2, `SEEK` to cylinder 2, then
   `READ ID` trying modes `0x48` (FM), `0x40` (MFM), `0x58`, `0x50`
   until one succeeds; the sector-size code `N` from the ID field is
   folded into the mode. For these double-density disks the result is
   mode `0x41` (MFM, N=1, 256-byte sectors).
3. Picks the boot CHS: mode `0x41/0x42` ⇒ start at (cyl 1, head 0,
   sec 1); otherwise (0,0,1). Reads `min(0x1000 >> (N+7), 26)` sectors —
   16 × 256 B for these disks — over **DMA channel 1** into `0x20000`.
4. Verifies `"M68MX-ID"` at `0x20004`, then `jmp 0x20000`, passing
   register state (drive status, memory top, POST test flags, etc.).

The ROM's only *visible* boot output is the line
`"Boot Device : Floppy Disk (Set Diskette)"` drawn into VRAM at row 20;
the "(C) COPYRIGHT 1985 SORD" banner string in the ROM is **dead data**
(no code path references it). After the jump, CP/M-68K comes up: its BIOS
carries the **"SORD CRT 00A05" console driver** that writes VRAM
directly (escape-sequence aware), so all further output is on the video
display. On these disks the boot chain continues through
`STARTUP.BAT` → (renames `AUTOEXEC.SUB`) → `SGX` (installs the graphics
driver) → … → `MENUMAN LEONIS.MDT` (SORD's integrated software). See §8.

---

## 5. Video subsystem

The display is a **640×500** raster. The emulator presents a single
640×500 graphics console and, each frame, composites the SGX bitmap
plane (when anything is drawn) *under* the text layer.

### 5.1 Text mode (HD6845 CRTC + text VRAM)

* **CRTC** (`0xE30001` address / `0xE30003` data): programmed for
  80×25. Observed register values include `R1 = 0x50` (80 displayed
  chars), `R6 = 0x19` (25 displayed rows), `R9 = 0x13` (character-cell
  height = 20 scanlines). The cursor address is held in **R14/R15**,
  read and rewritten by the BIOS on each character output; the emulator
  computes the cursor cell as `(R14<<8)|R15`.
* **Text VRAM cell format** (`0xE10000`, one longword per cell): a
  16-bit **attribute** word followed by a 16-bit **character** word. The
  boot clear value is `0x0007_0020`. Attribute values `0x0007` / `0x000F`
  are used (normal / bright); the emulator treats attribute low-nibble
  non-zero as "foreground on" (bright grey) vs. dim.
* **Raster**: 80 columns × 8-pixel font = **640** across; 25 rows × 20
  scanline cells = **500** down (matching CRTC R9=0x13).
* **Font**: the emulator renders glyphs from a bundled 8×16 VGA font
  (`vgafont16`) inside the 20-scanline cell (16 painted rows, 4 blank).
  The *real* SORD character font is not known; this is a cosmetic
  approximation. See §10.

### 5.2 SGX bitmap graphics board

The optional **SGX** ("SORD Graphics eXtension") card is a **planar**
frame buffer:

| Property | Value |
|---|---|
| Resolution | **640 × 500** |
| Depth | **4 bit-planes → 16 colours** (planar, 1 bit/pixel/plane) |
| Line stride | **80 bytes** per plane line |
| Bytes/plane | 40000 (visible); banked through a 64 KB window |
| Pixel address | plane byte `= Y*80 + X/8`, bit mask `0x80 >> (X & 7)` |
| Plane window | `0xE40000`, 64 KB |
| Read-select | `0xE20381` (write 0..3) — plane read back at `0xE40000` |
| Write-mask | `0xE203A1` (write bits0..3) — planes written at `0xE40000` |
| Palette | `0xE50000` — 16 registers, one byte at each even word offset |
| Board present | `0xE50001` bit7, **active-low** (0 = present) |

Because the four planes share one 64 KB window, the boot ROM's plain
`0xAA55` readback probe of `0xE40000` only ever sees the currently
selected plane; the emulator's reset default (read plane 0, write all
four planes) makes that probe behave like ordinary RAM.

Geometry was decoded from the SGX driver and GEDIT: Y is clamped to
`#499`, the X byte-index to `#80`, consecutive scanline pointers
(`0xE49BF0`, `0xE49C40`) differ by `0x50` = 80, and save/restore loops
copy `10000` longwords = 40000 bytes per plane. This matches the text
raster exactly (80×8 = 640, 25×20 = 500), which is why **text composes
on top of graphics**: kanji and the G-Editor menu overlay the bitmap.

**Compositing (emulator).** Each frame the renderer scans all four
planes; if any pixel is set it treats graphics as active and, for every
text cell, draws the glyph foreground opaque over a per-pixel background
taken from the 4-plane value through the 16-entry palette LUT (so
graphics shows through the cell background). If no graphics pixels are
set, the background is black (pure text console). The palette byte is
mapped to RGB through a standard 16-colour **IRGB** table; the exact
SORD DAC encoding is unconfirmed (see §10). A `SORD_GFX_TEST=1`
environment switch injects a 16-colour bar + diagonal into the planes to
validate geometry and colour independently of the app suite.

### 5.3 The SGX software API (`trap #1`)

All SGX drawing goes through a **`trap #1`** graphics API:

* `trap #1` is 68000 exception vector **0x84** (`0xE00A0C` in ROM), which
  out of the box is a bare `rte` stub — so graphics calls no-op until a
  driver is installed.
* The resident driver **`SGX.SYS`** installs the live handler. The
  loader program **`SGX`** (`SGX.68K`, a 1 KB `0x601A` executable):
  1. gates on `btst #7,0xE50001` (board present, active-low); absent ⇒
     prints the JIS "no graphics board" message and exits;
  2. calls the **SORD RSX / device manager** via `trap #2` (BDOS-style
     functions `0x100` and `0x3F`), builds a CP/M **FCB**
     `[drive]"SGX     SYS"` and does a **BDOS OPEN (func 15)**. The drive
     byte comes from **base-page offset `0x24`** = the drive `SGX.68K`
     itself was loaded from (verified constant `A: = 1`, *not* the
     current default drive);
  3. on success loads `SGX.SYS` as a resident extension; its init runs
     the `btst`/device-register/`movel #handler,0x84` sequence and
     **rewrites vector 0x84** to the RAM handler (observed `0x000F1AA0`).
     On failure it prints the status and leaves vector 0x84 as the ROM
     stub.
* Applications such as **GEDIT** then call `trap #1` for everything —
  the menu highlight uses SGX call `0x0102`, and every
  Box/Line/Circle/Fill/Paint is a `trap #1` (≈40 call sites in GEDIT).

**Emulation caveat.** The loader hard-codes the **A:** drive, but the
surviving distribution ships `SGX.SYS` only on **B:**, so out of the box
the OPEN fails and the API never installs. The working state was reached
entirely with the guest's own tools: clear the read-only attribute on
A:, erase a few unused utilities to free space, and `PIP A:=B:SGX.SYS`
so the file is reachable at `A:SGX.SYS`; then `SGX` installs the handler.
A prepared A: image with this done is saved as `/tmp/sord-run/d1_sgx.img`.
The remaining faithful alternative — a cold boot that runs the real
`STARTUP.BAT` with the system drive assigned so the OPEN resolves —
depends on the not-yet-located `.BAT` autostart mechanism (see §10).

---

## 6. Input subsystem

The M68MX has two intelligent serial input devices, each on its own
MC6850 ACIA. Both are **off by default** in the emulator, so the default
machine presents a plain ASCII serial console; they are enabled with
machine options `smartkbd=on` and `locator=on`.

### 6.1 Smart keyboard — ACIA0 (`0xE30161`), vector 0x42

* **Handshake.** At init the BIOS master-resets ACIA0, drains RX,
  transmits **`0x00`** (identify) and waits for a **2-byte ID** reply
  within a timeout. Reply present ⇒ **scancode mode** (`0xD854 = 0`); the
  BIOS then sends init commands `0x80..0x89` and `0xC0`, and installs the
  vector-0x42 RX ISR at `0x0108` with ACIA control `0x95` (receive
  interrupt enabled). Reply absent (timeout) ⇒ BIOS sends `0xFF` and sets
  `0xD854 = 0xFF` = the **plain-ASCII terminal path** (what the default
  machine uses; a serial terminal on ACIA0 then acts as the keyboard,
  with `0xF1..0xF5` prefix codes for specials). `^G` sends a beep
  sequence (`0xF6, 0x2n, 0x2n`) back to the keyboard.
* **Scancode wire format.** One byte per event: **bit7 = make (key
  down)**, bit7 clear = break (key up); bits6–0 = key number. The ROM
  keysym table at **`0xE0E2`** maps `scancode & 0x7F` to a 16-bit keysym
  `class<<8 | index`:
  * **class 7** = typewriter keys, `index = ASCII` (e.g. `'S'`=0x53 is
    scancode 0x52, CR is 0x4D, space is 0x50);
  * **class 2** = function keys F1..F20 (index 0..0x13);
  * **class 3** = edit / cursor keys (index 0..7);
  * **class 1** = shift/ctrl/lock modifiers; **class 6** = mode keys.

  The smart-mode and ASCII-mode decoders converge on the same internal
  `0xF1..0xF5`-prefix representation and the same key queues, so special
  keys behave identically in both modes. Typewriter keys reach the CP/M
  console (BDOS func 6, drained by the CCP).
* **Model (`smartkbd=on`).** ACIA0 becomes the intelligent keyboard: the
  `0x00` probe is auto-answered with a 2-byte ID (`0xA0 0x01`); other
  keyboard commands (`0x80–0x89`, `0xC0`, beep, LED) are consumed; and
  host bytes on `serial_hd(0)` are treated as **raw scancodes**
  (make = `0x80|code`, break = `code`) pushed into the ACIA RX FIFO.

### 6.2 Graphics locator / "puck" — ACIA1 (`0xE30141`)

This — not the keyboard — drives the GEDIT/LEONIS menu highlight and
drawing cursor. GEDIT polls it continuously.

* **Protocol.** The driver transmits **`0x00`** and reads a **3-byte
  reply** `[status, dx, dy]` (the "3-word answer" the ROM/BIOS aux probe
  expected on ACIA1). `dx`/`dy` are signed pixel deltas, used directly
  when no direction bit is set.
* **Status bits** (decoded from GEDIT's event reader at `0x12CF4`):
  `0x10` right, `0x20` left, `0x40` down, `0x80` up, `0x01`/`0x02` =
  buttons. Buttons map (via table `0x17CD4`) to **SPACE (0x20)** and
  **CR (0x0D)** — i.e. the locator buttons are the menu *select* keys.
  In GEDIT's menu, `cursor_x += dx` per poll and `item = cursor_x/60`
  (10 items) drives the highlight; SPACE/CR selects. In draw mode the
  direction bits move the crosshair by ±128 px (the raw deltas are
  ignored while a direction bit is set), button `0x01` places a point,
  button `0x02` exits the tool.
* **Model (`locator=on`).** ACIA1 answers the `0x00` poll with a latched
  `[status, dx, dy]`; the host **injects one event as a 3-byte packet**
  on `serial_hd(1)`, reported on the next poll and then auto-cleared (one
  event per packet). Injection is scripted over the serial socket (see
  `/tmp/sord-run/kbdlib2.py`: `KB.typ()` types via scancodes,
  `LOC.ev(status,dx,dy)` injects a locator event).

With both enabled, GEDIT is driven end-to-end: scancode typing runs CP/M
(`DIR`, `SGX`, `GEDIT`), the locator drives GEDIT's menus and, with the
API installed, draws its full 16-colour UI plus a user-drawn box onto the
SGX plane (proof screenshot `/tmp/sord-run/GEDIT_box_drawn.png`).

---

## 7. Floppy subsystem

* **Controller:** NEC **µPD765 / Intel i8272** FDC at `0xE20281`
  (Main Status Register) / `0xE20283` (data). Commands complete
  synchronously in the model (no seek/rotation delay); completion raises
  **interrupt vector 0x45**. The ROM's handler reads 7 result bytes when
  MSR bit CB is set, else loops `SENSE INTERRUPT STATUS` until `0x80`.
* **DMA:** the custom 4-channel controller (§3); the FDC uses **channel
  1**. Address is `page<<16 | addr`; transfers do not carry out of the
  64 KB page (the ROM splits transfers accordingly). Direction and count
  come from the DMA count register's top two bits.
* **Implemented commands:** SPECIFY, RECALIBRATE, SEEK, SENSE INTERRUPT
  STATUS, SENSE DRIVE STATUS, READ ID, READ DATA, WRITE DATA. `SENSE
  DRIVE STATUS` reports READY (ST3 bit5) when media is inserted and
  track-0 (bit4) when the head is at cylinder 0.
* **Media / geometry — mixed-density 8" IBM-3740 format:**
  **77 cylinders, 2 heads, 26 sectors/track.** Track (0,0) is
  **FM / single-density, 128 B/sector** (3328 B); every other track is
  **MFM / double-density, 256 B/sector** (6656 B). READ/READ-ID checks
  the requested recording mode against the track's real mode and returns
  a missing-address-mark error on mismatch (this is how the ROM's density
  detection works).
* **On-disk image layout.** The raw sector image is the SD track first,
  then tracks in `(cyl*2 + head)` order:
  `offset = 3328 + (cyl*2 + head − 1)*6656 + (sec − 1)*256`
  (single-density track uses `(sec − 1)*128`). Total image size
  1 021 696 bytes. The distribution disks are **IMD** (`M68MX_1.imd` /
  `M68MX_2.imd`); the emulator consumes the equivalent raw sector layout.
* **Writable media.** `sord_fdc_realize` requests `BLK_PERM_WRITE` on
  each attached backend (real M68MX floppies are read/write, and guest
  tools like PIP/FORMAT/SYSGEN need it), falling back to a read-only
  claim if the image is `readonly=on`. The WRITE-DATA path handles
  directory and data blocks including the high-track sector skew.

---

## 8. Software

### 8.1 CP/M-68K disk inventory

**Drive A (`M68MX_1`)** — 48 files, user 0:

* *System:* `CCP.SYS`, `CPM.SYS`, `IOKERNEL.SYS`, `SETUP.SYS`,
  `KEYTBL.SYS`, `FUNCTTBL.SY`.
* *Utilities:* `STAT`, `PIP`, `FIND`, `MEM`, `SIZE68`, `TOD`, `FUNC`,
  `INIT`, `DCACHE`, `ZSPACE`, `XREF`, `CHDR`, `CREATE`, `RTSIM`,
  `SYSSET`, `SYSGEN`, `FORMAT`, `COPYDISK`, `COMPDISK`, `FDDUMP`,
  `FDLIST`, `FDLOAD`, `LIST`, `POFF`, `PBUF`, `XFER`, `DYTM`, `STSCON`,
  `MENUMAN`, `SETUPIO`.
* *Communications:* `SJRX`, `SJSETUP`, `HCOPY`, `CFILE`, `CPT`.
* *Kanji (JIS) tools:* `KCP68`, `KEICNV`, `KSE`, `KPRINT`, `KPATN`,
  `EDIC`.
* *Graphics:* **`SGX`** (graphics-board driver loader), **`GEDIT`**
  (G-Editor), **`STDRCS`**, **`HCOPY`** (screen hardcopy).

**Drive B (`M68MX_2`)** — data/config:

* **`SGX.SYS`** (the resident graphics driver, loaded by `SGX`);
  `GR.FLG` / `SV.FLG` / `KS.FLG` (version-stamp flags).
* **`LEONIS.MDT`** + `MENUMAN` → SORD's integrated software (launched via
  `MENUMAN LEONIS.MDT`); `A_ALL2` / `A_END`; `JPLSMAIN.MDT`.
* Fonts `UKNJWK2.FNT`, `VWF.FNT`; dictionaries `KIHON2T`, `TANGOS`,
  `TANGOU.DIC`; `CEDRIC` / `KCEDRIC.HLP`; `FDUMP4`.

Extraction note: the on-disk image is block-linear at
`0x5B00 + N*4096` for low blocks only; higher blocks are sector-skewed,
so small (single-block ≤4 KB) files can be carved by their `0x601A`
header while multi-block files need the BIOS skew table.

### 8.2 Startup flow

```
B:STARTUP.BAT  = SETUPIO ! REN AUTOEXEC.BAT=AUTOEXEC.SUB ! SGX ! A_ALL2 !
                 SJRX ! HCOPY ! CFILE ! CPT C ! CPT D ! A: ! A_END
B:AUTOEXEC.BAK = MENUMAN LEONIS.MDT
```

So at every boot `SETUPIO` configures I/O, `SGX` makes the graphics
driver resident, the communications stack is loaded, and finally
`MENUMAN LEONIS.MDT` launches LEONIS. This is why `SGX` — hence the
`trap #1` graphics API — is expected to be installed on every real boot.

---

## 9. Running it in QEMU

Machine type: `-M sord-m68mx`. Machine options:

| Option | Default | Meaning |
|---|---|---|
| `strap=<byte>` | `0x82` | Configuration strap (see §4): `0x82` floppy, `0xE2` Sunbug monitor, `0xC2` S-record loader. |
| `smartkbd=on\|off` | `off` | Model the SORD intelligent keyboard on ACIA0 (scancode protocol). `serial_hd(0)` then carries raw scancodes. |
| `locator=on\|off` | `off` | Model the graphics locator on ACIA1; a 3-byte packet on `serial_hd(1)` is one event. |

The boot ROM is `m68mx-bootrom.bin` (16 KB); pass it with `-bios`. Disks
are attached as floppies (`if=floppy,index=0` = drive A, `index=1` = B).

**Text console (default, interactive `A>`):**

```
qemu-system-m68k -M sord-m68mx -bios m68mx-bootrom.bin \
  -drive if=floppy,index=0,format=raw,file=M68MX_1.img \
  -drive if=floppy,index=1,format=raw,file=M68MX_2.img \
  -serial unix:/tmp/kbd.sock,server=on,wait=off
```

CP/M reaches the `A>` prompt in ~100 ms; `DIR`, `STAT`, `TYPE`, etc.
typed over the keyboard ACIA all work.

**Sunbug monitor on serial:**

```
qemu-system-m68k -M sord-m68mx,strap=0xe2 -bios m68mx-bootrom.bin -serial stdio
```

**Graphics (SGX + GEDIT), with the smart keyboard and locator:**

```
qemu-system-m68k -M sord-m68mx,smartkbd=on,locator=on -bios m68mx-bootrom.bin \
  -drive if=floppy,index=0,format=raw,file=d1_sgx.img \
  -drive if=floppy,index=1,format=raw,file=M68MX_2.img \
  -serial unix:/tmp/kbd.sock,server=on,wait=off \
  -serial unix:/tmp/loc.sock,server=on,wait=off
```

Here `serial_hd(0)` (`/tmp/kbd.sock`) carries scancodes and
`serial_hd(1)` (`/tmp/loc.sock`) carries locator packets; drive A must be
an image where **`SGX.SYS` is reachable as `A:SGX.SYS`** (e.g. the
prepared `d1_sgx.img`, or add it in-guest with `PIP A:=B:SGX.SYS`). Type
`SGX` then `GEDIT` over the scancode line and drive the menus with the
locator.

Debug switches (environment variables): `SORD_TRACE=1` logs
CRTC/palette/plane-select/unknown-I-O writes (`SORD_TRACE_V=1` adds
per-pixel graphics writes); `SORD_GFX_TEST=1` injects a 16-colour test
pattern into the planes; `SORD_FDC_TRACE=1` / `SORD_KBD_TRACE=1` trace
the FDC commands and keyboard TX.

---

## 10. Emulation status and known gaps

**Faithful / working:**

* CPU reset from ROM vectors, DRAM sizing and device probing via genuine
  68000 group-0 bus errors.
* Boot ROM POST, strap-driven boot device selection, Sunbug monitor and
  S-record loader paths.
* Floppy boot of CP/M-68K to an interactive `A>`; the µPD765 command set
  used by the ROM and BIOS, the custom DMA, the mixed-density 8" format,
  and (newly) writable media.
* The HD6845 text console driven by the CP/M "SORD CRT" BIOS driver, and
  the SGX 4-plane 640×500 graphics with text-over-graphics compositing.
* Smart keyboard (scancode protocol) and graphics locator, enough to run
  GEDIT and draw through the live `trap #1` SGX API.

**Open items / deliberate simplifications:**

* **`SGX.SYS` load drive.** The `SGX` loader hard-codes drive **A:**
  (base-page[0x24] = 1), but the surviving dumps ship `SGX.SYS` only on
  **B:**, so the faithful "driver resident" state requires co-locating
  `SGX.SYS` on A: (done in-guest with PIP). The clean alternative —
  a cold boot that runs the real `STARTUP.BAT` with the system drive
  assigned — needs the SORD **`.BAT` autostart mechanism** (a
  MENUMAN/CCP autostart), which has not been located.
* **Kanji-console hook.** The resident `SGX.SYS` kanji-console hook did
  not activate in our runs; kanji output still lands as raw JIS bytes in
  the text plane rather than being rendered through the graphics plane.
  The driver installs via a SORD `trap #3` BIOS extension plus an
  interrupt-controller hook that the stub intc / `trap` handling does not
  fully honour.
* **`0xE20381` / `0xE203A1` shared decode.** Treated purely as graphics
  plane control; the interrupt controller in this model delivers vectors
  directly and does not use them. Verify against real hardware if intc
  masking is ever required.
* **Palette → RGB.** The palette byte is mapped through a standard
  16-colour IRGB LUT; the exact SORD DAC / colour encoding is
  unconfirmed (the software only ever loads an identity ramp 0..15).
* **CRTC / font details.** The text cell height is 20 (CRTC R9=0x13); the
  emulator renders an 8×16 VGA font inside it, leaving a 4 px gap — the
  real SORD glyph font is unknown (cosmetic).
* **FDC timing.** No seek/rotation delays; commands complete within the
  MMIO access and the interrupt is raised immediately. WRITE DATA works
  but relies on the guest's own skew tables for multi-block files.
* **RTC** reads as zero (not battery-backed in the model); the
  movep-programmed device at `0xE30081`, several aux latch ports, and the
  interrupt-controller config registers are write-only stubs. The HD
  controller (`0xE80005`) and Z8530 SCC (`0xE80105`) are intentionally
  left unmapped so the ROM's guarded probes mark them absent.

---

## Appendix — cross-source reconciliation notes

Inconsistencies found between the notes and the code while writing this
document (all resolved in favour of the code, which is authoritative):

1. **`0xE20381`/`0xE203A1` labelling.** The notes' early memory-map table
   calls `0xE20381/3A1/3C1/3E1` the "interrupt controller"; the later
   graphics section and the code establish `0xE20381` = SGX plane
   read-select and `0xE203A1` = SGX plane write-mask, with only
   `0xE203C1`/`0xE203E1` being the interrupt controller. This document
   uses the graphics meaning and flags the shared decode (§3, §10).
2. **Display-ID default.** The notes describe *all-zero* ID bytes ⇒
   kanji message mode. The emulator's `sord_machine_reset` writes `0xFF`
   to `0xE12001`, so the modelled machine **defaults to the ASCII message
   set**. Behaviourally consistent with the ROM, but the default is worth
   knowing (§3).
3. **VRAM region size.** The notes list the text VRAM as
   `0xE10000–0xE11FFF` (8 KB) and the display-ID bytes at `0xE12001` as a
   separate row; in the code both live in one 16 KB region
   (`SORD_VRAM_SIZE = 0x4000`) — text in the low 8 KB, ID bytes above.
   Same hardware, presented as one region.

These are documentation/labelling drifts, not behavioural disagreements;
no code change is implied.
