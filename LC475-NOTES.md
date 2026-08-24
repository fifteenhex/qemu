# Macintosh LC 475 / Quadra 605 machine ("lc475") — progress notes

Goal: boot the Quadra 605 ROM (`/workspace/files/mac-roms/quadra605.rom`,
1 MiB, CRC FF7439EE) and ideally MacOS 7.5.3 from
`/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda`.

## Hardware model (from Linux MAC_MODEL_Q605/P475, U-Boot oldmac.c, IIsi work)

- CPU: 68LC040 -> QEMU `m68040` (FPU present in QEMU; ROM will just detect it,
  harmless — real 475s are often upgraded to full 040s anyway).
- VIA1 site 0x50F00000: the LC475 has a **Cuda** MCU on the VIA1 shift
  register with the TIP(PB5)/TACK(PB4)/TREQ(PB3) handshake.  QEMU already has
  a complete Cuda + mos6522 model in `hw/misc/macio/cuda.c` (same protocol
  family that MacOS PPC drivers use; the 68k Quadra ROM driver is the same
  protocol).  Reused directly: its MMIO region is VIA-spaced (>>9), 0x2000.
  - cuda `timebase-frequency` must be nonzero (used as divisor in the timer
    counter override); counter rate comes out ~783.5 kHz independent of the
    value, close to the real 783.36 kHz VIA clock.
  - 60.15 Hz tick pulsed into CA1, 1 Hz into CA2 (as on maciisi/Egret).
- VIA2 0x50F02000: Quadra-class (emulated by PrimeTime on real HW) —
  `TYPE_MOS6522_Q800_VIA2` from mac_via.c.
- SCSI: 53C96 ESP at 0x50F10000, PDMA at 0x50F10100, DRQ -> VIA2 CA2
  (SCSI_DATA), IRQ -> VIA2 CB2 — same wiring as q800.
- SCC: ESCC at 0x50F0C020 (MAC_SCC_QUADRA layout), IRQ level 4 via GLUE.
- Floppy: SWIM(2) at 0x50F1E000.
- Sound: EASC at 0x50F14000.
- No SONIC ethernet, no djmemc/iosb (LC475 uses MEMCjr + PrimeTime; any
  regs the ROM needs will show up in the BERR trace and get stubbed).
- IRQ glue: reuse q800-glue (TYPE_GLUE), classic mode: VIA1=1, VIA2=2, SCC=4.
- Video: **Valkyrie/CSC-class**, non-identity: MacOS logical 0x51901000 ->
  physical ~0xf9000000 (U-Boot resolved via ROM page tables).  68040 MMU is
  fully emulated so the ROM's own page tables handle the logical mapping;
  we provide physical VRAM (1 MiB) at 0xf9000000 + a display device.
  CLUT register location to be discovered from the BERR/unimp trace.
- ROM at 0x40800000 (1 MiB), alias at 0x40000000; reset SP/PC poked to 0/4.
- Unmapped I/O bus-errors (MEMTX_DECODE_ERROR) with logging, as on maciisi —
  the ROM's machine identification relies on BERR probes.

## Status log — COMPLETE: MacOS 7.5.3 boots to the Finder

- [x] worktree builds (m68k-softmmu only), `build/qemu-system-m68k` OK
- [x] lc475.c skeleton registered, ROM starts
- [x] machine identification passes (see findings below)
- [x] video init + gray desktop
- [x] Cuda exchange + full ADB enumeration (keyboard/mouse found, polled)
- [x] SCSI boot from HD0-OpenRetroSCSI-7.5.3.hda via ESP pseudo-DMA
- [x] **MacOS 7.5.3 boots to the Finder** (1152x870 8bpp, mounted volume,
      menu bar, control strip; Return key over ADB dismisses the
      "not shut down properly" dialog -> desktop)

Boot command:

    build/qemu-system-m68k -M lc475 \
        -bios /workspace/files/mac-roms/quadra605.rom \
        -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
        -display none -monitor unix:/tmp/lc475mon.sock,server=on,wait=off

    (~3 minutes to the Finder; screendump via monitor.  NOTE: the machine
    auto-gets a default scsi-cd at ID 2; harmless.  MacOS mounts the disk
    read-write.)

## Key findings (Quadra 605 ROM, CRC FF7439EE)

1. **Machine ID register 0x5FFFFFFC**: read-only; long reads must return
   0xA55A2BAD; the ROM ALSO does `cmpiw #$2bad» 0x5ffffffc` (word read at
   the SAME address must return the LOW word 0x2bad, not 0xa55a).
   Identification table at ROM 0x408a79bc: entries with idword 0x2bad are
   the djMEMC-class machines, disambiguated by VIA1 port A straps.
2. **VIA1 port A straps**: probe helper 0x408047cc forces DDRA input and
   compares (PA & 0x56) against entry+36 byte.  0x42=LC475 (box 0x34),
   0x56/0x16=P475F (box 0x35), 0x50=Quadra 605 (box 0x39).  We use
   pins_a=0xF9 -> 0x50 (Q605).  Box byte at entry+16 `dc00XX0d`; gestalt
   = box + 37; low byte 0x0d = decoder kind 13.
3. **MEMCjr at 0x50F0E000**: reg +0x30/+0x34 = RAM timing (written per
   speed straps PA&0x14), regs +0x04..+0x28 = ten bank registers
   (0x100+0x10*n written, then rewritten 0x108|base after sizing);
   +0x00 = control (0x20), +0x2c = mask (0xffffffff -> 0x8).
   RAM-backed regbank stub suffices.
4. **PrimeTime at 0x50F18000**: regs +0x000 (rmw long, bit3 cleared),
   +0x200 (byte), +0x600 (word, 0xd5 written).  RAM-backed stub suffices.
5. **GLUE auxmode**: LC475 has no VIA1 PB6 auxmode output (VIA1 is inside
   the Cuda) — the GLUE must be PINNED to classic mapping (VIA1=1 VIA2=2
   SCC=4) or the EASC IRQ arrives at level 5 and storms.  Added
   `auxmode-default` property to q800-glue.
6. **Valkyrie video at 0xF9800000** (regbank observations):
   +0x00 ctrl(8), +0x04 (0x80 pulse = reset?), +0x08 (0x80/0x90),
   +0x0c (0x706/0x506), +0x10 mode (0x30/0x230/0x210), +0x18, +0x20 depth
   (3/7/3/2/0 written during probe), +0x100 (0xff2), +0x104 int enable
   (4), **+0x108 status: bit2 = VBL, polled after clearing via +0x10c**
   (60Hz timer sets it, +0x10c write clears), +0x118 (0x390), +0x120
   (0x248/0xa48 clock/sense drive), **+0x122 word read = monitor sense**
   (0 -> 21" 1152x870; forcing 6 selects 13" 640x480 family CRTC values),
   +0x124..+0x168 CRTC timing, +0x200 CLUT addr, +0x213 CLUT data,
   +0x303 (0x14).
   VRAM at physical 0xF9000000, **aperture wraps/aliases** (ROM sizes
   VRAM by aliasing; BERR at 0xf91ffffc is fatal) — 1MiB mirrored across
   8MiB.  Framebuffer base used by ROM = 0xF9001000 (ScrnBase logical
   0x51901000, MMU-mapped), gray desktop drawn rowbytes 0x240-ish.
7. **Cuda protocol is EGRET-STYLE, not the PPC cuda.c packet protocol.**
   ROM transport at 0x408b2d20 (send start) / 0x408b2dea (SR-int
   dispatcher, `btst #3,(via)` = TREQ state to continuation):
   - Host preloads SR (ACR=0x1c shift-out), then ORB &= ~0x30 (TIP+TACK
     both asserted), waits for **Cuda to assert TREQ (PB3 low)** as the
     session acknowledge.  QEMU's cuda.c never does this.
   - Multi-byte send: SR write + `eor ORB,#0x10/#0x30` per byte, each
     byte acked by SR interrupt.
   - Turnaround: ACR -> 0x0c (shift-in) WHILE session held, dummy SR
     read, TACK raise; command must be processed at ACR change (like
     maciisi's egret_acr_changed).  cuda.c only processes on TIP raise
     -> deadlock (this is the fundamental mismatch).
   - Receive: each SR int delivers a byte; host acks with `eor ORB,#0x30`
     (exactly one of TIP/TACK low mid-receive).  **TREQ low during valid
     response bytes; TREQ high at an SR int = end of response** (polarity
     INVERTED vs the IIsi Egret driver where PB3 high = more bytes).
   - Session release = both TIP and TACK high (0x30 set), NOT TIP alone
     (send-phase toggles raise TIP transiently!).
   - First exchange: single byte 0x00 then immediate turnaround (Egret
     "SendReset sync"); IIsi Egret answers 2 status bytes (01 01).
   - The ROM ALSO speaks the classic 343-0042 bit-bang RTC protocol on
     PB0-2 (Cuda emulates the old RTC chip) — port maciisi's RTC engine.
   => plan: drop TYPE_CUDA, implement maciisi-style engine in lc475.c
   with inverted TREQ + Cuda packet framing (type byte). cuda.c reverted
   to upstream state.
8. ROM error/death handler = SCC test monitor: tight poll of SCC chA RR0
   bit0 at 0x408ba0ea/0x408b9928 loop (PC there = something fatal
   happened earlier).
9. Boot flow after video: ADB init waits at 0x4080a870 for driver flag
   bit5 of transport globals+0x15d to clear (cleared when full ADB
   enumeration finishes — sync, Talk R3 of all 16 addresses, Listen R3
   relocations, then continuous Talk R0 polls of keyboard/mouse).

## Later findings (second half of the bring-up)

10. **Cuda transport, final semantics** (all confirmed against the ROM
    driver at 0x408b2c60-0x408b2fff and now implemented in lc475.c):
    - Commands are RAW ADB bytes (Egret framing, NO Cuda type byte):
      the enumeration sends 1-byte Talks (addr<<4|reg) directly.
      [0x00] alone = SendReset sync; answer = NO RESPONSE.
    - /TREQ stays HIGH during host sends.  /TREQ LOW at a send-byte
      interrupt = collision signal (driver flag bit7, turnaround to
      receive a Cuda-initiated packet, original command re-queued).
    - Turnaround cadence: ACR->input while both TIP/TACK low, dummy SR
      read, then `eor ORB,#0x30` RELEASES BOTH lines once before TIP is
      re-asserted — a release with a staged response must keep the
      session open (schedule the next SR int, don't reset).
    - Response bytes: fed on each TIP/TACK toggle, /TREQ HIGH while
      valid; /TREQ LOW at the first response interrupt = discard flag
      (used for "no response": two junk bytes with TREQ low); /TREQ LOW
      at a later interrupt = end of response (count kept).
    - Session close = both TIP and TACK high with nothing staged.
    - TREQ input state is clobbered by ROM probes that set DDRB=0xFF
      (VIA fingerprint, RTC bit-bang): restore idle-high whenever no
      exchange is active AND both handshake lines are released.
    - RTC/PRAM = 343-0042 bit-bang on PB0-2, engine ported from maciisi.
11. **SETUPTIMEK**: same TCG problem as q800 — TimeDBRA (lowmem 0xD00)
    calibrates to 0 -> divide-by-zero SysError in the Time Manager
    conversion `muluw #$0a60 / divuw 0xd00` at ROM 0x40843a80.  Ported
    the q800 hack: detect T2=0x30C + IER=0x20 end-of-calibration and
    stuff 0x2a00*3 / 0x079d*3 into 0xD00/0xD02.
12. **VRAM size probe**: reads past the end must WRAP (alias), never
    BERR (1 MiB mirrored across the 8 MiB window below 0xf9800000).
13. **The big one — boot-scan ddType vs PRAM**: the ROM boot scan
    (0x40807224) reads block 0, requires 'ER', then matches each DDM
    driver entry's ddType against the LOW BYTE OF _GetOSDefault
    (XPRAM 0xF8-0xFB; 1 = MacOS, 2 = A/UX).  Seeding PRAM "valid"
    ('NuMc') but zeroed makes the scan want ddType 0 and reject every
    disk (ddType is 1 on real disks) -> infinite lba-0 re-reads at
    ~1500/s.  FIX: leave PRAM invalid; the ROM rebuilds it with proper
    defaults (OSDefault=1) and the scan then accepts the disk, loads
    the driver at lba 64 and boots.  (maciisi's NuMc seeding must NOT
    be copied here.)
14. MacOS runs in **24-bit addressing** out of the rebuilt PRAM;
    master pointers are tag-bit-laden (0x80xxxxxx) — anything walking
    guest heap structures must mask with 0x00FFFFFF.
15. **Display**: instead of reverse-engineering the Valkyrie mode regs,
    the framebuffer device reads the guest's own idea of the screen
    every frame: MainDevice GDHandle (lowmem 0x8A4) -> GDevice+0x16 ->
    PixMap {baseAddr, rowBytes, bounds, pixelSize@+32}, with 24-bit
    pointer masking and sanity checks; falls back to 640x480x8 at VRAM
    +0x1000 until QuickDraw publishes real values.  MacOS chooses
    1152x870x8bpp (rowbytes 0x480, base VRAM+0x80).  1/2/4/8/16bpp
    draw paths implemented; CLUT captured from valkyrie +0x200 (addr) /
    +0x213 (R,G,B data triplets).
16. Practical: kill QEMU by SAVED PID only (a sibling process pkills
    qemu-system-m68k by name — run under a copied binary name if it
    recurs); the harness kills the process group of timed-out commands
    (use setsid); cap ALL log paths (poll loops produce GB/minute:
    esp_pdma traces hit 2 GB in 30 s).

## Remaining rough edges (for future sessions)

- Monitor sense forced to 6 (13") at valkyrie +0x122, but the rebuilt
  PRAM makes MacOS pick 1152x870 anyway; the mode-vs-sense interplay
  is not fully understood (CRTC families: 0x726-group for sense 6,
  0x648-group for sense 0).
- ASC sound untested; SWIM stubbed (no floppy boot).
- The default scsi-cd at ID 2 gets NOT-READY-polled each scan round
  (harmless; set mc->no_cdrom if undesired).
- valkyrie/memcjr/primetime remain logging regbank stubs (sufficient
  for ROM + MacOS 7.5.3).
- q800-glue gained `auxmode-default` property (LC475 pins classic
  mapping; q800 behavior unchanged, default 0).

## Build/test commands

    ninja -C build qemu-system-m68k
    build/qemu-system-m68k -M lc475 -bios /workspace/files/mac-roms/quadra605.rom \
        -display none -monitor unix:/tmp/lc475mon.sock,server=on,wait=off \
        -d unimp,guest_errors -D /tmp/lc475.log
    # screendump via monitor: screendump /tmp/lc475.ppm
