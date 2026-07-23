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

    0x00000000  onboard DRAM (tested with walking patterns + LFSR)
    0x0fc00000  second RAM region, 2 x 1MB cleared (0x0fc00000 and
                0x0fe00000) — possibly video/shared RAM, unconfirmed
    0xfe800000  1MB EPROM (this ROM; chip-select window mask 0xfff00000)
    0xfea00000  battery-backed SRAM ("SRAM/RTC" warnings in strings;
                CS window mask 0xfff00000; initial SP is fea01000)
    0xfec00000  onboard I/O / system ASIC region (details below)

DTT0=0xfe01a040 DTT1=0xfe018040: both transparent-translate
0xfe000000-0xffffffff as uncached/serialized I/O.

## 0xfec00000 I/O region — devices found so far

    0xfec01000  NVRAM (byte-wide; firmware scratch-tests reg 0xdb,
                masks reg 0xc3 with 0x3f, sets 0x80 in reg 0x2b,
                reads reg 0xaf).  Likely the SRAM/RTC combo chip;
                RTC registers not yet located.
    0xfec080f0  two 32-bit ASIC regs, init 0x000000cc / 0xcccccccc
                (DRAM refresh/timing? unconfirmed)
    0xfec20000  byte device: reg 0 read/complement/restore test, reg 4
                scratch test (sets flags 0x01000000 / 0x00020000 in d7
                on failure)
    0xfec30000  Zilog Z8536 CIO.  Standard hookup: +3 control
                (indexed), +2 port A, +1 port B, +0 port C.
                Init: PA mode=0, PB mode=0, PA DDR(0x23)=0x80,
                PB DDR(0x2b)=0xff, PC DDR(0x06)=0xf0, MCCR(1)=0x94;
                writes PA=0x88, PB=0xff, PC=0.
                Port C data (+0) = POST code display (LEDs);
                Port B (+1) read at boot = config/DIP switches.
    0xfec40000  probed device: write 0 to +0, then +2 must read 0x3a
                (firmware revision).  Then a FIFO loopback test: six
                bytes 0f/33/55/aa/cc/f0 written to +1, then +0=0, and
                the same six values must read back from +1 in order.
                Candidates: the "SCSI/Keyboard Interface" helper µC or
                a serial controller — unresolved.  Absence flag
                0x00010000 in d7.
    0xfec58000  secondary/slave CPU control?  Writes 0, then 0x20
                after planting a trampoline: vector at DRAM 0/4 points
                to code that writes 0xfeed to 0x1004 and STOPs; d7
                flag 0x00040000 set if 0xfeed appeared (i.e. someone
                else executed from DRAM 0) — reads as a second-CPU
                presence test.  "Secondary CPU :" string supports it.
    0xfec5c000  byte reg, 0 written during init (unknown)
    0xfec5e000  CPU-type register: firmware writes 1 for 68040, 4 for
                68060 (probed via movec PCR trap)
    0xfec64000  next device probed after fec58000 — analysis stopped
                here, continue at rmon.asm 0xfe800c04
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

1. Finish walking the RMON init path (currently at 0xfe800c04, device
   0xfec64000); map remaining devices (serial, LANCE, SCSI, VME
   bridge, RTC location, floppy).
2. Identify the serial chip (4 ports; rev-code 0x3a probe), the SCSI
   controller ("### No SCSI controller installed" implies a probe),
   and the ethernet (ILACC Am79C900 vs LANCE Am7990 — strings suggest
   both/TLANCE).
3. Create hw/m68k/e17.c machine `e17` (68040, -bios rmon.bin at
   0xfe800000 + mirror at 0), plus an e17-asic device modelling
   0xfec00000 with the chip-select unit, Z8536, NVRAM, POST-code
   port, CPU-type reg, etc.  Document every register in comments even
   when the model is a stub — that is the point of the project.
4. Goal: RMON banner on serial console, then menus, then LynxOS boot.

## Current state (2026-07-20)

RE only so far; no QEMU code yet.  ROM downloaded and disassembled,
reset path analysed up to 0xfe800c04 (POST code 0x29).  Memory map
and device inventory above.  No open questions for Daniel yet.
