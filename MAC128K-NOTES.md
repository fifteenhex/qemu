# Macintosh 128K QEMU machine — journal

Goal: boot Mac System 1.1/2.0 from a 400K floppy image to a working
Finder on a new `mac128k` machine, with a moving/clicking mouse.

Working tree: worktree of `/workspace/src/qemu-amiga` (branch `amiga`).
Build dir: `build/`.  Session-unique names use the `mac128k-` prefix;
QMP socket `/tmp/qmp-mac128k.qmp`, screenshots `/tmp/mac128k-*.png`.

## Assets (`/workspace/src/qemu-amiga/_mac_assets/`)

- `Mac128K.ROM` — 64KB Rev A ROM, checksum dword `28BA61CE` at offset 0
  (doubling as the garbage reset SP), reset PC dword at offset 4 =
  `0040002A` — an *absolute* address, the ROM hardcodes its 0x400000
  base, so the overlay only matters for the vector fetch.
- `system11.img` / `system20.img` / `system20-tools.img` — raw 400K
  (409600 byte) MFS floppy images.

## Hardware model

- 68000 @ 7.8336 MHz (QEMU's m68000 has M68K_FEATURE_ADDR24, so the
  top address byte is ignored like on the real bus — tagged Memory
  Manager pointers work).
- RAM 128KB at 0, mirrored through the whole 0x000000-0x3FFFFF window:
  the ROM sizes memory by looking for the wrap-around.
- ROM 64KB at 0x400000, mirrored through the 1MB block.
- Overlay: at reset ROM is aliased at 0 and RAM appears at 0x600000
  (mirrored over 0x200000); cleared when the ROM writes VIA PA4 low.
  Input pins with DDRA=0 read as pulled up, which conveniently makes
  the overlay default on when the VIA resets.
- Everything unmapped is OPEN BUS (reads 0), never a bus error — the
  128K has no bus error generator, and the early ROM runs with SP=0
  pushing exception frames into the 0xFFxxxx bit bucket.
- VIA 6522 at 0xEFE1FE, reg N at +N*512 (window at 0xEFE000, reg =
  (offset>>9)&0xF).  Port A: 0-2 volume, 3 sound page, 4 overlay,
  5 SEL to the floppy, 6 screen page (1 = main), 7 SCC wait (in).
  Port B: 0-2 RTC data/clock//enable, 3 mouse button (in, 0=down),
  4/5 mouse X2/Y2 (in), 6 hblank (in), 7 sound enable.  Timers run at
  783.36 kHz.  60.15Hz VBL on CA1, one-second RTC tick on CA2.
- SCC Z8530: read space base 0x9FFFF8, write space base 0xBFFFF9
  (both decode B-ctrl/A-ctrl/B-data/A-data at +0/2/4/6; the write
  window forwards to the same escc).  escc it_shift=1 bit_swap=true,
  chn[0]=B chn[1]=A.  Mouse X1/Y1 → DCD A/B: added "dcd" gpio inputs
  to escc.c that latch RR0.DCD, raise ext/status ints (WR1 bit0 +
  WR15 bit3) and set the RR2B modified vector (ext A=0x0A/0x50,
  ext B=0x02/0x40 for status-low/high); cleared by WR0=0x10.
- IWM at 0xDFE1FF, reg N at +N*512: see hw/block/iwm.c header for the
  latch scheme and the Sony drive protocol.  ROM equates: ph0L=+0x000,
  ph0H=+0x200 ... mtrOff=+0x1000, mtrOn=+0x1200, intDrive=+0x1400,
  extDrive=+0x1600, q6L/H, q7L/H.
- Video: 512x342 1bpp, white=0, main buffer at ram_top-0x5900 (alt at
  -0xD900), framebuffer console scans main RAM directly (the
  framebuffer helpers handle VGA dirty logging on a plain RAM
  region — same trick as maciisi-fb but without a dedicated VRAM).
- Sound: not modelled (PB7/PA3 ignored).  The 400K drive's PWM speed
  control is irrelevant because the tach always reports the nominal
  zone speed.
- Keyboard: not modelled; the ROM's model-number query times out
  harmlessly (like a real Mac with the keyboard unplugged).

## Iteration log

### Skeleton to insert-disk icon (2026-07-22)

- First boot attempt executed from address 0 as zeroes: the m68k CPU
  reset does NOT load SP/PC from the vector table, and the ROM
  loader's reset callback (which copies the -bios image into the ROM
  region) runs AFTER the machine's qemu_register_reset handler — so
  ldl_phys(0x400000) at reset time read 0.  Fix: stash the two vector
  dwords at init via rom_ptr() (maciisi does the equivalent with
  stl_phys into RAM).
- With that, the ROM runs: RAM test fills the screen with noise
  (visible in the framebuffer — video path proven), memory sizing
  probes the mirrors, overlay clears, the OS world starts (A-line
  traps, level-1 VBL interrupts), and the machine reaches the
  **blinking insert-disk floppy icon** on the gray desktop.
  Milestone (a).  /tmp/mac128k-shot2.png.
- Early-ROM curiosity: the start code runs with SP=0, deliberately
  pushing into the top-of-address-space bit bucket; first insn at 0
  before the fix decoded as `movel %pc@(0x61d0),%a4@` = the checksum
  dword — nice fingerprint that the vectors weren't loaded.

### Floppy boot: the drive probe fights back (2026-07-22)

Traced the ROM's boot-time drive probe through the new IWM latch/sense
logging (`-d unimp`).  Findings, each verified against the running ROM:

- The probe is: poll CSTIN (disk in), motor on, **strobe the eject
  register with LSTRB held across a timed delay**, motor off, then
  read sense reg 0xE once per connector and believe it forever.  The
  power-on sequence really does eject the disk and then waits for an
  insertion event.  Consequences for the model: eject requires a
  >=400us LSTRB pulse with stable address lines, and an eject with the
  image still attached re-inserts it one second later (a stand-in for
  the user pushing the disk back in).
- Sense 0xE (my {CA2,CA1,CA0,SEL} numbering) = DRIVE PRESENT, active
  low.  Cracked via the select-value table in linux swim.c
  (SWIM_DRIVE_PRESENT 0x077 -> SEL=0, CA=111); my first guess (DRVIN
  at 0xF) made the probe write off the internal drive.
- Sad mac **0F0004** (divide by zero) at ROM 0x401ec0: the ROM
  calibrates a PWM->speed table by measuring the tach at two PWM
  settings (0x401e82) and divides by the speed *difference* - a tach
  that always reports the nominal zone speed calibrates to a zero
  slope.  The 128K spins the drive itself: PWM bytes = low bytes of
  the sound-page words; SetSpeed (0x401c12, CurSpeed lowmem 0x138)
  encodes its 0..399 index as a **6-bit LFSR state**
  (`next = (s>>1) | (((s^(s>>1))&1) << 5)` from state 11, one step
  per 10 index units, two adjacent states dithered across the buffer).
  The model decodes buffer bytes back to step counts and answers with
  a linear RPM curve (695 - 10*steps); the ROM happily calibrates it.
- `-icount shift=7` is required for the tach/speed dance, exactly like
  the maciisi TimeDBRA story: the ROM samples the tach in dbra-timed
  windows, so the CPU must run at a realistic 68000 pace relative to
  the virtual clock.
- GCR worked on the first try: address field D5 AA 96 + 6&2 GCR
  track/sector/side/format/csum + DE AA; data field D5 AA AD + sector
  + 12 zero tags + 512 data through the three-rolling-checksum
  nibblizer (ROL/ADDX chain, MAME ap_dsk35-style) + csum group
  (c4,c3,c2,c1) + DE AA FF.  Sectors served in logical order,
  always-valid bytes; the polling reader doesn't care about pacing.

### Mouse: two interrupt-model bugs, then a driveable Finder

- **Level-3 livelock**: VIA on IPL0 + SCC on IPL1 OR-ed gives level 3
  when both assert - and the 64K ROM's level-3 autovector (0x6C) is a
  bare RTE (0x400bec, the tail of its SCC ISR).  With level-triggered
  re-sampling the CPU takes level 3 forever; the machine soft-locked
  the moment a VBL landed during mouse traffic.  The glue is now a
  priority encoder: SCC pending = 2, else VIA = 1.
- **Stale RR2**: the Mac SCC ISR (0x400bc0) reads RR0, selects reg 2,
  dispatches on the RR2B modified vector through Lvl2DT (0x1b2), and
  resets only the channel it dispatched to.  escc now recomputes the
  modified vector from the still-pending ext ints on set *and* clear
  (A first), else the second channel's edge dispatched to the null
  handler and its interrupt stayed asserted forever.  Related trap:
  escc's txint is latched by every data write even with tx ints
  disabled, so the vector-precedence guard must check enables.
- Mouse ISR protocol (from the ROM): saved RR0 copies at 0x2ce/0x2cf,
  per-channel handler tables at 0x2be/0x2c6, second entry taken when
  RR0 changed in *only* the DCD bit; direction = DCD level vs VIA
  PB4/PB5 phase (X positive when X2 != X1 after the edge, Y positive
  when Y2 == Y1 - found empirically).  Steps are paced at 1.5ms:
  400us outran the 7.8MHz guest's per-edge interrupt service.
- Guest-side debugging tricks that paid off: HMP `xp` of the mouse
  globals (MTemp/RawMouse/Mouse at 0x828/0x82C/0x830) to tell "cursor
  offscreen" from "counts lost", and gdb -p on the qemu process
  reading `current_machine` (never *call* functions from gdb - a
  qdev_get_machine() call aborted the VM on a BQL assertion).

### Milestones - all reached (evidence in /tmp)

- (a) blinking ?-icon: mac128k-fd1.png (also shot2 without disk)
- (b) happy mac: mac128k-fast-01.png
- (c) Welcome to Macintosh: mac128k-fast-02.png
- (d) Finder desktop: mac128k-fd8.png; System Disk window opened via
  mouse-driven File > Open: mac128k-open2.png (5 items, 288K in disk);
  menu tracking: mac128k-menu1.png (Edit), mac128k-menu3.png (File
  with Open/Get Info/Eject enabled)
- (e) mouse: cursor moves via QMP input-send-event, click selects
  (mac128k-click2.png), menus drag, File > Open executes.
- System 2.0/Finder 4.1 (system20.img) boots to its MiniFinder
  (mac128k-sys20.png) and clicking its Finder button brings up the
  Finder 4.1 desktop (mac128k-sys20-finder2.png).

Run recipe:
  build/qemu-system-m68k -M mac128k \
    -bios _mac_assets/Mac128K.ROM \
    -drive if=floppy,file=_mac_assets/system11.img,format=raw \
    -icount shift=7 -display none \
    -qmp unix:/tmp/qmp-mac128k.qmp,server,nowait

Known gaps / shortcuts (documented in the code):
- Disk always reads write-protected; no GCR write/decode path, so the
  Finder cannot save to floppy (fine for locked-floppy boots).
- Eject auto-reinserts after 1s whenever the image stays attached - a
  Finder-menu Eject puts the disk back in shortly after.
- Sound not modelled (the PWM bytes only feed the drive speed).
- The IWM data path ignores bit-level timing (always-valid bytes,
  position advances per read); writes are logged and dropped.

## MAC512K + MACPLUS (2026-07-23)

Extended the machine file into a small family: a Mac128kMachineClass
carries the per-variant knobs (ROM size / mirror span / filename,
SCSI present, 800K drive, SIMM-bank RAM rules).  `mac512k` and
`macplus` are subtypes of the `mac128k` machine type.

### mac512k — milestone (a)

Trivial: same 64K ROM, `default_ram_size = 512K`.  The screen/sound
buffers already track ram_top via ram_size, so nothing else changed.
Boots System 1.1 to the Finder identically to mac128k
(/tmp/macfam-reg-512.png).

### macplus

Hardware deltas from the 128K, each verified against the v3 ROM:

- **ROM**: 128KB v3 "Loud Harmonicas" (checksum 4D1F8172) at
  0x400000.  Crucially it mirrors *only* through the 256KB socket span
  (0x400000-0x43FFFF), not the 128K's 1MB block: the startup code at
  ROM 0x4003e4 does `cmpl 0x420000,0x440000` and treats equal reads as
  "512Ke decode, no SCSI".  Mirror the ROM to 0x40000 and the SCSI
  boot path arms; mirror it to 1MB and the ROM never scans the bus.
- **VIA port A latch**: the ROM writes its full port A value (overlay
  bit included) through register 15 (ORA-no-handshake) BEFORE it
  configures DDRA (ROM 0x4000ce `moveb %d2,%a5@(7680)`).  mos6522.c
  masks ORA writes by DDRA, so the overlay never cleared and the ROM
  looped on open-bus reads at 0x3fd5xx forever.  Fix in the machine's
  VIA frontend: store all bits into s->a on a reg-A/reg-15 write
  regardless of DDRA (the drivers, not the latch, are direction-gated),
  and read input pins as pulled up.  Overlay then clears at 0x4000d2.
- **SCSI**: NCR5380 (reg-shift 4) at 0x580000, polled "DACK" data
  pages at 0x580200 wired to ncr5380_pdma_read/write.  The Plus has NO
  DRQ-gated handshake aperture (unlike the IIsi) — the driver paces on
  the 5380 DRQ status bit.  Boot device is SCSI (`block_default_type =
  IF_SCSI`).  Two 5380 discoveries:
    - the ROM boot reader (0x417480..) arbitrates (ICR AIP/lost-arb
      read-only bits are polled — already right from the IIsi work),
      selects target 6 with correct selection-timeout, then reads via
      the DACK pages; the existing synchronous whole-transfer model
      served it byte-exact on the first try.
    - **data-out hang**: the ROM's blind writer (0x417418) waits for a
      TRAILING DRQ after its byte counter empties, before dropping DMA
      mode — it spun forever in the BSR poll at 0x41744e on the first
      WRITE.  Fix: re-assert BSR_DRQ at the end of a DMA-mode
      flush_data_out (the empty ODR re-raises DRQ once the target
      takes the last byte).  After that the whole boot chain streams.
- **800K double-sided drive**: iwm.c gained a `double-sided` property.
  Head select follows the drive-register lines parked at RDDATA0/1
  (CA2=1,CA1=CA0=0, SEL=head); raw 800K images interleave sides per
  cylinder; address fields carry head in side-byte bit 5 + format
  0x22; SONY_SIDES reports two-sided; the tach reports the nominal
  zone speed (the 800K drive self-regulates, no Mac PWM).  The ROM's
  power-on eject/probe dance runs on it; 400K images are rejected.
- **RAM**: SIMM banks, `-m` restricted to 1M/2M/2.5M/4M (default 4M).
  2.5M = full 2MB bank A + 512K bank B mirrored through its 2MB half.
  Power-of-two sizes mirror the whole bank as before.

- **M0110 keyboard** (all three machines): HLE over the VIA shift
  register.  The ROM driver (0x402568..) shifts a command byte out
  under the keyboard's external clock (ACR mode 7), takes an SR
  interrupt, flips ACR to shift-in, and receives one byte.  Model:
  a reg-SR write in shift-out-ext mode is a command; a timer pulses
  SR_INT, loads the response (Inquiry → queued key transition
  (code<<1)|1, bit7=release, or Null 0x7b after a 0.25s wait; Model
  resets; Test acks), pulses SR_INT again with the byte in s->sr.
  Verified: pressing 'A' on the mac128k Finder sets/clears the right
  KeyMap (0x174) bit; shift is held live in KeyMap on the Plus.

### macplus boot state — milestone (c)+ reached, (d) blocked by the disk

Boots the v3 ROM to the **happy Mac** (/tmp/macfam-plus3.png), scans
SCSI (the 800K probe eject dance included), loads the Apple driver
partition and the System, runs the startup extension parade and
renders System 7.5.3 dialogs — the "File System Access modules / Apple
Photo Access" alert draws and its Continue button is **clickable with
the working mouse** (RR15 fix below).  So the OS is live: video, SCSI
read/write, mouse and keyboard all exercised past "Welcome".

Then it **SysError bombs "illegal instruction"** in a loaded system
module (crash PC in RAM; the 64-byte code block matches the disk's
AppleShare / File Sharing Extension region at image offset 0x86465a).
Disassembly of that code shows `4e74 0008` = **RTD #8, a 68010+
instruction** — illegal on the 68000.  The macos753.hda was installed
for the 68030 Mac IIsi, so its system software carries 68020/030 code
a real 68000 Mac Plus cannot execute either.  Holding Shift keeps the
KeyMap disable-extensions bit set the whole time (confirmed 0x17b bit
0 = 1 through the parade) but this System still loads the module and
bombs.  **This is a disk/CPU-compat blocker, not a hardware-model
bug** — a 68000-clean 7.5.x install (or an older System that fits the
Plus) should reach the Finder on this exact machine model.  NEXT for
milestone (d): obtain/build a Mac-Plus-blessed System disk image
(needs Daniel's OK to fetch), or a 6.0.x/7.0 install; the model is
ready for it.

- **escc RR15**: the Z8530 returns WR15 when RR15 is read; the model
  never mirrored it, so RR15 read 0.  The Mac SCC ISR reads RR15 as
  its DCD-changed mask (`eorb old,new; andb RR15`), so with a zero
  mask every mouse quadrature edge dispatched to the non-DCD handler
  and *vertical* mouse motion vanished (horizontal survived only
  because channel A's two handler slots point at the same routine).
  Fixed in escc.c (mirror WR15→RR15 on write and at reset); vertical
  mouse now moves on the Plus.

Run recipe (macplus SCSI boot):
  build/qemu-system-m68k -M macplus \
    -bios _mac_assets/macplus_v3.rom \
    -drive file=/workspace/src/qemu-maciisi/_maciisi_assets/macos753.hda,\
format=raw,if=none,id=hd0,snapshot=on \
    -device scsi-hd,drive=hd0,scsi-id=0 \
    -icount shift=7 -display none \
    -qmp unix:/tmp/qmp-macfam-plus.qmp,server,nowait

Sockets used this session: /tmp/qmp-macfam-{128,512,plus}.qmp
(own processes only; never touched other agents' qemus).
