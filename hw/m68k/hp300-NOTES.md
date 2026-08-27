# HP 9000/300 (hp300) QEMU machine — hardware contract

Target: boot the Linux/m68k `hp300` kernel (CONFIG_HP300) to a serial shell.
Model emulated: **HP 9000/340** (16 MHz 68030, the only model the Linux
README says was actually tested). Machine name: `hp9000-340`.

Authoritative sources (in /workspace/src/linux-drmbounce):
- arch/m68k/hp300/config.c        model detect, RTC protocol, serial console setup
- arch/m68k/hp300/time.c          98630/MC6840 clock timer, IRQ_AUTO_6 tick
- arch/m68k/kernel/head.S         early console, MMU window, bootinfo parse
- arch/m68k/include/uapi/asm/bootinfo-hp300.h   BI_HP300_* tags, model IDs
- drivers/tty/serial/8250/8250_hp300.c          DCA UART glue
- include/linux/dio.h, drivers/dio/dio.c        DIO select-code → phys addr

## CPU / MMU / FPU
- CPU: m68030 (QEMU `m68030`). MMU: 68030 (pmove tables). No FPU (real 340
  has no FPU; virt.c also emits no BI_FPUTYPE for a 68030, kernel copes).
- Interrupts: **autovector** (IPL 3–6). Use `hw/intc/m68k_irqc.c`
  (TYPE_M68K_IRQC): gpio-in index = M68K_IRQC_LEVEL_n, raises CPU IPL with
  autovector n+25.

## Physical memory map (what QEMU maps)
The kernel head.S maps virtual **0xf0000000 → phys 0x0** (32 MB on 030,
`_PAGE_NOCACHE030`) as the I/O window, and maps main RAM separately via
PAGE_OFFSET (=0 on m68k). So all built-in I/O lives at LOW physical
addresses and RAM must live ABOVE the 32 MB I/O window.

| phys range              | contents                                    |
|-------------------------|---------------------------------------------|
| 0x00000000–0x0001ffff   | boot ROM / LED latch stub (absorb writes; LED at 0x1ffff) |
| 0x00420000–0x004200ff   | RTC (HP custom, command protocol, offs 1=data 3=cmd)      |
| 0x005f4000–0x005f4fff   | 030/040 FPU+cache "energise" ctrl reg (writes absorbed)   |
| 0x005f8000–0x005f80ff   | 98630 clock timer (MC6840; regs at odd offsets 1,3,5,7)   |
| 0x00690000–0x00690010   | DCA (98644) DIO card-control regs (ID/IPL/IC)             |
| 0x00690011–0x00690020   | DCA 8250 UART (8 regs, regshift 1, big-endian)           |
| everything else         | bus-error background (MEMTX_DECODE_ERROR) — DIO probe skips |
| 0x20000000 + size       | main RAM (default 16 MiB; use -m 128M for initramfs)     |

RAM base = **0x20000000 (512 MiB)**.  This is critical: the kernel's DIO scan
walks select codes 0..255; DIO-II codes 132..255 map to phys
0x1000000 + (sc-132)*0x400000, reaching up to 0x1fc00000.  RAM must live
ABOVE that whole range or the fault-protected probe reads RAM as bogus
"boards" (and floods "iounmap: bad pmd").  0x20000000 is exactly one DIO-II
frame past the last select code, so every DIO/DIO-II probe hits the
bus-error background and reads "no card" except the real DCA at scode 9.

Also required: the FPU/cache "energise" register at phys 0x5f400c — head.S
writes it (movel #0x60,0xf05f400c) immediately AFTER mmu_engage; without a
backing region this bus-errors (VBR still 0) and storms into a DOUBLE MMU
FAULT.  This was the first wall hit during bring-up.

Boot needs ~128 MiB (embedded/initramfs busybox unpack OOMs at 16 MiB:
"System is deadlocked on memory").  The stock hp300_defconfig also enables
the HP SDC/HIL controller (oopses probing 0xf0428000) and a pile of KUNIT
tests / raid6 benchmark that crawl for minutes under -icount; all disabled
in the minimal config used here.

## DIO select code → phys addr
- DIO_BASE 0x600000, DIO_DEVSIZE 0x10000. scode<32: phys = 0x600000 + scode*0x10000.
- DIO-II: DIOII_BASE 0x1000000, DIOII_DEVSIZE 0x400000, scode 132..255.
- **Built-in DCA serial: select code 9, ipl 5** → phys 0x690000
  (confirmed against NetBSD `dca0 at dio0 scode 9 ipl 5`).

## DCA (98644) UART — the console
- Card base = 0x690000. Register offsets (in_8, byte):
  - 0x01 DIO ID (read) = 0x02 (DIO_ID_DCA0); (write = reset, ignored).
  - 0x03 DIO IPL (read) = 0x20 → DIO_IPL = ((0x20>>4)&3)+3 = 5. (write = DCA_IC).
- 8250 UART starts at card+0x11 (UART_OFFSET=17), regshift 1 → reg N at
  0x690011 + N*2. LSR (reg5) at 0x69001b (matches head.S DCALSR=0x1b,
  DCADATA=0x11). baudbase = HPDCA_BAUD_BASE = 153600. IRQ → IRQC LEVEL_5.
- Early head.S console: iobase defaults to 0 (pre-MMU) then 0xf0000000
  (post-MMU) — both resolve to phys 0x690011, so no aliasing needed.
- Console registers as **ttyS0**; boot with `console=ttyS0`.

## Clock timer (0x5f8000) — the scheduler tick (ESSENTIAL)
MC6840-style PTM. sched_init: CR2(off3)=1 (select CR1), CR1(off1)=1 (reset),
movep.w INTVAL to off5 (T1 latch), request_irq(IRQ_AUTO_6), CR1=0x40 (enable).
T1 clock 250 kHz, INTVAL = 250000/HZ-1 = 2499 (HZ=100) → 10 ms tick.
Reused almost verbatim from apollo.c's MC6840 PTM model (reg = addr>>1). T1
flag+enable drives IRQC LEVEL_6. Ack protocol: read status (reg1) then read
counter MSB (reg2) clears the flag. Without this IRQ, calibrate_delay() hangs.

## RTC (0x420000) — must not hang boot
HP custom command RTC. config.c hp300_hwclk() busy-waits on status bits, so
it MUST respond: status read (off3) returns 0x41 (DATA_RDY set, BUSY clear,
high nibble RTC_STAT_RDY=0x40). Command protocol: write 0xe0 (SETREG) then
reg# to data; write 0xc3 (READREG); read data (off1) returns BCD digit of the
selected register (13 regs: sec/min/hour/wday/day/mon/year split tens/units).
Values from host time via qemu_get_timedate.

## bootinfo block (built in RAM at (kernel_high+1)&~1)
Tags (BOOTINFO macros, big-endian):
- BI_MACHTYPE   = MACH_HP300 (9)
- BI_CPUTYPE    = CPU_68030, BI_MMUTYPE = MMU_68030 (feature-detected)
- BI_HP300_MODEL (0x8000)      = HP_340 (2)
- BI_HP300_UART_SCODE (0x8001) = 9
- BI_HP300_UART_ADDR  (0x8002) = 0x690000  (DCA card base; head.S adds DCADATA)
- BI_MEMCHUNK   = (0x20000000, ram_size)
- BI_COMMAND_LINE = kernel cmdline
- BI_RAMDISK    = (initrd_base, initrd_size)  [if -initrd]
- BI_LAST

## Kernel load
vmlinux ELF is linked ~0x1000; load via load_elf with translate_fn adding
RAM_BASE (0x20000000). reset hook sets pc = elf_entry + RAM_BASE, a7 = top of
RAM - 0x1000. head.S is PC-relative and recomputes its own phys base.

## Result — booted to an interactive shell

    qemu-system-m68k -M hp9000-340 -m 128M -kernel vmlinux \
        -initrd initrd.cpio.gz -append "console=ttyS0" -nographic -icount shift=7

Console excerpt (full transcript in hp300-boot-transcript.txt):

    Detected HP9000 model 340
    Serial console is HP DCA at select code 9
    printk: console [ttyS0] enabled
    Calibrating delay loop... 7.57 BogoMIPS
    Scanning for DIO devices...
    select code   9: ipl 5: ID 02: 98644A DCA0 serial
    serial8250: ttyS0 MMIO:0x00690011 (irq = 5, base_baud = 153600) is a 16550A
    rtc-generic rtc-generic: setting system clock to 2026-08-27 ...
    Run /init as init process
    ==== HP9000/340 (hp300) USERSPACE ALIVE ====
    ~ # uname -a
    Linux (none) 7.2.0-hp300 ... m68k GNU/Linux
    ~ # cat /proc/cpuinfo
    CPU:  68030   MMU: 68030   FPU: none   BogoMips: 7.57
    ~ #

Both the embedded-initramfs (CONFIG_INITRAMFS_SOURCE) and the external
-initrd (BI_RAMDISK) paths work; the transcript above is the -initrd path.
