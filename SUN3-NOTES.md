# Sun-3/60 ("Ferrari") notes

Mission: `-M sun3-60` boots the 3/60 v3.0.1 PROM to the `>` monitor
prompt, then boots the NetBSD/sun3 1.5.2 RAMDISK kernel.
Assets in `_sun3_assets/` (see ASSETS.md there).

## Session 1 — recon

### Ground truth gathered

- PROM reset vector (file words 0/1): SP=0x0fe60c00 PC=0x0fef00ee,
  PROM lives at VA 0x0FEF0000.  In the hardware *boot state* (system
  enable register bit 0 clear) every supervisor **program** fetch is
  redirected to the EPROM regardless of address; data accesses go
  through the Sun-3 MMU normally.  Evidence from the PROM itself:
  the ROM checksum loop at 0xfefaca6 reads bytes 0..0xFFFD with
  `MOVES` and **SFC=6** (supervisor program) from VA 0 — only works
  if boot state redirects FC6 reads to the EPROM.
- Reset entry at 0xfef00ee is the *watchdog/warm* path (it checks
  bus-error reg bit 0 = watchdog, and old SP == 0x7777xxxx marker);
  cold start is at 0xfefab80.
- Control space (accessed via MOVES with SFC/DFC=3), decode by
  address bits 31..28 (VA in low 28 bits):
  - 0x0xxxxxxx IDPROM (byte reads)
  - 0x1xxxxxxx page map (32-bit, entry selected via current
    context+segmap by the VA in bits 27..0)
  - 0x2xxxxxxx segment map (byte; 2048 segs of 128KB per context)
  - 0x3xxxxxxx context register (3 bits, 8 contexts)
  - 0x4xxxxxxx system enable register (bit 0 = "not boot")
  - 0x5xxxxxxx user DVMA enable
  - 0x6xxxxxxx bus error register (PROM: bit 0 = watchdog)
  - 0x7xxxxxxx diagnostic LED register (cold start writes LED lamp
    patterns with delay loops here)
- Sun-3 PTE: VALID 0x80000000, WRITE 0x40000000, SYSTEM 0x20000000,
  NC 0x10000000, TYPE 0x0C000000 (0=obmem,1=obio,2=VME16,3=VME32),
  REF 0x02000000, MOD 0x01000000, PFN 0x0007FFFF (<<13).
  Page 8KB, segment 128KB (16 pages), 28-bit VA, 3/60 has 8 contexts
  and 256 PMEGs (PROM diag maps segmaps 0..255 and 4096 PTEs).
- obio (type-1) physical map, decoded from the RAMDISK/GENERIC
  kernels' cfdata (parsed a.out symtab -> cfdata locators) and
  cross-checked against PTE constants inside the PROM:
  - 0x000000 zs1 kbd/mouse (ipl 6)
  - 0x020000 zs0 ttyb/ttya (ipl 6; +0 B csr, +2 B data, +4 A csr,
    +6 A data — PROM zs self-test uses 0xfffe004/0xfffe006)
  - 0x040000 EEPROM (2KB)
  - 0x060000 Intersil ICM7170 RTC (ipl 5/7)
  - 0x080000 memory error register
  - 0x0A0000 interrupt register (PROM maps pfn 0x50)
  - 0x100000 EPROM (PROM maps pfn 0x80)
  - 0x120000 LANCE (ipl 3)
  - 0x140000 si NCR5380+DMA (ipl 2)
  - 0x1E0000 unknown, mapped by PROM with pfn 0xF0 (memory reg?)
  - 0xFF000000 bwtwo P4 framebuffer (GENERIC: bwtwo0 at obio
    0xff000000; the RAMDISK kernel has *no* bwtwo driver)
- The PROM uses short-absolute addressing (sign-extended) for its
  device VAs, e.g. 0xFFFFE004 -> the MMU only sees the low 28 bits
  (VA 0x0FFFE004).  The translate hook must mask VAs to 28 bits.
- PROM maps at cold start: VA 0x0FFFE000 -> obio 0x20000 (zs) with
  PTE 0xF4000010, VA 0x0FFF2000 -> obio 0xA0000 (intreg).
- netbsd.RAMDISK is OMAGIC a.out, MID_M68K: text 0xafd24 data
  0x6bc5c bss 0x253e8 entry 0x0E004000 (KERNBASE 0x0E000000, i.e.
  loaded at physical 0x4000).  No embedded config in either kernel.

### Design decisions (core surgery, kept gated)

- target/m68k gets a board "function code" hook, in the spirit of
  the per-level IACK commit: `m68k_set_fc_ops(cpu, ops, opaque)`
  with two callbacks:
  - `translate()` — called from m68k_cpu_tlb_fill *instead of* the
    built-in 030/040 MMU when registered; the board implements the
    Sun-3 segment/page maps, boot-state redirect (supervisor
    INST_FETCH -> EPROM) and REF/MOD bits.  Failure raises the
    existing 68020/030 format A/B bus error.
  - `moves()` — called from new moves_ld/moves_st helpers emitted
    (only when fc_ops is registered) instead of the plain
    moves_chk+TLB path, so MOVES can reach FC3 control space and so
    FC2/FC6 can honour boot state; the board decodes *all* function
    codes.  Unhandled FCs still bus error like today's moves_chk.
- Physical layout: the four Sun-3 PTE type spaces are separate
  32-bit buses; they are placed in the 64-bit system address space
  at (type << 32).  RAM at type 0 offset 0, obio devices at
  (1<<32)+addr.  Unassigned obio/obmem accesses hit a background
  MMIO region that latches the bus error register TIMEOUT bit and
  returns MEMTX_DECODE_ERROR -> format A/B bus error.
- Machine reset loads SP/PC from EPROM words 0/1 (that *is* the
  documented boot-state behaviour: vector fetch redirected to ROM).

### Milestone ladder

(a) PROM banner on ttya; (b) `>` prompt accepts commands;
(c) bwtwo banner; (d) NetBSD RAMDISK boots; (e) sysinst reachable.

## Session 1 — implementation log

Milestones reached: (a) PROM banner on ttya, (b) interactive `>`
monitor prompt, (d) NetBSD 1.5.2 RAMDISK kernel boots to userland
(welcome banner on ttya).  (c) bwtwo and (e) sysinst are open.

### How the PROM was made happy, in order

1. `tlb_set_page` must be fed TARGET_PAGE (4KB) granules even though
   Sun-3 pages are 8KB; the returned physical is byte-exact so
   masking works.  Flushes must cover both 4KB halves and all 16
   sign-extension aliases of a page (the PROM addresses its device
   pages via short absolute, e.g. 0xFFFE004 for the zs).
2. The memory error registers: the PROM's power-up test *forces*
   parity errors (CSR 0x20 = store bad parity, 0x40 = check, 0x10 =
   interrupt enable) and expects a level-7 NMI with 0x80|lanes in
   the CSR (lane = 0x8 >> (addr & 3)).  Implemented with an MMIO
   overlay that shadows RAM while test mode is active and a poison
   set of bad-parity addresses.
3. Level 7 must behave as an edge: the parity NMI arrives at SR
   IPL 7, and the monitor keeps its 100Hz clock on level 7 with the
   mask raised (intreg 0x81 = ALL_ENAB | CLOCK_ENAB_7, ICM7170 cmd
   0x1C, mask 0x02 = 100Hz).  Added nmi_edge_pending to the core.
4. The EPROM decodes the *untranslated* low 16 address bits: the
   monitor maps several ROM pages with pfn 0x80 and expects each to
   read its own offset (its device map table at ROM 0xD350 is read
   through such a page).  Handled in the translate hook for any PTE
   whose frame falls in obio 0x100000..0x11FFFF.
5. 3/60 high obmem devices (from the monitor's map table):
   0xFF000000 video RAM — the monitor runs its early stack at VA
   0xFE60C00 which maps there, so it must exist — and the EPROM
   again at 0xFF600000 (two mirrored 64KB images).  After init the
   monitor remaps its work RAM to the top two physical RAM pages.
6. ENA_NOTBOOT is bit 7 (0x80), not bit 0: the monitor leaves the
   boot state writing 0xA0.  With bit 0 assumed, `g 4000` executed
   ROM[0x4000] instead of RAM (boot-state redirect still live) and
   silently returned to the prompt.
7. EEPROM byte 0x1F selects the console (0x00 video, 0x10 ttya,
   0x11 ttyb); the PROM honours it without checksumming.  With 0x00
   and vram present the monitor happily uses the (invisible)
   video console — set 0x10 until bwtwo exists.
8. Interrupt register bits (evidence: PROM writes 3 / 0x21, monitor
   runs on 0x81): 0x01 ALL_ENAB, 0x02/4/8 soft 1-3, 0x10 video,
   0x20 clock-5, 0x80 clock-7.  Bus error register bits (PROM tests
   mask 0xFC and compare): 0x20 TIMEOUT, 0x40 PROTERR, 0x80 INVALID,
   0x01 watchdog.

### How NetBSD was made happy

* SSW function-code field: NetBSD's KDFAULT() requires FC=5 in the
  68020 SSW to route kernel-space copy faults to the kernel map;
  without it exec died with EFAULT (kernel pathbuf fault resolved
  against the user map).
* gdbstub debug accesses translate through the board hook
  (m68k_cpu_get_phys_addr_debug), which is what lets the kernel be
  restored into RAM at the monitor prompt.
* ESCC has one IRQ output per channel; wiring only output 0 lost all
  level-6 tty interrupts and userland hung on its first
  interrupt-driven console write.
* Carrier: the console open sleeps forever without DCD; the machine
  drives both channels' "dcd" gpios and escc keeps a board-driven
  DCD across chip resets (a listening socket chardev looks
  disconnected at reset time).
* MOVES accesses crossing an 8KB page boundary must be split into
  two translated cycles (copyin/copyout corruption otherwise).

### Boot path (documented shortcut)

No storage/network device exists yet, so the RAMDISK kernel is
injected at physical 0x4000 via the gdbstub at the monitor prompt
and started with `g 4000` — equivalent to being launched by the
PROM's boot path (context 0 maps low VA==PA; romVectorPtr at
0x0FEF0000 is live; the RAMDISK kernel roots on md0 and needs no
boot device string).  sun3-tools/boot-netbsd-ramdisk.py automates
it.  The honest alternative for later: si SCSI + a disk with a Sun
label and the miniroot in the swap partition (`b sd(,,1) -s`).

### Open bug: one EFAULTing open(2) per shell cycle

Userland comes up: init forks the RAMDISK small shell (ssh), .sshrc
runs `run cat /.welcome`, the banner prints.  Then exactly one
open(2) per cycle fails with EFAULT and the shell exits/is
respawned, so the installer environment loops instead of giving its
prompt.  Established with breakpoints on the kernel syscall error
store (0xe09754a in the RAMDISK kernel): per cycle errno=2 sys=58
(readlink, benign), errno=2 sys=33 (access PATH probes, benign),
errno=10 sys=7 (wait4 ECHILD, benign) and errno=14 sys=5 (open —
the failure).  The EFAULT is the recurring transient fault at
kernel VA 0xec54000 (a pool page for namei pathname buffers, seg
0x762 pmeg 0x2b): sometimes the fault resolves via
uvm_fault->pmap_enter (pgmap write e00001xx observed), but in this
one case uvm_fault fails => copyinstr's pcb_onfault => EFAULT,
suggesting the pool hands out a chunk whose backing page/VA has
been released (pool page reclaim vs freelist?), or that one more
fault-classification detail is still wrong.  Also unexplained: the
first ~4 characters of each process's first tty write burst arrive
as NULs ("\tWe" of the welcome).  Next steps: cross-check NetBSD
1.5.2 sys/kern/subr_pool.c + uvm_km.c against the fault trail; log
uvm_fault entry/exit for the failing va; check trap.c's fault-type
derivation (read vs write) for FC bits and SSW RW on *instruction*
faults; audit control-space segmap/pgmap sub-word access lanes.

### Debug crutches that proved useful

* -d int now traces diag-LED writes, enable/context/segmap/pgmap
  traffic and MMU faults (qemu_log_mask gated, zero cost otherwise).
* gdb-multiarch against the gdbstub works for kernel/user memory
  (debug hook translates), but register reads/writes are
  BYTE-SWAPPED on this gdb; breakpoints re-trap on plain `continue`
  (interpose `stepi`).
* QMP human-monitor-command "info registers" for cheap PC sampling.
* The RAMDISK FFS can be parsed offline (fs base at file offset
  0xB7984 in netbsd.RAMDISK, 400KB, bsize 8192/fsize 1024, ipg 128);
  /sbin/init is one crunched a.out (text 0x44000 at VA 0x2000, data
  at 0x46000) — see the session transcript for a reader.
