# MVME162/167 (Linux/m68k `mvme16x`) — QEMU machine bring-up notes

First 68040 machine in this tree to boot Linux/m68k. Board modelled: **MVME167**
(25 MHz 68040 + FPU, VMEchip2, PCCchip2, CD2401 serial). Machine name: `mvme167`.

Hardware contract reverse-engineered from the Linux mvme16x port
(`arch/m68k/mvme16x/config.c`, `arch/m68k/kernel/head.S`,
`arch/m68k/include/asm/mvme16xhw.h`,
`arch/m68k/include/uapi/asm/bootinfo-vme.h`).

## Physical memory map

| base        | size    | what                                                    |
|-------------|---------|---------------------------------------------------------|
| 0x00000000  | RAM     | main DRAM (kernel linked/loaded here, BI_MEMCHUNK 0)    |
| 0xfff40000  | 0x10000 | VMEchip2 (GCSR etc.) — absorb; reset @0xfff40107        |
| 0xfff42000  | 0x100   | PCCchip2: tick timer 1 + CD2401 SCC int-control + TPIACK|
| 0xfff45000  | 0x100   | CD2401 serial controller (byte-mapped register file)    |
| 0xfff46000  | -       | i82596 ethernet (not modelled; absorbed)                |
| 0xfffc0000  | 0x2000  | M48T59/M48T08 NVRAM+RTC (`MVME_RTC_BASE`)               |
| 0xf0c00000  | 16 MiB  | VME A24 slave window (host bridge) — AVME-352 console    |

head.S transparently maps 0xe0000000..0xffffffff (0.5 GiB) supervisor/nocache
via `mmu_map_tt`, so every on-board I/O address above is directly reachable
once the 68040 MMU is engaged. A low-priority absorber covers
0xfff00000..0xffffffff so stray probes (disabled drivers, PROM/SRAM) read 0
instead of bus-erroring.

## CPU / MMU

`m68040`. head.S builds its own 68040 page tables (4 MiB identity at 0) and
TT1 transparent window; QEMU's 68040 MMU handles the 040 pagetable/TTR format.
No external cache-control register writes on this board (unlike hp300).

## Board-ID struct (t_bdid, 32 bytes) — critical

`arch/m68k/kernel/head.S` copies 32 bytes from the **BI_VME_BRDINFO** bootinfo
record into the kernel `.data` symbol `mvme_bdid` (or calls 167Bug trap #15 if
the tag is absent). `config_mvme16x()` then does `strncmp("BDID", p->bdid, 4)`
and panics ("Bug call .BRD_ID returned garbage") if it fails. So we MUST supply
BI_VME_BRDINFO. Layout (`bootinfo-vme.h`, all multi-byte fields **big-endian**):

```
off  field
0    char   bdid[4]      = "BDID"
4    u8     rev          = 0x10   (BUG rev, printed as 1.0)
5    u8     mth          = 0x01
6    u8     day          = 0x01
7    u8     yr           = 0x26
8    be16   size
10   be16   reserved
12   be16   brdno        = 0x0167   <-- identifies MVME167
14   char   brdsuffix[2] = {0,0}
16   be32   options
20   be16   clun,dlun,ctype,dnum
28   be32   option2
```

`config.c` only reads `bdid`, `brdno`, `rev/mth/day/yr`, `brdsuffix`. For
brdno 0x0167 it does NOT touch MVME162_VERSION_REG (that path is 162/172 only),
and sets `mvme16x_config = GOT_LP | GOT_CD2401`.

## Bootinfo tags emitted (order)

```
BI_MACHTYPE   = MACH_MVME16x (7)
BI_CPUTYPE    = CPU_68040
BI_MMUTYPE    = MMU_68040
BI_FPUTYPE    = FPU_68040
BI_VME_TYPE   = 0x8000 -> VME_TYPE_MVME167 (0x0167)   (sets vme_brdtype; head.S
              selects the CD2401 console path for 166/167/177)
BI_VME_BRDINFO= 0x8001 -> 32-byte t_bdid (raw, size = sizeof(bi_record)+32)
BI_MEMCHUNK   = (0, ram_size)
BI_COMMAND_LINE
BI_RAMDISK    = (base, size)   (initrd at top of RAM)
BI_LAST
```
`mvme16x_parse_bootinfo()` only claims BI_VME_TYPE/BI_VME_BRDINFO; the rest are
handled by the generic scanner. BI_VME_TYPE/BRDINFO are board-local tags not in
QEMU's standard-headers, defined locally in mvme16x.c.

## Interrupts (autovector level + user vector)

`mvme16x_init_IRQ()` calls `m68k_setup_user_interrupt(VEC_USER=64, 192)`.
The low-level entry computes `irq = cpu_vector - (VEC_USER - IRQ_USER)
= cpu_vector - 56`. So a Linux irq number N maps to CPU vector N+56.

- **Tick timer**: `MVME16x_IRQ_TIMER = IRQ_USER+25 = 33` -> **CPU vector 0x59
  (89)** delivered at **level 6** (PCCTIC1 low nibble = 6). This is the only
  interrupt required to reach a shell (jiffies / scheduler / calibrate_delay).
- Abort switch (167: IRQ_USER+46) and VME sources are not needed for boot.

The machine aggregates interrupt sources like mvme147: each source presents
`(level<<8)|vector` (0 = idle); highest level wins and supplies the vector via
`m68k_set_irq_level()`.

## PCCchip2 (0xfff42000) — tick timer 1

From `mvme16x_sched_init()` / `mvme16x_read_clk()`:
```
+0x04 PCCTCMP1 be32  compare value (PCC_TIMER_CYCLES = 1e6/HZ; HZ=100 -> 10000)
+0x08 PCCTCNT1 be32  up-counter @ 1 MHz
+0x17 PCCTOVR1 u8    b0 TIC_EN, b1 COC_EN, b2 OVR_CLR(w1c); [7:4] overflow count
+0x1b PCCTIC1  u8    [2:0] level(=6), b3 INT_CLR(w1c), b4 INT_EN
+0x1d PCSCCMICR u8   CD2401 modem int-control (head.S writes 0x10)
+0x1e PCSCCTICR u8   CD2401 tx    int-control; b5(0x20)=tx-int-active (read)
+0x1f PCSCCRICR u8   CD2401 rx    int-control (head.S writes 0x10)
+0x25 PCTPIACKR u8   CD2401 tx peripheral interrupt ACK (read)
```
Timer: a QEMUTimer fires every TCMP1 microseconds when TIC_EN&INT_EN, sets the
pending flag and asserts (6<<8)|89; the handler clears it via PCCTIC1 INT_CLR /
PCCTOVR1 OVR_CLR. TCNT1 read returns elapsed-µs mod TCMP for the clocksource;
overflow nibble counts fired-but-uncleared ticks.

CD2401 SCC int-control: **PCSCCTICR bit5 (0x20) always reads set** (our CD2401 TX
is instantaneous) so the polled console loop in head.S / `mvme16x_cons_write`
always sees "tx ready". PCTPIACKR reads 0.

## CD2401 console (0xfff45000)

Reuse the existing `hw/char/cd2401.c` model (4 channels, chrA..D). The mvme16x
console is **polled through the PCCchip2**, not the CD2401's own IRQ/IACK, so:
byte register file directly at 0xfff45000 (region 0). head.S/`mvme16x_cons_write`
select channel 0 via CyCAR(0xee), set CyIER(0x11)=TxMpty, poll PCSCCTICR bit5,
read CyLICR(0x26) (returns svc_chan<<2 = 0 => channel 0), write char to
CyTDR(0xf8), end with CyTEOIR(0x85). CyGFRCR(0x81) reads nonzero. The model's
region 1 (IACK) is mapped out of the way (unused on this board).

Runtime interactive tty: this kernel has **no CD2401 tty driver** (only the
`earlyprintk` boot console, output-only). Interactive shell input therefore
comes from the **AVME-352** VME serial card (`aval,avme352`, driver
`drivers/tty/serial/avme352.c`, tty `ttyAVME`), attached on the VME host bridge
at 0xf0c00000 and described via a `-dtb` (BI_FDT) — same mechanism mvme147 uses.
Console: `console=ttyAVME0`. Boot log (printk) also appears via `earlyprintk`.

## Kernel / userspace

- Build from linux-drmbounce (`avme352-driver`) with CONFIG_MVME16x=y,
  CONFIG_M68040=y, CONFIG_EARLY_PRINTK=y, CONFIG_BLK_DEV_INITRD=y.
- Console name: `ttyAVME` (avme352) for interactive; `earlyprintk` for boot log.
- cmdline: `earlyprintk console=ttyAVME0` (+ initrd).

## Bring-up results (achieved)

**First 68040 Linux boot in this tree reaches userspace.** With
`-M mvme167 -m 128M -kernel vmlinux -initrd <cpio> -append "earlyprintk"
-icount shift=7` the mvme16x kernel:
- identifies the board from BI_VME_BRDINFO: `BRD_ID: Motorola MVME167 BUG 1.0`
- switches to the `pcc` clocksource, calibrates BogoMIPS (PCCchip2 tick IRQ,
  CPU vector 0x59 @ IPL6 works), mounts the initramfs, runs `/init`, and
  prints from userspace on the CD2401 `earlyprintk` console:
  `CPU: 68040 / MMU: 68040 / FPU: 68040`.
  (see contrib/mvme16x/boot-earlyprintk-userspace.log)

### Kernel config notes (to actually reach userspace under -icount)
- Disable the runtime self-test/benchmark initcalls or they spin forever
  under icount (TEST_*, *_BENCHMARK, DHRY, FIND_BIT_BENCHMARK, RAID6 async
  test, WW_MUTEX_SELFTEST, ...). RUNTIME_TESTING_MENU=n covers most.
- 128 MiB avoids an early OOM ("System is deadlocked on memory") with the
  large default kernel.

### Interactive tty via AVME-352 (ttyAVME) — status and the two walls hit
Attaching `-dtb mvme16x.dtb -device avme352,rom=avme352-ver1dot3.bin,
serial-base=4` (CD2401 on serial 0-3, ttyAVME0 = serial 4) the card boots
its own 68020 firmware, and the driver registers
`6 channels ... ttyAVME0..ttyAVME5, irq 13`; `/init` opens `/dev/ttyAVME0`
and starts a shell on it (contrib/mvme16x/boot-avme352-ttyregister.log).
Two constraints found:

1. **DT + early map / RAM.** head.S maps only `min(16 MiB, RAM)` early;
   `unflatten_device_tree()` (during setup_arch, before paging_init)
   allocates top-down from the memblock populated by the DT `/memory` node.
   So the DT `/memory` node must declare <= 16 MiB (keeps the early DT
   allocation inside the mapped window); bootinfo BI_MEMCHUNK (added to
   memblock only *after* unflatten) then supplies the full RAM for
   userspace. Use `memory@0 { reg = <0 0x1000000> }` in the DTB and
   `-m 64M`. Also disable CONFIG_VT (needs EXPERT) or its dummy tty0 hijacks
   the console from the CD2401 boot console.

2. **AVME-352 host interrupt was never delivered (fixed) + a deeper card
   firmware signalling limit (open).** Two device bugs surfaced opening
   ttyAVME0:
   (a) the avme352 VME dual-ported-RAM window was byte-access only, so the
       driver's 16/32-bit `iowrite` command words faulted the 68040 (bus
       error in `avme352_startup`). Fixed in hw/m68k/avme352.c: accept up
       to 32-bit and split wide big-endian accesses to the byte transfers
       the doorbell logic expects.
   (b) the card raises its host VMEbus interrupt through the generic
       `VMEDevice` whose `irq-level`/`irq-vector` **default to 0**, i.e.
       level 0 = never taken by the CPU. So the driver's TX/RX interrupt
       (it registers the single DT interrupt `intc_user 5` = CPU vector
       0x45 = 69) never fired and the shell TX stalled after the first FIFO
       (only newlines got out). Launch the card with
       `-device avme352,...,irq-level=4,irq-vector=69` and ttyAVME0 begins
       transmitting real characters.
   With both fixed, the shell on ttyAVME0 emits text, but throughput is
   still only a few characters before the card firmware model stops
   re-signalling (its mailbox event reports `CHAN=1` for a channel-0 open,
   suggesting an off-by-one / event-completion gap in the avme352 card
   firmware emulation). Driving a *fully* fluent interactive ttyAVME shell
   needs that card-side signalling finished; the host machine, CD2401
   console, timer, MMU and 68040 boot are all complete.
