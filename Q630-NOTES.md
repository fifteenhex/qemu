# Macintosh Quadra 630 / LC 630 / Performa 630 machine ("quadra630") — progress notes

Goal: boot the Quadra 630 ROM (`/workspace/files/mac-roms/quadra630.rom`,
1 MiB, header checksum 0x06684214) to MacOS 7.5.3 (Finder) over SCSI,
with the F108 MMIO IDE controller present (ideally enumerating a disk).

## Design: lc475.c + IDE

Per Linux `arch/m68k/mac/config.c` MAC_MODEL_Q630: 68040, Cuda ADB,
VIA_QUADRA, ESP SCSI (MAC_SCSI_QUADRA), MAC_IDE_QUADRA, ESCC, SWIM 2,
PDS/comm slot; video is Valkyrie.  That is exactly the lc475.c device
set plus IDE, so `hw/m68k/quadra630.c` is a copy/adapt of lc475.c
(hand-built Cuda-on-VIA1 engine, Valkyrie regbank + guest-published
framebuffer, ESP w/ VIA2 pseudo-DMA, ESCC, SWIM, EASC, MEMCjr/PrimeTime
regbank stubs — on the real Q630 those ASICs are F108 + PrimeTime II,
but the stubs are RAM-backed and grown empirically anyway).

## IDE (MAC_IDE_QUADRA) — the new piece

From Linux config.c + old drivers/ide/macide.c:
- Taskfile at **0x50F1A000**, register stride 4 (`pata_platform`
  `ioport_shift = 2`), i.e. reg n at base + 4*n, window 0x38.
- Alt-status/device-control at **0x50F1A038** (4 bytes).
- **IDE_IFR at 0x50F1A101** (byte, odd address!): bit5 = IDE IRQ flag
  (write 0 to clear), bit6 = IRQ enable, bit7 = any-interrupt; from
  macide.c which was reverse engineered from the MacOS driver.
- IRQ = **IRQ_NUBUS_F** = VIA2 "nubus" slot $F input — QEMU
  `via2 "nubus-irq"` gpio input 6 (named VIA2_NUBUS_IRQ_INTVIDEO in
  mac_via.h; on the Q630 that port A bit is the IDE interrupt).

Modelled with QEMU's TYPE_MMIO_IDE (hw/ide/mmio.c) with shift=2:
region 0 aliased (first 0x38 bytes) at macio +0x1A000, region 1
(alt/ctl) aliased (4 bytes) at +0x1A038, plus a hand-written IFR
register at +0x1A101 that latches the IDE IRQ and gates the VIA2
slot-F line (enable default ON at machine init, so polling drivers
that never touch the IFR still get slot interrupts... may need
flipping if a storm appears).  Drives attach with `-drive if=ide`
(mmio_ide_init_drives / drive_get(IF_IDE,0,0/1)).  mmio-ide's LE data
region = the "straight-through, data appears in correct byte order"
wiring, same convention that pata_platform + ata_sff expect.

## Machine identification (ROM 06684214 analysis)

- The ROM uses the same 0x5FFFFFFC machine-ID register scheme
  (0xA55A/0x2BAD constants near ROM+0x494e/0x4bfc code that reads
  0x5FFFFFFC) — kept lc475's 0xA55A2BAD register.
- Universal table: entries are 0x48 bytes; a low-ROM table at 0x3760
  (older boxes 5..0x14, decoder kinds 5..9) and the interesting one at
  **ROM+0xa7a30..0xa81c8**: kind-10 entries (boxes 0x17/0x1a/0x47,
  strap mask 0x56000000 val 0), kind-11 (boxes 0x22,0x15,0x38,...,
  mask 0), **kind-13 = the 630 family: boxes 0x57,0x58,0x59,0x5a,0x5b,
  0x5c(x6),0x5d(x3) — ALL with strap mask 0 val 0**, i.e. no VIA1 PA
  strap matching at all; the family is presumably disambiguated by
  decoder probe / other means.  Kept lc475's pins_a=0xF9; expected to
  be irrelevant for identification.  Box->machine attribution TBD
  empirically via lowmem BoxFlag 0x0CB3 once booted.
- The ROM references the IDE base 0x50F1A0xx at ROM+0x3464 (early,
  probably decoder probe) and in a driver at ~0xcee6c/0xcfb38 — so the
  ROM does contain an IDE driver.

## Status log

- [x] baseline m68k-only build OK
- [x] quadra630.c (lc475 copy + IDE), Kconfig QUADRA630 (+IDE_MMIO),
      meson entry; builds clean
- [x] machine ID cracked: 0xA55A2224 at 0x5FFFFFFC (see below)
- [x] Cuda: Q630 ROM speaks the REAL Cuda packet protocol (polled +
      int-driven drivers); engine reworked (see below); PRAM rebuild
      (GET_PRAM/SET_PRAM scans) completes
- [x] F108 I/O slice granularity = 0x20000 (decoder probe)
- [x] video: 1152x870 1bpp ROM UI renders (gray desktop) at an
      intermediate stage
- [x] Singer sound codec stub at 0x50F04000 (mailbox handshake + done
      latch)
- [x] ASC boot-chime FIFO shim (fast-drain so the chime "plays"
      instantly under TCG instead of timing out to the death monitor)
- [x] 24-bit-mode support: ROM window at phys 0x00800000 (enabled by
      PrimeTime +0x200 write) + RAM mirror at 0x80000000 (tagged
      DCE/handle pointers); .Sony DCE now resolves
- [ ] **BLOCKED: Sad Mac `0000000F / 00000001` = dsLoadErr (SysError
      15), a driver/segment LOAD failure during late startup.**  Does
      NOT reach the Finder.  Video renders the Sad Mac fine (framebuffer
      works).  IDE controller is present and enumerates an attached
      disk (verified via `info qtree` / `info block`).

## FINAL STATE (honest)

`-M quadra630` runs the ROM through: machine identification (box 0x57 =
Quadra 630), full real-Cuda exchange + PRAM rebuild, MEMCjr/F108 RAM
sizing, Valkyrie-630 video init, EASC/Singer sound init, and 24-bit
Memory-Manager / Device-Manager startup — then stops at a **Sad Mac
0000000F/00000001 (dsLoadErr, SysError 15)**: the ROM fails to load /
install a driver or code segment during startup.  It does not reach the
"Welcome to Macintosh"/Finder stage, so neither SCSI nor IDE disk
enumeration by MacOS itself was observed.

The **MAC_IDE_QUADRA controller IS present and wired** (QEMU `mmio-ide`
shift=2 at 0x50F1A000 + IFR at +0x101 -> VIA2 slot $F); `-drive if=ide`
attaches an `ide-hd` onto its `ide.0` bus (confirmed in `info qtree` /
`info block`).  Because MacOS never boots, the guest's own IDE
enumeration (Drive Setup / "uninitialized disk" dialog) could not be
reached.

A gray 1152x870 desktop WAS rendered at an intermediate bring-up stage
(before the Singer/ASC/ROM-window/RAM-mirror work); after those changes
the boot advances further in CPU terms but the ROM reaches its
driver-load diagnostic and paints the Sad Mac over it early (~6 s).

### Suspected next step for dsLoadErr (unfinished)

The failure is a driver/segment load (`_LoadSeg`/`DrvrInstall` family).
The .Sony DCE pointer was fixed by the ROM window; the remaining
failure is likely another F108 device the driver init touches (the
last live probe before the Sad Mac was a word read of a device
register at logical->phys 0x500FB964 == slice offset 0x1B964, an
unmodelled F108 window between IDE @0x1A000 and IWM @0x1E000 — possibly
a DMA/PSC or the F108 "combo" control).  Modelling that device (and/or
whatever `_LoadSeg` needs) is the next lever.  Making unmapped in-slice
I/O read as 0 instead of BERR was tried and does NOT change the Sad Mac
(that 0x500FB964 probe is recoverable), so the dsLoadErr is elsewhere.

## Q630 ROM (06684214) findings

1. **Machine ID**: 0x5FFFFFFC must read 0xA55A2224 (word reads = low
   word).  The universal entry list is at ROM+0xA79BC (self-relative
   offsets, entries 0x48 bytes); kind-13 entries carry the expected ID
   LOW WORD at entry+0x44 and have strap mask/val = 0 — identification
   is purely by ID word; bit 11 of the ID clear = no VIA1 PA strap
   check at all (runner code at 0x40804b86..0x40804c04).
   Family IDs: 0x2224=box 0x57, 0x2225=0x58, 0x2226=0x59, 0x2231=0x5A,
   0x2232=0x5B (Q630/LC630/P630 group, DecoderInfo 0xA8420);
   0x2250..0x2257=box 0x5C, 0x2258/A/B=box 0x5D (LC580 group,
   DecoderInfo 0xA84C4 — that one has +0x6C = 0x50F1A000 IDE and
   +0x38 = 0x50F24000).  We use 0x2224.
2. **I/O aliasing**: the kind-13 decoder probe checks VIA1 repeats at
   +0x20000 (probe of VIA1+0x1c00+0x20000 at 0x408030ea via the
   writability prober 0x408046ac); IO_SLICE is 0x20000 on the F108,
   not 0x40000 (lc475/q800 value) — identification loops forever
   otherwise.  Probes of 0x50F26000 (no RBV) BERR harmlessly.
3. **Cuda protocol is the REAL Cuda packet protocol** (NOT the Q605
   Egret-style raw-ADB framing):
   - Packets have a TYPE byte: 0=ADB, 1=pseudo; responses have a
     3-byte header [type, flags, cmd] + data.  Pseudo commands seen:
     0x26+0x22 (I2C-ish, generically ACKed), 0x01 AUTOPOLL, 0x07
     GET_PRAM (2-byte addr, returns 1 byte), 0x0C SET_PRAM, 0x03
     GET_TIME/0x09 SET_TIME.  ADB timeout flag = 0x02 in the header,
     autopoll flag = 0x40.
   - /TREQ semantics = real Cuda: LOW while the Cuda has response
     bytes pending, checked synchronously after every host TACK
     toggle (0x408b3bee); deasserted at the toggle after the last
     byte; asserted by ACR-turnaround processing when a response is
     staged.
   - THREE ROM driver styles share the engine: an early polled sync
     (TACK-only session at 0x40885718: Cuda must assert /TREQ as the
     ack; "cuda_sync" special case), a fully polled driver
     (0x408aa040/0x408b3a24: busy-waits IFR bit2, needs an SR int
     for EVERY event incl. one after each session close and one after
     the end-of-response toggle), and an int-driven request queue
     (0x408a9bb4, completion flag polled at 0x408a9b50).
   - The polled driver routinely ABANDONS responses after the header
     byte; a host ACR->output turnaround with stale response staged
     must DISCARD the stale bytes (else the next command is eaten).
   - PRAM: rebuilt via pseudo GET/SET_PRAM over the whole 256 bytes
     (bit-bang RTC engine kept but appears unused by this ROM).
4. **Video (Valkyrie-630)**: register block at 0xF9800000 but LAYOUT
   DIFFERS from the Q605 Valkyrie: monitor sense = drive/read dance
   on +0x200/+0x220 (return 6 at +0x220 reads = 13" 640x480); pixel
   clock PLL bit-banged through +0x3C0; CRTC longs at +0x124..+0x160;
   depth code at +0x20 (0/1/2/3/4 = 1/2/4/8/16bpp).  The ROM draws
   its startup UI as 1152x870 1bpp rowbytes 576 at VRAM+0x80
   REGARDLESS of sense (fb defaults set to that; MacOS geometry will
   come from the PixMap tracker).
5. **Singer sound codec at 0x50F04000** (F108 slice +0x4000): the
   sound driver writes reg/value pairs through a byte mailbox: +2
   command/status (bit0 = ready, polled at 0x408b9fec/0x408ba0f4),
   +6 data.  RAM-backed regbank + bit0-always-set on +2 reads
   satisfies it.  Failure to map it = infinite retry loop with VIA1
   PA3 toggles ("portA_write unimplemented" spam).
6. **24-bit addressing (the big one)**: MacOS boots this ROM in 24-bit
   mode (rebuilt PRAM), and the ROM sets ROMBase = 0x00800000 plus
   68040 DTTR1/ITTR1 = 0x807FC040 (transparent-translate logical
   0x80000000-0xFFFFFFFF to identical physical).  Two decodes are
   therefore required that lc475 didn't need:
   - **ROM window at physical 0x00800000**: the ROM installs Device
     Manager DCE driver pointers based at 0x00800000 (e.g. the .Sony
     DCE 0x0086C3E0).  A ROM alias is mapped there, ENABLED by the
     PrimeTime II write of 0x1B to 0x50F18200 (right after the PRAM
     addressing-mode byte 0x8A is stored) and kept disabled before
     that so the RAM test sees RAM in that range.
   - **RAM mirror at 0x80000000**: tagged handle/DCE pointers formed as
     0x80000000 | 24bit_addr (e.g. the .Sony DCE accessed as
     0x800061E0) transparent-translate to phys 0x80xxxxxx, so the low
     16 MiB of RAM is mirrored there.
   With both, the .Sony install and the tagged-pointer dereferences
   resolve; the boot then advances to the dsLoadErr Sad Mac above.
7. **ASC boot chime**: the ROM synthesises a startup chime into the ASC
   FIFOs and busy-waits on the FIFO half/empty status (+0x804) with a
   tight timeout (0x40845de0..0x40845e08); the real drain is host-real-
   time-paced and always loses under TCG -> death monitor.  Shim over
   +0x804 fast-drains the FIFOs so the status reads "played" instantly.
8. **PITFALL (cost ~2 h)**: `pgrep -f "M quadra630" | head -1` matches
   the LAUNCHING BASH WRAPPER, not qemu — every "kill" killed the
   wrong process and stale qemu instances accumulated, all appending to
   the same -D logfile and racing for the monitor socket (phantom
   BERRs from pre-fix binaries).  Use `QPID=$!` from the setsid launch,
   or `pgrep -f "qemu-system-m68k -M quadra630" | grep -v bash`.
9. **PITFALL**: a bash `python3 - <<EOF` heredoc that edits a file under
   `hw/m68k/` leaves the shell cwd there; a following `ninja -C build`
   then fails ("chdir to 'build'") and any `build/qemu-...` launches the
   STALE binary.  Always `cd /workspace/src/qemu-mac2` before build/run.

## Build/test commands

    ninja -C build qemu-system-m68k
    build/qemu-system-m68k -M quadra630 \
        -bios /workspace/files/mac-roms/quadra630.rom \
        -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
        -snapshot -serial null -serial null -display none \
        -monitor unix:/tmp/q630.sock,server=on,wait=off
    # kill by saved PID only (NEVER pkill by name); screendump via monitor
