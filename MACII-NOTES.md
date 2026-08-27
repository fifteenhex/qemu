# Macintosh II / IIx / IIcx — journal

Goal: `-M macii` (68020), `-M maciix` (68030), `-M maciicx` (68030, 3
slots) in `hw/m68k/macii.c`, booting MacOS 7.5.3 from SCSI with a REAL
NuBus bus + a NuBus video card (these machines have NO onboard video).
ROM: `macIIx.rom` (256K, checksum 97221136, version $0178 — the shared
"Mac II FDHD / IIx / IIcx / SE/30" ROM; `-M macse30` in this tree
already boots it to the Finder).

## RESULT (2026-08-26/27)

- **maciix**: MacOS 7.5.3 boots to a fully interactive Finder on the
  Radius 24Xp NuBus card in slot 9 (windows, menus, ADB mouse AND
  keyboard verified — the "improper shutdown" dialog was dismissed with
  a sent Return).  Screenshot: `/tmp/macii-final-shots/maciix-finder.png`.
- **maciicx**: same, full Finder.  `/tmp/macii-final-shots/maciicx-finder.png`.
- **macii** (68020): ROM POST passes, MacOS 7.5.3 boots through the
  happy-mac, "Welcome to Macintosh", the extension parade and the
  Mac OS splash+progress bar to the **Finder menu bar with a live,
  ADB-driven cursor** — but the Finder's process never becomes
  runnable (blocker documented below).
  `/tmp/macii-final-shots/macii-menubar.png`.
- **macse30** regression-checked on the same build: still boots to the
  Finder, bit-exact source (no shared file was touched).
  `/tmp/macii-final-shots/macse30-regression.png`.

Boot command (IIx; iicx/ii identical but for -M):

    /tmp/macii-build/qemu-system-m68k -M maciix \
      -bios /workspace/files/mac-roms/macIIx.rom \
      -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
      -snapshot -serial null -serial null -display none -icount shift=7 \
      -device radius-24xp,slot=9,romfile=/workspace/files/radius-24xp-lane3.bus \
      -monitor unix:/tmp/macii.sock,server=on,wait=off

(`/workspace/files/radius-24xp-lane3.bus` ==
`/workspace/src/macgfxre/prec.clr.24xp.v2.bus`, byte-for-byte — the
v2.0 lane-3 bus image of the Radius PrecisionColor 24Xp declaration
ROM.)

## Design

`hw/m68k/macii.c` is a copy of `macse30.c` (same ROM, same chipset:
classic VIA1 ADB transceiver + RTC engine, discrete VIA2, GLUE IPL
combiner, NCR5380 + pseudo-DMA/handshake apertures, SWIM, ASC, ESCC,
A31 low-24 decode, the ADB boot-rendezvous crutch) with the SE/30's
pseudo-slot-$E onboard video replaced by:

- a real `mac-nubus-bridge`: standard slot space alias at 0xF9000000
  (priority -1, above the A31 catch-all at -2), **super-slot space at
  priority -3 i.e. BELOW the A31 catch-all** — MacOS dereferences
  24-bit-tagged master pointers (0xA082xxxx etc.) that must resolve
  through the low-24 decode, and no modelled card decodes super-slot
  space anyway.  Slot IRQ n pulls VIA2 PA(n-9) low and edges VIA2 CA1
  ("any slot"), the same scheme the SE/30's pseudo-slot VBL proved out
  against this ROM's slot dispatcher (`macii_nubus_irq()`).
- three machine types over one abstract base (`MacIIMachineClass`):
  `macii` = m68020 + `hmmu` flag, `maciix` = m68030 (both straps 0xAF,
  6 slots $9-$E), `maciicx` = m68030, straps 0xEF, 3 slots $9-$B.
  Machine-ID straps per MAME apple/macii.cpp: VIA1 PA bit 6 clear =
  II/IIx, set = IIcx/SE30 (SE/30 already boots with 0xEF; 0xAF is
  0xEF&~0x40); II vs IIx are told apart by the ROM via CPU type.
- NCR5380 IRQ moved to VIA2 **CB2** (real Mac II wiring, Linux
  macints.h IRQ_MAC_SCSI = IRQ_VIA2_3): CA1 is the live NuBus line
  here, and the slot pulses leave CA1 high so a 5380 level-assert on
  the same line could arrive edge-less.  (SE/30 keeps its CA1 wiring.)
- `default_ram_size` 4 MiB (8 MiB made the ROM's RAM alias-sizing
  probe at 0x4080397e classify the bank as bad → POST error →
  serial MicroBug; traced via in_asm diff vs a working macse30 run).

### The 68020 Mac II: Apple HMMU modelling

The 020 has no PMMU; on the IIx/IIcx this ROM programs the 030 PMMU
with an IS=8 24-bit map, on the II the HMMU provides it in hardware.
Modelled WITHOUT PB3 mode toggling (PB3 is the real switch, MAME
`hmmu_via2_out_b`): MacOS SwapMMUModes around every 32-bit QuickDraw op
(~7000/s observed) and toggling memory regions at that rate melts into
flatview rebuilds (boot took virtual HOURS).  Instead, everything is
static and mode-independent:

1. `macii.hmmu24` (0x800000-0xFFFFFF, always on): ROM mirrored at
   0x800000 (writes ignored — the POST alias-test magic write lands
   here), 24-bit NuBus slot windows 0x900000-0xEFFFFF forwarding to the
   BOTTOM 1MB of each slot's standard space (MAME
   hmmu_translate_addr_ii formula; empty-slot reads squash to 0 so the
   POST 32-bit-mode RAM walk at 0x4083f8e4 doesn't fault-loop), mac-io
   at 0xF00000-0xFFFFFF → 0x50F0xxxx byte-split.
2. **Slot 24-bit-region mirrors** (all three machines): MacOS' 020-path
   24→32-bit slot address formula is `0xF0000000 | slot<<24 | 24bit`,
   so ScrnBase $900460's 32-bit twin is 0xF9900460 — the card's first
   megabyte is aliased at standard-slot offset (slot<<20), as real
   24-bit-compatible cards decode.  (Without it: QuickDraw's 32-bit
   blit bus-errors → sad mac 0F/0001.  The 030 machines' PMMU maps the
   24-bit window to the bottom of slot space instead and never hits
   this.)
3. Tagged-pointer decode for prefixes without A31: `macii.tag24-2x/6x`
   (0x20000000/0x60000000, low-24 forwarders — observed 0x6000EFD0
   reads from RAM code fault-looping), `macii.tag24-4x` (0x40000000
   gaps below rom/rom-alias — observed 0x40091E68 bomb during
   extension loading).
4. `macii.tag24-ram-*`: **RAM aliased directly at 0x80000000 /
   0xA0000000 / 0xC0000000 / 0xE0000000** (the locked-tag prefixes).
   24-bit MacOS executes code out of locked handles; instruction fetch
   through the A31 *io* forwarder is MMIO-exec with no TB caching —
   observed as the system "running" at ~1000x slowdown (PC sampled
   inside 0x800Fxxxx, Ticks showed 2.5 virtual hours to reach the menu
   bar).  With the aliases, tagged code/data in RAM runs at native
   speed; low-24 targets beyond RAM still fall to the forwarders.
5. `macii_a31_ops` `.valid.max_access_size` raised 4→8 (`.impl` stays
   4): QuickDraw's bitfield blitters (bfextu/bfins on tagged bitmap
   pointers) issue single 8-byte accesses on the 020; the memory core
   rejected them (`txn fail size=8` at ROM 0x4081ade0) → sad mac
   0F/0001.  (macse30.c has the same latent limit but its PMMU strips
   tags before physical dispatch, so it was left untouched.)

### macii remaining blocker (documented, not solved)

After the splash completes, the menu bar is drawn, the ADB cursor
tracks (interrupt level fine — the jCrsrTask VBL runs _SwapMMUMode +
draw every tick), and key/mouse events ARE posted to the low-mem event
queue (verified via monitor: qHead/qTail advance on injected input) —
but the single Process Manager process (the Finder) sits with its
entry's +0x70 byte = 1, which the scheduler loop (RAM ~0xB9606:
tst.b (a4,0x6E); cmpa CurProc; cmpi.w #$DEAD,(a4,0x4A); tst.b
(a4,0x70) → skip) treats as permanently ineligible, so it idles
forever and nothing consumes the events.  Identical on: the RadiusWare
7.5.3 disk, the clean q630 7.5.3 disk, with extensions disabled
(shift held through boot), with -icount shift=4/7, without icount
(without icount the machine dies earlier — icount required as on the
other machines), and with the 5380 on CA1 or CB2.  No transaction
failures remain in a full -d mmu boot trace other than the Slot
Manager's normal empty-slot probes.  Next session: find what writes
process+0x70 (gdb watchpoint was started but is impractically slow
mid-boot; better: breakpoint the launch path once the write site is
known from a IIx comparison trace).

## Debug session log (how each blocker was found)

1. **8MB default RAM → POST death** (both 030 machines): serial
   MicroBug loop at 0x408032xx polling SCC.  in_asm trace diff vs
   macse30 diverged at 0x4080397e (RAM alias-sizing magic 0xA5B4C3D2
   at the 8MB mark).  Fix: 4MB default.
2. **IIcx/IIx then booted straight to the Finder** (first try after
   the RAM fix) — Slot Manager enumerated the Radius card, ran its
   PrimaryInit from the decl ROM, loaded its video driver.
3. **macii sad mac 0F/0001 #1**: `nubus_super_slot_read addr
   0xa082d72c` — tagged pointer into super-slot space → priority -3
   remap + the HMMU 24-bit map (first as PB3-toggled aliases — POST's
   32-bit RAM walk faulted on empty slot windows; then the always-on
   forwarder design above).
4. **sad mac 0F/0001 #2**: `txn fail size=8` — bitfield blit vs
   `.valid.max_access_size`.
5. **sad mac 0F/0001 #3**: read of 0xF9900460 (ScrnBase's 32-bit twin)
   → slot 24-bit-region mirrors (first attempt mapped them at
   0xF0900000 — wrong base arithmetic, caught by identical gdb
   register dumps across "fixed" runs).
6. **fault-retry loop at 0x6000EFD0** → tag24-2x/6x forwarders; then
   the extension-time bomb at 0x40091E68 → tag24-4x.
7. **virtual-hours boot** (menu clock frozen, Ticks racing) → PC
   sampling showed execution inside the A31 io region → tag24-ram
   aliases.
8. Where things stand: blocker above.

## Build

    mkdir -p /tmp/macii-build && cd /tmp/macii-build && \
      /workspace/src/qemu-macii/configure --target-list=m68k-softmmu \
        --disable-docs --disable-tools && ninja qemu-system-m68k
    # incremental: ninja -C /tmp/macii-build qemu-system-m68k

Files: `hw/m68k/macii.c` (new), `hw/m68k/Kconfig` (+MACII),
`hw/m68k/meson.build`.  No shared file touched (macse30/maciici etc.
bit-exact by construction).
