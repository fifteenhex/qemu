# ELTEC Eurocom E17 emulation — working notes

Reverse-engineering + emulation project: model the ELTEC "Eurocom 17"
VMEbus 68040 board as a new m68k machine named `e17`.  This is an RE
project — we model/document things that are not strictly needed for
emulation, so record everything learned about the hardware here.

Work happens on the `amiga` branch base (it carries the m68k core
fixes), working clone `/workspace/src/qemu-e17`.

## Firmware

RMON monitor ROM, 1MB:

    curl -L -o rmon.bin https://archive.decromancer.ca/bitsavers.org/pdf/eltec/EUROCOM-27/rmon.bin
    md5 863805ee1f03abf0c90979877f3b4263

Identifies as "RMON 3.1.3, 02apr97", "Eurocom 27", "ELTEC Elektronik
GmbH, Mainz", and "ELTEC Eurocom 17 LynxOS Bootstrap" — the E27 and
E17 apparently share this monitor.  Strings mention: VME (A32, "VME
Standard access"), LANCE/ILACC/TLANCE ethernet, SCSI ("SCSI/Keyboard
Interface"), AT-KBD with German/American layouts, 4 serial ports,
NVRAM, RTC, watchdog, floppy (TEAC FC-1 720KB), LynxOS, secondary CPU
("Secondary CPU :"), BAB40/IPIN (other ELTEC products, used as
ethernet-address sources).

## RE working directory

`/workspace/src/qemu-e17-re/` (not in git):

    rmon.bin   — the ROM
    rmon.asm   — full disassembly, regenerate with:
      m68k-linux-gnu-objdump -b binary -m m68k:68040 -D rmon.bin \
        --adjust-vma=0xfe800000 > rmon.asm

## Memory map (as discovered from the reset path, rmon offset 0x5d0)

Reset vector: SP=0xfea01000 PC=0xfe8005d0, i.e. ROM is (also) mapped
at 0 out of reset, runs live at 0xfe800000.

The 1MB ROM image is a 256KB image mirrored 4 times (verified by
cmp of the quarters) — the actual EPROM content is 256KB.

    0x00000000  onboard DRAM (tested with walking patterns + LFSR;
                sized by writing 0x55555555 in 1MB steps and watching
                for wrap, see fe8045ca)
    0x0fc00000  video RAM, 2MB (two 1MB halves cleared at init;
                the colour/Overlay strings + CRTC confirm onboard gfx)
    0xfe800000  1MB EPROM window (256KB x4; CS mask 0xfff00000)
    0xfea00000  battery-backed SRAM ("SRAM/RTC" warnings in strings;
                CS window mask 0xfff00000; initial SP is fea01000)
    0xfec00000  onboard I/O region (details below)

DTT0=0xfe01a040 DTT1=0xfe018040: both transparent-translate
0xfe000000-0xffffffff as uncached/serialized I/O.

## 0xfec00000 I/O map

    0xfec01000  RTC/NVRAM/watchdog, byte-wide on byte lane 3 of
                32-bit words (reg N at N*4+3).  Best guess: Dallas
                DS1386-style "RAMified watchdog timekeeper" ("SRAM/RTC
                battery exhausted" + "Watchdog Timer" strings).
                Init/error paths do: reg 48 (+0xc3) &= 0x3f and
                reg 10 (+0x2b) = 0x80 (DS1386 command reg, TE bit).
                Scratch test on reg 54 (+0xdb); +0xaf, +0x77, +0xbb,
                +0xe7 also accessed.  NVRAM holds the system config
                (mirrored to/from DRAM 0x800, "system area" 0xdff
                bytes).
    0xfec080f0  two 32-bit ASIC regs, init 0x000000cc / 0xcccccccc
                (DRAM refresh/timing? unconfirmed)
    0xfec10000  second Z8536 CIO (control port +3; programmed from a
                reg/data pair table, 16 pairs, 0xff-terminated — see
                fe80453c)
    0xfec20000  byte device: reg 0 read/complement/restore test, reg 4
                scratch test (flags 0x01000000 / 0x00020000 in d7 on
                failure).  Also a structured 2KB region (+0x468,
                +0x5fc, +0x700, +0x7f8) — shared RAM of some kind,
                function unknown.
    0xfec30000  Zilog Z8536 CIO #1.  Standard hookup: +3 control
                (indexed), +2 port A, +1 port B, +0 port C.
                Init: PA mode=0, PB mode=0, PA DDR(0x23)=0x80,
                PB DDR(0x2b)=0xff, PC DDR(0x06)=0xf0, MCCR(1)=0x94;
                writes PA=0x88, PB=0xff, PC=0.
                Port C data (+0) = POST code display (LEDs);
                Port B (+1) read at boot = config/DIP switches;
                PA bit 7 (btst #7 on +2) selects console type.
    0xfec40000  video RAMDAC + pixel clock PLL.  Probe: +0=0 then +2
                must read 0x3a; palette loopback test (addr reg +0=0,
                six bytes 0f/33/55/aa/cc/f0 through data reg +1, read
                back in order) — RAMDAC palette autoincrement.
                PLL programming: index to +0, 16-bit data split over
                +5/+6; indexes 1..10 seen; +2 written 0x63 (VGA-ish
                misc output).  The PLL search synthesizes the pixel
                clock from a 25.175MHz reference, ranges checked
                against 75/37.5/18.75MHz with /1 /2 /4 /8 dividers.
                Chip not identified yet (GENDAC/SDAC-like).
                Absence flag 0x00010000 in d7.
    0xfec48000  video CRTC: regs +0..+5 direct (pixel format, pitch,
                enable), timing register file via index +6 / data +7,
                20 16-bit registers (0..0x13), plus regs 0x16, 0x13
                touched after.  Chip not identified yet.
    0xfec50000  status/ack register, read (only) when a flag says so —
                interrupt acknowledge or abort-switch status.
    0xfec54000  I2C master port.  Used with device address
                0xA0 | (board-select bits) — reads a 24Cxx-style
                serial EEPROM ("IPIN" — source of ethernet address
                and serial number).  Helpers: fe80476e (start),
                fe804860 (send byte), fe8048fe (read/ack).
    0xfec58000  secondary/slave CPU control.  Boot writes 0, then 0x20
                after planting a trampoline at DRAM 0/4; the
                trampoline writes 0xfeed to 0x1004 and STOPs; d7 flag
                0x00040000 records that a second CPU executed it.
                "Secondary CPU :" string supports this.
    0xfec5c000  byte reg, 0 written during init, also written from a
                DRAM global later (fe804474) (unknown, LED/config?)
    0xfec5e000  CPU-type register: firmware writes 1 for 68040, 4 for
                68060 (probed via movec PCR trap)
    0xfec60000  AT keyboard controller: data +0, status +1 (status
                bit 0 / bit 1 = ready bits).  Probe sends 0xFF
                (keyboard reset) and expects 0xAA (BAT OK).
    0xfec64000  Cirrus CD2401 4-channel serial controller (the four
                "Serial Port N" menus).  Identified by: GFRCR (+0x81)
                nonzero probe at boot, CAR channel select written at
                +0xEE, DMA address regs used as word scratch (+0x40).
                Same chip as the MVME167.
    0xfec660fb  config/status byte (bits 0-1 and 2-3 decoded as
                2-bit fields near the CD2401 code — serial-related
                mode/DIP byte?)
    0xfec68000  Am7990 LANCE ethernet: RDP at +2, RAP at +6 (16-bit
                regs on the low half of 32-bit lanes).  Classic CSR0
                STOP/STRT/IDON sequences in the TLANCE driver.
                Station MAC read as low nibbles from a PROM at
                +0x1d01/+0x1d81 area (odd byte lanes).  Strings also
                mention ILACC (Am79C900) — the E27 variant, maybe.
    0xfec6c000  NCR 53C710 SCSI, byte-swapped/big-endian wiring:
                probe writes ISTAT(BE +0x17)=0x40 (SRST) then 0,
                checks SCNTL0(BE +0x03)==0xc0 and DSTAT(BE +0x0f)
                ==0x80 (the 53C710 reset values).  While probing, the
                CS timing reg 0xfec70034 is temporarily set to 0xbe7.
                Absence flag 0x00080000 -> "### No SCSI controller
                installed".
    0xfec70000  chip-select / memory controller.
                reg 0x00 = own base address (reads 0xfec00000)
                regs 0x04..0x38 = timing/control (values 0x0f30,
                  0x0b84, 0x0b83, 0x0b84, 0x1bc3, 0x0b81, 0x0b81,
                  0x0b84, 0x0b82, 0x1b82, 0x0f30, 0x0f30, 0x0830,
                  0x0b80 — from the init table at ROM offset 0x520)
                reg 0x40/0x44/0x48 = CS bank: base 0xfe800000,
                  mask 0xfff00000, ctl 0x0a03  (ROM)
                reg 0x50/0x54/0x58 = CS bank: base 0xfea00000,
                  mask 0xfff00000, ctl 0x0a04  (SRAM)
                reg 0xa4 = 0xfc000000 (written at init)
                reg 0xa8 = status/config: boot waits for
                  (a8 & 0xf00) == 0x200; then rewrites it as
                  ((a8 & 0x18) << 9) | 0x4000

## DRAM globals (VBR=0 after POST)

    0x000-0x7ff  vector table (vectors 2..255 default to fe800e3c)
    0x800        system configuration block (mirrored to NVRAM)
    0x1000       POST device flags (the d7 word; bit16 video absent,
                 bit17 fec20000 reg4, bit18 slave CPU present, bit19
                 SCSI absent, bit24 fec20000 reg0, bit25 warm boot)
    0x1004       slave-CPU handshake word (0xdead/0xfeed)
    0x1008       CPU type as decimal (68040/68060)
    0x8000       initial stack top

## Firmware conventions (useful when reading rmon.asm)

- d6 = current POST checkpoint code, written to the Z8536 port C at
  0xfec30000 before each init stage (1,2,3,4,0x14,5,0x15,0x25,0x35,
  6,7,0x17,8,0x18,0x19,0x29,...).  On a fatal init error the code
  loops back re-running the stage, so a stuck POST code identifies
  the failing stage.
- fp/a6 = bus-error resume pointer: the early vector table (ROM
  offset 0x120, VBR set to 0xfe800120) points fatal vectors at
  0xfe800df8 which resumes at the address in fp.  So `lea pc@(X),%fp`
  before a probe means "on bus error, retry/continue at X".
- d7 = device presence/error flag word accumulated during POST.
- ROM checksum: first 0x200 bytes summed bytewise must match the
  32-bit word at ROM offset 0x1c (0x00013d43).
- The ROM stores the CPU type as *decimal* 68040/68060 (0x109c8 /
  0x109dc) in d1/a5 during init.

## Plan

1. DONE: walk the RMON init path; the I/O map above is the result.
2. Create hw/m68k/e17.c machine `e17` (68040, -bios rmon.bin at
   0xfe800000 + boot mirror at 0), plus device models for the
   0xfec00000 region: chip-select unit, 2x Z8536, POST-code display,
   CPU-type reg, RTC/NVRAM, CD2401 serial (new model needed — QEMU
   has none), LANCE (reuse lance/pcnet core), 53C710 (QEMU only has
   53C895A — start with a stub that passes the probe, decide later),
   AT kbd controller, I2C EEPROM.  Document every register in
   comments even when the model is a stub — that is the point of the
   project.
3. Get the RMON banner out (serial console via CD2401 channel or the
   video console), then menus, then LynxOS boot.
4. Open RE questions: exact video chip (RAMDAC 0x3a rev; CRTC), the
   fec20000 shared-RAM device, fec5c000, fec660fb semantics, VME
   bridge location (probably part of the ASIC/fec70000 space),
   floppy controller location (TEAC FC-1 strings — maybe via the
   SCSI/Keyboard interface µC?).

## Current state (2026-07-20)

RE pass over the boot path complete; I/O map above.  No QEMU code
yet — next step is the machine skeleton (hw/m68k/e17.c).
NOTE: another session is working on this same branch — always
`git pull --rebase` before pushing.
