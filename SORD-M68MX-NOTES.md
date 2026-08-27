# SORD M68MX QEMU machine — journal

Goal: run the M68MX boot ROM (`/workspace/files/m68mx-bootrom.bin`, 16KB,
"M68MX BOOT ROM REVISION 01E"), then boot CP/M-68K from the OldComputers.es
floppy images (`/workspace/files/M68MX_1.img` / `M68MX_2.img`).

Build: `mkdir -p /tmp/sord-build && cd /tmp/sord-build &&
/workspace/src/qemu-sord/configure --target-list=m68k-softmmu
--disable-docs --disable-tools && ninja qemu-system-m68k`
(NEVER build on /workspace — only ~770MB free).

Disassembly: `m68k-linux-gnu-objdump -b binary -m m68k:68000 -D
m68mx-bootrom.bin --adjust-vma=0xE00000` (kept at /tmp/sord-rom.dis).

## Reverse-engineered memory map

| Range | What | Notes |
|---|---|---|
| 0x000000-... | DRAM | sized by writing 0 upward from 0x400 until **bus error**; ROM needs ≥64K (initial SP=0x10000). SR/vector RAM at 0, ROM stores its service-table pointer at 0x0 |
| 0x020000 | floppy boot load address | ROM checks "M68MX-ID" at 0x20004 then `jmp 0x20000` |
| 0xDFFFF0 | expansion ROM probe | guarded read; 8-byte signature "M68MX-ID"; unmapped ⇒ absent |
| 0xE00000-0xE03FFF | boot ROM 16KB | reset SP/PC in first 8 bytes; word-checksum of 0..0x1ECE must be 0 |
| 0xE10000-0xE11FFF | text VRAM | 1 long per cell: attr word (0x0007/0x000F) + char word; 80x25; cleared to 0x0007'0020 |
| 0xE12001,3,...,1F | display-type ID bytes | 16 odd bytes; **all zero ⇒ Japanese/kanji message mode (bit9)**, any nonzero ⇒ ASCII messages |
| 0xE20201+2*ch | DMA ch address reg (2 byte writes: lo,mid) | custom DMA, 4 channels; FDC=ch1, HD=ch3 |
| 0xE20209 | DMA control: bit0 go/master, bit(1..4)=ch enable (1<<ch), bit5 dir | |
| 0xE20211+2*ch | DMA ch count reg (2 writes lo,hi); count=(len-1)&0x3FFF; bits15-14 of the 16-bit value = direction (0x4000 dev→mem, 0x8000 mem→dev) | |
| 0xE20281 / 0xE20283 | **NEC µPD765/i8272 FDC** MSR / data | SPECIFY 03 DF 14; RECAL/SEEK/SENSE INT/SENSE DRIVE/READ ID/READ/WRITE DATA; completion via interrupt **vector 0x45** (handler e00dd8: MSR CB set ⇒ read 7 result bytes; CB clear ⇒ SENSE INT loop until 0x80) |
| 0xE20301 | **addressable latch** (LS259-style): written byte = bit-address in bits2-0, data in bit3; readback returns the 8 latched bits | bit1=speed/density select (written as 01/09), bit2=motor/ready (0x0A=on), bit5=write precomp (05/0D per-op, 0D when cyl≥0x3B), bit3, bit7=RTC hold (0F/07). Boot init seq: 01 0A 03 0C 05 06 07 |
| 0xE20305/307/30F | aux ports (cleared) | stub |
| 0xE2030B / 0xE2030D | RTC address / data (nibble regs 0-12, MSM5832-style) | via latch bit7 hold |
| 0xE20381/3A1/3C1/3E1 | interrupt controller (E203E1=0x40 = vector base 0x40; sources: 0x45 FDC, 0x46 HD) | stub, vectors delivered directly |
| 0xE20781 | **strap register** (read) | boot device = (~strap)>>6 & 3: 1=Floppy 2=HD 3=First Active; 0 ⇒ raw bit5: set=Sunbug monitor, clear=serial S-rec loader on ACIA0. Low nibble: bit1 must be set, bit2 clear, ≠0xF (else "Strap ID Error" path via svc e0148e). **Default modeled value 0x82 = boot floppy** |
| 0xE207A0 | word 0xA1A1 written at boot (strap latch enable?) | stub |
| 0xE207C1+off | DMA page regs (address bits 23-16); ch1@+0, ch3@+2 | |
| 0xE30001 / 0xE30003 | **HD6845 CRTC** addr/data | 80x25 (R1=0x50, R6=0x19); cursor R14/15 read+written by char output |
| 0xE30081-8F | unknown (movep-programmed timer/counter: 0x82, 0x4EF1, 7, 0xEE) | stub |
| 0xE30141 / 43 | MC6850 ACIA #1 (aux/keyboard?) | init 03 then 15 |
| 0xE30161 / 63 | MC6850 ACIA #0 (Sunbug console / serial loader) | init 03 then 15 |
| 0xE40000-0xE4FFFF | optional 64K graphics RAM | probed with **unguarded** 0xAA55 readback ⇒ must be mapped RAM |
| 0xE50000-1F | 16 word writes 0..15 (palette?) | stub |
| 0xE80005 | HD controller probe (guarded) | unmapped ⇒ "Hard Disk is not connected" |
| 0xE80101/105 | Z8530 SCC (guarded probe; kbd/serial?) | unmapped ⇒ absent |

d7 test-flag bits set during POST: 0=RAM ok, 1=HD ctrl present, 2=?,
5=E40000 RAM present, 6=SCC present, 8=expansion ROM at 0xDFFFF0,
9=Japanese display mode.

Messages are drawn into VRAM (not serial): format byte x, byte y, word len,
text; msg0 "Boot Device : Floppy Disk (Set Diskette)" etc.  The "(C)
COPYRIGHT 1985 SORD..." banner string at 0xE0000C is *never referenced* by
the boot flow (no pointer to it anywhere in the ROM) — the visible output is
the Boot Device line and then whatever the floppy's loader prints.
Sunbug monitor (strap bits7:6=11, bit5=1) talks on ACIA0.

## 68000 bus-error frame (target/m68k)

The ROM sizes RAM and probes devices with a vector-8 handler that does
`tas 0x0; addal #8,sp; rte` — i.e. it expects the real 68000 group-0
14-byte frame ([FC.w][access addr.l][IR.w][SR.w][PC.l]) and expects the
pushed PC to have advanced past the faulting instruction (each probe is
followed by two NOPs to absorb the prefetch skid).  QEMU's m68k core only
had 030-format-A/040-format-7 frames.  Added for CPUs without
M68K_FEATURE_EXCEPTION_FORMAT_VEC: push the group-0 frame with PC = the
faulting instruction, and set `bus_error_suppress` + `bus_error_pc` so
that when the handler RTEs back to the same instruction the re-run bus
cycle completes as an unassigned access (read 0 / write ignored) instead
of re-faulting — execution then continues at the next instruction, which
lands inside the ROM's NOP skid pad.  (Equivalent end state to real HW.)

## Boot flow (ROM)

1. SR/register test, ROM checksum, DMA+CRTC+latch init, VRAM clear.
2. Guarded probes (HD, SCC, expansion ROM), E40000 RAM test, memory sizing.
3. Clear screen, read strap 0xE20781 → device (floppy).
4. Print "Boot Device : Floppy Disk (Set Diskette)" at (15,20).
5. SENSE DRIVE STATUS on units 0-3 until one is READY (ST3 bit5).
6. Density detect (e00ADC): RECAL x2, SEEK cyl2, READ ID with mode 0x48
   (FM), 0x40 (MFM), 0x58, 0x50 until success; mode |= N from the ID field;
   for our DD disks ⇒ mode 0x41 (MFM, N=1, 256B).
7. mode 0x41/0x42 ⇒ boot CHS=(1,0,1) else (0,0,1); sectors=min(0x1000>>(N+7),26)
   ⇒ 16 sectors x 256B (DD) read via DMA ch1 to 0x20000.
8. "M68MX-ID" at 0x20004 (matches image offset 0x2704 = cyl1 head0 sec1 +4)
   ⇒ `jmp 0x20000` with d1=?, d2=ST3, d3=memtop, d4=test flags, d5=result.

## Floppy geometry (CP/M-68K disks)

77 cyl, 2 heads, 26 sec/track.  Track (0,0) is FM/single-density 128B/sec
(3328 bytes); all other tracks MFM/double-density 256B/sec (6656 bytes).
Raw .img layout = (0,0) SD track first, then tracks in (c*2+h) order:
offset = 3328 + (c*2+h-1)*6656 + (r-1)*256.  Total 1021696 bytes ✓.

## CP/M-68K BIOS findings (from the booted system, RE of RAM dump)

- The BIOS carries its own "SORD CRT 00A05" console driver writing the
  VRAM directly (escape-sequence aware); console output is the video
  display, not serial.
- **Keyboard = ACIA0 (0xE30161)**, interrupt driven on **vector 0x42**
  (handler installed at address 0x108, ACIA control set to 0x95 = RIE).
  At init the BIOS probes the keyboard: drains RX, sends 0x00, expects a
  2-byte ID; on timeout it sends 0xFF and marks "no smart keyboard"
  (flag 0xD854=0xFF) — in that mode incoming bytes are treated as plain
  ASCII (0xF1-0xF5 prefix codes for specials) instead of scancodes, so a
  serial terminal on ACIA0 works as the keyboard.  ^G sends a beep
  command sequence (0xF6, 0x2n, 0x2n) back to the keyboard.
- ACIA1 (0xE30141) gets a similar 0x00-probe expecting a 3-word answer
  (mouse/tablet?); absent is fine.
- Interrupt controller vectors confirmed: 0x42 keyboard, 0x45 FDC (0x46
  HD per ROM table).  Modelled as a stub that ORs sources and delivers
  the lowest pending vector at IPL 2.

## Result / status

Machine `sord-m68mx` (hw/m68k/sord_m68mx.c).  **CP/M-68K boots from
the M68MX_1.img floppy to the `A>` prompt and is fully interactive**
(DIR of both drives, STAT, etc. typed over the keyboard ACIA):

    qemu-system-m68k -M sord-m68mx -bios m68mx-bootrom.bin \
      -drive if=floppy,index=0,format=raw,file=M68MX_1.img \
      -drive if=floppy,index=1,format=raw,file=M68MX_2.img \
      -serial unix:/tmp/kbd.sock,server=on,wait=off

Boot sequence observed: ROM prints "Boot Device : Floppy Disk (Set
Diskette)" at row 20 (~40ms), density-detects the disk (FM READ ID
fails, MFM returns N=1), loads 16x256B from cyl1/head0 to 0x20000,
verifies "M68MX-ID", jumps; CP/M is at the A> prompt in ~100ms.

`-M sord-m68mx,strap=0xe2` instead selects the **Sunbug monitor on
serial** ("M68MX Sunbug Ver.01A / (C) Copyright SORD ,1985"); strap
0xC2 selects the ACIA S-record loader ('*' prompt).

The "(C) COPYRIGHT 1985, SORD..." ROM banner string is dead data — no
code path references it (verified by scanning the whole ROM for any
absolute/pc-relative reference); the machine's visible boot banner is
the Boot Device line + the CP/M prompt.

Evidence (in /tmp/sord-run/): EVIDENCE.txt (text screens),
cpm-session.png / cpm-dir.png / *.ppm (80x25 screendumps of DIR+STAT
session), sunbug-serial.log (serial banner capture).

Known simplifications: FDC has no seek/rotation delays (commands
complete within the MMIO write, interrupt raised immediately); WRITE
DATA implemented but untested; RTC reads as zero; the E30081 movep
device, palette at E50000 and interrupt controller registers are
write-only stubs; HD controller (0xE80005) and SCC (0xE80105) are left
unmapped on purpose so the ROM's guarded probes mark them absent.

## Graphics board (SGX bitmap card) — RE'd 2026-08

The M68MX's optional bitmap board is the **SGX** card ("SORD Graphics
eXtension").  Driven by real CP/M-68K software from the disks; every
finding below is from disassembling the on-disk programs + observing the
hardware they touch.

### CP/M software inventory (both drives)

Drive A: (M68MX_1) — 48 user files, all user 0:
  System: CCP.SYS CPM.SYS IOKERNEL.SYS SETUP.SYS KEYTBL.SYS FUNCTTBL.SY
  Utils : STAT PIP FIND MEM SIZE68 TOD FUNC INIT DCACHE ZSPACE XREF CHDR
          CREATE RTSIM SYSSET SYSGEN FORMAT COPYDISK COMPDISK FDDUMP
          FDLIST FDLOAD LIST POFF PBUF XFER DYTM STSCON MENUMAN SETUPIO
  Comms : SJRX SJSETUP HCOPY CFILE CPT
  Kanji : KCP68 KEICNV KSE KPRINT KPATN EDIC  (JIS/kanji tools)
  **Graphics: SGX (graphics-board driver loader), GEDIT ("G-Editor"
  graph editor), STDRCS, HCOPY (screen hardcopy)**
Drive B: (M68MX_2) — user data/config:
  SGX.SYS (the resident graphics driver, loaded by SGX), GR.FLG/SV.FLG/
  KS.FLG (version-stamp flags), LEONIS.MDT + MENUMAN → SORD's integrated
  software (launched via AUTOEXEC: `MENUMAN LEONIS.MDT`), A_ALL2/A_END,
  fonts UKNJWK2.FNT / VWF.FNT, dictionaries KIHON2T/TANGOS/TANGOU.DIC,
  CEDRIC/KCEDRIC.HLP, FDUMP4, JPLSMAIN.MDT.
  STARTUP.BAT = `SETUPIO ! REN AUTOEXEC.BAT=AUTOEXEC.SUB ! SGX ! A_ALL2
  ! SJRX ! HCOPY ! CFILE ! CPT C ! CPT D ! A: ! A_END` — i.e. **SGX is
  installed at every boot**.
  (On-disk image is logically block-linear at data-block N -> file offset
  0x5b00 + N*4096 for LOW blocks only; higher blocks are sector-skewed,
  so extract small (<=4KB, single-block) files by carving their 0x601A
  header; multi-block files need the BIOS skew table.)

### How the graphics is driven

`SGX` (1KB, 0x601A exe) parses args (`SGX` load / `SGX OFF` unload /
`-N`), loads the resident driver **SGX.SYS** into low memory (seen at
~0x10200 after load) and stays resident; messages are JIS kanji, e.g.
"SGXをロードしました。" (loaded) / "グラフィック基板がありません。"
(no graphics board).  `GEDIT` is the graph editor: with SGX resident it
comes up with a menu (Edit/Load/Save/Redraw/Display/Trace/Clear/Exit),
opens a drawing sub-menu (Box-open/Box-fill/Circle/Color/Line/Paint/
Pattern) and reads the board-status port.  **Interactive drawing could
NOT be driven from QEMU because the menu/cursor uses SORD smart-keyboard
function/arrow keys, which the ASCII-serial keyboard path does not
emulate** (plain ASCII command lines like DIR/TYPE work fine).

### Graphics hardware (registers + format) — decoded from the driver

  0xE40000  64K window onto the graphics VRAM.  The board is **4 bit-
            planes** banked through this one window (so the ROM's 64K
            probe only ever sees the selected plane).
  0xE20381  **plane READ-select** (write 0..3): selects which plane is
            read back at 0xE40000.  (Was mis-modelled as an interrupt-
            controller stub; the intc actually only uses 0xE203E1/C1.)
  0xE203A1  **plane WRITE-mask** (write bit0..3 = 1/2/4/8): which planes
            receive writes to 0xE40000.
  0xE50000  palette: 16 colour registers, one byte at each even word
            offset (0xE50000,2,4,...,1E).  16 colours => 4 bpp.  Boot ROM
            + driver load an identity ramp 0..15.
  0xE50001  board-status read: **bit7 = board present, active-low**
            (`btst #7,0xE50001`; 0 => present).  Used by both SGX.SYS and
            GEDIT to detect the card.

Geometry (from GEDIT/SGX.SYS: Y clamped to `#499`, X byte-index to `#80`,
consecutive line pointers 0xE49BF0/0xE49C40 differ by 0x50, save/restore
loops copy 10000 longs = 40000 bytes/plane):
  **640 x 500, 1 bit/pixel/plane x 4 planes = 16 colours packed as
  bit-planes; 80-byte line stride; 40000 bytes per plane; line Y at
  plane-offset Y*80, pixel X in byte Y*80 + X/8, bit 0x80>>(X&7).**
  Matches the text raster: 80 cols x 8 = 640, 25 rows x 20-scanline
  cells (CRTC R9=0x13) = 500.  Text composes ON TOP of graphics (kanji
  and the G-Editor menu overlay the bitmap).

### What the emulator now models

`sord_m68mx.c`: 0xE40000 is 4 real bit-planes (`plane[4][64K]`), banked
via `gfx_read_sel` (0xE20381) and `gfx_write_mask` (0xE203A1); default
mask 0xF / read plane 0 keeps the ROM's unbanked 0xAA55 probe working.
0xE50000 palette is stored; 0xE50001 read returns board-present (bit7=0).
The display is now **640x500** and the update routine composites the
4-plane bitmap (16-colour palette LUT) under the text layer (text glyphs
opaque, cell background transparent so graphics shows through).  Verified
with a 16-colour bar + diagonal test pattern injected into the planes
(`SORD_GFX_TEST=1`, screenshot gfx_bars.png): all 16 colours + geometry
correct, "A>" text overlaid.  Debug tracing: `SORD_TRACE=1` logs CRTC/
palette/plane-select/unknown-I-O writes (+ `SORD_TRACE_V=1` per-pixel).

## Smart keyboard + graphics locator — RE'd 2026-08 (input to drive GEDIT)

Two separate input devices had to be modelled to drive the graphics apps;
both were reverse-engineered from the ROM keyboard driver, the booted BIOS
(cpm-low.dis) and GEDIT.68K carved out of a live RAM dump.

### Smart keyboard = ACIA0 (0xE30161), scancode protocol

- **Handshake (BIOS init, cpm-low @0xd858):** master-reset ACIA0, drain RX,
  transmit **0x00** (identify) and wait for a **2-byte ID** reply within a
  timeout.  Reply present ⇒ *scancode mode*, flag **0xD854 = 0**, BIOS then
  sends init commands **0x80..0x89** and **0xC0** and installs the vector-
  0x42 RX ISR at 0x0108 (ACIA0 ctrl = 0x95, RIE).  Reply absent (timeout)
  ⇒ BIOS sends 0xFF and sets **0xD854 = 0xFF** = the plain-ASCII terminal
  path (what the default machine uses).
- **Scancode wire format:** one byte per event, **bit7 = make (key down),
  bit7 clear = break (key up)**; bits6-0 = key number.  The ROM keysym
  table at **0xE0E2** (`*(0xC3EE) + *( +4)`) maps `scancode&0x7f` → a 16-bit
  keysym `class<<8 | index`:
  - **class 7** = typewriter keys, `index = ASCII` (so 'S'=0x53 is sc 0x52,
    CR is sc 0x4d, space is sc 0x50, etc.);
  - **class 2** = function keys F1..F20 (index 0..0x13);
  - **class 3** = edit/cursor keys (index 0..7);
  - **class 1** = shift/ctrl/lock modifiers; **class 6** = mode keys.
  Both the smart-mode decoder (0xdc3a→0xdeb8) and the ASCII-mode decoder
  (0xe2f2) converge on the *same* internal `0xF1..0xF5`-prefix + payload
  representation and the same key queues, so special keys are identical in
  both modes.  Special keys land in the BIOS key ring at ~0xC370; the CP/M
  console (BDOS func 6, drained by CCP) reads the typewriter keys.

### Graphics locator ("puck"/direction pad) = ACIA1 (0xE30141)

This — not the keyboard — is what drives the GEDIT/LEONIS menu highlight
and drawing cursor.  GEDIT polls it continuously (busy-loop at 0xc290):

- driver transmits **0x00**, reads a **3-byte reply** `[status, dx, dy]`
  (the "3-word answer" the ROM/BIOS probe expected on ACIA1);
- **status** bits (decoded by GEDIT's event reader 0x12cf4): `0x10 right,
  0x20 left, 0x40 down, 0x80 up, 0x01/0x02 = buttons`; dx/dy = signed
  deltas used directly when no direction bit is set;
- buttons map through table 0x17CD4 to **SPACE (0x20)** and **CR (0x0D)** —
  i.e. the locator buttons are the menu *select* keys.  GEDIT's menu
  routine (0x11bba): cursor_x += dx each poll, `item = cursor_x/60`
  (10 items) drives the highlight, SPACE/CR selects.

### How they are modelled (`sord_m68mx.c`)

- `-M sord-m68mx,smartkbd=on` — ACIA0 becomes the intelligent keyboard:
  the 0x00 probe is auto-answered with a 2-byte ID (0xA0 0x01), other
  keyboard commands (0x80-0x89/0xC0/beep/LED) are consumed, and **host
  bytes on serial_hd(0) are treated as raw scancodes** (make = 0x80|code,
  break = code) pushed into a new ACIA RX FIFO.
- `-M sord-m68mx,locator=on` — ACIA1 answers the 0x00 poll with a latched
  `[status,dx,dy]`; the host **injects one event as a 3-byte packet on
  serial_hd(1)** (status, dx, dy), reported on the next poll and cleared.
- Injection is the scripted byte-injection path over the two serial
  sockets (see /tmp/sord-run/kbdlib2.py: `KB.typ()` types via scancodes,
  `LOC.ev(status,dx,dy)` injects a locator event).  Both properties
  default **off** so the default machine is the unchanged ASCII console.

### Result: GEDIT is driven end-to-end

With `smartkbd=on,locator=on`: scancode typing runs CP/M (`DIR`, `SGX`,
`GEDIT` all work), and the locator drives GEDIT's menus — selecting item 0
opens the Edit drawing sub-menu (Box-open/Box-fill/Circle/Color/Line/…),
moving right 9 items + SPACE selects Exit and returns to `A>`.  So the
menu/cursor input the apps need is fully solved.

### SOLVED: SGX.SYS loads, trap#1 goes live, GEDIT draws pixels (2026-08)

GEDIT does *all* of its plotting through **`trap #1`** (the SGX graphics
API — ~40 wrapper call sites in GEDIT.68K; the menu highlight → SGX call
**0x0102**, plus every Box/Line/Circle/Fill).  Out of the box **`trap #1`
= vector 0x84 = 0xE00A0C = a bare `rte` ROM stub**, so graphics calls no-op.
The fix is to get the resident driver **SGX.SYS** loaded and initialised;
its init then installs a live handler at vector 0x84 and every GEDIT draw
call reaches it.

**What the boot/startup files do (read in-guest with `TYPE`):**

```
B:STARTUP.BAT  = SETUPIO ! REN AUTOEXEC.BAT=AUTOEXEC.SUB ! SGX ! A_ALL2 !
                 SJRX ! HCOPY ! CFILE ! CPT C ! CPT D ! A: ! A_END
B:AUTOEXEC.BAK = MENUMAN LEONIS.MDT
```

So at every boot `SGX` runs to make the driver resident, then MENUMAN
launches SORD's integrated software (LEONIS).

**How `SGX.68K` actually loads the driver (decoded by live instruction
trace of the loader running at the TPA base 0x10000):**

1. `btst #7,0xE50001` — board-present gate (active-low; **passes** in our
   model).  Absent ⇒ prints the "no graphics board" JIS message and exits.
2. Present ⇒ it calls the **SORD RSX / device manager** (BDOS-style
   `trap #2` funcs **0x100** and **0x3F**, handler at low RAM 0x7de4).
   Sub-function 0 builds a CP/M **FCB** `[drive][SGX     SYS]` and does a
   **BDOS OPEN (func 15)** of it.  The drive byte comes from **base-page
   offset 0x24** = the drive `SGX.68K` itself was loaded from (**A: = 1**,
   *not* the current default drive — verified: constant 1 whether run as
   `SGX` from A> or `A:SGX` from B>).
3. If the open **succeeds**, it loads `SGX.SYS` as a resident extension and
   its init (the `btst #7,0xE50001` / device-register / `movel #handler,0x84`
   sequence decoded earlier) runs → **vector 0x84 is rewritten to the SGX
   trap#1 handler in RAM (observed 0x000F1AA0 = `lea …,a0; bsr; rte`).**
   If the open **fails**, it prints the (JIS) status message and returns
   **without installing** — vector 0x84 stays the ROM stub.

**Refined root cause (supersedes the earlier "already-resident shortcut"
guess):** it is *not* a shortcut against another driver — the loader always
runs the real path, but its `BDOS OPEN` of **A:SGX.SYS fails** because
`SGX.SYS` ships **only on B:** while the loader hardcodes the A: drive
(base-page[0x24]).  The board gate passes, the RSX/`trap #2` service works;
the single missing ingredient is the *file* being reachable at `A:SGX.SYS`.

**The finish (route c from the plan — "place SGX.SYS where the loader looks",
done entirely with the guest's own tools):**

1. Enable floppy writes (real floppies are writable media — modelled now,
   see below).  On the emulator this needed the FDC to request
   `BLK_PERM_WRITE`; the WRITE-DATA path was then exercised for the first
   time and works (directory + data blocks, high-track skew included).
2. In-guest: `STAT A:*.68K $R/W` to clear the distribution disk's read-only
   attribute, `ERA` a handful of unused utilities to free ≥20K, then
   **`PIP A:=B:SGX.SYS`** — the guest BIOS writes the 20K driver onto A:
   with correct sector skew (verified by reading it back and by re-running).
3. `SGX` now opens **A:SGX.SYS**, loads it, and installs vector 0x84.
   Confirmed: `xp/1wx 0x84` = `0x00E00A0C` before, **`0x000F1AA0` after**;
   a gdb breakpoint at 0xF1AA0 fires **~1000×** during a GEDIT session
   (function selectors d0 = 0x0101/0x0102/0x10101 cursor+menu, etc.).

A prepared A: image is saved at `/tmp/sord-run/d1_sgx.img` (pristine A:
with the RO utilities erased + `SGX.SYS` added); boot with it as index 0
and `SGX` installs the API immediately.

**Driving GEDIT to draw (smartkbd + locator):** with the API live,
`-M sord-m68mx,smartkbd=on,locator=on`, type `SGX` then `GEDIT` (scancodes),
locator selects **Edit → Box-open** (button 0x01 = SPACE = menu select),
which enters draw mode (crosshair on the canvas).  In draw mode the locator
**direction bits move the crosshair by ±128 px** (`0x10` R / `0x20` L /
`0x40` D / `0x80` U — the raw dx/dy deltas are ignored while a direction bit
is set), **button 0x01 (SPACE) places a point**, button 0x02 (CR) exits the
tool.  Placing two corners draws the rectangle.  **Result: GEDIT paints its
full 16-colour UI (menu, pattern bar, palette grid, rulers, crosshair) and a
user-drawn box outline onto the 640×500 SGX plane** — real app pixels.
Proof screenshot: `/tmp/sord-run/GEDIT_box_drawn.png`.

### Modelled: writable floppies

`sord_fdc_realize` now negotiates `BLK_PERM_WRITE` on each attached backend
(falling back to read-only if the image is `readonly=on`).  Real M68MX
floppies are read/write, and guest tools (PIP/FORMAT/SYSGEN) need it; the
default boot is unchanged (still lands at `A>` on the plain ASCII console).

### Gaps / things still missing

- **SGX.SYS must be present on A: (the loader's hardcoded drive).**  The
  two OldComputers dumps ship it only on B:, so the faithful working state
  (SGX.SYS resident) requires co-locating it on A: (done here with in-guest
  PIP; a real installed system would have had it on the boot/system drive).
  The alternative faithful route — driving the real `STARTUP.BAT` on a cold
  boot — still needs whatever SORD component auto-runs `.BAT` files (a
  MENUMAN/CCP autostart not yet located); once found it would run `SGX` with
  the system drive assigned so the open resolves.
- The resident **SGX.SYS kanji-console hook did not activate** in our
  runs (kanji output still lands as raw JIS bytes in the text plane, not
  rendered through the graphics plane).  The driver installs via a SORD
  `trap #3` BIOS extension + interrupt-controller hook (0xE20381/A1 dbl
  duty, and it patches vectors) that our stub intc/`trap` handling does
  not fully honour.  Needs the SORD BIOS resident-install semantics.
- 0xE20381/0xE203A1 are shared decode with the interrupt controller
  region; we treat them purely as graphics plane control (the intc in
  this model delivers vectors directly and does not use them) — verify
  against real HW if intc masking is ever needed.
- Palette byte -> RGB mapping uses a standard 16-colour IRGB LUT; the
  exact SORD DAC/colour encoding is unconfirmed (identity ramp is what
  the software loads).
- Text cell height is now 20 (was 16); the 16-pixel VGA font leaves a
  4px gap per row, matching CRTC R9=0x13 — cosmetic, real font unknown.
- No app on these two disks writes the plane on plain invocation (SGX is
  a resident loader; the drawing apps need the keyboard/driver gaps
  above).  DSP*.RGB names seen inside a data file (user-1 catalog) hint
  at stored bitmap images but no such files are in the live directory.
