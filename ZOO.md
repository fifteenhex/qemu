# The m68k QEMU Zoo — machine & OS index

A single navigable catalog of every machine on the `amiga` integration branch
and how far each one boots. This branch extends upstream QEMU's m68k target
with a large family of Motorola-680x0 machines and a shared 68030/68851 PMMU
core (see **`PMMU-DEEPDIVE.md`** / **`M68851-NOTES.md`**).

Per-machine detail — hardware contracts, register maps, RE journals, exact
repro commands — lives in the individual `*-NOTES.md` / `*-HOWTO.md` files
referenced in each row. This file is only the map.

Status legend: **✅ shell/desktop** reached · **◐ deep boot** (kernel/OS runs,
walled short of a shell) · **○ firmware/early** · **⧗ in progress** (active RE).

Last updated: 2026-09-02.

---

## Macintosh (68020/030/040) — MacOS 7.5.3 to the Finder

Shared Mac core: NuBus/RBV/V8/GLUE/DAFB video, VIA/Egret/Cuda ADB, SWIM,
NCR5380/53C96 SCSI, SONIC. Most boot MacOS 7.5.3 from a shared SCSI image.

| Machine (`-M`) | CPU | Milestone | Notes |
|---|---|---|---|
| `maciix` | 68020 | ✅ interactive Finder | `MACII-NOTES.md` |
| `maciicx` / `macii` | 68020 | ◐ boots through MacOS 7.5.3 | `MACII-NOTES.md` |
| `maciici` | 68030 | ✅ interactive Finder | `CLASSIC030-NOTES.md` |
| `macclassicii` / `macse30` | 68030 | ◐ classic-030 boot path | `CLASSIC030-NOTES.md` |
| `maciifx` | 68030 | ✅ interactive Finder | `IIFX-NOTES.md` |
| `maciivx` / `maciivi` / `performa600` | 68030 | ◐ into MacOS 7.5.3 | `IIVX-NOTES.md` |
| `maciisi` | 68030 | ○ "insert disk" floppy icon | `MACIISI-NOTES.md` |
| `maclc550` / `colorclassicii` | 68030 | ◐ deep into MacOS 7.5.3 | `maclc550-NOTES.md`, `CCLASSIC-NOTES.md` |
| `lc475` / `quadra605` | 68040 | ✅ interactive Finder | `LC475-NOTES.md` |
| `quadra630` | 68040 (+IDE) | ✅ interactive Finder | `Q630-NOTES.md` |
| `quadra700` | 68040 | ✅ interactive Finder | `Q700-NOTES.md` |
| `quadra650`/`centris650`/`quadra610`/`centris610` | 68040 | ✅ interactive Finder | `Q650FAM-NOTES.md` |
| `q800` | 68040 | ✅ interactive Finder (baseline) | `Q650FAM-NOTES.md` |
| `quadra950` / `quadra900` | 68040 | ○ boot scan / flashing-"?" | `Q950-NOTES.md` |
| `mac128k` / `mac512k` / `macplus` | 68000 | ○ early System (128K-class) | `MAC128K-NOTES.md` |

## Amiga (68000/020/030/040) — Kickstart / AmigaOS

Full ECS/AGA chipset (Agnus/Denise/Paula, CIA 8520, Gayle, Zorro II/III,
Akiko), booted from a 512K Kickstart 3.1 ROM via `-bios`.

| Machine (`-M`) | CPU | Milestone | Notes |
|---|---|---|---|
| `a500` / `a500plus` / `a600` / `a1000` / `a2000` | 68000 | ◐ Kickstart 3.1 | `AMIGA-NOTES.md` |
| `a1200` | 68020 (AGA) | ◐ Kickstart 3.1 | `AMIGA-NOTES.md` |
| `a3000` / `a4000` / `a4000t` | 68030/040 | ◐ Kickstart 3.1 | `AMIGA-NOTES.md` |
| `cd32` | 68020 (AGA+Akiko) | ◐ Kickstart / CD32 | `AMIGA-NOTES.md` |

## Sun (Motorola-680x0 Sun workstations)

| Machine (`-M`) | CPU | Milestone | Notes |
|---|---|---|---|
| `sun3-60` ("Ferrari") | 68020 (Sun-3 MMU) | ✅ NetBSD/sun3 RAMDISK + Linux `sun3` shell | `SUN3-NOTES.md`, `hw/m68k/sun3-linux-NOTES.md` |
| `sun3x` (Sun 3/80) | 68030 (68851) | ✅ Linux shell · ⧗ **genuine 3/80 PROM boots real SunOS 4.1.1: root mounts off the UFS miniroot (`root on sd6a`), the kernel recognizes the Sun-3/80, prints its ethernet address, and enters device autoconfiguration** (`sm0 at obio`, `st0-3`/`sr0` SCSI probes) — banked at the interrupt-driven `sm`-driver boundary (kernel probes each SCSI target; absent-target completion needs the Am9516-UDC interrupt path the polled model doesn't implement; wall M5) | `hw/m68k/sun3x-NOTES.md`, `hw/m68k/sun3x-SUNOS-{HOWTO,NOTES}.md` |

The SunOS bring-up is a QEMU first: reverse-engineered `si` NCR5380 / Am9516
DVMA / IDPROM / reg5 / SCSI-state-walk unlocks let the real PROM load the SunOS
4.1.1 kernel via 79 READ(6) transfers, **mount root off the UFS miniroot, and
run the genuine kernel to memory init**. All SunOS behavior is `-bios`-gated;
stock Linux `sun3x` is byte-for-byte unchanged.

## Apollo (Domain workstation)

| Machine (`-M`) | CPU | Milestone | Notes |
|---|---|---|---|
| `apollo-dn3000` | 68020 (68851) | ✅ Linux `-kernel` (fb console) · ⧗ **Domain/OS AEGIS SR10.4 reaches its `kernel(8), revision 10.4` banner**, walled at a post-banner page-build demand-fault storm | `APOLLO-NOTES.md`, `APOLLO-DOMAINOS-{HOWTO,NOTES}.md`, `hw/m68k/apollo-display-NOTES.md`, `APOLLO-DN3000-RESEARCH.md` |

Includes an OMTI-8621 Winchester, Archive SC-499 cartridge tape + Am9517A DMA,
and a linear framebuffer — enough to run the SR10.4 tape install (`invol`)
end-to-end. Uses the 1 KiB `TARGET_PAGE_BITS` from the PMMU work.

## Unix / VME / embedded (Linux/m68k & firmware)

| Machine (`-M`) | CPU | Milestone | Notes |
|---|---|---|---|
| `mvme147` | 68030 | ○ 147Bug firmware / Linux base machine | (branch base) |
| `mvme167` | 68040 | ✅ Linux/m68k serial shell | `hw/m68k/mvme16x-NOTES.md` |
| `avme352` | 68040 | ✅ Linux shell (`ttyAVME` console) | `hw/m68k/mvme16x-NOTES.md` |
| `bvme4000` (BVME6000-class) | 68040 | ✅ Linux/m68k serial shell | `hw/m68k/bvme6000-NOTES.md` |
| `hp9000-340` (hp300) | 68030 | ✅ Linux/m68k serial shell | `hw/m68k/hp300-NOTES.md` |
| `q40` | 68040 | ✅ Linux/m68k serial shell | `hw/m68k/q40-NOTES.md` |
| `e17` (ELTEC Eurocom E17) | 68030 | ⧗ firmware bring-up | `E17-NOTES.md` |
| `sord-m68mx` | 68000 | ⧗ boot ROM up; CP/M-68K off floppy (µPD765 FDC) | `SORD-M68MX-NOTES.md` |

## DragonBall PDAs (MC68EZ328) — PalmOS

| Machine (`-M`) | Milestone | Notes |
|---|---|---|
| `palmv` / `palmvx` / `palmiiix` / `palmm100` / `palmm500` / `palmm515` | ◐ PalmOS to the Launcher (Memo Pad opens) | `PALM-NOTES.md` |
| `clie-sj33` / `clie-s300` / `clie-t600c` (Sony CLIÉ) | ◐ past DragonBall SoC init to "Setup 2 of 4" | `CLIE-POC-NOTES.md`, `CLIE-RESEARCH.md`, `UBOOT-CLIE-SJ33.md` |

## Other

| Machine (`-M`) | Milestone | Notes |
|---|---|---|
| `atarist` / `atariste` (1040 STF/E) | ◐ TOS 1.04 runs; walled before the GEM desktop | `ATARIST-NOTES.md` |
| `megadrive` (Sega) | ◐ Sonic attract demo plays with correct graphics | `MEGADRIVE-NOTES.md` |
| `an5206` / `mcf5208evb` / `virt` / `next-cube` | upstream QEMU baselines (ColdFire / virt / NeXT) | — |

---

## Shared CPU-core work (the reason the vendor OSes run)

The 68030 on-chip PMMU + MC68851 emulation was audited and fixed to run real
vendor operating systems, not just Linux. Highlights (full detail in
`PMMU-DEEPDIVE.md`):

- **PTEST level / A(n) descriptor-address writeback** — the root cause of
  sun3x non-determinism.
- **PMOVEFD TLB flush** (MacOS SwapMMUMode), **SRE-tagged ATC**, interrupt
  SR-mask *set*-not-OR.
- **`TARGET_PAGE_BITS` 12 → 10 (1 KiB)** so the MC68851's sub-4K pages cache
  correctly — required for Apollo AEGIS's 1 KiB page frames.

Regression set (must stay green): Mac Finders, hp300 / sun3x / bvme / q40
Linux shells, and the sun3x genuine-PROM + Apollo-AEGIS banner boots.

## Conventions (for contributors / agents)

- Build **only** to tmpfs, out-of-tree (`/tmp/<x>-build`); `/workspace` is
  disk-tight. Configure `--target-list=m68k-softmmu`.
- Determinism: `-icount shift=6`/`7`. gdb: `set endian big`,
  `set architecture m68k:68030`.
- Firmware/ROM blobs and disk images live outside the tree (`/workspace/files/…`,
  gitignored); boot transcripts are gitignored (`*.log`, `*-transcript.*`) and
  must never be committed.
</content>
</invoke>
