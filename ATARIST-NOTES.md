# Atari 1040STF QEMU machine — journal

Goal: boot TOS 1.04 UK to the GEM desktop on a new `atarist` machine,
with a working mouse; stretch goal an Automation compilation floppy.

Working tree: `/workspace/src/qemu-amiga` (branch `amiga`).  Build dir:
`build/`.  Session-unique names use the `atarist-` prefix; QMP socket
`/tmp/qmp-atarist-test.sock`, screenshots `/tmp/atarist-*.png`.

## Assets (`/workspace/src/qemu-amiga/_atarist_assets/`)

- `tos104uk.rom` — TOS 1.04 UK 192KB, maps at 0xFC0000.  Reset vector
  pair at offset 0: SP dword `602E0104` (garbage — `bra.s` + version,
  TOS sets its own stack), PC dword `00FC0030`.
- `AUT_095.MSA` — Automation menu 95 in MSA format; `autom100.zip`
  holds a raw 720K .ST alternative.

## RE notes from tos104uk.rom (m68k-linux-gnu-objdump -b binary -m
m68k:68000 --adjust-vma=0xfc0000)

Reset flow at fc0030:
- `move #$2700,SR; reset` — the RESET instruction fires first thing,
  so the board's reset-out wiring runs on every boot.
- `cmpi.l #$FA52235F,$FA0000` — diagnostic cartridge probe with **no
  bus error handler and no valid stack**: the cartridge window
  0xFA0000..0xFBFFFF must never bus-error.  We back it with a region
  that reads as all-ones (floating bus, no cartridge).
- memvalid check (fc066a): $420==752019F3, $43A==237698AA,
  $51A==5555AAAA → warm boot: restore memcntlr from $424 to ff8001 and
  skip sizing.
- 50Hz setup (fc0dc2, runs **before RAM sizing**, header byte fc001d
  bit 0 = PAL): programs MFP **Timer B in event-count mode**
  (TBCR=8, TBDR=240) and polls TBDR down to 1, then spins until TBDR
  stops changing for 616 reads — i.e. waits for vertical blank, since
  Timer B's event input is the Shifter DE line (one event per visible
  scanline).  Without an event-mode model the boot hangs right here.
  Our model computes the count on read from the video device's frame
  phase (200 visible lines per frame); no per-scanline timer needed.
- Cold RAM sizing (fc00e0): writes $0A (2MB/2MB) to ff8001, fills
  words 8..0x1FE in both bank windows (0 and 0x200000) with a
  0,FA54,F4A8,... pattern, then for each bank checks whether the
  pattern *reappears* at +0x208 (bank is 128K: DRAM drops column bit
  A9), at +0x408 (bank is 512K: drops A10), or holds distinct data at
  both and reads back at +8 (2MB).  A bank that fails all three
  (e.g. reads back zeroes) is absent.  So the MMU model must fold
  addresses column-first when the configured bank size exceeds the
  installed size: word = (a>>1) & (cols-1) | ((a >> (1+cfg_cols)) &
  (rows-1)) << cols.  Absent banks must read as zero *without* bus
  faulting (no handler is installed during the probe).
- phystop scan (fc0148): installs a bus-error handler at vector 8 and
  walks 128K blocks, testing 43 words below each boundary; the scan is
  terminated either by a read-back mismatch or a bus error.  So with
  the final config written, addresses beyond installed RAM inside the
  4MB ST-RAM window must bus-error (group-0 frame) — which the m68k
  core in this tree delivers from MEMTX_DECODE_ERROR.
- ff8001 config value: bank0 in bits 3-2, bank1 in bits 1-0; code
  0=128K 1=512K 2=2MB.  520ST=$04, 1040ST=$05, 4MB=$0A.
- Early PSG writes via `lea $8800(a5)` with a5=0 → address
  0xFFFF8800: **everything relies on 24-bit address masking**, which
  QEMU's 68000 already has (M68K_FEATURE_ADDR24, see MAC128K-NOTES).
- After sizing: screen base = phystop - 0x8000 → ff8201/03.

## Debug war stories (in boot order)

### TOS's 50ms FDC timeout is an equality test

`fc0610`: d1 = hz_200 + 10, then poll GPIP5 until `cmp.l $4BA,d1`
*equals*.  If hz_200 ever steps by 2 between two loop iterations the
timeout is missed forever.  On real hardware that cannot happen; in
QEMU without icount the virtual clock free-runs at wall speed, so a
host scheduling stall right after an interrupt delivery can slip a
second 200Hz tick in before the loop resumes — the boot then hangs at
`fc0616` with a stale d1.  Made mostly moot by implementing the FDC
(the wait normally ends via the interrupt, not the timeout), fully
moot under `-icount shift=7`.  Headless test loop uses
`-icount shift=7,sleep=off` (deterministic *and* fast); the other
machine units on this host use icount the same way.

### The AES trap dispatcher needs precise SMC (the big one)

Symptom: deterministic reboot loop; the AES jumps through
0x95F80000 (bus error) out of its line-F dispatcher, ~40s into boot,
desktop never appears.  RAM archaeology showed the AES's RAM-copied
resource data with `+0xAECE0000` added to longs — the RSC base
0xAECE relocated into the *high* half — and the dispatcher's
self-modified code cell at 0xAA9A holding `unlk fp` + 0xAECE.

Root cause was none of the hardware models: TOS 1.04's line-F
dispatcher (vector 0x2C handler, copied to RAM at 0xAA60) patches the
register mask of a `moveml (sp)+` **six bytes ahead of the PC** and
falls into it.  Legal on a real 68000 (the prefetch queue is only two
words), but QEMU's m68k had no `precise_smc`: the store invalidated
the current TB yet execution continued in the stale translation, so
every dispatch restored the *previous* call's register set.  The
mangled registers then drove the resource fixup loops.  Fixed with
`.precise_smc = true` in the m68k TCGCPUOps (commit
"target/m68k: enable precise self-modifying-code handling").
Diagnosis trick that finally worked: overlaying a forwarding MMIO
region on the corrupted RAM (logs every CPU write with PC) — plus
noticing that the overlay *changed* TB layout and hid the bug, which
is what pointed at translation-block staleness.

### Floppy side select is active low, and I inverted it twice

PA0=0 selects side 1; the FDC used `!side1` in the LBA math, so every
read fetched the opposite head.  The Automation boot sector came back
as 0xE5 MSA-fill bytes (checksum != 0x1234) and TOS silently fell
through to the desktop.  Symptom to remember: repeated identical
boot-sector reads in the FDC trace and 0xE5 at `_dskbufp` (0x181C).

### Assorted

- `pkill -f "qemu-system-m68k -M atarist"` matches the *calling
  shell's* command line when typed inline — use the helper scripts
  (/tmp/atarist-run.sh, /tmp/atarist-stop.sh) or the pidfile.
- QEMU's gdbstub accepts exactly one connection per run; a killed gdb
  leaves it dead.  TCG watchpoints were too slow to catch the AES
  corruption; the MMIO overlay trick was orders of magnitude faster.
- The IKBD GET-TIME reply now returns a fixed valid BCD date; TOS
  feeds it through its date conversion unchecked.
- `-drive if=floppy` needs the backend attached via
  `DEFINE_PROP_DRIVE` or blockdev reports the drive as orphaned.

## Verified state (2026-07-22)

- Milestone (a) RAM sizing: cold sizing detects 512K/1M/2.5M?/4M via
  the fold model; phystop 0x80000/0x100000/0x400000 confirmed for
  512K/1M/4M, desktop up on all three.
- Milestone (b): 200Hz tick measured at 200.0Hz, VBL 50Hz, white
  screen then desktop.
- Milestone (c): GEM desktop (green, menu bar, two floppy icons,
  trash) — /tmp/atarist-desktop.png.
- Milestone (d): mouse moves via QMP input-send-event, File menu
  pulls down on hover (/tmp/atarist-menu.png), left click selects a
  desktop icon (/tmp/atarist-click2.png).
- Milestone (e): AUT_095.MSA converted with atarist-tools/msa2st.py
  boots the Automation CD 95 menu (/tmp/atarist-aut3.png); Cosmic
  Pirates loads through cracktro + ZIPPO trainer to the NEST 51 game
  menu (/tmp/atarist-game1c.png), all via GEMDOS sector reads over
  the WD1772/DMA model.  Hostages (menu option 2) loads its ~1100
  sectors and runs its main loop but sits on a black palette —
  unresolved.  (Its MFP state shows timer A delay-mode at ~13.7kHz
  - digi music - and timer B *off*, so the later timer-B
  event-interrupt work was not the missing piece; next suspects are
  the video address counter granularity and IKBD joystick reports.)

## Gaps / next steps

- Timer B event-count *interrupts* are now delivered (terminal count
  scheduled from the video frame phase); timer A event mode is still
  count-on-read only, and pulse modes are stubs.
- FDC write track (format) and read track are stubs; write sector
  works.
- Sound: PSG is a register file only.
- TOS 2.06 (256KB at 0xE00000) not wired up.
- Mono monitor (GPIP7 low + 70Hz timing) untested/unsupported.
- The screen is rendered from the base register, ignoring mid-frame
  base/palette tricks (fullscreen demos will look wrong).

## Design decisions

- Everything in `hw/m68k/atarist.c` (glue, MMU banks, Shifter video,
  MFP 68901, ACIA+IKBD, PSG, WD1772+DMA as QOM types in one file),
  like the mac128k pattern.
- Interrupts: GLUE priority encoder as a small device.  MFP is IPL6
  vectored, VBL IPL4/HBL IPL2 autovectored and held until IACK.  The
  m68k core gets a new "iack-out" GPIO (pulsed with the taken level
  after a hardware interrupt), so the GLUE can drop VBL/HBL latches
  and the MFP can do its IPR→ISR transition exactly at acknowledge
  time — without it the MFP would storm (IPR never clears) and
  VBL edges would be lost (m68k_set_irq_level latches only levels).
- Framebuffer scans machine->ram directly via a MemoryRegionSection on
  the RAM region (not the flatview) per the mac128k lesson: the
  scanner must not resolve through CPU-visible aliases.
