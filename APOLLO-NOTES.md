# Apollo DN3000 bring-up notes (Phase 1: MD PROM to the `>` prompt)

Status: **WORKING** — the DN3000 boot PROM (`3000_BOOT_8475_7.bin`,
MD8 REV 7.0) boots to the interactive Service-Mode MD `>` prompt on the
serial console, and answers commands (`DR` register dump, `SK` console
re-selection, `EX AEGIS` → clean "ring: init error" with no boot
device).

Run it:

```
qemu-system-m68k -M apollo-dn3000 \
    -bios /workspace/files/apollo/3000_BOOT_8475_7.bin \
    -serial mon:stdio -display none
```

then **press Return twice** (real-hardware behaviour: the PROM's
autobaud needs one CR to measure the rate and a second to confirm; see
"autobaud" below). Banner + prompt:

```
MD8 REV 7.0, 1988/08/16.15:14:39
>
```

## Confirmed DN3000 memory map (runtime-verified against the PROM)

All from MAME `apollo.cpp` dn3000_map + Linux `apollohw.h`, confirmed by
watching the PROM run:

| Range | Device | Notes confirmed at runtime |
|---|---|---|
| `0x000000-0x007fff` | boot PROM (32K) | reset SSP/PC read from words 0/1 (`00100180` / `0000598a`); PROM *writes* to ROM early (a ROM-write probe) — must be ignored, not bus-error |
| `0x008000` | CSR status reg (16b, RO, write=clear) | boots with `SERVICE|BIT15` (0x8001) |
| `0x008100` | CSR control reg (16b) | Linux writes it 32-bit wide — accept sizes 1-4 |
| `0x008400-0x0087ff` | SIO (SCN2681 DUART) | **the PROM addresses registers at BASE+1+2n (odd bytes!)** — reg index must be `(offset >> 1) & 0xf`, exactly like MAME's `offset/2`. ch A = keyboard, ch B = serial console |
| `0x008800-0x0088ff` | MC6840 PTM | regs on odd bytes too, index `(offset>>1)&7`; T1/T2/T3 clocks 250k/125k/62.5kHz |
| `0x008900-0x0089ff` | MC146818 RTC | *direct* byte addressing (`0x8900+reg`), not index/data — wrapped QEMU's ISA model with a two-dispatch shim (write index port, then data port) |
| `0x009000/0x009100` | Am9517A DMA 1/2 | RAM-backed register stubs suffice for the PROM |
| `0x009200/0x009300` | DMA page regs / parity latch | stubs |
| `0x009400/0x009500` | i8259 master/slave | 2 registers each at +0/+1 (Linux: `pica+1` = IMR) |
| `0x009600` | Node ID PROM | 16-bit reads, id byte in the *high* byte lane; regs 1..3 = id msb..lsb, 15 = checksum |
| `0x040000-0x05ffff` | AT bus I/O | unmapped ISA floats `0xff` — **no bus error** (the graphics probe at `0x5d800` lands here and must fail soft) |
| `0x080000-0xffffff` | AT bus memory | same float-high behaviour; RAM overlays it |
| `0x100000-0x8fffff` | RAM (8 MB max) | RAM config byte `0x20` ("2-2-2-2") presented on the SIO input port IP0-IP6 |
| everything else | **bus error** + latch CSR `CPU_TIMEOUT` (0x0100) | PROM sizes RAM/probes devices this way (e.g. `0xac00`, `0x8000000`) |

Implemented as a 4 GiB background MMIO region at priority -2 whose
read/write ops set the CSR bit and return `MEMTX_DECODE_ERROR`, which
the m68k core turns into a real format-A bus fault.

## Interrupts (wired, PROM programs them; not yet exercised to the CPU)

- Dual i8259, slave INT → master **IR3** (not the PC's IR2!).  QEMU's
  `pic_read_irq()` hard-codes the PC cascade, so the board carries its
  own copies of `pic_get_irq`/`pic_intack` operating on
  `PICCommonState` (from `hw/isa/i8259_internal.h`).
- Master INT → CPU IPL 6, vectored: vector computed non-destructively
  at assert time (QEMU m68k wants it at `m68k_set_irq_level()` time),
  the destructive 8259 acknowledge runs from the CPU's `iack-out[6]`
  gpio (this fork pulses it when the exception is taken).
- Master INT state mirrors CSR status bit 0x0008 (INTERRUPT_PENDING).
- IRQ0 = PTM, IRQ1 = SIO, IRQ3 = cascade, IRQ8 = RTC (slave IR0).
  Vector base 0xa0 is programmed by the OS/PROM, not hard-wired.
- The i8259s + RTC live on a private, CPU-invisible ISA bus; their
  Apollo register windows are MMIO shims that
  `memory_region_dispatch_*` into the devices' own I/O regions.

## SCN2681 DUART model (`hw/char/scn2681.c`, new)

Register file per the 2681/68681 datasheet, index = `(offset>>1)&0xf`:

| idx | read | write | modelled |
|---|---|---|---|
| 0/8 | MR1/MR2 (ptr) | MR1/MR2 | yes, pointer semantics + reset-ptr cmd |
| 1/9 | SR | CSR | SR bits RxRDY/FFULL/TxRDY/TxEMT; CSR stored (feeds autobaud quirk) |
| 2/a | BRG/1x-16x test | CR | full CR: rx/tx enable/disable + misc cmds 1-5 |
| 3/b | RHR | THR | 3-deep RX FIFO; TX instant to chardev |
| 4 | IPCR | ACR | IPCR = IP low nibble, no deltas; ACR stored (CT mode) |
| 5 | ISR | IMR | full ISR/IMR → single IRQ output |
| 6/7 | CTU/CTL | CTUR/CTLR | lazy counter/timer, ISR[3], start/stop via reads of idx e/f |
| c | IVR | IVR | stored (2681 reserved) |
| d | IP | OPCR | IP = `0x80 \| input-port` property (DN3000: RAM config byte `0x20`) |
| e/f | start/stop counter | set/reset OP bits | OPR stored |

Properties: `chardev-a` (keyboard), `chardev-b` (console),
`input-port`, `xtal` (3.6864 MHz), `apollo-autobaud` (below).

### The autobaud quirk (why two Returns)

The MD PROM listens at 2000 baud (CSRB=0x77) and deduces the console's
real rate from the *garbled* byte a CR at another rate produces:
0xff→9600, 0xfe→4800, 0xc7→2400, 0x72→1200, 0xc0→600 (dispatch table at
ROM 0x828-0x89c; recognition loop at 0x7b8 polls SRA/SRB of both
channels).  QEMU has no bit-level timing, so with `apollo-autobaud` set
the DUART returns 0xff for any byte received while CSR==0x77 — the
PROM then programs 9600 (CSRB=0xbb) and the next CR reads back clean.
MAME needs the same hack (apollo_m.cpp `apollo_sio::read`, 0xfe→0xff).
`SK` at the prompt re-runs this selection (hence banner reprints).

## CPU: 68030 core as the 68851 stand-in

The PROM *requires* a PMMU: at 0x226e it executes `pmove %d0,%tc`
(68851 probe).  If that F-line-traps it falls into a MOVES-to-FC3
probe (0x2280, DFC=3, addr 0x8000000) that bus-faults forever — i.e. a
DN3000 without an MMU never reaches the console.  Two changes:

1. Machine default CPU is **m68030** (its on-chip PMMU is the
   68851-compatible subset; MAME similarly runs the DN3000 on its
   "M68020PMMU" hybrid).  Swap to the real 68020+68851 pairing once
   the `add-68851` work lands in `target/m68k`.
2. `target/m68k/translate.c` pmmu030: added **data-register-direct
   PMOVE** for the 16/32-bit MMU registers (68030 only takes memory
   EAs; the 68851 allows `pmove %d0,%tc`, which is exactly what the
   PROM uses).

## Other landmines hit

- **Deferred ROM loading**: `load_image_mr()` copies the image at the
  *first reset after* board init (`rom_check_and_register_reset` runs
  last), so reading the reset SSP/PC out of the ROM during init sees
  zeros and the CPU executes the vector table as code.  Load the PROM
  eagerly with `load_image_size()` into the rom_device's RAM.
- ROM writes must be ignored (rom_device with a log-and-drop write op);
  a plain readonly region would bus-fault the PROM's early ROM-write.
- This fork's header layout: `hw/core/sysbus.h`, `hw/core/irq.h`,
  `hw/core/qdev-properties*.h`, `system/memory.h`; chardev frontend
  type is `CharFrontend` (not `CharBackend`).

## Verified session (2026-08-26, log at /tmp/apollo-serial-session.log)

```
MD8 REV 7.0, 1988/08/16.15:14:39
>DR
100182:        0        0        0 ...
1001A2:  ...   100180   140000
>EX AEGIS
ring: init error
>
```

## Phase 2 — Linux/m68k boots to userspace, MMU exercised (DONE)

Status: **WORKING.** Linux boots on `apollo-dn3000` with the **real
MC68851** (`-cpu m68020-68851`, merged from `add-68851`), turns on
paging through the 68851 long-format tables, runs the whole kernel from
virtual memory, unpacks the initramfs, and reaches the **userspace init
exec** handoff.  Boot logs in `/tmp/apollo-linux-boot.log` (full kernel
init) and `/tmp/apollo-linux-userspace.log` (reaches userspace).

Run it:

```
qemu-system-m68k -M apollo-dn3000 -m 128M \
    -kernel /tmp/apollo-vmlinux \
    -initrd /workspace/files/rootfs.cpio.lz4.040 \
    -append "earlyprintk keep_bootcon initcall_blacklist=dnfb_init" \
    -serial file:/tmp/apollo-linux-boot.log -display none
```

Kernel built from `/workspace/src/linux-m68kdt` `apollo_defconfig`
(vmlinux linked at phys 0, entry 0x2000).

### What the boot proves (log highlights)

```
ABCPGHIJK                         <- head.S MMU-setup markers; 'P' = Apollo
                                     mmu_map 0x80000000, MMU then enabled
Linux version 7.1.0-apollo ...
Apollo hardware found: [DN3000 (Otter)]   <- BI_APOLLO_MODEL parsed
Zone ranges: DMA [0x00100000-0x08ffffff]  <- RAM at 1 MB, 128 MB
Calibrating delay loop... 629 BogoMIPS    <- timer IRQ works (see below)
Memory: 117412K/131072K available
devtmpfs / SCSI subsystem / NET PF_INET/UNIX/PACKET/KEY ...
Trying to unpack rootfs image as initramfs...
```
The kernel is running entirely under the 68851 MMU by the time any of
this prints (paging is enabled in head.S, `mmu_engage_030` path).  With
a bogus `rdinit=/x` the run ends in `The end !` (`dn_dummy_reset()`
writing straight to the SIO) - i.e. it completed the full boot, tried
to exec userspace, panicked, and rebooted: userspace handoff reached.

### `-kernel` boot support added (hw/m68k/apollo.c)

- Load the vmlinux ELF with a +`APOLLO_RAM_BASE` paddr bias (kernel is
  linked at phys 0, RAM is at 1 MB; the kernel is PC-relative at entry
  and recomputes its own `phys_kernel_start`).
- Synthesized bootinfo: `BI_MACHTYPE=MACH_APOLLO`, `BI_CPUTYPE=68020`,
  `BI_MMUTYPE=68851`, `BI_APOLLO_MODEL=DN3000`, `BI_MEMCHUNK`
  (base 0x100000, size = ram_size), `BI_COMMAND_LINE`, `BI_RAMDISK`.
- `-bios` optional when `-kernel` is given.

### Load-bearing hardware discoveries (all confirmed at runtime)

1. **DMA address-translation map at phys 0x17000** must be RAM-backed:
   `config_apollo()` clears `addr_xlat_map` (IO_BASE+0x17000, 0x400
   u16 entries) *unconditionally* even on the DN3000 (it is really a
   SAU7/DN3500 address).  Without backing it, the very first C code
   after MMU-on bus-errors.  Reaching this confirms the MMU is live
   (virtual 0x80017000 -> phys 0x17000).
2. **head.S early console is hard-wired to SAU7/DN3500 offsets**
   (`LSRB0 0x10412`, `LTHRB0 0x10416`, `LCPUCTRL 0x10100`) and runs
   *before* the MMU with `iobase = 0`, i.e. at those physical
   addresses.  We alias the DUART at 0x10400 and the CSR-control reg at
   0x10100 so earlyprintk and `set_leds` work on the DN3000.  The SIO is
   pre-initialised (channel B TX/RX enabled at reset via the new
   `preinit-console` property) because the kernel "counts on the PROM
   initializing SIO1" and the PROM never runs on `-kernel` boot.
3. **The PROM's 8259 init has to be replayed.** Linux's Apollo port
   (`dn_init_IRQ`/`apollo_irq_chip`) never issues the 8259 ICW
   sequence - it assumes the PROM set vector base `APOLLO_IRQ_VECTOR`
   (0xa0) and left the on-board sources unmasked.  On `-kernel` boot we
   program the PICs ourselves (master base 0xa0, slave 0xa8, slave
   cascaded on master IR3, master IMR unmasking IR0/IR3).  This must run
   in a **`MachineClass::reset` hook after `qemu_devices_reset()`** -
   the 8259 qdev reset runs *after* the legacy `qemu_register_reset`
   handlers and would otherwise wipe the programming (irq_base back to
   0 -> timer IACK returns vector 0 -> `BAD KERNEL TRAP`).
4. **Interrupt vectoring:** the PTM timer on master IR0 delivers vector
   0xa0; the m68k core maps that to Linux irq 8 (`user_irqvec_fixup =
   0xa0 - IRQ_USER = 152`, so irq = vector - 152).  Delivery is via
   this fork's per-level `iack-out[6]` gpio (peek the vector at assert,
   destructive 8259 ack from the IACK hook).

### Real 68851 (merged from add-68851)

Machine now defaults to **`-cpu m68020-68851`** (the true 68020 + external
MC68851, historically correct for the DN3000) instead of the 68030
stand-in.  The merge brought the four PMMU table-walk fixes (U-bit on
invalid descriptors, S-bit from short descriptors, instruction-fetch
SSW FC/FB, plus the real PMOVE/PFLUSH/PTEST superset).  The data-
register-direct PMOVE support (`pmove %d0,%tc`) added in Phase 1 is
retained - it is complementary (the 68851 accepts the Dn form the PROM
uses; add-68851's decode reaches `gen_lea`, which faults on Dn).  MD
prompt reverified after the merge and CPU switch.

### One kernel fix required (saved as apollo-linux-dn_ints-irq-fix.patch)

The DN3000 port's `apollo_irq_startup()` unmasked the PIC but never
installed the m68k user-interrupt vector (`user_inthandler`) - unlike
`atari_irq_startup()`/the default `m68k_irq_startup()`.  So vector 0xa0
stayed `bad_inthandler` and the first timer tick was dropped
("unexpected interrupt from 640"), hanging `calibrate_delay()`.  The
one-line fix adds `m68k_irq_startup(data)` (and `m68k_irq_shutdown` for
symmetry).  Applied to `/workspace/src/linux-m68kdt/arch/m68k/apollo/
dn_ints.c`; patch archived in this tree.

### Console visibility limitation (why no interactive shell on serial)

Userspace *runs* but is not visible on the serial line, for two
compounding reasons, both outside Phase 2 scope:

- The m68k earlyprintk console's write routine (`debug_cons_nputs` in
  head.S) lives in `.init` memory, so `printk_late_init()`
  force-unregisters it before userspace *regardless of* `keep_bootcon`
  ("bootconsole uses init memory and must be disabled").
- The Apollo has **no serial tty driver** in this kernel; its only
  userspace console is the framebuffer (`dnfb`), whose text rendering
  goes through the Apollo **blitter** (AP_CONTROL ROP registers +
  MGM at 0xfa0000) which is not modelled.  So fbcon (`tty0`) output is
  invisible here.

Making the shell visible would need either a Linux SCN2681 serial tty
driver or the Apollo graphics/blitter device (research §2.11) + a QEMU
display - both larger than the MMU-bring-up goal.  `initcall_blacklist=
dnfb_init` keeps the serial bootconsole alive as long as possible (up to
the `printk_late_init` cutoff), which is what the captured logs show.

## Phase 3+ (not started)

- SCN2681 serial tty driver or Apollo graphics/blitter + display, to get
  an interactive userspace console.
- OMTI-8621 disk, node-ID-from-disk, keyboard — per research §2.10-2.11.
