# Quadra/Centris 650 & 610 machines ("quadra650", "centris650", "quadra610", "centris610") — progress notes

Goal: add the Quadra 650, Centris 650, Quadra 610, Centris 610 as thin
variants of the existing q800 machine (same chipset per Linux
arch/m68k/mac/config.c: VIA_QUADRA, djMEMC+IOSB, ESP at 0x50F10000, ESCC,
SONIC, DAFB, ADB-II, SWIM 2), boot MacOS 7.5.3 from
`/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda` (with -snapshot).

ROMs (`/workspace/files/mac-roms/`):
- `quadra650.rom` (F1ACAD13, 1 MiB) = the Quadra 800/650/610 ROM (the one
  `-M q800` already boots) -> Quadra 650 and Quadra 610.
- `centris650.rom` (F1A6F343, 1 MiB) -> Centris 650 and Centris 610.

## Machine-ID straps

Mechanism from the quadra700 work: the ROM identifies the box from the
VIA1 port A machine-ID straps, bits PA1/PA2/PA4/PA6 = mask 0x56
(mac_via.c `cpuid` property on TYPE_MOS6522_Q800_VIA1; q800 default
0x12).

### Universal-table dump (both ROMs; parsed from the ROM images)

The F1ACAD13 layout differs slightly from the Q700 ROM (420DBFF3)
documented in Q700-NOTES.md: decoder-block offset list at ROM+0x3210,
universal-table entry offset list at ROM+0x3230 (each slot's entry =
slot address + 32-bit offset), entries 0x40 bytes apart.  Entry fields
(same as Q700): +0x00 DecoderInfoPtr (self-relative), +0x12 box, +0x13
decoder kind, +0x20/+0x24 strap mask/value longs (top byte = VIA1 PA).
The runner (0x2f74/0x2f94) probes decoder blocks in list order, then
matches entries on decoder kind + `(PA & mask) == value`.

Both quadra650.rom and centris650.rom contain IDENTICAL universal
tables.  The 650/610 entries live in the high-ROM patch area, in an
extended entry list at ROM+0xd2800 (how it is dispatched not fully
traced — the low-ROM list at 0x3230 only reaches the Q800 entry; deal
with it empirically).  Relevant entries (deckind 12 = Q800 decoder
probe at 0x31c2, deckind 13 = new decoder block 0xd3034, both probe
djMEMC-class hardware):

| entry     | box  | deckind | straps (PA & 0x56) | machine     |
|-----------|------|---------|--------------------|-------------|
| 0xd2da4   | 0x1b | 12      | 0x12               | Quadra 800  |
| 0xd2b64   | 0x18 | 13      | 0x46               | Centris 650 |
| 0xd2ba4   | 0x1e | 13      | 0x52               | Quadra 650  |
| 0xd2ca4   | 0x2e | 13      | 0x40               | Centris 610 |
| 0xd2ce4   | 0x2f | 13      | 0x44               | Quadra 610  |
| 0xd2be4   | 0x1d | 13      | 0x12               | ? (Q800-strapped, deckind 13) |
| 0xd2c24   | 0x2d | 13      | 0x56               | ? |

Machine attribution for the strap values confirmed against MAME
`src/mame/apple/macquadra800.cpp` (read_pa1/2/4/6 constants per model):
Q800=0x12, C610=0x40, C650=0x46, Q610=0x44, Q650=0x52.  MAME clocks:
C610 68LC040/20MHz, C650 68LC040/25, Q610 68040/25, Q650 68040/33 —
QEMU has no LC040, all variants keep m68040 (FPU present; harmless for
MacOS, reports as full 040).

Linux BI_MAC_MODEL Gestalt IDs (bootinfo-mac.h): C650=30, Q650=36,
C610=52, Q610=53 (Q800=35).

## Design

q800.c: new Q800MachineClass {via1_cpuid, rom_filename, mac_model,
nubus_slot_mask} + four sibling types with parent TYPE_Q800_MACHINE and
a tiny class_init fed from class_data.  q800 itself keeps exactly its
old values (cpuid 0x12, MacROM.bin, MAC_MODEL_Q800, slots 9/c/d/e); the
init function reads the class fields instead of constants.  pins-a is
left at the q800 default (sentinel) for all — the F1ACAD13 ROM already
boots on q800 without it.  610s get NuBus slots 9/e only (single-slot
adapter machines); 650s keep 9/c/d/e (3 slots like Q800... actually
C650/Q650 have 3 slots c/d/e - same mask as Q800).

## Status log — COMPLETE: all four variants boot MacOS 7.5.3 to the Finder

- [x] ROM table analysis + strap values (above)
- [x] implement variants in q800.c (Q800MachineClass + 4 sibling types)
- [x] incremental build (`ninja -C build qemu-system-m68k`)
- [x] q800 regression: boots MacOS 7.5.3 to the Finder exactly as
      before (shutdown-warning dialog at ~110 s, Return dismisses it,
      Finder with mounted "OpenRetroSCSI 7.5" volume; screendump
      verified)
- [x] quadra650 boots to the Finder (~110 s incl. dialogs, same as q800)
- [x] centris650 boots to the Finder
- [x] quadra610 boots to the Finder
- [x] centris610 boots to the Finder
- [x] identification verified from the guest: low-memory global BoxFlag
      (0x0CB3, = productKind of the matched universal-table entry) read
      via monitor `xp /1b 0xcb3` on the booted system:
      quadra650 -> 0x1e, centris650 -> 0x18, quadra610 -> 0x2f,
      centris610 -> 0x2e — exactly the intended boxes.

## Findings

1. Both ROMs (F1ACAD13 and F1A6F343) contain byte-identical universal
   tables; the same strap values work in both.  The two images differ
   elsewhere (resources/drivers), so keep using the matching ROM per
   family anyway.
2. The only per-model hardware knob needed for these ROMs to identify
   and boot is the VIA1 port A strap value (`cpuid` qdev property on
   TYPE_MOS6522_Q800_VIA1).  No pins-a override, no new devices, no
   address changes: the ROMs are happy with the exact q800 wiring
   (djMEMC, IOSB, ESP@0x50F10000, VIA2-IFR pseudo-DMA, DAFB).
3. Surprise: with the stock q800 straps 0x12, this ROM matches box
   0x1d (the deckind-13 entry strapped 0x12), NOT the "Quadra 800"
   box 0x1b (deckind 12) — BoxFlag reads 0x1d on `-M q800` too, and
   that is pre-existing behaviour (the explicit cpuid=0x12 my change
   sets equals the property default; boot sequence unchanged).  I.e.
   QEMU's q800 hardware passes the ROM's decoder-kind-13 probe, and
   all five machines here identify through deckind-13 entries.  Box
   0x1d is presumably the later/djMEMC-revision Quadra 800 table entry;
   MacOS shows no visible difference.
4. machine_class_base_init() re-allocates mc->compat_props for every
   non-abstract machine class, so the sibling class_init must re-add
   hw_compat_q800 (Apple SCSI quirk props) or the variants would
   silently lose them.
5. QEMU has no 68LC040 CPU model, so the Centrides keep m68040 with
   FPU (real C610/C650 base configs were LC040; MacOS just sees a full
   040).  Clock differences (20/25/33 MHz) are not modelled — TCG has
   no fixed clock anyway.
6. NuBus: 650s get the Q800 mask (pseudo-slot 9 + 0xc/0xd/0xe); the
   single-slot 610s get pseudo-slot 9 + 0xe only.

## Implementation (hw/m68k/q800.c only; no meson/Kconfig changes)

- New Q800MachineClass {via1_cpuid, rom_filename, mac_model,
  nubus_slot_mask}; q800_machine_init() reads these instead of the
  Q800 constants (BI_MAC_MODEL for -kernel Linux boots also comes from
  the class, so Linux would see the right model too — untested here).
- q800 keeps its exact old values (0x12 / MacROM.bin / 35 / 9,c,d,e).
- Four sibling types with .parent = TYPE_Q800_MACHINE and a common
  class_init fed by .class_data:
  quadra650 (0x52, quadra650.rom, 36), centris650 (0x46,
  centris650.rom, 30), quadra610 (0x44, quadra650.rom, 53),
  centris610 (0x40, centris650.rom, 52).

## Build/test commands

    ninja -C build qemu-system-m68k
    build/qemu-system-m68k -M quadra650 \
        -bios /workspace/files/mac-roms/quadra650.rom \
        -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
        -snapshot -serial null -serial null \
        -display none -monitor unix:/tmp/mon.sock,server=on,wait=off
    # (same for centris650 / quadra610 / centris610 with the right -bios;
    #  quadra610 uses quadra650.rom, centris610 uses centris650.rom)
    # screendump via monitor socket -> /tmp/*.ppm; kill by saved PID only
