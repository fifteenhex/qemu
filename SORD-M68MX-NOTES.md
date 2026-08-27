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
