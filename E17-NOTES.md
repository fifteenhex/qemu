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

## Running it

    qemu-system-m68k -M e17 -bios rmon.bin
    # rmon.bin: 256KB or the 1MB bitsavers dump, see "Firmware" above

With video fitted (the default) RMON adopts the 800x600 framebuffer
as its console, so the monitor prompt appears in the QEMU display
window; serial port 1 is still there (-serial ...).  Keyboard input
for the video console is NOT modelled yet — for an interactive
monitor use -nographic (video still probes OK; RMON only switches
the console to video, output-wise; input handling is the open item).
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
1. Boot noise + wrong RAM size — one interlinked problem, partially
   analysed (see "The boot-time hook crash" below for everything
   known).  Continue from there.
2. NVRAM: `we` at the prompt fixes the "Wrong parameter checksum"
   warning for the current run; persist the NVRAM (and battery SRAM)
   via -drive if=mtd like mvme147 so it survives restarts.  Model the
   RTC time registers (chip still not identified — DS1386-like, byte
   lane 3).
3. Real 53C710 model for SCSI disk boot ("boot" command, LynxOS),
   or port/extend an existing 53c9x-family model — QEMU has none for
   the 710.
4. LANCE at 0xfec68000: reuse the existing lance/pcnet core with the
   E17's RDP+2/RAP+6 lane arrangement; MAC PROM nibbles at +0x1d81.
5. Video (RAMDAC rev 0x3a + CRTC) and the AT keyboard for a console
   on the framebuffer; VRAM is already mapped at 0x0fc00000.
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

- `scsi` (bus scan) exercises the 53C710 like this: ISTAT SRST
  pulse; SCNTL1 = 0x08 pulse (SCSI bus reset), SCID = 7, SCNTL0 =
  0xc4, DCNTL pokes; then per target ID: SODL/SOCL writes (0x20,
  then a bus-ID bitmask 0x80|target to BE offset +0x57, SCNTL1 =
  0x50, 0x30, 0x10) and ~786000 polls of SBCL waiting for the target
  to respond before timing out.  A future 53C710 model must make
  selection timeouts fail fast (SBCL stays 0) or implement SCRIPTS
  properly.  All offsets confirm the byteswapped-within-longword
  wiring.
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
  embedded "OS-9/68K System Bootstrap" which endlessly retries
  "boot: Can't initialize the boot device / Boot failed, error
  status $00F6" against the SCSI stub.  So the road to booting an
  OS is: make the 53C710 model real.  RMON/the bootstrap drive it
  with manual register-level selection (see the scsi trace above),
  not SCRIPTS, so a phase-engine model in the style of the other
  QEMU SCSI HBAs should be enough to boot OS-9 from a disk image.
  THIS IS THE HIGHEST-PAYOFF NEXT TASK.
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

## The boot-time hook crash (open investigation)

Symptom, in serial output order: "### Reserved (1) Exception",
"### Error in hook initialization routine", (first boot only:
"Wrong parameter checksum" + "Reading default configuration values
from EPROM"), "### Bus Error Exception at address 2f0841ee", and
"RAM available : 65535 MBytes".  The monitor is fully usable
afterwards; bus errors at the prompt recover cleanly.

What was established (all in rmon 3.1.3 addresses):

- fe807cc6 is the startup/configuration routine, called once from
  fe800d88 with the POST flags.  It reads the DIP switches (CIO1
  port B), copies a configuration profile to DRAM 0x800 (profiles at
  ROM fe801f5e/fe80275e/fe802f5e/fe800f5e, chosen by the jump table
  at fe807d78 on the switch low nibble; out-of-range goes through
  fe804096 = read-from-NVRAM with the checksum warning), forces the
  console device byte config[0x459] to 0x11 (= serial) when the
  video-absent POST flag is set, runs the device inits
  (fe8041f8/fe804380/fe804490/fe80453c/fe8045b4/fe804698), then:
  - config[0x348] "Additional Init" hook: if != 0xffffffff, call it
    under a setjmp (fe806f06 = setjmp: saves full context at
    fp+0x2338, returns 0; the generic exception handler fe806f88
    longjmps back with the exception number in d0).  On error:
    fe808288 prints "### <name> Exception" (names at fe807644,
    16-byte entries) and fe81e03c prints the string at fe807b98
    ("hook initialization" error).
  - then a user-module autostart: pointer = config[0x34c], defaulting
    to 0xfea00000 (battery SRAM!) when 0xffffffff; launched through
    setjmp + fe80714c, gated by config[0x370] bits and the warm-boot
    flag (checks around fe808128).
  - RAM sizing (fe8045ca) runs only AFTER all this and stores the
    size in config[0x496]; the "65535 MBytes" is its 0xffff error
    return, a consequence of the earlier crash (vector 2 in DRAM
    holds garbage 0x2f0841ee at that point — instruction bytes from
    fe806f8c, i.e. from inside the generic exception handler — so
    the sizing bus error at end-of-DRAM double-faults).
- The setup menu ("setup" -> f Hooks) shows all four hook parameters
  as ffffffff, and a gdb breakpoint confirms config[0x348..0x357] ==
  ffffffff right before the hook check on the first pass — yet the
  hook error still prints, and a breakpoint at fe808050 fires with
  a1=fe806f88 (the generic exception handler!) and a0=0.  So some
  path reaches the hook-call code with a0 (config base) = 0, reading
  the "hook pointer" from DRAM 0x348 = the exception vector table
  (vector 210), which holds fe806f88.  Who calls fe807cc6 (or jumps
  into its middle) with a0=0 is THE open question.  Next session:
  breakpoint fe808044 with a proper register dump each hit (it fires
  more than once), and/or watch DRAM 0x8 for the write of 0x2f0841ee
  (gdb: set endian big!  the qemu stub + gdb-multiarch otherwise
  byte-swaps register values).

Debug tips that work: qemu -gdb tcp::PORT -S plus gdb-multiarch
batch scripts ("set architecture m68k" + "set endian big"); rwatch on
ROM string addresses to catch prints; -d int shows only real
exceptions (the hook error prints WITHOUT any logged exception, so
the "Reserved (1)" report is made up from state, not a taken CPU
exception).
