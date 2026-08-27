# Booting Linux/m68k `sun3` (CONFIG_SUN3) on QEMU `-M sun3-60`

Goal (met): boot the Linux `sun3` kernel to an **interactive serial shell**,
exercising the discrete **Sun-3 MMU** under Linux's `arch/m68k/sun3/mmu_emu.c`
(the segment/page-map MMU, a distinct path from the 68030/68851 PMMU work).

Result: Linux 7.2.0-sun3 boots to a busybox shell on the `zs` serial console;
`cat /proc/cpuinfo` shows `CPU: 68020 / MMU: Sun-3 / FPU: 68881`.  NetBSD/sun3
1.5.2 still boots on the same, **unmodified** machine model.

## Headline finding: no QEMU change was needed

`hw/m68k/sun3.c` was **not modified**.  The Sun-3 MMU model (context reg,
per-context segment map, page map, control space, boot-state EPROM redirect,
REF/MOD, the four PTE type buses), the `zs`/ESCC console, the ICM7170 timer at
level 5, the interrupt register and the IDPROM that were built to boot NetBSD
are already a faithful enough Sun-3/60 for Linux.  All of the adaptation is on
the Linux side (a bit-rotted, barely-maintained port) plus a boot shim.

The Linux IDPROM expectations match the model exactly:
`id_machtype = SM_SUN3|SM_3_60 = 0x17`, `id_format = 0x01`, checksum = XOR of
bytes 0..14 — which is exactly what `sun3_build_idprom()` writes.

## How NetBSD boots here (the pattern reused for Linux)

There is no storage/net boot device modelled.  `sun3-tools/boot-netbsd-ramdisk.py`
lets the resident 3/60 v3.0.1 PROM run its power-on init (so the Sun monitor
`romvec` at VA `0x0FEF0000` is fully live and the low RAM is identity-mapped in
MMU context 0), waits for the `>` monitor prompt, injects the OMAGIC a.out
kernel at physical `0x4000` through the **gdbstub**, and runs the monitor's
`g 4000`.  Context 0 maps low VA==PA, so gdb writes to VA land at the intended
physical pages; the kernel roots on the md0 ramdisk.

## What Linux `sun3` expects at entry — and how we satisfy it

* **romvec at `0x0FEF0000`.**  `arch/m68k/include/asm/openprom.h` defines
  `LINUX_OPPROM_BEGVM = 0x0fef0000` for CONFIG_SUN3 — identical to this PROM.
  `prom_init()` just stores that pointer; `config.c` then reads
  `romvec->pv_sun3mem` (memory size), `romvec->pv_monid`, and `mmu_emu_init()`
  calls `romvec->pv_setctxt()` to copy segmaps into the other 7 contexts.  All
  of these are real Sun-3 monitor routines and work because the PROM ran first.
* **Load address / entry.**  `asm/sun3-head.h`: `KERNBASE = 0x0E000000`,
  `LOAD_ADDR = 0x4000`.  The kernel is linked at `0x0E00_2000`
  (`vmlinux-sun3.lds`, `. = 0xE002000`) and entered at `_start = 0x0E002000`.
  `sun3-head.S` runs first at the *physical* copy (VA==PA via the monitor's low
  identity map), copies the bootloader's segmaps from VA 0 up to KERNBASE until
  the first `SUN3_INVALID_PMEG` (0xFF), then `jmp`s into the KERNBASE-linked
  high half.  So we load PT_LOAD segment `p_vaddr` at physical
  `p_vaddr - 0x0E000000` and `g (entry - 0x0E000000)` — i.e. `g 2000`.
* **Console = the PROM.**  sun3 has no in-tree serial/tty console
  (sun3_defconfig enables none, and `sun3-head.S` provides no
  `debug_cons_nputs`).  We add `hvc_sun3` (below) which drives
  `romvec->pv_nbputchar/pv_nbgetchar`; the PROM routes these to `ttya`
  (serial_hd(0)), so the console appears on the same `-serial` socket NetBSD
  uses.  Interactive input works because the PROM stays mapped at runtime
  (mmu_emu reserves PMEG 247 for the ROM and copies the ROM/IO maps into every
  context).

### The 8 MB injection budget

The monitor identity-maps only the **low 8 MB** (probed: reads succeed through
`0x7FE000`, fault at `0x800000`).  The gdb-injected image must therefore fit in
8 MB.  The kernel is trimmed (no NET/SCSI/most FS/tests) + a small PIE-busybox
initramfs so the loaded image is ~5.5 MB.

## Linux-side deltas (see `sun3-tools/linux-sun3.patch`)

The stock CONFIG_SUN3 build in `linux-drmbounce` does **not even compile/boot**;
the port rotted as generic mm/DT/hardening code advanced.  Fixes, all sun3-local:

1. **`arch/m68k/include/asm/sun3_pgtable.h`** — define `pte_special()` /
   `pte_mkspecial()` (bit `0x00080000`, a free software bit above the 19-bit
   page-frame field, ignored by the MMU).  `arch/m68k/Kconfig` selects
   `ARCH_HAS_PTE_SPECIAL if MMU`, but sun3 only had the motorola versions →
   `mm/memory.c` failed to build.
2. **`arch/m68k/sun3/config.c`** — the sun3 memory init never talked to
   `memblock`, so `paging_init()`/slab found no memory:
   * `memblock_add(0, __pa(memory_end))` + `memblock_reserve(0, __pa(memory_start))`
     (register RAM, reserve the kernel+initramfs);
   * `memblock_set_bottom_up(true)` so `paging_init()`'s page-table allocations
     come from the *mapped* low RAM, not the unmapped top;
   * **`min_low_pfn` / `max_low_pfn`** — `__free_memory_core()` clamps the pages
     it releases into the buddy allocator to `max_low_pfn`; with it left 0 the
     whole of RAM stayed reserved and the first slab alloc BUG'd.
   * a small CON_BOOT PROM console so the early setup path is visible.
3. **`arch/m68k/include/asm/elf.h`** — lower sun3 `ELF_ET_DYN_BASE` from
   `0x0D800000` to `0x04000000`.  User space is only ~224 MB (kernel at
   `0x0E000000`); a dyn-base near the top left no room for a PIE executable +
   its interpreter + stack + brk, so `busybox --install`'s re-exec died with
   "elf segment at 0d800000 requested but the memory is mapped already".
4. **`drivers/tty/hvc/hvc_sun3.c` (+ Kconfig/Makefile)** — the PROM-backed
   polled console/tty (`hvc0`), modelled on `hvc_udbg.c`.

Boot loader (`sun3-tools/boot-linux.py`) also, mirroring a real Sun booter:
* loads each PT_LOAD at `p_vaddr - KERNBASE` and **zeroes the BSS tail**
  (`sun3-head.S` never clears BSS), and
* writes a minimal **bootinfo** (`BI_MACHTYPE=MACH_SUN3`, then `BI_LAST`) at
  `_end`, because this tree's `setup_arch()` NULL-derefs
  (`m68k_find_bootinfo_record(BI_MACHTYPE)`) if no bootinfo is present.

Config: from `sun3_defconfig`, add embedded initramfs + `HVC_SUN3`, drop
NET/SCSI/most FS, the runtime self-tests (they OOM 16 MB), `BLK_DEV_RAM`,
`HARDENED_USERCOPY` (false-positive copying `".."` from kernel text in
`filldir64`) and `STRICT_KERNEL_RWX` (`mark_rodata_ro` WARN).  Boot with
`-m 24M` for userspace headroom.  Full `.config` in `sun3-tools/sun3-linux.config`.

## Build & run

```
# kernel (out-of-tree build dir; keep the shared linux tree clean)
cd /workspace/src/linux-drmbounce
git apply .../sun3-tools/linux-sun3.patch          # the 4 kernel fixes + hvc_sun3
make O=/tmp/sun3-kbuild ARCH=m68k CROSS_COMPILE=m68k-linux-gnu- sun3_defconfig
cp .../sun3-linux.config /tmp/sun3-kbuild/.config   # or re-derive; sets INITRAMFS_SOURCE
make O=/tmp/sun3-kbuild ARCH=m68k CROSS_COMPILE=m68k-linux-gnu- olddefconfig vmlinux

# boot
cd /workspace/src/qemu-sun3
SUN3_EXTRA="-m 24M" SUN3_CMDS="uname -a; cat /proc/cpuinfo" \
  python3 sun3-tools/boot-linux.py /tmp/sun3-linux.log
```

## Console excerpt (interactive shell)

```
===== Linux/sun3 userspace is up =====
Linux (none) 7.2.0-sun3-...-dirty #15 m68k GNU/Linux
Interactive shell on /dev/hvc0:
/bin/sh: can't access tty; job control turned off
~ # cat /proc/cpuinfo
CPU:            68020
MMU:            Sun-3
FPU:            68881
BogoMips:       506.26
~ # uname -a
Linux (none) 7.2.0-sun3-...-dirty #15 m68k GNU/Linux
~ # echo SUN3_LINUX_SHELL_OK_$((21*2))
SUN3_LINUX_SHELL_OK_42
```

Transcripts: `/tmp/sun3-linux-final.log` (canonical), `/tmp/sun3-linux-shell3.log`
(full `--install` + `ls`/`free`), `/tmp/sun3-netbsd-verify.log` (NetBSD still boots).

## Known issue (non-blocking)

A *sporadic* spurious `SIGUSR1` (signal 10) is occasionally delivered to a
running process — the shell prints "User defined signal 1" — but only now and
then (e.g. once across six commands), and always *after* the affected command
has already produced its correct output, so it neither blocks the shell nor
corrupts results.  Because it is asynchronous and not tied to any particular
syscall it looks like a rare spurious interrupt/signal delivery rather than a
per-exec trap; a sun3 signal-frame / 68881 FSAVE-frame interaction is the
leading suspect.  Not yet root-caused.

Separately, the guest wall clock runs several times fast (the ICM7170 100 Hz
tick vs. `legacy_timer_tick`), which also shows up as an implausible
`Clocking:`/BogoMips value in `/proc/cpuinfo`.  Cosmetic; timing-based waits
still complete.

The MMU, memory, console, fork/exec and demand-paging via
`mmu_emu_handle_fault()` are all otherwise solid.
