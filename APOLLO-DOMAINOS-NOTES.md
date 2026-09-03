# Apollo DN3000 — Domain/OS (AEGIS) bring-up: OMTI-8621 Winchester + image sourcing

Status: **OMTI-8621 disk controller modelled and validated by the real
DN3000 boot PROM.** The PROM's `EX AEGIS` disk-boot path now drives the
emulated controller end-to-end: it reads the drive configuration, reads
block 0, parses the Apollo physical-volume label, walks the volume
looking for the AEGIS bootstrap `sysboot`, and fails only because the
disk has **no Domain/OS installed on it** (`error: sysboot not found`).

**Domain/OS does NOT boot to a shell** — and cannot in this session —
because there is *no freely available pre-installed Domain/OS disk
image anywhere*, and the only route (a multi-tape SR10.4 install) needs
an additional device model (the Archive SC-499 cartridge-tape
controller) plus a long, multi-volume install run. This is a hard,
well-diagnosed wall: **the blocker is the OS image, not the OMTI model
and not the PROM boot path** (both proven working below).

---

## 1. Image sourcing recon (the gate) — no bootable image exists

Searched bitsavers, archive.org, GitHub, Docker Hub, and the community
references (MAMEDEV wiki, Jim Rees' Apollo archive/otterlator, HackMD
vintage-computers notes, preterhuman/Higher-Intellect install guide).

**Conclusion: there is no ready-to-boot `.awd` Winchester image
published anywhere.** Every reference describes *building one yourself*
by installing Domain/OS from the original cartridge tapes under MAME.

Leads chased and ruled out:
- **Jim Rees otterlator / apollo-archive** — source only, no disk image.
- **HackMD `@vintage-computers/SysqKn3hj`** — networking/`invol` notes,
  no image, no download.
- **`domainos-archeology/apollofs` (GitHub)** — a Go tool that *reads*
  an existing `dn3500_sr10.4.awd`; it does not ship one.
- **`king019/apollo` (Docker Hub)** — a false positive: this is Ctrip's
  "Apollo" config-center (Java microservices on AlmaLinux), completely
  unrelated to Apollo/Domain. Inspected its manifest + the 253 MB layer
  (candidate for a compressed 348 MB `.awd`) via the registry API — it
  is only GUI/JDK libraries. Ruled out.
- **archive.org** — hosts only the SR10.x *documentation* and the
  bitsavers tape mirror, no pre-built disk.

The only media are the SR10.4 **cartridge-tape** install images on
bitsavers (`http://www.bitsavers.org/bits/Apollo/SR10.4/`), which the
prior research already inventoried:

| File | Size |
|---|---|
| `019593-001.CRTG_STD_SFW_BOOT_1-REV.A.ct.gz` | 13 MB (boot tape) |
| `019594-001.CRTG_STD_SFW_1.ct.gz` | 15 MB |
| `019594-002.CRTG_STD_SFW_2.ct.gz` | 14 MB |
| `019594-003.CRTG_STD_SFW_3.ct.gz` | 17 MB |
| `019594-004.CRTG_STD_SFW_4.ct.gz` | 16 MB |

(~75 MB compressed; not downloaded — see §5 for why the install path is
out of scope this session.)

### The documented install procedure (from the MAMEDEV Apollo wiki)

1. Create a blank `.awd` of exactly **348 701 760 bytes** (= 330 210
   sectors × 1056 B → the 348 MB Maxtor geometry, 1223×15×18).
2. Boot the PROM with `-disk1 blank.awd -ctape BOOT_1.ct`, at the MD
   `>` prompt run `ex invol` and initialise the volume:
   `7` (badspot list) → `1` (init virgin physical volume) → `8` (create
   OS paging file).
3. Run `minst` and restore each of the four software cartridges in
   turn.
4. Thereafter Normal Mode auto-boots Domain/OS from the Winchester
   (Service Mode drops to `>` and needs `ex aegis`).

Reproducing this needs the SC-499 ctape model (§5) and is a
multi-hour, multi-tape run — a separate work item.

---

## 2. OMTI-8621 model (`hw/m68k/apollo.c`) — implemented & validated

Ported from MAME `src/devices/bus/isa/omti8621.cpp` /`.h`
(Ostermeyer/Belmont), the authoritative DN3000 Winchester model.

**Where it lives in the address map.** The controller is an SMS
OMTI-8621 on the AT/ISA bus at ISA I/O base **0x1a0** (ESDI jumper
default for the Apollo variant). The Apollo ISA I/O address translation
    `isa_addr = (off & 3) + ((off & ~0x1ff) >> 7)`, `off = cpuaddr/2`
maps that to **CPU physical `0x04d000`** (8-byte window), overlaid on
the AT-bus-I/O float-high background. This address was *confirmed at
runtime*: an instrumented run of `EX AEGIS` (pre-implementation)
status-polled exactly ISA byte 0x1a0 / CPU 0x04d000 ~5 M times, proving
the DN3000 boot PROM contains a **built-in Winchester driver** and that
**no OMTI option ROM needs to be mapped** (the `3000_OMTI_8621` BIOS ROM
is for PC-style access, unused by the AEGIS boot path).

**Register interface** (byte lanes; the Apollo `io16_swap` byte-swap
cancels the 68k-BE/ISA-LE mismatch, giving a natural 1:1 CPU-offset →
port mapping, verified):

| CPU off | read | write |
|---|---|---|
| 0 | data-in / command-status | data-out / CDB byte |
| 1 | status port | software reset |
| 2 | config (`~jumper`) | select (start command) |
| 3 | mask port | mask (DMA/INT enable) |

16-bit accesses to offset 0 are the **PIO data register** (get/set),
returning big-endian-assembled sector bytes so disk data lands in guest
memory in natural order.

**Command set** (`omti_do_command`): TEST_DRIVE_READY, RECALIBRATE,
REQUEST_SENSE, READ_VERIFY, SEEK, READ / READ_DATA_TO_BUFFER,
READ_SECTOR_BUFFER, WRITE / WRITE_DATA_FROM_BUFFER, WRITE_SECTOR_BUFFER,
READ_CONFIGURATION, RAM/INT diagnostics, plus the sense/invalid paths.
CDB disk-address decode, geometry validation, and 4-byte sense data all
match MAME.

**State machine:** IDLE → (write SELECT) → COMMAND → collect 6/10-byte
CDB → do_command → DATA (PIO in/out) → STATUS → IDLE, with the
BUSY/REQ/CD/IO/IREQ status bits driven exactly as the controller does.

**Interrupt:** IRQ14 (Winchester) = slave 8259 IR6
(`qdev_get_gpio_in(pic[1], 6)`), asserted on command completion when
`MASK.INTE` is set, cleared on status read / mask-disable. (The PROM
boot path is pure polling, so this is exercised by an OS, not the PROM.)

**Backing store / geometry.** Backed by a raw `.awd` via
`-drive if=mtd` (unit 0), attached with `blk_attach_dev` so it is not
reported orphaned. Sector size **1056 B** (1024 data + 32 overhead);
byte offset = `diskaddr × 1056`; reads/writes via `blk_pread`/
`blk_pwrite`. Geometry is chosen from image size exactly as MAME's
`device_reset`: `≥ 300 000` sectors → Maxtor 348 MB (1223×15×18), else
Micropolis 155 MB (1023×8×18). `READ_CONFIGURATION` returns
`cyl-1, heads-1, sectors-1, 0x0244, …`.

**No Am9517A DMA needed for the disk.** In MAME the OMTI's DMA/`dack`
path only feeds the embedded floppy; the Winchester transfers are PIO
through the data register — so the existing DN3000 DMA stubs are left
untouched and the disk path is DMA-free.

---

## 3. Runtime validation (this is the soundness payoff for the OMTI)

`EX AEGIS` at the MD prompt with a `-drive if=mtd` `.awd` attached:

**Blank disk (all zero):**
```
>EX AEGIS
Z       2952       2704       2014
2952: 4841
>
```
CDBs issued by the PROM (traced): `00` TEST_DRIVE_READY → `ec`
READ_CONFIGURATION → `08` READ block 0. The PROM read the geometry and
block 0, found no valid label in the zeros, and aborted.

**Block 0 seeded with a non-zero pattern (`A5 5A …`):**
```
>EX AEGIS
error: sysboot not found
>
```
CDBs: `00`, `ec`, then `08` reads of blocks **0, 2, 3, 4, 5, 6, 7, 8,
9, 10, 11 …** — i.e. the PROM parsed block 0 as an Apollo physical-
volume label, *walked the volume directory following the on-disk
pointers*, and searched for the AEGIS secondary bootstrap `sysboot`.

That the PROM's parse and read-sequence changed with the sector
contents proves the **read-data PIO path delivers correct bytes** and
that the **entire PROM disk-boot path works through the model**. The
`error: sysboot not found` is precisely the DN3000 PROM's message for a
disk whose volume structure is (partly) readable but has no installed
Domain/OS bootstrap — exactly the expected state for an un-installed
disk.

**No regressions:** the MD `>` prompt still comes up with no disk, and
the Linux `-kernel` boot still reaches userspace (initramfs unpack,
delay calibration) unchanged.

---

## 4. Exact commands

Build (tmpfs only):
```
cd /tmp/apollo-build && \
  /workspace/src/qemu-apollo/configure --target-list=m68k-softmmu && \
  ninja qemu-system-m68k
```
Create a blank Winchester and drive the PROM disk boot:
```
truncate -s 348701760 /tmp/domainos.awd
qemu-system-m68k -M apollo-dn3000 \
    -bios /workspace/files/apollo/3000_BOOT_8475_7.bin \
    -drive file=/tmp/domainos.awd,format=raw,if=mtd \
    -serial mon:stdio -display none
# press Return twice for the MD '>' prompt (autobaud), then:  EX AEGIS
```

Firmware hashes (SHA1, verified against MAME):
- boot PROM `3000_BOOT_8475_7.bin` — `6c383d2266719a3d069b7bf015f6945179395e7a`
- OMTI BIOS `3000_OMTI_8621_102640-B.bin` — `cf1990ad72eac6b296485410f5fa3309a0d6d078`
  (present in `/workspace/files/apollo/`, **not** mapped — PROM has its
  own driver)

---

## 5. What remains to actually boot Domain/OS (next work item)

1. **Source or build a Domain/OS `.awd`.** No download exists; it must
   be produced by the SR10.4 tape install (§1). Requires:
2. **Model the Archive SC-499 cartridge-tape controller** (MAME
   `-ctape`, ISA, IRQ5) + the `.ct` tape container format. This is a
   second, independent ISA device of comparable size to the OMTI.
3. **Run the multi-tape install** (`ex invol` → `minst` × 4 tapes) — a
   long emulated run. Disk budget is a real constraint here: the
   `/workspace` volume has < 2 GB free and a 348 MB `.awd` + 75 MB of
   tapes + the build are tight; do this in `/tmp` (tmpfs, ~3 GB).
4. Only *then* does `ex aegis` load `sysboot` → the AEGIS kernel →
   Display Manager / shell, and only then is the **68851 PMMU exercised
   under the real vendor OS** (the intended validation). The PROM
   disk-boot path itself runs MMU-off (physical), so it does not add
   PMMU coverage beyond what Linux already provides.

**Bottom line for the coordinator:** the OMTI-8621 (the disk half of
the Domain/OS story) is done and proven against the real PROM. Reaching
the Domain/OS shell now depends entirely on getting an installed image,
which means implementing the SC-499 ctape and running the tape install
— a separate, sizeable effort, gated further by the tight disk budget.

---

## 6. UPDATE — SC-499 cartridge tape + DMA modelled; SR10.4 standalone utilities boot off tape

The image gate above was resolved by modelling the tape path and using the
SR10.4 install cartridges directly (there is still no pre-built `.awd`).
See `APOLLO-DOMAINOS-HOWTO.md` for the full reproducible recipe; this
section records the device internals.

### Archive SC-499 cartridge-tape controller (`hw/m68k/apollo.c`)
Ported from MAME `src/devices/bus/isa/sc499.cpp`.  ISA I/O base **0x200**
→ CPU **0x050000** (same Apollo ISA-I/O translation that placed the OMTI),
**IRQ5** (master PIC IR5), **DRQ1**.  Modelled: the data/command,
status/control, DMAGO and RSTDMA ports; the reset and 6-byte READ STATUS
handshakes; and READ DATA block streaming.  `.ct` images are a flat stream
of 512-byte blocks; a file-mark block is 128× `DE AF FA ED`.

### Am9517A DMA controller 1 (made functional)
The tape streams over ISA DMA (unlike the OMTI Winchester, which is PIO), so
controller 1 gets a real i8237 register file (per-channel base/current
addr+count with the byte flip-flop, mode, mask, command/status) plus a
**burst** transfer engine.  The 24-bit transfer address is
`(page_register[channel2page[1]=3] << 16) | channel-1 16-bit address`.  The
device latches DRQ on DMAGO and the burst fires when the driver **unmasks
channel 1** — this reproduces the real DMAGO→program→unmask ordering
deterministically instead of via a timer (which raced).

### Timing / determinism — icount is required
The boot PROM's tape driver polls tight loops whose timeouts are
guest-cycle-based.  Under QEMU's default wall-clock virtual time these race
(the same script sometimes worked, sometimes fell to "ring: init error").
Running with **`-icount shift=6`** makes virtual time track guest cycles and
the SC-499's small MAME-derived delays (100 µs handshakes, etc.) fire
deterministically.  The SC-499 reset-complete EXC pulse is additionally made
flag-based (deterministic) rather than timer-based.

### Verified end-to-end
`DI C` (select ctape) + `LD` lists the DN3000 standalone utilities off tape
1 — `SAU8/{CALENDAR,CHUVOL,CONFIG,DEX,DOMAIN_OS,FBS,INVOL,RWVOL,SALVOL,
SELF_TEST}` — i.e. the PROM reads the SAU directory through the SC-499 +
DMA.  `EX INVOL` and `EX CALENDAR` load and run; CALENDAR sets the RTC;
INVOL reads the disk geometry (via the OMTI) and formats the diagnostic
cylinder.

### Further device fidelity found while driving invol/calendar
- OMTI-8621 **FORMAT_TRACK** (fill track with 0x6c) + **FORMAT_BAD_TRACK**:
  invol's badspot-init issues these; missing them → INVALID_COMMAND →
  invol panic.
- OMTI-8621 command-completion **IRQ14 deferred** via a short timer (real
  controller latency) instead of re-entrant assertion inside the host
  access.
- MC146818 **day-of-week** register translated between Domain/OS's
  0=Sunday and QEMU's 1=Sunday conventions.

### Disk init COMPLETE — the two invol blockers were device-model bugs

Both prior blockers were found by disassembling the vendor `/SAU8/INVOL`
(loaded at 0x102000) and are now fixed (Apollo-local, on `apollo-domainos`);
`invol` runs options 7/1/8 to a valid, bootable-format physical volume.

1. **`Crash_Status 000D0004` @ PC 0x12660A** was a mis-diagnosis of a
   "calendar" check: that PC is the tail of invol's **MC6840 timer ISR**
   (0x126500), which panics on a *spurious* timer interrupt (PTM status
   port 0x8802 reads no flag).  QEMU's i8259 zeroes its edge-sense memory
   (`last_irr`) on `ICW1`; invol re-inits the master PIC to edge mode
   while a PTM flag is set (IRQ line high), so the still-high line is
   sampled as a fresh 0→1 edge → phantom IR0 with an empty PTM status.
   **Fix:** preserve `last_irr` across `ICW1` in `apollo_pic_reg_write`
   (a line held high must make a real low→high transition, per the 8259).

2. **`Crash_Status 00080016` @ PC 0x10F89C** (options 7 and 1) is invol's
   OMTI completion poll timing out (waits for the status port to return to
   idle 0xC0).  The OMTI `READ_DATA_TO_BUFFER` (0x1e) and
   `WRITE_DATA_FROM_BUFFER` (0x1f) commands move the controller sector
   buffer to/from disk with **no host data phase**, but were modelled with
   a spurious host data-in/out transfer, hanging the controller mid state
   machine.  **Fix:** 0x1f executes immediately (buffer→disk); 0x1e reads
   disk→buffer only (`omti_do_command`, `omti_write8`).

### AEGIS MMU bring-up double-fault — FIXED (1 KiB TARGET_PAGE)

The SR10.4 standalone AEGIS (`/SAU8/DOMAIN_OS`, ~950 kB, entry 0x101C24)
double-faulted in MMU bring-up.  The prior "`%tc = 0`, translation OFF,
needs a RAM high-alias" reading was **wrong** — it came from the CPU dump
showing the 68040 `tcr/urp/srp` (always 0 on the 020+851 path).  Dumping
the real 68851 registers shows **`TC030 = 0x80a47650`, E bit set**:
translation is ON, AEGIS builds valid 1 KiB-page tables (PS field 0xa,
CRP → physical 0x101400) mapping its high virtual region onto low RAM, and
runs virtually.  There is **no RAM high-alias**.

Root cause was a QEMU CPU-core bug: `TARGET_PAGE_BITS` was 12 (4 KiB) while
the guest MC68851 page is 1 KiB, and AEGIS's page frames are not 4 KiB
aligned (vector table at physical 0x101800).  `tlb_set_page()` masks the
frame to TARGET_PAGE, so virtual 0x8 (→ correct physical 0x101808) was
cached as physical 0x101000+8 = 0x101008 (garbage), sending a demand fault
through a bogus vector → storm → double fault.  **Fix:**
`target/m68k/cpu-param.h` `TARGET_PAGE_BITS 12 → 10`.  AEGIS then boots to
its `Domain/OS kernel(8), revision 10.4` banner (calendar must be set
first — the RTC powers up BCD/1980, so `EX CALENDAR` is required).  Linux
and the PROM MD prompt re-verified unaffected.

### Remaining wall (HOWTO §7): post-banner demand-fault storm

After the banner AEGIS aborts a *second* `DOUBLE MMU FAULT` (PC 0f0dd7f6,
fault 0x0f1f03fc, translation still on).  On `apollo-domainos` this branch
now carries Fable's full 030/68851 PMMU-core fixes (merged from `amiga`:
PTEST An writeback, PMOVEFD TLB flush, SRE-tagged ATC, interrupt mask) —
they do **not** change this wall (AEGIS still reaches the banner; the same
second fault follows).

**Both of Fable's hypotheses for this wall are DISPROVEN** by
instrumentation:
- *Not* the DF-cleared software-completion gap (finding 7 / gap 1): AEGIS
  **never clears SSW.DF** (`m68k_rte`'s suppress path never triggers;
  `bus_error_suppress` stays 0), so it is not trying to software-complete
  the faulted cycle.
- *Not* an unhandled indirect page descriptor (gap 9): the walk of
  `0x0f38b0a0` returns via `DT_INVALID` at level 3, not the table-type
  (indirect) leaf path — the page is genuinely unmapped and the walker is
  correct.

Mechanism: the primary fault is a `jsr` **return-address push at PC
0x0f245772** with **SP≈0x0f38b0a4 in an unmapped page**; the CPU vectors to
vector 2 (access fault), but PC ends up executing a **non-code region at
0x0f0dd3e8** (`movepw`+zeros+pointer longs; next page 0x0f0dd400→phys
0x171000 is zeros), NOPs into 0x0f0dd6xx, derefs `a3=0x0f38b0a0`, and nests
until the ISP overflows.

**Verified NOT a PMMU bug.**  Comparing the runtime `cpu_ldl` vs the DEBUG
walk at the vector-2 delivery (the method that caught the TARGET_PAGE_BITS
bug) shows they agree: `handler=0f0dd3e8 == vecval(dbg)`, and the handler
VA→phys (0x25a3e8) is consistent.  The vector fetch and translation are
correct; the defect is that phys 0x25a3e8 (which is **above the loaded image**
0x1e8bff, so populated by AEGIS init) holds **garbage/zeros instead of the
handler code** — a memory-content corruption, not a translation error.

**It's a page-table-build timing defect.**  A store-trace shows the fault
happens while AEGIS runs its **page allocator** (`jsr 0x0f22c614` in the
loop at `PC 0x0f2011xx`) handing out freshly-zeroed 1 KiB pages: VA
0f0dd000→phys 0x25a000 (the vector-2 handler page) and 0f389c00/0f38a000/
0f38a400→0x25a400/800/c00 (stack pages).  The faulting stack page 0f38b000
(SP≈0f38b0a4) is just past this still-growing mapping and unmapped, and
vector 2 = 0x0f0dd3e8 points into a just-allocated zero page (handler not
copied in yet).  So a fault taken *mid-build* dispatches through a not-ready
handler → storm.  A `-d int` trace shows the vector-0xa0 **timer IRQ fires on
a separate, mapped stack (sp=0f1af7f8)** just before the fault, so the IRQ
handler itself is *not* the faulting context; the first fault is the
page-build's own `jsr 0x0f245772` push with **SP=0f38b0a4 in the unmapped
0f38b000 page** — i.e. the build is *using* a stack whose page it has not yet
mapped.  The IRQ cadence + input jitter only *modulate* the timing of the
race (SP-use vs SP-page-map).  Apollo-local (page-build stack mapping /
ordering), not PMMU.

**NOT a QEMU store/TLB-coherence bug.**  The faulting page's leaf descriptor
is at phys 0x002088b0 (walk: L0 001015e4/0021620b → L1 00216388/001ffd0a(long)
→ L2 002088b0/00000000).  Polling 0x002088b0 every tlb_fill shows it stays 0
the whole boot — AEGIS NEVER writes a valid descriptor for 0f38b000, so there
is no dropped/stale store and nothing for a negative TLB to cache wrong; the
softmmu is correct.  Adjacent stack pages ARE mapped in the same L2 table
(0f389c00/0f38a000/0f38a400→0x25a400/800/c00); only the *top* stack page is
missing — a setup ordering/off-by-one, timing-exposed.

**Now deterministic.**  The RTC preset removes CALENDAR input jitter → crash
every run (fast, any `-icount shift`).  With CALENDAR driven, ~1 in 4 runs
miss the fault window and AEGIS boots to `BOOT VOLUME NEEDS SALVAGING.
Proceed to bring up OS (and risk volume)?`.  **Ruled out**: PMMU/softmmu
(runtime==debug), a dropped/stale store (descriptor never written), and the
**PTM6840 rate** — PTM0 is a normal 60 Hz (latch=4166/250 kHz), so the ~14
vector-0xa0 IRQs before the fault are ~230 ms of guest time, not a storm.  The
remaining root is a subtle setup ordering (SP set to the top stack page before
it's mapped / before the handler is installed) whose taken-vs-not branch
depends on device-read timing our emulation gets slightly off — bisect the
setup from PC 0x0f2011xx.  Or ride the good path and answer `Proceed to bring
up OS? y`.
(Build caveat: a leaf-walk `fprintf` of the possibly-uninitialised
`desc`/`table` trips `-Werror=maybe-uninitialized`, and ninja then silently
reuses a stale binary — always `strings qemu-system-m68k | grep '\[MARK\]'`
to confirm instrumentation actually compiled in; this invalidated several
runs before it was caught.)

---

## 7. UPDATE — post-banner wall re-characterised: an early IDLE then a ~1-minute stall, THEN the storm (NOT purely a page-build push race)

Fresh session, built clean from `apollo-domainos` HEAD (b2ad382).  Reproduced
the whole pipeline from scratch — build → download+`gunzip` the five SR10.4
`.ct` tapes (boot-tape SHA1 `4b7e6415…` matches §HOWTO) → blank 155 MB `.awd`
→ `EX INVOL` options **7/1/8 all run to completion** ("Initialization
complete.", "Done.") writing a valid PV label (checkpoint
`/tmp/domainos.awd.invol_done`).  Then drove `EX DOMAIN_OS` many times.

### The wall is DETERMINISTIC here — the notes' "~1 in 4 good path" did NOT reproduce
Across **13+ attempts** on the invol'd disk I never once reached
`BOOT VOLUME NEEDS SALVAGING / Proceed to bring up OS?`:
- minimal driver (no CALENDAR), invol'd disk, with natural wall-clock jitter
  **plus** an explicit *N*-carriage-return phase perturbation, at `-icount
  shift` 5/6/7 → **9/9 silent** (idle→storm).
- CALENDAR-driven driver (the notes' stated good-path recipe), invol'd disk →
  **4/4 silent**.
None printed the salvage prompt.  So in this environment the good path is
either far rarer than 1-in-4 or host-load-sensitive (the only non-determinism
under `-icount` is the guest-cycle at which typed serial bytes land) in a way I
could not tickle.  (Blank/un-invol'd disk instead crashes **earlier** and
cleanly at `Crash_Status 80080011 PC 0F23C6AE pid 0001`, 4/4 — AEGIS can't
mount a blank volume; this is *before* the idle/storm point, so a blank-disk
"pass" is NOT the page-build race being won.)

### What actually happens after the banner (from a full `-d int` capture)
`-d int` over a deterministic invol'd boot shows a very different shape than
"fault mid page-build":

1. AEGIS prints the banner, does ~19 interrupts of setup, then **enters an
   IDLE/HALT loop at VA `0x0f22d4dc`** (disassembled live):
   ```
   0f22d4dc:  andiw #-1793,%sr      ; -1793 = 0xF8FF -> force IPL 0 (ints on)
   0f22d4e0:  bras  0x0f22d4dc      ; spin
   ```
   i.e. the classic "nothing runnable — wait for an interrupt" scheduler/idle
   spin.  It reaches this **early** (by INT ~19), *not* deep in a page build.
2. While idling, **OMTI/Winchester completion IRQs fire ~11× (vector `0x2b8`
   = 0xae = slave IR6 = IRQ14), INT 19–30, then cease**; thereafter only the
   60 Hz PTM timer (vector `0x280` = 0xa0) ticks.  So the disk I/O the boot
   process was blocked on *does* complete — but AEGIS never advances (never
   prints salvage/mount output).
3. AEGIS then **idles ~3763 timer ticks ≈ 62 s of guest time (~1 minute)**
   doing no useful work.
4. Only THEN does the first real fault fire: **Access Fault at `PC
   0x0f245772`** (on the interrupt stack, sp≈`0f1f0ea8`).  It vectors to the
   vector-2 handler at VA `0x0f0dd3e8`, which is an **all-zero page** — exactly
   as prior notes found.  Execution runs through the zeros (disassemble as
   `orib #0,%d0` NOPs) up to a stray `movepw %a5@(-10424),%d7` at
   `0x0f0dd7f4/f6`, which re-faults; the handler **nests**, driving the ISP
   down `0f1f0ea8 → 0f1f043c` into the unmapped `0x0f1f0000` page →
   QEMU `cpu_abort("DOUBLE MMU FAULT")` (`target/m68k/op_helper.c:385`).

### Re-interpretation (lead for whoever continues)
The dominant feature is the **~1-minute idle-then-fault**, with disk
completions arriving but the boot process never resuming.  Combined with
AEGIS's known "calendar is more than a minute slow" logic, this looks like an
**early boot task blocked on an async event whose completion does not wake it
(a device-completion → scheduler-wakeup miss, or a ~60 s clock/watchdog
timeout)** rather than (only) a stack-page-mapping race.  The vector-2 handler
for this early phase is not yet populated (still zeros), so when the
timeout/fault finally fires it is fatal → storm → double fault.  This is the
same *class* as the earlier INVOL OMTI-completion-semantics bugs (§6): an
interrupt/completion the guest expects is not delivered in exactly the state it
polls for, so it waits.  Prime suspects: (a) an OMTI completion AEGIS's async
driver waits for after the initial 11 that never comes, or (b) MC146818 RTC
advance vs. AEGIS's boot clock check.

### The decisive missing datum + why it was hard to get
The single most useful next fact is the **disassembly of `0x0f245772` and the
address it faults on**.  I could not capture it:
- QEMU **hard-aborts (`cpu_abort` → exit)** on the double fault, so gdb can't
  inspect the crashed state.
- The guest simulates the ~62 s idle in **< 5 s wall** (icount races ahead;
  `-icount shift=6,sleep=on` did not throttle it enough here), so the
  idle→abort window is too short to reliably attach gdb (one lucky attach under
  heavy host load *did* let me disassemble `0f22d4dc`/`0f0dd7f6`, but it is not
  repeatable).
- **Hardware breakpoints via the m68k gdb stub did NOT stop the guest** in this
  build — only post-hoc `x/i` memory reads on an already-stopped/idling guest
  worked.
Recommended next step: a **committed** debug switch that, on the double MMU
fault, **halts the CPU and stays gdb-attachable instead of `cpu_abort`ing** (a
clean `cs->halted=1` that survives the pending-IRQ re-fault loop — my quick
`EXCP_HLT`+`cpu_loop_exit` attempt made QEMU exit, so it needs care), or dump
the loaded AEGIS image once (`0x101c00..0x1e8c00`) during the idle window and
disassemble `0f245772`/`0f22d4dc`/`0f0dd3e8` offline.

### Environment gotcha
A **concurrent `sun3x-sunos` task on the same VM periodically `kill`s every
`qemu-system-m68k` process**, which was silently killing these runs.  Worked
around by running the built binary copied to **`qemu-apl68k`** (so its
`grep qemu-system-m68k` cannot match); anyone iterating here should do the
same.  No code changed this session (docs-only); tree left build-clean.

---

## 8. UPDATE — decisive datum captured: the fault is an unbounded per-tick enqueue overrunning into an unmapped page while boot is stalled (AEGIS-internal, NOT a device-model miss)

Added a committed, env-gated debug affordance and used it to park the guest AT
the fault and inspect it (previously impossible — QEMU `cpu_abort`s on the
double fault and the idle→abort window is too short to attach).

### Debug switches added (all zero-effect when the env var is unset)
- `target/m68k/op_helper.c` (the `EXCP_ACCESS` handler):
  `APOLLO_DFHALT` parks the CPU in a **gdb debug stop** (EXCP_DEBUG +
  `cpu_loop_exit`) on the *double* fault instead of `cpu_abort`ing;
  `APOLLO_FAULTPC=<hex>` parks on the **first** access fault taken at that
  faulting PC (clean pre-storm state).  The parked CPU is fully
  gdb-inspectable (registers, memory, disassembly) over `-gdb`.
- `hw/m68k/apollo.c`: `APOLLO_DEVTRACE` logs every OMTI command / OMTI+SC499
  IRQ; `APOLLO_OMTI_LAT_NS` overrides the 20 µs OMTI completion-IRQ latency.
(Confirm compiled in with `strings qemu-system-m68k | grep 'park EXCP_ACCESS'`.)

### The first fault, captured (`APOLLO_FAULTPC=0f245772`, then gdb on :1245)
```
[DFHALT] park EXCP_ACCESS pc=0f245772 fault_ar=0f38b0a0 ssw=0145 a7=0f1f0ea8 mmu.fault=0
a2=0f38b080  a3=0f38b0a0   <- fault address = a3, a DATA fault (ssw DF=1)
```
So the fault is a **data dereference of `a3 = 0x0f38b0a0`** in the unmapped
`0x0f38b000` page — sp (`0f1f0ea8`) is fine, it is **not** a stack-push fault.

The faulting routine (entry ~`0x0f2456d0`) is an **enqueue into an `a5`-based
global ring buffer**:
```
0f2456e6: movew %a5@(1244),%d1        ; index
0f2456ec: movew %d3,%a5@(1e,%d1:l:2)  ; store into slot
0f2456f0: cmpiw #256,%a5@(1244)       ; wrap the 1244 index at 256
0f2456f8: movew #1,%a5@(1244)
0f245704: addqw #1,%a5@(1248)         ; ALSO bump a *separate* counter 1248
0f245708: movew %a5@(1248),%d1
0f24570e: addl  %a5@(16),%d1          ; slot ptr = base a5@(16) + 1248-counter
0f245712: movel %d1,%fp@(-12)         ; ... eventually loaded into a3 and deref'd
```
`a5@(1244)` wraps at 256, **but the byte offset `a5@(16) + a5@(1248)` grows
without bound**.  Base `a5@(16)` sits in the mapped `0f38a000` region; after
~3758 increments the slot pointer reaches `a3 = 0f38b0a0`, i.e. **it overruns
the end of the mapped page-database array into the unmapped `0f38b000`
page**.  Adjacent array pages (`0f389c00/0f38a000/0f38a400`) are mapped — only
the page it grows *into* is not, exactly matching §6's boundary observation.

Call chain at the fault (walked via the fp links):
`0f245772` ← `0f216f32` (helper `0f216f02`) ← `0f217398` (a `dbf` loop
iterating a table of objects, `a2 += 4` each step, calling the helper per
entry).  It's kernel eventcount/queue bookkeeping keyed off `a5` (global),
with a lock at `a5@(544)` (`jsr 0xf219318`).

### Why the fault is *fatal* (the storm), confirmed
Vector 2 (access fault) `= *(VBR=0 + 8) = 0x0f0dd3e8`.  Read live, `0f0dd3e8`
holds **non-code data** (`movepw %sp@(30552),%d7` then `orib #0` = zero words,
`0f0dd400+` all zeros) — it is **not a valid access-fault handler**.  So the
otherwise-recoverable demand fault at `0f38b0a0` dispatches through garbage,
NOPs down to the stray `movepw` at `0f0dd7f6`, re-faults, nests on the ISP
(`0f1f0ea8 → 0f1f043c`) into the unmapped `0f1f0000` page → `cpu_abort`.

### It is NOT a device-model missing-completion (ruled out three ways)
- **`APOLLO_DEVTRACE` timeline:** AEGIS issues the boot disk reads (CDBs `01`,
  `00`, `ec`, then `1e`/`0e` READ_DATA_TO_BUFFER + READ_SECTOR_BUFFER pairs at
  scattered LBAs `41dd/42dd/251ff/44df/64ae0`, then LBA 0) — **every read
  completes and every completion IRQ is delivered**; the sequence ends
  cleanly with the buffer drained by PIO.  No OMTI command is left pending.
- **Interrupt histogram over the whole boot** (`-d int`): vector `0x280`
  (0xa0 = 60 Hz PTM timer) ×3758, `0x2b8` (0xae = OMTI IRQ14) ×11 (all early),
  `0x284` (0xa1 = SIO) ×1.  **No RTC (IRQ8), no SC499 (IRQ5)** — AEGIS awaits
  no device interrupt during the idle; only the timer ticks.
- **`APOLLO_OMTI_LAT_NS` sweep** 2 µs → 20 ms (9 values, deterministic,
  minimal driver): **9/9 still storm.**  The wall is completely insensitive to
  disk-completion timing.

### What is actually happening (mechanism) and classification
AEGIS boots, does its disk reads, then **parks in the IPL-0 idle spin
(`0f22d4dc`) waiting for a software event that never arrives in this run**.
While parked, a **per-timer-tick enqueue** keeps bumping the unbounded
`a5@(1248)` offset; after ~3758 ticks (~62 s) the slot pointer overruns the
mapped page-database array into `0f38b000` → demand fault → storm (vector-2
handler not valid code in this early phase).  So the ~62 s "≈1 minute" is
**not a watchdog timeout — it is how long the per-tick counter takes to walk
off the end of the array while boot is stalled.**  On the notes' ~1-in-4 good
runs the awaited event is satisfied (serial-input jitter shifts fine-grained
instruction interleaving) and boot advances to `NEEDS SALVAGING` before the
counter overruns.

**Classification: AEGIS-internal, timing-sensitive.**  Not a device-model
completion/IRQ we drop (disk path proven correct; no awaited device IRQ; wall
insensitive to OMTI latency across 4 orders of magnitude).  The two things that
make it fatal rather than merely slow are AEGIS-internal too: (a) the enqueue
offset `a5@(1248)` is not bounded/wrapped like its sibling `a5@(1244)`, and
(b) the vector-2 access-fault handler at `0f0dd3e8` is not valid code yet, so
the array-overrun demand fault cannot be serviced.  Pinning the exact software
event AEGIS blocks on needs symbol-level RE of the SR10.4 kernel (`a5`-global
eventcount/queue subsystem around `0f216f02`/`0f2456d0`/`0f219318`), which is
the recommended next step; there is no device-model knob that advances it (the
OMTI-latency sweep proves this).

### Status
Domain/OS reaches the kernel banner + full early init incl. the boot-volume
disk reads, then stalls as above; it does not pass the wall (salvage prompt)
in this environment.  Debug switches committed (useful for the symbol-RE next
step).  Linux `-kernel` and the PROM MD prompt re-verified unaffected with the
switches off (all env-gated).  Tree left build-clean.

---

## 9. UPDATE — symbol-level RE of the wake path: the wall is AEGIS-internal (a boot-sync stall), NOT device-model or config; device/console/strap causes ruled out

Followed the wall into AEGIS's kernel with the park switch (`APOLLO_FAULTPC=0f245772`
→ gdb on :1245), dumped the loaded image, and disassembled the eventcount/queue,
scheduler, ISR-dispatch and lock primitives.  Added one more env-gated knob
`APOLLO_RAM_CFG` (overrides the SIO RAM-config strap) for the config test below.

### The device-completion angle is definitively dead (four independent rulings)
1. **ISR dispatch table is generic.** The shared device ISR `0f23545c` dispatches
   by vector through a table at `0f235484`; **every** device-vector entry
   (RTC 0xa8, OMTI 0xae, …) points at the *same* generic handler `0f20dcb0` —
   there is **no device-specific ISR that advances an eventcount**.  AEGIS's
   standalone disk driver is **polling** (as the PROM's is), so a disk completion
   has no eventcount/callback side-effect to get wrong.
2. **No device IRQ is awaited during the idle** (`-d int` histogram over the whole
   boot: only vector 0xa0 timer ×3758, 0xae OMTI ×11 early, 0xa1 SIO ×1; no RTC,
   no SC499).
3. **OMTI completion-latency sweep** `APOLLO_OMTI_LAT_NS` 2 µs→20 ms: 9/9 storm.
4. The last disk read completes and is PIO-drained before the stall.

### The config-strap angle is dead too
`APOLLO_RAM_CFG` sweep of the SIO input-port byte (0x00, 0x11, 0x20, 0x22, 0x2a,
0x33, 0x55, 0xaa, 0xff — diverse bit patterns): **all 8+ still banner→storm, no
change.**  So no presence/size bit in the SIO strap gates the wakeup.  Console
input is not it either: spamming the serial console during the idle window — AEGIS
**echoes** the characters (its console driver is live) but the boot stays blocked
and still storms at ~62 s.

### What the fault actually is (fully reconstructed)
`APOLLO_FAULTPC=0f245772` park + gdb: `fault_ar=0f38b0a0 ssw=0145 a7=0f1f0ea8
mmu.fault=0`, **a3=0f38b0a0** (fault addr), a2=0f38b080 — a **data deref of a3** in
the unmapped `0f38b000` page.  The routine is an **enqueue into an `a5`-global
buffer** (a5=`0f298258`): it stores into a 256-entry word ring `a5@(0x1e + idx*2)`
whose index `a5@(1244)` correctly wraps at 256 (= the segment count at global
`0f22714e` = 0x100), **but it also grows a *second*, unwrapped byte counter
`a5@(1248)` and dereferences `a5@(16) + a5@(1248)`** — that pointer walks off the
end of the mapped array into `0f38b000`.  Call chain (all on the **ISP**, i.e.
timer-interrupt context): `0f245772 ← 0f216f32(0f216f02 helper) ← 0f217398
(a dbf table-walk over 256 objects, `pea/jsr 0f219318` lock) ← 0f201f1c`.  Vector 2
(access fault) = `*(0x8)` = `0f0dd3e8`, which holds **non-code data** (`movepw`,
pointer longs, then zeros) — so the demand fault storms instead of being serviced,
nesting the ISP `0f1f0ea8→0f1f043c` into the unmapped `0f1f0000` page → double
fault.

### Where the main thread blocks (from a bounded `-d exec` capture)
Traced the transition **into** the idle spin (`0f22d4dc`): TIMER ISR `0f2302xx`
→ scheduler `0f20d0xx/0f20d1xx` → context switch `0f22c8xx/0f22caxx` → dispatch to
the **idle task** `0f22d4cc` (which poisons all regs to -1, `andi.w #$F8FF,sr`,
spins).  Immediately *before* the idle, the main thread runs a boot loop of
**disk-read (`0f21bcxx`) + enqueue (`0f2456xx`, the same routine that later
faults) + table-walk (`0f217axx/0f217bxx`) + lock (`0f2192xx/0f2193xx`)** — i.e.
it is **mounting the boot volume / building the segment (AST) table**, enqueuing
one entry per on-disk object into the `a5` buffer.  When that loop finishes it
enters AEGIS's lock/critical-section primitives (`0f218e02` does
`ori.w #$0700,sr` = mask all IRQs) and then **stops being runnable** — the very
next scheduler pass (driven by the timer) finds nothing to run and idles.  So the
main boot thread **blocks/completes into a wait after volume-mount and never
becomes runnable again**; the timer-driven path keeps bumping `a5@(1248)` until it
overruns at ~62 s.

### Classification: AEGIS-internal, timing-sensitive — no device/config fix exists
Every external cause is ruled out (device ISR/completion, OMTI latency, SIO
RAM/presence strap, console input).  The wakeup AEGIS waits for is a **software
event/task-dispatch internal to its own boot synchronization** (the eventcount/AST
subsystem around `0f216f02`/`0f2456xx`/`0f219318`), which in our **deterministic**
run (RTC preset, no input jitter) never fires before the `a5@(1248)` overrun turns
fatal.  The prior notes' "~1 in 4 with CALENDAR jitter reaches salvage" was **not
reproducible here in ~25 runs** across minimal/calendar/console-spam drivers and
all the sweeps above — consistent with a **narrow, host-timing-dependent race in
AEGIS's own boot**, not a defect we can satisfy from the machine model.  The two
things that make the stall *fatal* rather than merely a hang are also AEGIS-side:
the unwrapped `a5@(1248)` counter, and the not-yet-installed vector-2 handler at
this early phase.

**Bottom line for the coordinator:** there is **no device-model or config bug to
fix** for this wall — the OMTI/SC499/DMA/interrupt models are behaving correctly
(disk is polled; the one interrupt that matters, the 60 Hz timer, is delivered
correctly).  Advancing past it requires either (a) hitting AEGIS's internal
boot-sync race by timing (the elusive good path), or (b) full symbol-level RE of
the SR10.4 eventcount/scheduler to identify and force the specific wait — a large,
separate effort.  Debug affordances (`APOLLO_DFHALT`/`APOLLO_FAULTPC`/
`APOLLO_DEVTRACE`/`APOLLO_OMTI_LAT_NS`/`APOLLO_RAM_CFG`, all env-gated, zero-effect
off) are committed for that follow-on.  Tree left build-clean; Linux/PROM
unaffected.
