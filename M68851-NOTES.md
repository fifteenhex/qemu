# MC68851 PMMU emulation for a bare 68020 (branch add-68851)

Goal: give QEMU's m68k a working 68020+68851 (the Apollo DN3000 / Amiga
A2620 / Mac II-with-A/UX-socket configuration) and boot a Linux/m68k
kernel through Linux's rarely-exercised 68851 code path.

## Starting point (consolidated amiga branch)

The amiga branch already models the *68030 on-chip* PMMU completely:

- `get_physical_address_030()` in target/m68k/helper.c: full long/short
  format table walk off `tc030`/`crp030`/`srp030` with limit checking,
  early termination, U/M bit writeback, and a 16-entry software ATC.
- `DISAS_INSN(pmmu030)` in target/m68k/translate.c: PMOVE to/from
  TC/CRP/SRP/PSR/TT0/TT1, PFLUSH/PFLUSHA/PLOAD (ea consumption),
  PTEST via `helper_ptest030`.
- 68020/030-style format A/B bus-error frames in op_helper.c
  (`m68k_interrupt_all`, EXCP_ACCESS, gated on !M68040), and RTE
  support for formats 0xa/0xb.

All of it was gated on `M68K_FEATURE_M68030`. The MC68851's
translation model (TC/CRP/SRP + the same long-format tables) is a
superset of the 030's PMMU minus TT0/TT1, plus DRP, access levels
(CAL/VAL/SCC/AC), breakpoints (BAD/BAC), PCSR and a few extra insns
(PVALID, PFLUSHR, PFLUSHS, PSAVE/PRESTORE, PBcc/PDBcc/PTRAPcc).
Linux only uses the common subset: PMOVE to TC/CRP/SRP, PMOVE from
PSR, PFLUSHA/PFLUSH, PTEST.

## QEMU changes

- `target/m68k/cpu.h`
  - new feature `M68K_FEATURE_M68851` (never set together with M68030).
  - 68851-only register storage in the mmu struct: `drp851[2]`,
    `cal851`, `val851`, `scc851`, `ac851`, `pcsr851`, `bad851[8]`,
    `bac851[8]`. Plain storage: PMOVE round-trips, nothing translates
    through DRP, access levels are modelled as disabled.
- `target/m68k/cpu.c`
  - `m68020_68851_cpu_initfn`: m68020 features + M68K_FEATURE_M68851.
  - registers CPU model **`m68020-68851`** (explicit TypeInfo entry
    because the name contains a '-').
- `target/m68k/translate.c`
  - `INSN(pmmu030, f000, ffc0, M68851)` alongside the M68030 line, so
    the cp-id 0 coprocessor decodes on the 020+851 too.
  - inside `DISAS_INSN(pmmu030)`, gated on the M68851 feature:
    - TT0/TT1 PMOVE now 030-only (the 68851 has no TT registers; Linux
      head.S `mmu_map_tt` skips them on a '020 via `is_020()`).
    - PMOVE to/from DRP (long, 2 words), CAL/VAL/SCC (byte), AC (word),
      PCSR (word), BAD0-7/BAC0-7 (word, breakpoint # in ext[4:2]).
    - PVALID (ext 0x2800/0x2c00+n): consumes its ea, otherwise no-op
      (access levels disabled, so no violation can arise).
    - PFLUSHS ea-form (ext 0x3c00): ea consumed, flushes like PFLUSH.
    - PFLUSHR (ext 0xa000, case 5): consumes the RP-descriptor ea and
      flushes everything.
    - NOT implemented (F-line trap as before): PSAVE/PRESTORE,
      PBcc/PDBcc/PTRAPcc, access-level faults (EXCP_MMU_ACCESS never
      raised since ALs are off). Linux uses none of these.
- `target/m68k/helper.c`
  - `m68k_cpu_tlb_fill`: the `is_030` route (tc030 enable check,
    `get_physical_address_030` walk, 030-style SSW + EXCP_ACCESS on
    fault) now also fires for M68K_FEATURE_M68851. Frame formats A/B
    already come for free because op_helper.c keys on !M68040.
  - `m68k_cpu_get_phys_addr_debug`: previously used the 68040 tcr/table
    walk unconditionally; now uses tc030 + `get_physical_address_030`
    for 030/851 CPUs (fixes gdb/monitor physical translation on those,
    no effect on execution).
- `hw/m68k/virt.c`: when the CPU has the M68851 feature, the bootinfo
  advertises `BI_MMUTYPE = MMU_68851` and `BI_FPUTYPE = FPU_68881` in
  the CPU_68020 branch (Linux requires an MMU type; the 020's FPU is
  the 68881 coprocessor and QEMU's FPU model already emits 6888x
  fsave frames on this branch).

## Linux kernel (vehicle: qemu -M virt)

Kernel tree: /workspace/src/linux-m68kdt (branch m68kdt-a500try). The
tree has an in-tree build owned by its author, so a detached worktree
was checked out to /tmp/linux-68851-src and built out-of-tree to
/tmp/k68851-build (tmpfs).

- Config: `virt_defconfig` + `CONFIG_M68020=y`. CONFIG_VIRT selects
  M68040, but m68k kernels are multi-CPU: with M68020 also enabled the
  kernel picks 020/68851 at runtime from bootinfo (head.S, setup_mm.c).
  No kernel source change was needed for the 68851 path.
- Local tree quirk: commit 31c1b0465b56 "allow umh to be disabled"
  gates kernel/umh.o on a CONFIG_UMH symbol that exists nowhere, so
  CONFIG_FREEZER code (kernel/power/process.c, dragged in by
  CGROUP_FREEZER in virt_defconfig) fails to link
  (`__usermodehelper_set_disable_depth` undefined). Worked around in
  config only: `CONFIG_CGROUP_FREEZER=n`.
- Kernel 68851 path exercised:
  - head.S `mmu_engage_030`: PMOVE to SRP, `pmove %a0@(8),%tc` with
    TC=0x82c07760 (E|SRE, 4K pages, 7/7/6 indexes) -> our tc030 walk.
  - `mmu_map_tt` skipped on 020 (`is_020`), so no TT dependence.
  - context switch: `pmovefd %crp; pflusha` (mmu_context.h) -> FD bit
    handling in the pmmu030 disas keeps cached translations alive.
  - bus errors: format A/B frames -> traps.c `bus_error030`, which
    re-probes with PTEST and reads the PSR.

## Three latent bugs found while getting Linux to userspace

Booting Linux all the way to userspace shook out one machine bug and
two bugs in the pre-existing (MacOS-validated) 030 walk that the
68851 inherits.  Each was diagnosed from a concrete failure:

1. **hw/m68k/virt.c emitted two BI_CPUTYPE bootinfo records** (an
   unconditional CPU_68000=0x20 one first, from this branch's
   "680[01]0 boot info" commit).  head.S `get_bi_record` returns the
   FIRST match, saw a cputype with none of the 020/040/060 bits, fell
   into its "must be a 68030" default and executed `pmove %tt1` -->
   illegal instruction on the 851 (and would have broken plain-040
   `-M virt` Linux boots on this branch too).  Fix: emit exactly one
   BI_CPUTYPE record; CPU_68000 only as the fallback.

2. **The 030 walk set the U bit in INVALID descriptors.**  On a table
   miss the U-bit writeback ran even for an empty (0) descriptor,
   writing 0x8 into it.  Linux's `pud_none()` then no longer saw the
   pgd slot as empty, skipped `__pmd_alloc`, and dereferenced a NULL
   pointer-table base ("BAD KERNEL BUSERR", oops in handle_mm_fault
   reading address 4, page 0 being deliberately unmapped on 020/030
   kernels).  Real silicon does not update U in invalid descriptors
   (their upper bits are software-defined).  Fix: skip the writeback
   when dt == INVALID.

3. **The S (supervisor-only) bit was tested in short-format
   descriptors**, where bit 8 is part of the table/page address.  Any
   user page whose physical address had bit 8 set was spuriously
   super-only: the write retry after a successfully handled fault
   faulted again, PTEST said the page was fine, Linux's bus_error030
   took its "unexpected bus error" branch and execve() died with
   -EFAULT.  S exists only in long-format descriptors; fix moves the
   check into the long-format branch.

4. **Instruction-fetch faults never set SSW FC/FB.**  Linux's
   bus_error030 only treats a fault as an instruction fault if
   `ssw & (FC|FB)` (else it returns having done nothing), and derives
   the fault address as pc+2 (FC) or pc+4 (FB) from the format A
   frame.  With neither bit set, the first fetch of a not-yet-mapped
   user text page looped forever (566k identical EXCP_ACCESS at the
   process entry point).  Fix (op_helper.c, frame build): set FC, or
   FB when only pc+4 lands on the missing page (straddle case).

Items 2-4 change 68030 behaviour too, deliberately: they are
accuracy fixes (matching the MC68851/MC68030 manuals), they were
latent because MacOS runs supervisor-mode with wholesale table
rebuilds, and with them Linux/m68k now boots to userspace on a plain
`-M virt -cpu m68030` as well (it oopsed identically before).
Regression-tested against MacOS: see below.

## Results

`-M virt -cpu m68020-68851` boots Linux to userspace.  A static
userspace init prints /proc/cpuinfo over the goldfish-tty console:

    CPU:            68020
    MMU:            68851
    FPU:            68881
    Clocking:       3078.5MHz
    BogoMips:       769.63

Boot log: /tmp/boot-68851-virt-success.log (also -final.log).
PMMU instructions observed executing (via `-d in_asm`, saved to
/tmp/pmmu-insns-68851.log): `pmove %srp/%tc` + `pflusha` (head.S
mmu_engage), `pmove %crp` (context switch), `pflush #0,#4[,%a0@]`
(TLB shootdown), `ptestr` + `pmove %psr` + `ploadr/ploadw`
(bus_error030 fault re-probe) -- i.e. the full 68851 repertoire
Linux uses, including the 851 code path in head.S that skips
mmu_map_tt via is_020().

Regression matrix (all boot to userspace/Finder, logs in /tmp):

- `-M virt` (default m68040): Linux userspace OK
  (/tmp/boot-040-virt.log) -- note this was BROKEN before the
  bootinfo fix, not by it.
- `-M virt -cpu m68030`: Linux userspace OK (/tmp/boot-030-virt.log),
  newly working.
- `-M virt -cpu m68020-68851`: Linux userspace OK.
- `-M q800` (68040, mac_defconfig kernel): Linux userspace OK
  (/tmp/boot-q800.log).
- `-M macclassicii` (68030) + System 7.5.3: boots to interactive
  Finder (screenshot /tmp/c030-8851-screen.png), so the 030-walk
  fixes do not disturb the MacOS use of the PMMU.

Reproduce:

    # QEMU (tmpfs build)
    mkdir -p /tmp/q68851-build && cd /tmp/q68851-build
    /workspace/src/qemu-68851/configure --target-list=m68k-softmmu \
        --disable-docs --disable-tools && ninja qemu-system-m68k

    # kernel: clean worktree of /workspace/src/linux-m68kdt on tmpfs
    git -C /workspace/src/linux-m68kdt worktree add --detach \
        /tmp/linux-68851-src HEAD
    cd /tmp/linux-68851-src
    make O=/tmp/k68851-build ARCH=m68k CROSS_COMPILE=m68k-linux-gnu- \
        virt_defconfig
    scripts/config --file /tmp/k68851-build/.config \
        --enable M68020 --enable M68030 --disable CGROUP_FREEZER \
        --enable BLK_DEV_INITRD --enable RD_GZIP
    make O=/tmp/k68851-build ARCH=m68k CROSS_COMPILE=m68k-linux-gnu- \
        olddefconfig vmlinux -j$(nproc)

    # initramfs: single static init printing /proc/cpuinfo
    # (gen_init_cpio list with a real /dev/console c 5 1 node --
    # without it init runs silently)

    /tmp/q68851-build/qemu-system-m68k -M virt -cpu m68020-68851 \
        -m 128M -kernel /tmp/k68851-build/vmlinux \
        -initrd /tmp/initramfs-68851.cpio.gz \
        -append "console=ttyGF0 rdinit=/init" -nographic

## Known gaps / future work

- PSAVE/PRESTORE (68851 context save/restore) still F-line trap; the
  PBcc/PDBcc/PTRAPcc coprocessor conditionals are not decoded.  Linux
  does not use any of them (A/UX would).
- Access levels are storage-only; EXCP_MMU_ACCESS (vector 58) is
  never raised.  EXCP_MMU_CONF is likewise not raised for garbage
  TC/RP values (PMOVE accepts anything, like the 030 code before).
- PFLUSHR flushes everything rather than by root pointer; harmless
  (over-flushing) and Linux never issues it.
- No VMState subsection for the 030/851 PMMU registers (matches the
  pre-existing 030 state of the branch): savevm across an
  MMU-enabled 020/030/851 guest won't restore translation.
- The kernel tree quirks (CGROUP_FREEZER link failure from the local
  "allow umh to be disabled" commit; unconditional timer-everdrive.o
  in drivers/clocksource/Makefile breaking mac_defconfig) live in
  /workspace/src/linux-m68kdt, not here; both worked around locally
  in the throwaway /tmp worktree, no kernel source changes needed for
  the 68851 itself.
