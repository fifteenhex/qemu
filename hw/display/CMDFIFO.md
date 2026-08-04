# Voodoo3 command FIFO (cmdFifo) — design + spec

Goal: replace per-command PIO register writes with a DMA command FIFO (a large
ring the card pulls from), to cut CPU/PCI submission cost on real hardware and
the per-command vmexit cost under QEMU.  Constraint: on one target the card
**cannot DMA from system RAM**, so the ring lives in **VRAM** (card-accessible,
and in the model RAM-backed so guest writes to it cost no vmexit).

Authoritative source: glide3x `h3` (Avenger = Voodoo3) — `incsrc/h3gdefs.h`
(packets), `incsrc/h3regs.h` (`CmdFifo` struct), `glide3/src/gpci.c`
(`cRegs = BAR0 + 0x80000`), `glide3/src/fxcmd.h` (`GR_BUMP_N_GRIND`).

## Registers  (SstCRegs block at aperture 0x80000; cmdFifo0 at +0x20)
    0x80020 baseAddrL   ring base >> 12 (4KB pages), byte base = baseAddrL<<12
    0x80024 baseSize    (size_pages-1) | EN(bit8) | AGP(bit9) | DISABLE_HOLES(bit10)
    0x80028 bump        write: advance by this many BYTES (commit)
    0x8002c readPtrL    card read pointer (byte address)   [+0x80030 readPtrH]
    0x80034 aMin        valid-range low     0x8003c aMax  valid-range high
    0x80044 depth       pending bytes (guest bumps up, card drains)
    0x80048 holeCount

## Submit protocol (GR_BUMP_N_GRIND)
Guest writes packets into the ring at its write pointer, then:
    bump = (writePtr - lastBump)   // bytes appended since last commit
Card: depth += bump; execute packets from readPtr for that many bytes,
advancing readPtr (wrapping at base+size via a PKT0 JMP the guest writes, or
when DISABLE_HOLES the guest pads/jumps to base).  depth==0 => engine idle.
Model is synchronous, so on the `bump` write we parse+execute the whole
appended range immediately and set readPtr = writePtr, depth = 0.

## Packet format (word0 low 3 bits = type)
PKT0 control: func[5:3] NOP0 JSR1 RET2 JMP_LOCAL3 JMP_AGP4; addr[28:6] (<<? see
     h3gdefs).  Only NOP + JMP_LOCAL needed (ring wrap to base).
PKT1 register burst: REGBASE[12:3] (reg byte-offset>>2), 2D(bit14),
     INC(bit15, else all words to same reg), NWORDS[31:16]; then NWORDS data
     words -> execute as writes to 3D (or 2D if bit14) reg REGBASE(+i if INC).
PKT2 masked reg write (mask[31:3]).            PKT4 masked reg write (2D).
PKT3 native triangle/vertex packet.            PKT5 LFB/texture burst.

## Plan (staged, each validated)
1. MODEL: cmdFifo0 registers + PKT0/PKT1 parser reading the VRAM ring on `bump`,
   executing via the existing 3D/2D write handlers.  (PKT2..5 later.)
2. VALIDATION: a cmdFifo probe (drive a tiny PKT1 stream, read back) -> model
   reference; diff on real hardware to confirm the packet format/offsets.
3. DRIVER: carve a VRAM ring, program baseAddr/baseSize/EN, expose ring offset
   + bump to userspace (the ring is in the fbdev VRAM smoltdfx already maps).
4. smoltdfx: emit register writes as PKT1 bursts into the ring + bump, instead
   of PIO; keep a PIO fallback.  Then smolminigl gets it for free.
5. Re-evaluate: with per-command vmexits gone, the realistic timing model can be
   re-enabled cheaply.
