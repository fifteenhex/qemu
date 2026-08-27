# m68k PMMU deep-dive: 68030 on-chip PMMU / MC68851 audit

Scope: the PMMU emulation shared by the 68030 (sun3x, hp300, Macs, mvme147)
and the 68020+MC68851 (apollo-dn3000) — `target/m68k/helper.c`
(`get_physical_address_030`), `op_helper.c` (exception frames),
`translate.c` (PMOVE/PTEST/PFLUSH decode), `cpu.h`/`cpu-param.h`.
Driven by the sun3x SunOS-PROM non-determinism; unified with the Apollo
Domain/OS (AEGIS) findings.  Fixes are committed on branch `pmmu-audit`
(`5a154b8910` + cherry-pick `77ff0d114e` of apollo's `256455f764`).

Validation summary (all with the full fix stack):

| Target | Before | After |
|---|---|---|
| sun3x + genuine 3/80 PROM 3.0.3 | `>` reached only sometimes; auto-boot died with a garbage-address bus error (`Vaddr FEF74000, Paddr FEF82000 ... at 0xFEFE0934`); occasional `DOUBLE MMU FAULT` abort (NMI vectors through 0) | Monitor-entered runs reach `>` **every** run, unattended (103/103 NMIs vector to the real handler `0xfefe04fe`); auto-boot probes devices cleanly (`le: cannot initialize` … `No bootable devices found` → `>`); no double faults in ~40 runs |
| hp300 Linux (68030, 4 K pages) | boots | still boots to userspace shell |
| Apollo AEGIS `EX DOMAIN_OS` (68851, 1 K pages) | banner (with apollo's page-bits fix) | still reaches the Domain/OS 10.4 banner on this branch; second wall unchanged (see finding 7) |
| Apollo Linux `-kernel` | serial output `ABCPGHIJK` (fb console) | byte-identical |

---

## 1. CONFIRMED + FIXED — 030/851 PTEST ignored the LEVEL and A/An fields
**The sun3x root cause.**  `target/m68k/translate.c` (`pmmu030`, case 4) and
`HELPER(ptest030)` (`target/m68k/helper.c`) decoded PTEST but discarded the
extension word's level (bits 12:10) and A/An (bits 8, 7:5) fields: the walk
always ran to the leaf, and **An was never written back**.

Per the MC68030 UM (PTEST; identical on the MC68851), with A=1 the
instruction returns **the physical address of the last table descriptor
fetched** in An, stops after LEVEL descriptor fetches (level 0 probes the
ATC only), and the PSR reports N (levels searched, bits 2:0), M (modified,
bit 9) and T (transparent hit, bit 6) besides B/L/S/W/I.

Why it wrecked the Sun-3/80 monitor: the PROM's *entire page-table
read/modify API* is built on `ptestr #7,%a0@,N,%a0`:

- `fefe081e/0864/08ac` — get level-3/1/2 descriptor: PTESTR with A=1,
  then map the 8 KiB page containing the returned descriptor address at
  scratch window `0xfef74000` and read it;
- `fefe08f2/093a/0988` — the corresponding setters (write the descriptor
  through the window);
- callers include the early monitor init (`fefe034a`), the auto-boot
  device mapping loop (`fefe4578`), and the monitor `map` machinery.

With An left holding the *probed virtual address*, every monitor page-map
update targeted a garbage physical page.  Even the baseline "success"
transcript shows it: `Auto-boot ... Timeout Bus Error: Vaddr: FEF74000,
Paddr: FEF82000, Write, FC 5, Size 4 at 0xFEFE0934` — PC 0xFEFE0934 is the
level-3 setter's descriptor write through the mis-mapped window.  Whether a
given monitor context ended up with a valid interrupt stack / clock mapping
depended on what those wild writes hit — the reported context-sensitive
stalls and double-faults.

**Fix** (committed): `get_physical_address_030()` takes a `PTest030Info`
(max level, levels walked, last descriptor address); `HELPER(ptest030)`
returns the new An, sets PSR N/M/T, and implements level 0 as an
atc030/TTR probe; translate.c writes An back when A=1.
After the fix the monitor's device-probe/auto-boot path runs correctly and
`>` is reached deterministically (given the monitor is entered at all —
see finding 8 for the two remaining *board* nits).

## 2. CONFIRMED + APPLIED — `TARGET_PAGE_BITS` must not exceed the guest page size
Apollo finding, commit `256455f764` (cherry-picked here as `77ff0d114e`):
AEGIS runs the 68851 with **1 KiB pages** whose frames are not 4 KiB
aligned; `tlb_set_page()` masked the frame to the 4 KiB TARGET_PAGE and
every access in the page read the wrong physical address (vector at
0x101808 read as 0x101008) → double-fault.  `TARGET_PAGE_BITS` 12 → 10.

sun3x impact: **not** the sun3x root cause — the 3/80 monitor runs
TC=0x80D07660 (PS=0xD → 8 KiB pages, IS=0, TIA/TIB/TIC=7/6/6), frames 8 KiB
aligned.  Verified no regression for 8 K (sun3x) and 4 K (hp300) guests.

Residual risk (documented, not hit by any current guest): the 030
early-termination path (`helper.c`, "Early termination" block) only
requires the descriptor address to be **256-byte** aligned
(`table &= 0xffffff00`).  A guest using an early-terminated mapping whose
frame is aligned to less than 1 KiB would still be misrepresented at
TARGET_PAGE granularity.  If ever hit, the same class of fix applies
(smaller TARGET_PAGE, at a TLB-density cost).

## 3. CONFIRMED + FIX PROVIDED (board, sun3x.c) — NMI delivered before the monitor's level-7 vector exists
The **remaining sun3x double-fault** after fix 1: instrumented delivery
showed the crash run's very first clock NMI arriving with the translation
*correct* (`vbr+7c=fef60c7c → phys 00ffec7c`, exactly the monitor's live
tables) but the **vector content still 0** — the board's `clk_enabled`
heuristic (clock-register mapping visible) latches a tick or two before
the monitor copies its vector table into the currently-mapped vector page.
The CPU vectors through 0 → wild execution (`0xf400`, `0x1e800`) → F-line
storm at `0x1fffc` → ISP marches below `0xfef60000` → double bus fault
(`qemu: fatal: DOUBLE MMU FAULT`).  Real hardware has no such window: the
monitor only enables the clock chip's interrupt after installing the
handler.

**Fix** (validated here; belongs on the `sun3x-sunos` branch — patch saved
at `/tmp/sun3x-nmi-vector-gate.patch`): in `sun3x_update_irq()`, only
assert level 7 while the *MMU-translated* vector at `VBR+0x7c` holds a
non-zero handler:

```c
bool vec_ok = false;
if (s->clk_enabled) {
    CPUM68KState *env = &s->cpu->env;
    hwaddr vph = m68k_cpu_get_phys_addr_debug(CPU(s->cpu), env->vbr + 0x7c);
    if (vph != -1) {
        vec_ok = ldl_be_phys(&address_space_memory, vph) != 0;
    }
}
qemu_set_irq(qdev_get_gpio_in(s->irqc, 7 - 1),
             s->clk_pending && s->clk_enabled && vec_ok);
```

With this gate + fix 1, no double fault was seen again and the countdown
completes every monitor-entered run.

## 4. CONFIRMED + FIXED — PMOVEFD (FD=1) skipped the QEMU softmmu TLB flush
`translate.c` only called `pmmu030_flush` when the PMOVE extension's FD bit
was clear.  FD ("flush disable") preserves the guest-visible ATC — but the
QEMU TLB also caches what the real ATC never held: **identity mappings
installed while translation was disabled** (real hardware bypasses the ATC
entirely with TC.E=0, so nothing stale can survive an enable) and an
unbounded number of pages versus the real 22-entry ATC.  The Sun PROM
performs *every* TC/CRP/SRP load with `pmovefd` (flushing separately with
`pflusha` — see `fefe029a..02aa`: `pflusha; movel #0x80D07660,%a0@;
pmovefd %a0@,%tc`), so pages touched in the flush→load window (including
data pages) kept their MMU-off identity translations into MMU-on execution.

**Fix** (committed): the FD form now calls a new `pmmu030_flush_fd` helper
that flushes only the (guest-invisible) QEMU TLB and ends the TB; atc030
is preserved, so refills after the flush still serve the old translations
— which is exactly what FD means architecturally, and keeps MacOS's
SwapMMUMode `PMOVEFD %crp` + `PMOVE %tc` transient working.

## 5. CONFIRMED + FIXED — atc030 not tagged by root pointer (TC.SRE aliasing)
With TC.SRE set, supervisor walks go through SRP and user walks through
CRP, and the real ATC tags entries by function code.  Our atc030 lookup
matched on vaddr only, so a supervisor and a user translation of the same
address aliased (served each other's physical page/protection).  Not hit
by the Sun PROM (its TC has SRE=0) but a live hazard for any SRE-using
guest (AEGIS is the obvious candidate as it exercises more of the 68851).
**Fix** (committed): entries carry an `sroot` tag; lookup/insert/level-0
PTEST all match on it.  No behavior change when SRE is clear.

## 6. CONFIRMED + FIXED — hardware interrupt entry OR-ed the SR mask
`m68k_interrupt_all()` did `sr |= (env->sr & ~SR_I) | (level << 8)`:
the old I field was never cleared, so the new mask was `old | new` (e.g.
level 5 at mask 3 → mask 7), wrongly blocking intermediate levels until
RTE.  (The ColdFire path had it right.)  **Fix** (committed):
`sr = (sr & ~SR_I) | (level << SR_I_SHIFT)`.  Not the sun3x root cause
(the monitor mostly runs at mask 7 anyway) but wrong for every m68k
machine.

## 7. DIAGNOSED (not fixed) — 68020/030 bus-fault frame completeness
Audit of the format $A/$B frames (`op_helper.c`, `EXCP_ACCESS`, non-040
branch) against the MC68030 UM §8.2, relevant to Apollo AEGIS's post-banner
demand-fault storm and to any handler that does more than "map page and
RTE":

*Correct today*: frame layout/offsets and sizes (A = 32 bytes, B = 92
bytes); SR/PC/format/vector; data-cycle fault address (`env->mmu.ar`,
offset 0x10); SSW DF (0x0100), RW (0x0040), FC2:0; FC/FB heuristics for
instruction faults; RTE understanding of both formats including the
DF-cleared "complete without rerun" contract; frame-B recognition by
Linux/NetBSD-style handlers (hp300/mac Linux demand paging works).

*Gaps* (in priority order for AEGIS):
1. **DF-cleared software completion only works for physical bus errors.**
   `m68k_rte` sets `bus_error_suppress` when the handler cleared SSW.DF,
   but only `m68k_cpu_transaction_failed()` honors it.  The **MMU-fault
   path (`m68k_cpu_tlb_fill`) raises `EXCP_ACCESS` unconditionally** — a
   handler that clears DF on a *page* fault (expecting the faulted cycle
   to be skipped/emulated) re-faults on the same access forever.  That is
   precisely an "every faulting word re-faults, ISP overflows" signature.
   Recommended fix: check `env->bus_error_suppress` in the tlb_fill fault
   path and complete the access as an unassigned cycle (and clear the
   flag), mirroring the physical-bus-error path.
2. **Data output buffer (offset 0x18) is pushed as 0.**  A handler that
   software-completes a faulted *write* reads the write data from there;
   we should push the store value (available at fault time in the
   tlb_fill/store path if plumbed through `env->mmu`).
3. **Data input buffer (offset 0x2C, format B) is ignored on RTE.**  A
   handler that software-completes a faulted *read* deposits the data
   there and clears DF; the rerun currently completes as an unassigned
   read instead of using the handler's value.
4. **SSW RC/RB (0x2000/0x1000), RM (0x0080) and SIZE (0x0030) are never
   set**, and the instruction-pipe stage B/C words (0x0E/0x0C) and stage-B
   address (0x24) are zero.  Handlers keying on operand size or
   read-modify-write (TAS/CAS) semantics mis-decode the fault.

Suggested next step for the Apollo agent: breakpoint AEGIS's access-fault
handler and observe which of these frame fields it reads and whether it
clears DF — that decides whether gap 1 (most likely) or 2–4 is the second
wall.  None of these affect the sun3x monitor (its bus-error handler only
prints the frame and RTEs to the prompt loop).

## 8. Remaining sun3x nits (board scope, pre-existing, NOT the PMMU)
- **Parity self-test wall**: with no console input, the full self-test
  reaches the "Parity Memory Forced Error Test" stage-2, which requires a
  modelled level-7 parity NMI + parity error register 0x07
  (`error1: no parity error (level 7) interrupt occurred`).  The PROM
  stops polling input there, so a run that missed the Esc abort cannot be
  recovered.  Needs parity-error injection modelling in sun3x.c to pass
  path A; path B (Esc abort) is unaffected.
- **Esc-abort acceptance race**: the PROM polls the zs for the abort key in
  narrow windows; whether an externally-timed keypress lands is host-load
  dependent (and with `-icount` it deterministically misses).  Harness
  artifact — the interactive monitor accepts input fine once at `>`.
- **TOD "Oscilator is not running!" loop** (rare, seen on baseline too):
  the self-test TOD check is sensitive to host timing of the RAM-backed
  Mostek seconds; occasionally loops re-initializing the TOD.  Board
  model, MMU-off phase, unrelated to the PMMU.

## 9. Smaller PMMU gaps noted (no current guest hits them)
- **Indirect page descriptors** (a short/long *table*-type descriptor at
  the last index level, MC68030 UM 9.5.3.3) are treated as invalid; the
  040 path models them (`M68K_PDT_INDIRECT`) but the 030 walk does not.
- **68851 function-code lookup** (TC030 FCL bit) is not implemented — an
  FCL-enabled guest would walk the wrong tree.  Worth a check before
  Domain/OS multi-address-space work.
- **PLOAD** is modelled as a full flush rather than a load; PTEST level-0
  reports from the software atc030 (16 entries vs the real 22, pressure
  differs from silicon).
- PMOVE *to* PSR / TT with FD clear over-flushes (harmless).

## Repro / validation commands
```sh
# sun3x PROM battery (fixed build; Esc-abort within the first second):
qemu-system-m68k -M sun3x -m 16M -bios sun3_80_v3.0.3.bin \
    -display none -serial mon:stdio     # Esc during self-test → countdown → '>'

# hp300 Linux regression:
qemu-system-m68k -M hp9000-340 -m 128M -kernel vmlinux \
    -initrd initrd.cpio.gz -append console=ttyS0 -nographic -icount shift=7

# Apollo AEGIS (needs apollo-domainos board code + invol'd disk):
#   > DI C ; EX CALENDAR (n/UTC/y/1992/02/14/12:30) ; DI C ; EX DOMAIN_OS
#   → "Domain/OS kernel(8), revision 10.4" banner
```
