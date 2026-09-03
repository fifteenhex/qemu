# HOWTO: install & boot Apollo Domain/OS (AEGIS SR10.4) under QEMU

A from-tapes Domain/OS install under emulation is essentially undocumented
anywhere; this is a step-by-step recipe for the QEMU `apollo-dn3000`
machine.  It is **work in progress**: the emulated Archive SC-499
cartridge-tape controller and OMTI-8621 Winchester now let the DN3000 boot
PROM run the SR10.4 *standalone utilities* (`invol`, `calendar`, …) off
tape, and **`invol` now initialises the Winchester end to end** — options
7 (badspot list), 1 (initialize virgin physical volume, "Initialization
complete."), and 8 (OS paging file) all run to completion and write a
valid Apollo physical-volume label to the `.awd`.  With the calendar set,
the SR10.4 standalone AEGIS kernel (`EX DOMAIN_OS`) now **boots through
its MC68851 page-table bring-up and prints its `Domain/OS kernel(8),
revision 10.4` banner** — the prior "MMU bring-up double-fault" was a QEMU
CPU-core bug (a 4 KiB softmmu page vs the guest's 1 KiB MC68851 page),
fixed in `target/m68k/cpu-param.h` (see §7).  The install then hits one
further wall — an **intermittent** post-banner fault (a timing race, *not*
the PMMU) that on ~1 in 4 runs AEGIS clears, booting through to the
boot-volume mount / "Proceed to bring up OS?" prompt (§7 "resume point").
Everything up to and including "AEGIS prints its kernel banner" is
reproducible today.

Companion docs: `APOLLO-DOMAINOS-NOTES.md` (device internals) and
`APOLLO-NOTES.md` (PROM/Linux bring-up).

---

## 1. Assets

### 1.1 Boot PROM and option ROMs (already in this workspace)
`/workspace/files/apollo/` :

| File | SHA1 | Role |
|---|---|---|
| `3000_BOOT_8475_7.bin` | `6c383d2266719a3d069b7bf015f6945179395e7a` | DN3000 boot PROM (MD8 REV 7.0). **Required** (`-bios`). |
| `3000_OMTI_8621_102640-B.bin` | `cf1990ad72eac6b296485410f5fa3309a0d6d078` | OMTI-8621 option ROM. **Not needed** — the boot PROM has a built-in Winchester driver. |
| `3000_3C505_010728-00.bin` | `7ac36cc6fc90b90ddfc56c45303b514cbe18ae58` | 3C505 Ethernet ROM (unused here). |

### 1.2 Domain/OS SR10.4 cartridge tapes
Downloaded from `https://www.bitsavers.org/bits/Apollo/SR10.4/` (bitsavers
blocks non-browser UAs — send a `Mozilla/5.0` User-Agent). These `.ct.gz`
are gzip'd raw tape images; the SC-499 model consumes the **decompressed**
`.ct` (a flat stream of 512-byte blocks; a file-mark block is 128×
`DE AF FA ED`).

| File (`.ct.gz`) | Size | SHA1 (of the `.gz`) |
|---|---|---|
| `019593-001.CRTG_STD_SFW_BOOT_1-REV.A.ct.gz` | 13.8 MB | `fb2670c51637454e2fb8b9604ef16152c86b4a95` |
| `019594-001.CRTG_STD_SFW_1.ct.gz` | 15.5 MB | `a857ad20edfe5f476de900e82b667a9838cf4611` |
| `019594-002.CRTG_STD_SFW_2.ct.gz` | 14.8 MB | `0d424507bf8970008719b4beb17962081f52d7bf` |
| `019594-003.CRTG_STD_SFW_3.ct.gz` | 18.0 MB | `a9fc9a5b975a235c376496edc3102abd1b008ede` |
| `019594-004.CRTG_STD_SFW_4.ct.gz` | 16.3 MB | `959aa8d409a13bd9d26a9d093593caeee6ea5afc` |

Decompressed boot tape (`019593-001…ct`, 53,678,592 bytes / 104,841 blocks)
SHA1 `4b7e64153b3046da25cef0e2115553611d57929e`.

```
mkdir -p /tmp/tapes && cd /tmp/tapes
for f in 019593-001.CRTG_STD_SFW_BOOT_1-REV.A 019594-001.CRTG_STD_SFW_1 \
         019594-002.CRTG_STD_SFW_2 019594-003.CRTG_STD_SFW_3 019594-004.CRTG_STD_SFW_4; do
  curl -sS -A "Mozilla/5.0" -O "https://www.bitsavers.org/bits/Apollo/SR10.4/$f.ct.gz"
done
gunzip -k *.ct.gz          # keep the .gz; the emulator needs the .ct
```

Licensing: Domain/OS was commercial HP/Apollo software; bitsavers hosts it
under the usual historical-preservation posture. Treat as abandonware for
research/preservation.

---

## 2. Build QEMU

Branch `apollo-domainos` in the worktree `/workspace/src/qemu-apollo`
(built into tmpfs to spare the small `/workspace` volume):

```
mkdir -p /tmp/apollo-build && cd /tmp/apollo-build
/workspace/src/qemu-apollo/configure --target-list=m68k-softmmu
ninja qemu-system-m68k
```

Devices added for this effort (all in `hw/m68k/apollo.c`):
- **OMTI-8621** Winchester controller — ISA I/O 0x1a0 (CPU 0x04d000), IRQ14,
  PIO data path.  Backed by a raw `.awd` via `-drive if=mtd,index=0`.
- **Archive SC-499** cartridge tape — ISA I/O 0x200 (CPU 0x050000), IRQ5,
  DRQ1.  Backed by a raw `.ct` via `-drive if=mtd,index=1`.
- **Am9517A DMA controller 1** made functional (the SC-499 streams over it).

---

## 3. Create the blank Winchester (`.awd`)

The geometry is chosen from the image size: `< 300000` sectors ⇒ the
155 MB type (1023 cyl × 8 heads × 18 sec, 1056-byte sectors). Prefer this
smaller disk to save space:

```
truncate -s 155561472 /tmp/domainos.awd      # 147312 * 1056  (155 MB)
```
(For the 348 MB type use `348701760` = 330210 × 1056.)

---

## 4. Run invocation (all steps use this base command)

```
qemu-system-m68k -M apollo-dn3000 -m 8M -icount shift=6 \
    -bios /workspace/files/apollo/3000_BOOT_8475_7.bin \
    -drive file=/tmp/domainos.awd,format=raw,if=mtd,index=0 \
    -drive file=/tmp/tapes/019593-001.CRTG_STD_SFW_BOOT_1-REV.A.ct,format=raw,if=mtd,index=1,readonly=on \
    -serial mon:stdio -display none
```

**`-icount shift=6` is mandatory** for anything touching the tape: the boot
PROM / standalone-utility tape driver spins in tight, guest-cycle-timed
poll loops, and without deterministic (icount) virtual time they race and
fall through to "ring: init error".

Because the interactions are long and prompt-driven (and icount slows
wall-clock), drive them with the included expect-style helper
`scripts/apollo-domainos-drive.py` (talks to `-serial unix:…`), rather than
piping stdin with fixed sleeps.

---

## 5. Boot the standalone utilities off tape (VERIFIED)

At the MD `>` prompt (press Return twice first — PROM autobaud):

```
> DI C          # DEFINE DSK = Cartridge tape  -> select ctape as boot device
> LD            # LIST SAU   -> lists the standalone utilities off tape 1
```
Expected (SAU8 = the DN3000 standalone-utility set):
```
SAU8/CALENDAR  SAU8/CHUVOL  SAU8/CONFIG  SAU8/DEX  SAU8/DOMAIN_OS
SAU8/DOMAIN_OS.MAP  SAU8/FBS  SAU8/INVOL  SAU8/RWVOL  SAU8/SALVOL
SAU8/SELF_TEST
```
Seeing this list proves the SC-499 + DMA path works end to end.

Run a utility with `EX <name>` (loads `/SAU8/<name>` off the boot device):
```
> EX INVOL      # -> the invol (init_volume) rev 10.4 menu
> EX CALENDAR   # -> the standalone calendar (sets the hardware clock)
```

### Setting the calendar (VERIFIED)
`invol` option 1 refuses to run unless the calendar is set. `EX CALENDAR`:
```
Please select the disk ... enter none (N):   n
Time-zone:                                    UTC
... reset it?                                 y
today's date (year/month/day):               1992/02/14
local time (hour:minute):                     12:00
-> "The calendar has been set to: 1992/02/14 12:00:00 UTC."  Done.
```
The calendar is stored in the (session-lived) RTC, so CALENDAR and INVOL
must run in the **same** QEMU session.

### invol (init_volume) — initialise a bootable volume (VERIFIED, whole run)
`EX INVOL` now reaches its full menu and runs the init options to
completion.  Do them in ONE invol session (answer **`y`** to "Anything
more to do?" between options to stay in the menu; **`n`** exits to `>`).
The calendar must have been set earlier in the same QEMU session.

**Option 7 — initialize physical badspot list** (must precede option 1):
```
Option: 7
Select disk: w                        # w = Winchester (OMTI unit 0)
Use automated badspot entry? n
: <blank line>                        # no badspots on an emulated disk
Is the badspot information you entered correct? y
Anything more to do? y
```

**Option 1 — initialize virgin physical volume** (formats + writes it all):
```
Option: 1
Select disk: w
Physical volume name: domain          # any label
Enter verification option: 1          # 1 = no verification (fast)
Expected average file size ...: <CR>  # default (5 kB)
volume 1: all                         # one logical volume using all blocks
Use pre-recorded badspot info? y      # 'y' uses the list written by option 7
...  Formatting ... 100 ...
Initialization complete.
Anything more to do? y
```
invol reads the geometry correctly (`0 .. 23F6F`, "1022-7", the 155 MB
1023×8×18 disk), formats every cylinder, and writes the physical-volume
label + logical-volume/VTOC/badspot structures.  (The whole-disk format is
slow under `-icount`; allow a few minutes.)

**Option 8 — create the OS paging file** (needed to boot Domain/OS):
```
Option: 8
Select disk: w
Enter logical volume number: 1
Size in kB for the OS paging file (CR for default value = 640) <CR>
Done.
Anything more to do? n                # exits invol -> MD '>' prompt
```
After this the `.awd` is a valid, initialised Apollo physical volume:
block 0 holds a real PV label (`invol` prints
`Volume built by Invol version "revision 10.4" on Feb. 14, 1992`,
`Physical volume "domain"`, one 146879 kB logical volume).  Checkpoint it
(`cp /tmp/domainos.awd /tmp/domainos.awd.invol_done`).

Two device-model bugs had to be fixed to get here (both committed on this
branch, both Apollo-local); see §7 and `APOLLO-DOMAINOS-NOTES.md`:
1. a phantom PTM timer IRQ fabricated by the 8259 across INVOL's `ICW1`
   re-init (crash `000D0004`), and
2. the OMTI `READ_DATA_TO_BUFFER`/`WRITE_DATA_FROM_BUFFER` (0x1e/0x1f)
   buffer commands modelled with a spurious host data phase (crash
   `00080016`).

---

## 6. Boot the standalone AEGIS kernel (`EX DOMAIN_OS`)

Once the disk is initialised (§5), the SR10.4 install proceeds by booting
the standalone AEGIS kernel off the boot tape.  **The machine now presets
the RTC to a valid binary-mode date at reset** (commit "preset RTC to a
fixed binary-mode date"), so `EX DOMAIN_OS` (and invol) boot **without the
interactive `EX CALENDAR` dance** — AEGIS no longer prints "The calendar is
more than a minute slow".  Just:

```
> DI C
> EX DOMAIN_OS       # loads /SAU8/DOMAIN_OS (the standalone AEGIS, ~950 kB)
```

(`EX CALENDAR` still works and is documented in §5 if you want to change the
date; on stock QEMU without this preset the RTC powers up BCD/1980 and
CALENDAR would be mandatory.)  Removing that wall-clock-dependent input also
makes the `-icount` boot deterministic — see §7.

AEGIS then brings up its MC68851 page tables and prints its banner:

```
low: 00101C00  high: 001E8BFF  start: 00101C24
Domain/OS kernel(8), revision 10.4, February 14, 1992  11:50:22 am
```

Getting to this banner required a QEMU CPU-core fix (§7).  The install then
continues (restore the four software cartridges `019594-00{1..4}.ct`, then
`EX AEGIS` the disk) but is blocked one wall further on (§7 "resume point").

The `scripts/` helper `drive_aegis`-style expect flow (calendar → DOMAIN_OS)
is fiddly on a loaded host; `-icount` runs are wall-clock-slow under CPU
contention, so use generous (>120 s) per-prompt timeouts.

---

## 7. Status & resume point (for whoever continues)

Reproducible today: **build → blank `.awd` → boot PROM → `DI C` → `LD`
lists the SAUs → `EX CALENDAR` sets the clock → `EX INVOL` runs options 7,
1, 8 to completion → the `.awd` is a valid, initialised Apollo physical
volume with an OS paging file** (§5).  Checkpoint it before killing QEMU.

### The two INVOL blockers are FIXED (both committed on `apollo-domainos`)

The prior "two open blockers" were both device-model bugs, found by
disassembling the vendor `/SAU8/INVOL` (loaded at `0x102000`) — dump it
from a running guest with gdb (`dump binary memory … 0x102000 0x144000`),
`m68k-linux-gnu-objdump -b binary -m m68k:68040 -EB --adjust-vma=0x102000`.

1. **`Crash_Status 000D0004` at PC `0x12660A`** was NOT a calendar/RTC
   check — that PC is the tail of INVOL's MC6840 *timer* interrupt handler
   (`0x126500`), which EOIs the master 8259, reads the PTM status port
   (`0x8802`), and panics if no timer flag is set (a spurious-timer-IRQ
   panic).  Root cause: INVOL leaves a PTM flag set (IRQ line held high)
   while it re-programs the master 8259 with `ICW1`; QEMU's i8259 zeroes
   its edge-sense memory (`last_irr`) on `ICW1`, so the still-high line is
   mis-sampled as a fresh 0→1 edge and a phantom IR0 is delivered with the
   PTM status already empty.  **Fix:** preserve `last_irr` across `ICW1`
   in the Apollo PIC register wrapper (`apollo_pic_reg_write`).

2. **`Crash_Status 00080016` at PC `0x10F89C`** (INVOL options 7 and 1)
   is the tail of INVOL's OMTI completion poll: it spins for the OMTI
   status port to return to idle (`0xC0`) and panics on timeout.  The OMTI
   never went idle because `READ_DATA_TO_BUFFER` (0x1e) and
   `WRITE_DATA_FROM_BUFFER` (0x1f) — which move the controller's sector
   buffer to/from disk with **no host data phase** — were modelled with a
   spurious host data-in/out phase, leaving the controller stuck mid
   transfer.  **Fix:** 0x1f executes immediately (buffer→disk); 0x1e reads
   disk→buffer with no host phase (`omti_do_command` / `omti_write8`).

Trace scaffolding for both (compiled out) lives behind `APOLLO_RTC_TRACE`
and `APOLLO_OMTI_TRACE` in `hw/m68k/apollo.c`.

### The AEGIS MMU-bring-up double-fault is FIXED (committed)

The earlier "double-fault in AEGIS MMU bring-up, TCR 0" diagnosis was a
**misread**: the CPU-state dump printed the 68040-style `tcr/urp/srp`
registers, which are always zero on the 68020+68851 path, and hid the real
68851 state.  The dump now also prints `TC030/CRP/SRP030` (translate.c),
which reveals **`TC030 = 0x80a47650` — the E(nable) bit IS set**.  So this
SAU8 AEGIS runs *with the 68851 translation ON*, not off, and there is **no
RAM high-alias**: AEGIS builds valid 68851 long-format page tables (CRP →
physical `0x101400`) that map its high AEGIS virtual region
(`~0x0EF40000..0x0FFC0000`) onto the low physical RAM, then runs there
virtually.  `TC030 = 0x80a47650` decodes to **1 KiB pages** (PS field
`0xa`), IS = 4, TIA/TIB/TIC = 7/6/5 — the classic Apollo page geometry.

Root cause of the double fault: **QEMU's m68k `TARGET_PAGE_BITS` was 12
(4 KiB) but the guest MMU page is 1 KiB.**  QEMU's softmmu maps a whole
`TARGET_PAGE` at one physical base, so `tlb_set_page()` masks the frame to
`TARGET_PAGE` — and AEGIS's page frames are **not** 4 KiB aligned (its
vector table is at physical `0x101800`).  `get_physical_address_030()`
resolved virtual `0x8` → physical `0x101808` correctly, but the softmmu
then cached `0x101808 & ~0xfff = 0x101000`, so at run time virtual `0x8`
read physical `0x101008` (garbage `0x2e780000`) instead of `0x101808`
(the real access-fault vector `0x404`).  A demand fault therefore
dispatched through a garbage handler, stormed, and overflowed the stack →
`DOUBLE MMU FAULT`.  (Chase it with a fresh-vs-cached compare: the DEBUG
walk `m68k_cpu_get_phys_addr_debug()` skips the softmmu and returned the
right `0x404`, while the runtime `cpu_ldl` returned `0x2e780000`.)

**Fix:** `target/m68k/cpu-param.h` `TARGET_PAGE_BITS 12 → 10` (1 KiB), the
smallest page Domain/OS uses, so the softmmu granularity is never coarser
than the guest MMU page.  Larger guest pages (Linux 4 KiB, ColdFire 8 KiB)
still work as multi-page mappings — Linux/m68k re-verified (same kernel
code paths / timer IRQs, `-d int`), and the DN3000 PROM MD prompt is
unaffected.  With the fix (and the calendar set, §6) AEGIS prints its
`Domain/OS kernel(8), revision 10.4` banner.

### The NEW wall: post-banner demand-fault storm at `0x0f38b000`

After the banner, AEGIS aborts with a *second* `DOUBLE MMU FAULT`:
**`PC = 0f0dd7f6`, fault at `0x0f1f03fc`, `TC030 80a47650`** (translation
still on).  What happens: at `PC 0f24575e` AEGIS does a long read of
virtual `0x0f38b0a0`; that page (`0x0f38b000`, ~4.5 MB into the high AEGIS
region) has **no descriptor** in AEGIS's own tables, so it faults.  The
fault is then serviced by code around `PC 0f0dd6a6/0f0dd7e2` that reads
*down* through `0x0f38b09e, 9c, 9a …` — every word faults, the handler
**nests** rather than mapping the page, and the interrupt-stack (ISP
`0x0f1f043c`, itself in a 1 KiB page whose neighbour `0x0f1f0000` is
unmapped) overflows on the frame push → double fault.

Confirmed with `-m 16M`: **identical** fault, so it is *not* a RAM-size
issue (AEGIS neither sizes nor maps more RAM with 16 MB; the SIO RAM-config
byte is a fixed `0x20`/8 MB anyway).

**Both of Fable's PMMU-audit hypotheses for this wall are now DISPROVEN**
(instrumentation on `apollo-domainos` after merging `amiga`):

- **NOT the DF-cleared software-completion gap** (PMMU-DEEPDIVE finding 7,
  gap 1).  Instrumenting `m68k_rte`'s format-A/B DF-clear path shows AEGIS
  **never clears SSW.DF** — `bus_error_suppress` is never set, and every
  `tlb_fill` fault in the storm has `supp=0`.  AEGIS's handler does not
  software-complete the faulted cycle; it never even reaches an RTE.
- **NOT an unhandled indirect page descriptor** (gap 9).  Instrumenting the
  030 leaf handling shows the walk of `0x0f38b0a0` returns via the
  **`DT_INVALID` path at level 3** (`[INVAL] va=0f38b0a0 level=3`), *not*
  the `dt != PAGE`/table-type (indirect) path (`[LEAF]` never fires).  So
  the page really is unmapped in AEGIS's tables; the walker is correct.

Mechanism (traced): the primary fault is the **return-address push of a
`jsr` at `PC 0x0f245772`** (a mutex/lock routine) — i.e. the **stack pointer
is ≈`0x0f38b0a4`, in an unmapped page**.  The CPU vectors to the
access-fault handler (vector 2 = `*(VBR=0 + 8)`), but PC ends up executing a
**non-code region at `0x0f0dd3e8`** (disassembles as `movepw` + `0x0000`
words + a few pointer-like longs; its next VA page `0x0f0dd400`→phys
`0x171000` is all zeros), NOPs into `0x0f0dd6xx`, and the stray words there
deref `a3=0x0f38b0a0`, so every word re-faults, nesting on the ISP until it
overflows `0x0f1f0000`.

**This is NOT a PMMU-core bug.**  Instrumenting the vector-2 delivery
(`op_helper.c`, `m68k_interrupt_all`) and comparing the **runtime `cpu_ldl`
vs the DEBUG walk** (the exact method that caught the `TARGET_PAGE_BITS`
bug) shows **both translate identically**:
```
[VEC2] ar=0f38b0a0 vbr=0 vec=8 -> handler=0f0dd3e8 | vecphys=00101808
       vecval(dbg)=0f0dd3e8 | handlerphys=0025a3e8 code[0](dbg)=0f0f7758
```
So the vector *fetch* and the handler *VA→phys* are both correct — the
68851/TARGET_PAGE path is sound.  The defect is that the memory the handler
VA points at (phys `0x25a3e8`, which is **above the loaded image** `≤0x1e8bff`
so it must be *populated by AEGIS init*) contains **garbage/zeros instead of
handler code**.  Vector 2 legitimately points there, but the code was never
correctly placed/copied — a **memory-content corruption**, not a
translation error.

**It happens DURING AEGIS's own page-table build.**  Instrumenting stores
that resolve into the handler physical page shows AEGIS is running its page
allocator (`jsr 0x0f22c614` in the loop at `PC 0x0f2011xx`) that hands out
freshly-**zeroed** 1 KiB physical pages and installs their descriptors —
mapping e.g. VA `0f0dd000`→phys `0x25a000` (the vector-2 handler page) and
VAs `0f389c00/0f38a000/0f38a400`→`0x25a400/800/c00` (adjacent stack pages).
The faulting stack page `0f38b000` (SP≈`0f38b0a4`) sits just past this
still-growing mapping and is **not mapped yet**, and vector 2 = `0x0f0dd3e8`
points into one of these **just-allocated, zero/uninitialised pages** —
i.e. the real access-fault handler has **not been copied in yet**.  So a
fault taken *mid-page-table-build* dispatches through a not-yet-ready
handler and storms.  This is a **critical-section / timing** defect, not
the PMMU.

**Determinism + the good path.**  With `-icount` the boot is deterministic
*except* for wall-clock-dependent inputs.  Presetting the RTC at machine
init (commit "preset RTC to a fixed binary-mode date") removes the CALENDAR
dance and its input-timing jitter, and the crash then reproduces **every
run** (fast, ~3 min, any `-icount shift`) — pinning it deterministically for
debugging.  Conversely, when the CALENDAR *is* driven (its serial-input
timing jitters the guest-cycle alignment), ~1 in 4 runs the fault does *not*
land mid-build and AEGIS boots through to **boot-volume mounting**:
```
Domain/OS kernel(8), revision 10.4, February 14, 1992  11:50:22 am
BOOT VOLUME NEEDS SALVAGING.
Proceed to bring up OS (and risk volume)?
```
i.e. AEGIS reaches the standalone-OS bring-up prompt on the invol'd disk —
proof the page-table build *does* complete when the fault window is missed.

**It is NOT a QEMU store/TLB-coherence bug.**  Traced the exact leaf
descriptor: the walk of `0f38b0a0` is `L0 entry=001015e4 desc=0021620b(dt3)`
→ `L1 entry=00216388 desc=001ffd0a(dt2, long)` → **leaf `L2 entry=002088b0
desc=00000000 (INVALID)`**.  Polling phys `0x002088b0` every `tlb_fill` shows
it goes `0xffffffff→0` once (PROM at pc=598a) and **stays `0` the entire
boot** — AEGIS **never writes a valid descriptor for `0f38b000`**.  So there
is no dropped/stale store and no negative-TLB-not-invalidated case (nothing
is ever written to go stale); the softmmu/PMMU is behaving correctly.  AEGIS
**genuinely uses `SP=0f38b0a4` before `0f38b000` is mapped and before its
access-fault handler is installed** (vector 2 still points at a
just-allocated zero page).  The adjacent stack pages *are* mapped in the same
L2 table (`0f389c00/0f38a000/0f38a400`→phys `0x25a400/800/c00`), so it is the
**top page of the stack** that is missing — an ordering/off-by-one in AEGIS's
setup that our emulation's timing exposes and that the good path (right
timing) does not hit.

**Ruled out so far** (all instrumented on this branch): the PMMU/softmmu
(runtime==debug translate), a QEMU dropped/stale store (the leaf descriptor
is *never* written), and the **PTM6840 tick rate** — the PTM0 heartbeat is a
normal **60 Hz** (`latch=4166`, 250 kHz clock, period 16.668 ms), so the
`-d int` burst of ~14 vector-0xa0 IRQs before the fault is just ~230 ms of
guest time at 60 Hz, not a storm.  So the timing-sensitivity is *not* from a
too-fast timer.

**Next steps (for whoever continues):**
1. The remaining root is subtle: AEGIS's setup uses `SP=0f38b0a4` (top stack
   page `0f38b000`) before that page is mapped and before its access-fault
   handler is installed, and *which side of that ordering* the run lands on
   depends on guest-cycle timing our emulation gets slightly different from
   real HW.  Bisect the setup: single-step (deterministic via the RTC preset)
   from the page-allocator loop (`PC 0x0f2011xx`) forward, watching the
   moment `SP` is set to `0f38b0a4` vs the moment the handler is installed
   (vector 2 rewritten away from the zero page / the handler code copied to
   phys `0x25a3e8`), and find the branch/`jsr` whose taken-vs-not depends on
   a value our emulation returns differently (a device read timing, not the
   PMMU).
2. Or ride the good path: drive CALENDAR (its serial-input jitter gives
   ~1-in-4 good boots even with the RTC preset) and answer `Proceed to bring
   up OS? y` (handled in the `drive_aegis`-style scripts) to continue toward
   the installer/shell.  Under host load (~13) the calendar+load+crash cycle
   is ~14 min, so budget `FINALWAIT`≈800 s and retry several times.
3. Instrumentation gotcha: a leaf-walk `fprintf` printing possibly-uninit
   `desc`/`table` trips `-Werror=maybe-uninitialized`; ninja then silently
   keeps a **stale binary** — always `strings qemu-system-m68k | grep MARK`
   to confirm the instrumentation compiled in before trusting a run.

**How to reproduce / debug the second wall:**
- Boot with the calendar set (§6), `EX DOMAIN_OS`, `-m 8M` (or `16M`).
- Log the first fault path by adding, at the 030 `EXCP_ACCESS` delivery in
  `m68k_cpu_tlb_fill()` (helper.c) and at the `DT_INVALID`/`dt!=PAGE`
  returns in `get_physical_address_030()`, a `getenv`-gated
  `fprintf(stderr,...)` — **print only always-initialised locals**
  (`address`, `dt`, `level`); printing `desc`/`table` at the leaf trips
  `-Werror=maybe-uninitialized` and the build silently keeps a stale binary
  (verify markers with `strings qemu-system-m68k | grep '\[INVAL\]'`).
- The decisive next trace is a full instruction/`-d exec` capture of the
  transition **into** `0x0f0dd6xx` (find the last TB before it — the jumper).
  WARNING: `-d exec,nochain` over the whole high-VA range fills `/tmp`
  (16 GB) within seconds — use a *narrow* `-dfilter` and a short window.
- To disassemble AEGIS code at high virtual PCs, dump the loaded image at
  the entry breakpoint (`hbreak *0x101c24`; **`set endian big`** on the
  gdb-multiarch stub, port 1244!) `dump binary memory /tmp/aegis.bin
  0x101c00 0x1e8c00`; the `0x0f2xxxxx` code region is virtual = phys +
  `0x0f0fdc00`, other regions use scattered table mappings (resolve live).
- Iteration is slow: the host is CPU-contended (load ~12), each
  calendar+`EX DOMAIN_OS`+crash run is ~7–10 min wall and the crash lands
  a variable time after the banner, so use `FINALWAIT`≈700 s.

Re-verify Linux (`-kernel`, §`APOLLO-NOTES.md`) and the PROM MD prompt
after any further CPU-core change; both currently pass with
`TARGET_PAGE_BITS = 10`.

Checkpoint any initialised `/tmp/domainos.awd` (e.g.
`domainos.awd.invol_done`) before killing QEMU so a fresh agent can resume
from the disk state.

---

## 8. Gotchas

- **icount is mandatory for the tape path** (`-icount shift=6`); without it
  the PROM's tape poll loops race and error out. It also makes runs
  deterministic (the same script otherwise sometimes worked, sometimes
  didn't).
- **Tape is read-only** (`readonly=on`): the SC-499 reports write-protect,
  which is fine for reading/installing. The `.awd` disk (index 0) is
  writable.
- **Disk geometry follows image size** — keep the `.awd` exactly
  155,561,472 or 348,701,760 bytes so invol sees a clean 1023×8×18 /
  1223×15×18 disk.
- **Calendar + invol must share one QEMU session** (RTC state is not
  persisted across boots). On real hardware the RTC is battery-backed.
- **RTC weekday** register uses Domain's 0=Sunday convention; the machine
  translates to/from QEMU's 1=Sunday (see `apollo_rtc_read/write`).
- **Driving prompts**: use `scripts/apollo-domainos-drive.py`
  (`EXPECT`/`SENDLN` over a `-serial unix:` socket) — fixed-sleep stdin
  piping is unreliable under icount.
- The OMTI Winchester transfers are **PIO** (no DMA); only the SC-499 tape
  uses DMA (controller 1, channel 1).
