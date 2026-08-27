# Sun 3/80 (`sun3x`) QEMU machine — hardware contract

Boots the Linux/m68k `sun3x` kernel (`CONFIG_SUN3X`) to a serial shell.
Model: **Sun 3/80** — 68030 (on-chip PMMU, the same unit already used by
HP9000/340 and Apollo), Zilog Z8530 (zs) serial console, Mostek-style
NVRAM/clock, a Sun interrupt/enable register, and a synthesised boot PROM.

Contract reverse-engineered from the Linux sun3x port:
`arch/m68k/sun3x/{config.c,prom.c,time.c,dvma.c}`,
`arch/m68k/sun3/{idprom.c,sun3ints.c}`,
`arch/m68k/kernel/head.S` (the `CONFIG_SUN3X` MMU path),
`arch/m68k/include/asm/{sun3x.h,sun3xprom.h,idprom.h,machines.h,openprom.h}`.

## CPU / MMU / boot flow

* **m68030** with the standard on-chip PMMU (no custom MMU like sun3-60).
* `PAGE_OFFSET_RAW == 0` for sun3x → kernel virtual == physical for RAM.
* Kernel entered **MMU-off** at its ELF entry (linked low, ~0x1000), running
  physically in RAM at phys 0. `head.S` builds `kernel_pg_dir` and
  `mmu_engage`s. This is the same from-scratch mechanism as hp300/bvme6000:
  load ELF PT_LOADs into RAM (RAM base 0, so no translation), append a
  bootinfo block after the image, set reset PC = entry, SP = top of RAM.
* sun3x uses the **standard m68k bootinfo** (`head.S` `get_bi_record`):
  BI_MACHTYPE(MACH_SUN3X=11), BI_CPUTYPE(CPU_68030), BI_MMUTYPE(MMU_68030),
  BI_MEMCHUNK(0, ram_size), BI_COMMAND_LINE, [BI_RAMDISK], BI_LAST.
  No BI_FPUTYPE (kernel uses FPU emulation, like hp300/030).

### head.S SUN3X specifics (must be satisfied by the machine)
1. `oriw #0x4000,0x61000000` — "enable copro" write to SUN3X_ENAREG. Absorb.
2. Early debug `putc`: `movel 0xFEFE0018,%a1; jbsr (%a1)` — calls the PROM
   **pv_putchar** pointer (romvec+24). Char pushed on stack (arg at 4(sp)).
3. MMU map path copies the PROM's mapping for VA `0xfee00000..0xff000000`
   (2 MiB) into kernel_pg_dir, translating 8K→4K pages, and reads the PROM
   page-table root indirectly:
   `movel 0xfefe00d4,%a1 ; movel %a1@,%a1` then copies `(0x200000>>13)=256`
   32-bit page descriptors (each doubled: entry, entry+0x1000).
   Also sets `TT1` transparent-translate for `0x40000000` size `0x40000000`
   (all on-board I/O; covers zs/intreg/eeprom/iommu).

## Physical memory map

| Phys base    | Size     | What                                              |
|--------------|----------|---------------------------------------------------|
| 0x0000_0000  | ram_size | Main RAM (default 16 MiB). Kernel VA==PA.         |
| 0x6000_0000  | 0x1000   | SUN3X_IOMMU — DVMA page table (RAM-backed scratch)|
| 0x6100_0000  | 0x2000   | ENAREG/INTREG/DIAGREG block                       |
|  +0x1400     | 1 byte   | SUN3X_INTREG (0x61001400) — modelled              |
| 0x6200_0000  | 0x2000   | SUN3X_ZS1 — Z8530 (ESCC), console                 |
| 0x6200_2000  | 0x2000   | SUN3X_ZS2 — second Z8530 (kbd/mouse, idle)        |
| 0x6400_0000  | 0x800    | SUN3X_EEPROM (NVRAM). idprom @ +0x7d8, clk @ +0xf8|
| 0xfee0_0000  | 0x200000 | PROM window (romvec/stubs/pagetable); identity    |

TT1 (`0x40000000`+`0x40000000`) makes all the `0x6xxxxxxx` I/O reachable
after mmu_engage; the PROM window is reachable via the copied page table.
Everything unmapped reads 0 / absorbs writes (sun3x does no bus-error probing;
memory comes from BI_MEMCHUNK, devices are at fixed addresses).

## IDPROM (SUN3X_IDPROM = 0x640007d8)

`struct idprom` (32 bytes). Checksum = XOR of bytes[0..14] stored in byte 15.
* byte0 id_format = 0x01
* byte1 id_machtype = SM_SUN3X|SM_3_80 = 0x40|0x02 = **0x42**
* bytes2..7 ethaddr, 8..11 date, 12..14 sernum (left plausible), 15 cksum.
`idprom_init()` panics (`prom_halt`) on bad format/checksum, and
`display_system_type` halts on an unknown machtype — 0x42 → "Sun 3/80".

## Interrupt / enable register (SUN3X_INTREG = 0x61001400, 1 byte)

`sun3_intreg` bitmask (arch/m68k/sun3/sun3ints.c):
* bit0 = master enable (all interrupts). `sun3_init_IRQ` writes 1.
* bitN = enable autovector level N. `sun3_enable_irq(5)` sets bit5.
The **timer tick** is IRQ_AUTO_5: a periodic (HZ) clock latches a level-5
request; it is delivered while `(intreg & 1) && (intreg & (1<<5))`.
`sun3_int5` acks by pulsing bit5 low→high (`sun3_disable_irq(5); enable`),
which clears the latch. Model: level-5 line = latch && bit0 && bit5; writing
intreg with bit5 clear clears the latch. Level→CPU via TYPE_M68K_IRQC.

## Timer (arch/m68k/sun3x/time.c)

`sun3x_sched_init` just enables IRQ 5 (the free-running clock). No timer
programming — the periodic source is the on-board clock at HZ. QEMUTimer at
HZ (default 100 → 10 ms) sets the level-5 latch. `sun3x_hwclk` reads a
Mostek `mostek_dt` at SUN3X_EEPROM+0xf8 (BCD wall-clock); RAM-backed with a
fixed plausible date is sufficient (read-only wall clock).

## The synthesised PROM (phys 0xfee00000, 2 MiB, identity-mapped)

`romvec` = `struct linux_romvec` at **0xfefe0000** (SUN3X_PROM_BASE). Field
offsets used by Linux (32-bit fields, m68k):
* +20 pv_getchar, +24 pv_putchar, +28 pv_nbgetchar, +32 pv_nbputchar
* +76 pv_monid (char* → version string), +96 pv_reboot, +152 pv_abortentry

`sun3x_prom_init` also reads the raw P_* slots (0xfefe0014.. = same offsets).
`head.S` reads `*(0xfefe00d4)` → ptr → PROM page table (256 8K descriptors).

Layout inside the PROM region (offsets from 0xfee00000; VA==PA identity):
* 0x1e0000 (0xfefe0000): romvec. Fill getchar/putchar/nb* → stub code addrs,
  pv_monid → version string, pv_reboot/pv_abort → halt stub.
* 0x1e00d4 (0xfefe00d4): = 0xfefe0200 (PTP pointer word)
* 0x1e0200 (0xfefe0200): = 0xfefe0400 (page-table base)
* 0x1e0400 (0xfefe0400): 256 × u32, entry[i] = (0xfee00000 + i*0x2000) | 0x59
  (0x59 = _PAGE_PRESENT|_PAGE_ACCESSED|_PAGE_DIRTY|_PAGE_NOCACHE030 → a valid
  68030 resident page descriptor). identity map of the 2 MiB PROM window.
* 0x1e0800 (0xfefe0800): m68k stub routines (hand-assembled):
  - putchar: enable zs ch-A TX (WR5|=TXEN,8bit), poll RR0 TXEMPTY, write DATA.
    reads char from 4(sp). used for pv_putchar/pv_nbputchar.
  - getchar: enable zs ch-A RX (WR3|=RXEN,8bit), read RR0; if RXAV read DATA
    else return -1. used for pv_nbgetchar/pv_getchar.
  - halt: `stop`/branch-self.

zs console is ch-A of the Z8530 at 0x62000000 (it_shift=1 ESCC layout):
ch-A CTRL @ +4, ch-A DATA @ +6 (chn=(addr>>2)&1, reg=(addr>>1)&1). The stub
must enable RX/TX itself because the minimal PROM does no zs init and the
ESCC drops TX unless WR5.TXEN and blocks RX unless WR3.RXEN.

## Console (Linux side — needs a patch, see sun3x-tools/)

sun3x now has a **native, console-only Z8530 serial driver** that drives the
console zs (ch-A at 0x62000000) directly, giving an interactive
`console=ttyS0` — the runtime console runs on the real hardware, not through
the PROM.  See `sun3x-tools/zs-console.patch`
(`drivers/tty/serial/sun3x_scc.c` + Kconfig/Makefile) and the config change
`CONFIG_BOOTPARAM_STRING="console=ttyS0"`, `CONFIG_SERIAL_SUN3X_SCC=y`.

The driver adapts the mvme147/mvme16x/bvme6000 Z8530 console:
* register access is the standard Z8530 two-step (write reg# to the ctrl
  port, then the value); RR0/WR0 and the data port are addressed directly.
  With the QEMU ESCC `it_shift==1`, ch-A **ctrl = 0x62000004**, **data =
  0x62000006**.  hw init = WR4 async 8N1 x16, WR3 = Rx8|RXEN, WR5 =
  Tx8|TXEN|RTS|DTR.
* The zs IRQ is **not wired to the CPU** on this model (the only autovector
  source is the level-5 clock tick), so **RX is polled** by a 1-jiffy timer
  and **TX polls RR0.TX_EMPTY**.  This is enough for an interactive shell.
* The zs registers are reachable at their physical addresses because head.S
  sets TT1 (transparent, cache-inhibited) over 0x40000000..0x80000000.

The console is registered at `console_initcall` (its `setup()` just
(re)inits the zs — deliberately **not** `uart_set_options()`, whose
`port->state` isn't allocated that early) and the tty port is added at
`device_initcall` (owned by a small platform_device so serial-core has a
valid `port->dev`).

`hvc_sun3` is **retired for sun3x** (`CONFIG_HVC_SUN3` off in the sun3x
config); the shim + driver remain in the tree for the real SUN3.  The PROM
putchar/getchar romvec stubs are still required — head.S uses the PROM for
the earliest pre-console `putc` and for the page-table root — so the
synthesised PROM must stay.
