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

    0xfec01000  VIC068A VMEbus interface controller (also decoded at
                0xfec00000), byte-wide on byte lane 3 of 32-bit words
                (reg N at N*4+3).  The old "DS1386 RTC" guess here was
                wrong — the real NVRAM/RTC is the M48T02 at 0xfec20000.
                Init (fe8041f8): regs 9..15 (+0x27..+0x3f) = 0x80
                (local interrupt control, masked), reg 46 (+0xbb) = 0,
                scratch test on reg 57 (+0xe7, value 0x55 — VIC
                presence/variant check), then a table of (value,
                offset) pairs from the NVRAM config is written out
                with special-casing for offsets 0xc7/0xcf based on
                chip-select reg 0xfec700a8 bits 3-4.  LICR6 monitors
                the CD2401 interrupt line (see the model).
    0xfec080f0  two 32-bit ASIC regs, init 0x000000cc / 0xcccccccc
                (DRAM refresh/timing? unconfirmed)
    0xfec10000  second Z8536 CIO (control port +3; programmed from a
                reg/data pair table, 16 pairs, 0xff-terminated — see
                fe80453c)
    0xfec20000  M48T02 timekeeper: 2KB battery SRAM with the clock in
                the top 8 bytes — identified from the OS-9 bootstrap's
                time-of-day routine, see "The NVRAM/RTC is an M48T02"
                below.  POST does a reg 0 read/complement/restore test
                and a reg 4 scratch test (flags 0x01000000/0x00020000
                in d7 on failure).  Layout: 0x000-0x5fb system config
                ('we'/'re' copy of DRAM 0x800; inverted 32-bit sum
                checksum at 0x5fc-0x5ff), +0x468 board ID block,
                +0x700 OS-9 bootstrap parameter block, +0x7f8 clock
                registers (ctl/sec/min/hr/dow/date/month/year, BCD).
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
    0xfec6c000  NCR 53C720 SCSI (NOT a 53C710: the register map RMON
                uses is the second generation 720/8xx layout — ISTAT
                at 0x14, STEST0-3 at 0x4c, SIDL/SODL/SBDL at
                0x50/0x54/0x58).  Byte lanes reversed within 32-bit
                words: BE offset = LE register ^ 3.  Probe: ISTAT
                (BE +0x17) = 0x40 (SRST) then 0, then SCNTL0
                (BE +0x03) == 0xc0 and DSTAT (BE +0x0f) == 0x80, the
                reset values.  While probing, the CS timing reg
                0xfec70034 is temporarily set to 0xbe7.  Absence flag
                0x00080000 -> "### No SCSI controller installed".
                Driven exclusively in low level (bit-bang) mode, see
                "The SCSI driver protocol" below.
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

## Running it

    qemu-system-m68k -M e17 -bios rmon.bin
    # rmon.bin: 256KB or the 1MB bitsavers dump, see "Firmware" above

The board was offered with a 68060 as well; -cpu m68060 works and
RMON identifies it through its movec-PCR probe ("for the Eurocom 27
- 68060" in the banner).  See "Running as a 68060" below for what
that needed in target/m68k.

With video fitted (the default) RMON adopts the 800x600 framebuffer
as its console and the AT keyboard for input, so the machine is a
fully interactive monitor in the QEMU display window; serial port 1
is still there (-serial ...).  With video=off both console
directions fall back to serial port 1.
-smp 1 removes the second 68040 ("Secondary CPU : Not installed").

## Current state (2026-07-20, evening)

RMON 3.1.3 boots to a fully interactive monitor prompt on serial
port 1 (CD2401 channel 0): banner, help, memory display, setup menus
all work.  POST runs clean through all checkpoints (watch with
-trace e17_post_code).

Code so far:
- hw/m68k/e17.c — machine (68040, memory map per this file)
- hw/misc/e17_sysc.c — everything in the 0xfec00000 window except
  the CD2401: CS controller, 2x Z8536, RTC/NVRAM as plain storage,
  AT kbd self test, 53C710 probe stub, video absent (open bus reads
  make RMON fall back to the serial console)
- hw/char/cd2401.c — async subset: CAR banking, CCR commands
  complete instantly, polled interrupt dispatch via the 0xfec660fb
  acknowledge byte (LIVR | type), TDR/TFTC/RFOC/RDR/EOI in service
  context
- target/m68k/helper.c — movec to PCR/BUSCR now raises the illegal
  instruction exception on non-060 CPUs (RMON's CPU-type probe)

Things learned while bringing it up:
- machine-init qemu_register_reset handlers run BEFORE ROM blobs are
  copied into their regions (rom_check_and_register_reset runs after
  machine init), so the e17 reset hook takes SP/PC from the ROM file,
  not guest memory.
- CD2401 CCR (0x13): firmware polls it until the chip clears it —
  commands must complete.
- RMON programs LIVR = 0x50 | channel << 2; the acknowledge byte at
  0xfec660fb is LIVR with the interrupt type in bits 1:0.

Known issues / next steps, roughly in order:
1. DONE: boot noise + wrong RAM size — root cause was missing DRAM
   mirroring plus a Z8536 reset-protocol bug, see "The boot-time
   hook crash: SOLVED" below.
2. DONE (except battery SRAM): NVRAM/RTC identified as an M48T02 and
   modelled with the in-tree sysbus-m48t02, persistent via
   -drive if=mtd (see "The NVRAM/RTC is an M48T02" below).  Still
   open: persist the 1MB battery SRAM at 0xfea00000 the same way.
3. DONE: SCSI modelled (ncr53c720, see "The SCSI chip is a 53C720"
   below); disk boot now only lacks an OS-9 disk image.
4. LANCE at 0xfec68000: reuse the existing lance/pcnet core with the
   E17's RDP+2/RAP+6 lane arrangement; MAC PROM nibbles at +0x1d81.
5. DONE: video console (e17-vid) and now the AT keyboard too — the
   framebuffer monitor is fully interactive (see "The AT keyboard"
   below).  Still open on video: identify the RAMDAC/CRTC chips.
6. Split the Z8536 out of e17_sysc into a reusable device model.
7. Secondary CPU: RMON probes for one; find out what a dual-68040
   E17 looks like before modelling anything.

NOTE: another session is working on this same branch — always
`git pull --rebase` before pushing.

## Findings from driving the RMON command line (2026-07-20, night)

Method: run with -M e17,video=off, drive the serial console from a
script, and watch the model traces (-trace 'e17_sysc_*' for
everything — beware, the idle loop makes that huge — or the targeted
-trace 'e17_nvram_*').  What the RMON drivers did against the stubs:

- `scsi` (bus scan) exercises the SCSI chip with an ISTAT SRST
  pulse; SCNTL1 = 0x08 pulse (SCSI bus reset), SCID = 7, SCNTL0 =
  0xc4, DCNTL pokes; then per target ID a manual low-level
  selection and ~786000 polls of SBCL waiting for the target to
  respond before timing out.  (These accesses originally suggested
  a 53C710; decoding the full driver later proved the register map
  is the 53C720/8xx one — see "The SCSI chip is a 53C720" below,
  which documents the whole protocol.)
- NVRAM (0xfec20000 block): the "system area" is 0x000-0x5ff,
  written wholesale by `we` (copy of the DRAM config at 0x800) with
  a checksum in the last bytes (0x5fd-0x5ff: fb b3 86 for the
  default config); `re` reads it back and validates.  POST's reg
  0/4 pokes are just the presence scratch test.  No RTC registers
  are touched by RMON at all — the clock layout stays unknown until
  an OS driver or the manual tells us.
- setup menus (full screens captured): Video Interface says the mode
  is "800x600 35kHz 56Hz", sync H/Neg V/Neg, 8bpp, fg black bg
  white.  Boot Parameters: OS "OS-9" (default), device
  Harddisk/Floppy/Streamer Tape/Ethernet, ID 06, LUN 00.  Special:
  Input Port (factory AT-KBD) and Output Port (factory Graphic
  Adapter) are the console selection — with video absent RMON forces
  both to Serial Port 1.  SCSI/Keyboard: typematic rate, language,
  SCSI own ID 7, "SCSI Reset on startup".
- `boot` with device=Harddisk (the default, OS "OS-9"): runs an
  embedded "OS-9/68K System Bootstrap" which endlessly retried
  "boot: Can't initialize the boot device / Boot failed, error
  status $00F6" against the old SCSI stub.  DONE since: the
  ncr53c720 model (see "The SCSI chip is a 53C720" below) takes it
  all the way to reading the disk — only an OS-9 disk image is
  missing now.
- Beware when scripting the setup menus: exiting setup after changes
  raises a strict "Save parameters (y/n)?" prompt that eats any
  other keystrokes — the earlier "netboot hangs silently" was this
  prompt swallowing the "boot" command (found by walking the a5
  frame chain from the getc loop back to the y/n comparison at
  fe80ad42).
- `boot` with device=Ethernet (after answering the prompt) runs
  "Network bootstrap V1.3": prints its configuration (local/server
  internet address from NVRAM, boot file name, "Booting via
  TFTPBOOT") and then programs the LANCE exactly per the datasheet:
  CSR1/CSR2 = init block at phys 0x8000, CSR3 = 0x0002 (ACON), a
  CSR4 write 0x45 (ILACC probe? plain LANCE has CSR0-3 only), then
  CSR0 = INIT and endless CSR0 polling for IDON.  With 0.0.0.0
  addresses it will BOOTP first ("Received BOOTP response" string).

## Netboot status: WORKS end to end (2026-07-20, later)

The LANCE is wired up (see the "wire up the LANCE" commit for the
model details — it is an Am79C900 ILACC, 32-bit init block and
descriptors, full byte lane reversal on descriptor DMA, pass-through
data).  With e17-tools/netserv.py as the network peer, RMON's
netboot completes: RARP assigns the addresses, TFTP transfers the
boot file, the header is validated and the image is executed at its
entry point (verified with a payload that dumps its own entry state,
see below).

### "TLANCE: chain err 2" root cause (fixed in hw/net/pcnet.c)

The TLANCE driver's RX path (fe817152) polls the OWN bit of RMD1 in
a tight loop (byte at RMD+4: OWN=7 ERR=6 ... STP=1 ENP=0), then
requires ERR clear and STP|ENP both set — "chain err 2" is
literally `(RMD1_byte & 3) != 3`.  QEMU's pcnet core stored the
final descriptor of a received frame TWICE: once from
PCNET_RECV_STORE() with OWN already cleared but ENP/MCNT not yet
written, then again with ENP+MCNT.  The polling guest hits the
window between the stores every time (the iothread does the DMA
while the TCG thread spins) and sees OWN=0, STP=1, ENP=0, MCNT=0.
The journal's earlier ring dump confirms it post-mortem: descriptors
read 0x8040fa12 — 0x40 at byte RMD+5 is pcnet's PAM bit (0x0040,
unicast match) from the SECOND store, layered under the driver's
re-arm which only rewrites byte +4 and clears the MCNT word at +10.

Fix: pcnet_receive now stores every non-final descriptor as before
but releases the final one with a single RMDSTORE carrying ENP, the
match bits and MCNT together (real chips also write MCNT before
relinquishing OWN).  This is an upstreamable fix — any driver that
polls (or races the interrupt) can hit it.

### TLANCE driver RMD/TMD semantics (from the disassembly)

RX (fe817152): poll byte RMD+4 for OWN clear; ERR set -> decode
FRAM/CRC/BUFF ("chain err" alone = ENP missing on an error frame);
else require STP|ENP==3; MCNT = word at RMD+10; frame copied from
buffer+14 (after dst/src/ethertype), length MCNT-14; re-arm = clear
word +10, byte +4 = 0x80.  TX (fe816fec): descriptor built with
word +6 = -length, word +8 cleared, byte +4 = 0x83 (OWN|STP|ENP);
after OWN clears, ERR -> word at TMD+8 decoded: bit15 BUFF ("
transmit buffer error"), bit14 UFLO, bit10 RTRY with TDR in the low
10 bits ("cable jammed. tdr = %d").

## The netboot image format (RE'd, VERIFIED by booting a payload)

Header check: block callback fe819016 (first TFTP block), error
code -> message dispatch at fe81851a (10 bad header, 11 bad size,
12 bad load address, 13 bad entry address, 14 bad checksum, 7 cpu
type mismatch — the last one is NOT produced by the TFTPBOOT path;
its trigger is still unlocated, likely another loader/protocol).

All big-endian, 22 bytes, image data follows immediately:

    +0  u32  magic 0x134FEE73
    +4  u16  cpu type — never read by the TFTPBOOT loader, only
             covered by the header checksum; 0 works
    +6  u32  size (must be > 100 and <= 0x01FE0000)
    +10 u32  load address (checked against RAM bounds; load+size+2
             must fit)
    +14 u32  entry address; if 0 the firmware uses the u32 at image
             offset 4; must be load <= entry < load+size
    +18 u16  image checksum — the firmware stores this word at
             load+size and the checksum over (load, size+2) must
             verify
    +20 u16  header checksum — makes the 22-byte header verify

Checksum (fe819bf8) = RFC1071 internet checksum: one's-complement
sum of big-endian u16s (trailing odd byte counts as high byte),
returns ~sum & 0xffff, 0 == valid.  First TFTP block must be >= 30
bytes.  e17-tools/mke17boot.py wraps a raw binary accordingly.

### Entry state (measured with a register-dump payload)

Launch path: fe80bd66 -> context block -> fe80714c, which does
move #0x2700,SR, restores DFC/SFC/CACR/VBR/USP/ISP/MSP from the
block, pushes a fake format-0 frame and RTEs into the image:

    SR = 0x2000 (supervisor, interrupt mask 0 — interrupts ON)
    VBR = 0 (RMON's vector table), CACR = 0 (caches off)
    a0 = entry address, sp = 0x7ec4 (RMON low-DRAM stack)
    other regs: d0=d1=0xc20 d4=0xc d5=0x16 d6=0xffff0000
    d7=0xffffffff a2=0xe00 a5=0x7eec (RMON internals, don't rely)

An exception (e.g. illegal instruction) returns cleanly to the
monitor prompt — RMON's generic handler longjmps back, so payload
tests are cheap.  Repro/driver scripts: ~/e17-re/netbt8.py (boots,
serves a wrapped image via netserv.py on unique ports 18485/18487,
gdb-dumps the result); payloads built with mke17boot.py.

Next for u-boot: build still blocked on bison/flex (waiting for
Daniel).  Once it builds: wrap u-boot.bin with mke17boot.py at
TEXT_BASE 0x600000 (entry = start), serve via netserv.py, and it
should get control in supervisor mode with caches off — exactly
what its start.S expects.

## The SCSI chip is a 53C720, and RMON bit-bangs it (2026-07-20)

The earlier "NCR 53C710" identification was wrong.  Tracing the
`scsi` bus scan against the stub (all accesses byte-sized; BE
offset = LE register ^ 3) and decoding the driver at fe81a380..
fe81a6dc gives a register map that only fits the second generation
"SCSI SCRIPTS" family, i.e. the 53C720 (the 68k-bus member; the
PCI 53C8xx parts share the map):

    ISTAT at 0x14 (SRST=0x40 soft reset — 710 has ISTAT at 0x21)
    STEST2 at 0x4e, written 0x03 = EXT|LOW: LOW LEVEL MODE enable
    SODL   at 0x54 (selection ID bitmask goes here)
    SBDL   at 0x58 (incoming bytes are read from the live bus)
    SOCL   at 0x09, SBCL at 0x0b, SCID at 0x04, SCNTL0/1 at 0/1

### The RMON driver protocol (all polled, no SCRIPTS, no IRQs)

init (fe81a398): DCNTL=0x20; optional SCNTL1 RST pulse (bit 3);
SCID = own id (7); SCNTL0 = 0xc4; STEST2 = 0x03 (low level mode).

selection (fe81a402): wait for bus free (SBCL BSY clear); SOCL =
0x20 (BSY), SODL = (1<<own)|(1<<target), SCNTL1 = 0x50 (ADB|CON),
SOCL = 0x30 (SEL|BSY), SOCL = 0x10 (release BSY, keep SEL); then
up to 786432 polls of SBCL waiting for the target's BSY.  Timeout:
SOCL = 0x02, SCNTL1 = 0, error.  Success: SOCL = 0x02, enter the
phase loop.  No ATN — the target goes straight to COMMAND phase.

phase loop: wait REQ (SBCL bit 7); phase = SBCL & 7; per byte the
initiator writes SODL (out phases) or reads SBDL (in phases), then
pulses ACK by writing SOCL = 0x40|phase followed by SOCL = phase,
waiting for REQ to drop in between.  Phases handled: 0 data out,
1 data in, 2 command, 3 status, 6 msg out (sends NOP 0x08 with
ATN), 7 msg in.  After the msg-in byte (command complete) the
driver waits for BSY to drop = bus free.  The LUN travels in CDB
byte 1 bits 7:5, SCSI-1 style — no IDENTIFY message is ever sent.

### The model (hw/scsi/ncr53c720.c)

New sysbus device "ncr53c720": the 0x60-byte register file with a
target phase engine behind SOCL/SBCL/SODL/SBDL implementing
exactly the low level mode — one REQ/ACK handshake per byte
against the QEMU SCSI bus (scsi_req_new/enqueue/continue, like a
miniature esp).  SCRIPTS, DMA and interrupts are intentionally
absent until some guest uses them; all other registers are plain
storage.  A "lane-swap" property applies the E17's reversed byte
lanes; the e17 machine maps it at 0xfec6c000 over the sysc block
(stub now removed) and services -device scsi-hd/-drive if=scsi.

Verified via the RMON CLI (~/e17-re/scsiscan1.py, bootdisk1.py):

    -device scsi-hd,scsi-id=6,drive=hd0 \
    -drive id=hd0,file=disk.img,format=raw,if=none

- `scsi` scan: "6 : + - - - <vendor> <product>" — INQUIRY data
  arrives byte-banged; absent LUNs (CHECK CONDITION) print "-",
  absent targets time out via the SBCL poll counter.
- `boot` (Harddisk, ID 6 default): OS-9 bootstrap reads sector 0:
  empty disk -> "No bootfile installed on disk" ($00F0); planting
  DD_BT/DD_BSZ (offsets 0x15/0x18 in sector 0) makes it read the
  bootfile LSN and verify the 0x4AFC module sync ("Kernel has bad
  module header" on a dummy).  A real OS-9 boot now only needs a
  real OS-9/68K disk image (kernel + bootfile).

Open: which chip errata/registers the LynxOS/OS-9 drivers use once
an OS runs — SCRIPTS support may become necessary then.

## The NVRAM/RTC is an M48T02 (2026-07-20)

Two stale guesses resolved at once.  The "DS1386 RTC at 0xfec01000"
entry was wrong twice over: that address is the VIC068A (the 0x80
writes there are its local interrupt control registers, and the
"table-driven init" is the VME configuration from NVRAM).  The real
NVRAM/RTC is the 2KB byte-wide device at 0xfec20000, and the OS-9
bootstrap identifies it beyond doubt: its time-of-day routine
(fe83b7d2, in the embedded bootstrap module) does

    or.b  #0x40, (0xfec207f8)   ; control register: set READ latch
    move.b (0xfec207fb) -> hours   (BCD)
    move.b (0xfec207fa) -> minutes (BCD)
    move.b (0xfec207f9) -> seconds (BCD)
    and.b #~0x40, (0xfec207f8)  ; release the latch
    -> seconds since midnight

which is exactly the SGS-Thomson/Mostek M48T02 "Timekeeper" — 2KB
of battery SRAM with control/sec/min/hour/day/date/month/year in
the top 8 bytes, BCD, READ latch bit 0x40 / WRITE latch bit 0x80 in
the control byte.  ("SRAM/RTC battery exhausted" refers to this one
chip, not two devices.)

Also decoded while in there:
- +0x468: board identification block, written by fe803bb4: 2 bytes
  + 5 ASCII + 8 bytes (ethernet address material) + 16-bit bytewise
  sum — the source of the banner's ethernet address / serial number
  when no IPIN EEPROM answers.
- +0x700: OS-9 bootstrap parameter block, 16 bytes: 12 data bytes
  (read/written by fe83b5a0/fe83b6ee; bytes 10-13 hold a 32-bit
  address validated as >= 0x300000 and 4KB aligned), 16-bit bytewise
  sum over the first 14 bytes at +0x70e.  Checksum failure prints
  "Data in battery-backed-up RAM corrupted!".

Model: the machine now instantiates QEMU's sysbus-m48t02 at
0xfec20000 (replacing the plain-storage block in e17-sysc), so the
clock runs and the contents can persist:

    -drive if=mtd,format=raw,file=nvram.img     # 2KB raw file

Verified from the RMON CLI (~/e17-re/rtctest1.py, rtcbanner.py):
`db fec207f8` shows the live clock in BCD matching host UTC
(00 26 07 08 01 20 07 26 = 08:07:26 Monday 2026-07-20), `we`
writes the config through to the backing file (checksum fb b3 86
for the default config, as documented above), and a restart on the
saved file boots without the "Wrong parameter checksum" warning
while a blank file shows it once.

Note for scripted runs: with -serial unix:...,server,nowait the
guest's output before the client's first byte is DROPPED — the
boot banner is only visible with -serial file:... or if the client
connects and transmits immediately.  (This is why the driver
scripts wait for the prompt by sending a CR first.)

## Netboot plan (for loading u-boot or other payloads)

1. Wire QEMU's existing LANCE model (hw/net/lance.c, as mvme147
   does) at 0xfec68000 with the E17 lane arrangement: RDP at +2,
   RAP at +6 (16-bit regs in the low half of 32-bit words) — two
   2-byte memory region aliases onto the lance's RDP/RAP.  DMA byte
   order should match the mvme147 ledma swap (same 16-bit LANCE on
   big-endian 68k bus; the tree's lance already routes through
   those hooks).
2. Station address: RMON reads the MAC from a PROM around LANCE
   +0x1d01/+0x1d81 (odd byte lanes, low nibbles) — model as a small
   ROM region, or rely on the EPROM-default address (banner shows
   00:00:5B:00:49:62 without it).
3. Use slirp's built-in BOOTP+TFTP: -netdev user,tftp=DIR,
   bootfile=FILE; the bootstrap does BOOTP then TFTP.
4. DONE: payload format RE'd and verified — see "The netboot image
   format" below; wrap payloads with e17-tools/mke17boot.py.
   (Alternative payload path that needs no network: `sload`
   S-record download over serial + `gm` start user module.)
- Session hygiene: other Claude sessions also run QEMU here — always
  use unique names (socket paths under ~/e17-re, odd gdb ports, kill
  only via stored pids, never pkill by generic pattern).

## The u-boot port (branch e17 in /workspace/src/uboot-e17)

Daniel's port lives on origin/mc68000_megadrive (checked out as
`e17`); board files board/eltec/e17, configs/eltec-e17_defconfig,
DTS arch/m68k/dts/eltec-e17.dts, chip headers include/cd2401.h and
include/vic068a.h (from the hardware manual — these named the sysc
blocks, see the VIC068A commit).  TEXT_BASE 0x600000, SPL 0x400000;
the machine's new -kernel option loads an ELF and jumps to it.
Building is BLOCKED on missing bison/flex (+m4) in the sandbox —
waiting for Daniel on how to provide them.  The CD2401 model now
covers everything the u-boot serial driver does (polled TISR,
PILR-matched IACK, VIC LICR6 line readback), so once it builds the
UART should work — test with:
    qemu-system-m68k -M e17,video=off -kernel u-boot -serial stdio

## The boot-time hook crash: SOLVED (2026-07-20, night)

The "### Reserved (1) Exception / ### Error in hook initialization
routine / ### Bus Error Exception at address 2f0841ee / RAM
available : 65535 MBytes" cascade had ONE root cause with a second
bug hiding behind it:

1. DRAM mirroring.  RMON sizes memory (fe8045ca) by planting
   0x55555555 at 0 and probing at doubling addresses for the DRAM
   controller's wrap (verified with 0xAAAAAAAA).  On real hardware
   the fitted RAM aliases across the decode window, the probe finds
   the mirror and returns cleanly — NO exception is ever taken.  In
   QEMU the RAM just ended: the probe ran into unassigned space,
   faulted, and the longjmp recovery popped the saved a0 from a
   skewed stack slot — the routine returned d0=-1 ("65535 MBytes")
   with a0=0 instead of the config base.  Everything after in
   fe807cc6 then read config fields from the low vector table:
   hook pointer config[0x348] -> vector 210 (= the generic
   exception handler, whose call longjmps straight back — the
   "Reserved (1)" + "hook initialization" pair), banner strings
   blanked ("for the  - 68040"), and the autostart context filled
   from garbage (the 2f0841ee bus error).  Fix: the machine now
   mirrors RAM across the DRAM window (0..VRAM base) like the
   hardware; RAM size must be a power of two.

2. Z8536 reset-state protocol (uncovered by fix 1).  With the boot
   no longer poisoned, a "### Line 1111 Emulator Exception"
   appeared: the DIP-switch read returned 0xff instead of 0x00.
   RMON initialises the CIO with the documented reset dance —
   pointer 0x00, data 0x01 (MICR RESET), then a bare 0x00 that the
   chip must route to the MICR (while RESET is set every control
   write addresses the MICR).  The model treated that 0x00 as a new
   pointer, desyncing every subsequent pointer/data pair: the DDRs
   never loaded and port B reads returned the 0xff output latch.
   Switch nibble 0xf >= 8 enables the battery-SRAM AUTOSTART
   (fe8080c8: nibble 0-3 = NVRAM config, 4-7 = forced ROM profiles
   1-4, >= 8 = NVRAM config + autostart), which jumped to
   *(0xfea00000) = 0 and executed vector 0 (0xfea01000 = an F-line
   opcode).  Fix: the CIO model now implements the reset state
   (powers up with MICR.RESET set).

With both fixed the boot is clean: RAM available : 16 MBytes, full
banner (Eurocom 27, ELTEC copyright), no spurious exceptions; the
first boot on a blank NVRAM warns about the parameter checksum once,
as the real board does.  The "Extended slave address : 0x80000000"
line is the EPROM default configuration, not a bug.

Autostart, decoded along the way (fe8080c8-fe80811a): with switch
nibble >= 8, RMON builds a launch context with PC = ISP = USP =
VBR = *(module), MSP = *(module+4) from the pointer in
config[0x34c] (default 0xfea00000 = battery SRAM) and runs it via
the fe80714c RTE launcher under a setjmp — no header validation at
all.  A future "boot from battery SRAM" payload path, essentially
free once the SRAM persists.

Debug tips that work: qemu -gdb tcp::PORT -S plus gdb-multiarch
batch scripts ("set architecture m68k" + "set endian big"); rwatch on
ROM string addresses to catch prints; -d int shows only real
exceptions (the hook error prints WITHOUT any logged exception, so
the "Reserved (1)" report is made up from state, not a taken CPU
exception).

## The AT keyboard (2026-07-20, night)

The keyboard interface at 0xfec60000 (data +0, status +1) is driven
by RMON like a raw AT keyboard behind a dumb latch — no 8042-style
command port, no translation:

- status bit 1 = a scancode/reply byte is waiting (the LED and
  typematic routines DRAIN the port while it is set); bit 0 =
  interface ready/present, polled high before commands.
- reset probe (fe81ac7c): write 0xFF, wait bit 0, wait bit 1, read
  data until 0xAA (BAT complete) — tolerates the 0xFA ack arriving
  first.
- LED update (fe81ad1c): 0xED + LED byte, each expecting 0xFA; the
  LED shadow lives at DRAM 0x58e8.  Typematic (fe81ae30): 0xF3 +
  rate byte from the NVRAM config (the SCSI/Keyboard setup menu).
- reader (fe81af2a): poll bit 1, read the scancode; 0xAA hot-plug
  re-runs the whole init; everything else goes through the
  translation object at DRAM 0x3840.  The translation table at ROM
  fe81b01c is laid out in AT SCAN CODE SET 2 order (0x15='q',
  0x16='1', 0x1a-0x1e = z s a w 2), i.e. the keyboard talks raw
  set 2 with F0 break prefixes.

Model: e17-sysc now embeds QEMU's PS2 keyboard core (scan code set
2, no translation) behind those two registers — the PS2 "irq" line
is the OBF status bit.  Verified end to end with the QEMU monitor:
sendkey h/e/l/p/ret against the video console, screendump shows the
echoed command and the full help screen rendered on the 800x600
framebuffer.  The machine is now fully interactive in a QEMU
display window (RMON factory console: video out + AT keyboard in).


## Running as a 68060 (2026-07-20, late)

The E17/E27 shipped with a 68060 option and RMON carries both CPU
paths (it probes by executing movec from PCR, which traps on the
68040).  -cpu m68060 now boots to a clean banner ("for the Eurocom
27 - 68060", CPU type register at 0xfec5e000 written 4), and the
SCSI scan runs; the machine restricts -cpu to m68040/m68060.

QEMU's m68060 model needed fixes for RMON's very first instructions
(all upstreamable):
- CINV/CPUSH and PFLUSH were registered for the 68040 feature only,
  but the 68060 keeps all of them (it drops PTEST) — RMON F-lined
  on the cinva at fe8005e4 before any console output.
- movec to/from ITT0/ITT1/DTT0/DTT1 was 68040-gated; the 68060 has
  the same transparent translation registers (RMON sets DTT0/DTT1
  right after the cache invalidate).
- movec to/from PCR/BUSCR was cpu_abort().  PCR is now modelled:
  identification 0x0430, revision 1, writable EDEBUG/dFP/ESS bits
  (0x83); BUSCR is plain storage.  RMON's CPU probe reads PCR and
  runs the 68060 setup path with it.

## VxWorks bring-up (2026-07-20, late night — in progress)

Daniel found a VxWorks image on bitsavers:

    http://bitsavers.informatik.uni-stuttgart.de/pdf//eltec/EUROCOM-27/e17vxworks.bin
    md5 b61db2fe930477f8f6421176d4a95f3e, 256KB (EPROM sized)
    -> /workspace/src/qemu-e17-re/e17vxworks.bin

It is a VxWorks 5.3.1 COMPRESSED BOOT ROM (zlib strings, built
"Mar 30 1998"), i.e. the boot loader with the interactive [VxWorks
Boot] shell.  Layout: reset-vector pair at +0 (SP=0x1000,
PC=0xfea00008 — it lives in the battery SRAM/EPROM sockets at
0xfea00000), two movew #0x3700,%sr entries at +8/+0x10, stub sets
the 040 TTRs, copies+inflates itself to DRAM ~0xd0000-0x13c000 and
runs there.  NOT wrapped in the RMON netboot header.

Run it by injection (no RMON):  ~/e17-re/vxrun.py — qemu -S, gdb
restore of the image at 0xfea00000, set $sp=0x1000 $pc=0xfea00008,
continue.  (vxinspect.py / vxstep.py for poking at it.)

What its BSP taught us (all now modelled, see the interrupt commit):
- system clock = CIO2 CT3, 60Hz (TC 41666 => CIO PCLK 2.5MHz),
  CT vector 0x50, MICR=0x84 (MIE + CT VIS), MCCR CT3 enable;
- VIC068A LICR1 = 0x11 (CIO tick), LICR6 = 0x05 (CD2401, level 5);
- reads the M48T02 clock through the +0x7ff8 mirror;
- runs the kernel in MASTER MODE (SR.M) — which flushed out two
  target/m68k bugs (vectored-interrupt entry ignored SR.M; format 1
  throwaway RTE unwound through the stale stack pointer).

Current state: kernel boots, reaches the windExit idle loop, takes
60Hz tick interrupts with a perfectly stable stack (interrupt log
shows constant sp across ticks).  At exactly tick 60 (1 second) a
kernel-context callout jumps through a poisoned pointer (0x0f0f0f0f
pattern) landing mid-instruction in a fill loop at 0xe19f0 =>
F-line => vector-table corruption cascade => double fault.  Smells
like a watchdog/callout armed with a garbage routine during driver
init — possibly the console (CD2401) or another device probe
returning something the BSP dislikes.  No console output yet (the
banner would come from usrRoot, exactly what dies).

Next steps: catch the wdStart/callout with a watchpoint on the
kernel work queue, or breakpoint f836c (TCB context save) and walk
taskIdCurrent; compare what sysHwInit reads from our devices vs a
real board (VIC/CIO/CD2401 probe results).  The RMON regression
suite still passes (netboot, scsi, video console).

### VxWorks bootrom: analysis artifacts and address map

Artifacts (qemu-e17-re/, rescued from /tmp before a machine reset):

    vx-ram.bin — the bootrom DECOMPRESSED into DRAM, dumped from the
      guest (0xd0000-0x140000).  Regenerate: run ~/e17-re/vxrun.py,
      then gdb: dump binary memory vx-ram.bin 0xd0000 0x140000
    vx-ram.asm — its disassembly:
      m68k-linux-gnu-objdump -b binary -m m68k:68040 -D vx-ram.bin \
        --adjust-vma=0xd0000 > vx-ram.asm

Driver scripts in ~/e17-re: vxrun.py (boot + report), vxinspect.py
(boot + arbitrary gdb commands), vxstep.py (runs a gdb script file:
vxload.gdb inject-only, vxwatch.gdb vector-table watch, vxstep.gdb/
vxstep2.gdb break at the tick handler and single-step).

Addresses in the DECOMPRESSED image (all confirmed by stepping):

    0xdc664   vector table init loop (fills vectors with the stub)
    0xfa81a   uninitialized-interrupt stub handler
    0x7fd8c4  intConnect trampoline installed at vector 0x50 (tick)
    0x7fd898  ditto at vector 0x52
    0xf82fa   intEnt (entered from the trampolines)
    0xf8276   windExit kernel idle spin: tstl 0x138e88; bne .
    0x138e88  workQIsEmpty flag (1 = empty; ISR work clears it)
    0xf83d8/f8402/f843c/f847e  workQAdd0/1/2 (clear workQIsEmpty)
    0xf84a0   workQDoWork (sets it back at 0xf84d8)
    0xf830c   intExit; 0xf8322 btst #4,sp@(4) = frame SR.M test;
              0xf834c the plain rte (unwinds the throwaway frame)
    0xf834e   intExit reschedule path: saves the interrupt frame
              into the TCB (0xf836c: sp@ -> TCB+0x16e SR,
              sp@(2) -> +0x170 PC — a straddled layout), stack from
              0x138a60, then windExit dispatch
    0x138950  taskIdCurrent (TCB list head at 0x138a70)
    0x126796/0x126798  saved SR values used for int lock/unlock
              (0x126798 holds 0x3000: S|M, IPL0 — master mode!)
    0xe19f0   THE CRASH SITE: mid-instruction inside a longword fill
              loop (dbf at 0xe19ee); a callout fired at tick 60 jumps
              here (poison pattern 0x0f0f0f0f); vector table then gets
              shredded and the machine double faults

Crash timeline (deterministic): 60 clean tick interrupts (all at the
idle pc, constant sp — entry/exit verified good), then F-Line at
0xe19f0 in a fresh context (sp=0x7ffd5c), then the Address Error
cascade.  Next session: watch the workQ/TCB (0xf836c breakpoint) or
watch writes of 0x000e19f0 anywhere in RAM to catch who computes the
bad pointer; suspect a wdStart with a garbage routine from a device
driver init (console tty is a candidate — no banner ever printed).
