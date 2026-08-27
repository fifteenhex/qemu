# BVME4000/6000 (Linux/m68k `bvme6000`) QEMU model — hardware contract

Target: boot the Linux/m68k `bvme6000` platform (CONFIG_BVME6000) to an
interactive serial shell.  We model a **BVME4000-class** board: 68040 CPU,
`BI_VME_TYPE = VME_TYPE_BVME4000` (0x4000).  QEMU's 68040 is solid; the 68060
of a true BVME6000 is incomplete, and the `bvme6000` Linux platform supports
the 68040 via CONFIG_M68040 (config.c: cputype != 68060 => BVME4000).

Primary references (linux-drmbounce, branch avme352-driver):
  arch/m68k/bvme6000/config.c            model/timer/RTC/console wiring
  arch/m68k/bvme6000/rtc.c               DP8570A RTC user driver
  arch/m68k/include/asm/bvme6000hw.h     register addresses
  arch/m68k/include/uapi/asm/bootinfo-vme.h  BI_VME_TYPE / t_bdid
  arch/m68k/kernel/head.S                early SCC debug putc + page tables
  drivers/tty/serial/serial_mvme147_scc.c   the Z8530 SCC console driver
QEMU template: hw/m68k/mvme16x.c (68040 VME, bootinfo/reset/irq plumbing),
hw/m68k/mvme147.c (same Z8530 SCC console driver, ESCC device).

## CPU / machine
  m68040, 1 CPU.  BI_CPUTYPE/MMUTYPE/FPUTYPE = 68040.
  BI_MACHTYPE = MACH_BVME6000 (8).  Machine registered as `bvme4000`.
  RAM at physical 0, default 32 MiB.  head.S transparently maps
  0xe0000000..0xffffffff (TT1, supervisor, no-cache) so all on-board I/O is
  identity-mapped; low RAM has real page tables.

## Physical memory map (on-board I/O, all in 0xff000000-0xffffffff)
  0x00000000  RAM (BI_MEMCHUNK 0, ram_size)
  0xff000000  NCR53C710 SCSI            (not modelled; absorbed)
  0xff100000  i82596 Ethernet          (not modelled; absorbed)
  0xff20000f  BVME_LOCAL_IRQ_STAT      (abort/ether status; reads 0)
  0xff400000  ACR address-control regs (bvme_acr_*; writes ignored)
  0xff500003  BVME_CONFIG_REG          (DIP switches; reads 0)
  0xff900000  DP8570A RTC + timer      (RtcRegs_t, 128 bytes) -- MODELLED
  0xffa00000  MC68230 PIT              (PitRegs_t) -- stub, pcdr bit2 modelled
  0xffb00000  Zilog Z8530 SCC (dual)   (16 bytes) -- MODELLED (console)
  Everything else in 0xff000000-0xffffffff is absorbed by a background region
  (reads 0, writes ignored) so firmware/driver probes don't fault.

## Console UART — Zilog Z8530 SCC  (0xffb00000, the mvme147/bvme console chip)
Register byte lanes (from bvme6000hw.h + head.S), stride 4, byte at +3:
  0xffb00003  channel B control (RR0/WR0 pointer register)
  0xffb00007  channel B data
  0xffb0000b  channel A control     <- console
  0xffb0000f  channel A data        <- console
Decode inside the 16-byte region: reg=(off>>2)&1 (0=ctrl,1=data),
channel=(off>>3)&1 (0=B,1=A).  This is the ESCC "it_shift=2" layout; the
console is channel A (serial_hd(0)), matching mvme147.c's chrA mapping.

Access idioms the software uses (custom driver + head.S):
  * head.S early putc: `btst #2,0xffb0000b` (RR0 bit2 = Tx buffer empty),
    then `moveb d0,0xffb0000f` (write char to DATA port A) -> transmit.
  * driver TX: SCCwrite(WR8,ch) = write 8 to CTRL (point-high sets ptr=8),
    then write ch to CTRL with ptr=8 -> transmit.  (NOT via the data port.)
  * driver RX: read RR8 = write 8 to CTRL (ptr=8), read CTRL -> rx char.
  * RR0 status: CTRL read with ptr==0.  We return
    0x04 (Tx empty) | 0x20 (CTS) | 0x08 (DCD) | (rx_full ? 0x01 : 0).
  * WR0 command decode: cmd=(val>>3)&7, ptrlow=val&7.  cmd==1 => "point high"
    ptr = 8|ptrlow; otherwise ptr = ptrlow and cmd 2..7 are reset ops
    (Reset Ext/Status, Reset TxIP, Error Reset, Reset Highest IUS) -> no-ops
    except they recompute the IRQ.  After any non-WR0 register access ptr->0.
  * WR1 bit3/4 (Rx Int on first/all char) + WR9 bit3 (Master Int Enable)
    gate the receive interrupt.  We do NOT raise Tx or Ext/Status interrupts
    (console TX is fully polled; DCD is static), avoiding spurious vectors.

### SCC interrupt (the interactive-input path)
BVME6000 SCC interrupts are *vectored* m68k user interrupts.  The kernel:
  m68k_setup_user_interrupt(VEC_USER=64, 192);  vectors[64+irq-8] = handler.
  request_irq(BVME_IRQ_SCCA_RX = IRQ_USER+12 = 20) installs at CPU vector 76.
The Z8530 "vector includes status, status low" (WR9 bit0) modifies bits 3..1
of the WR2 base per source; channel A Rx = code 0b110 -> base|0x0C.  For the
kernel's vectors[76] the base must be VEC_USER = 0x40, giving 0x4C for A-Rx
(and B 0x40..0x46, A 0x48..0x4E).  The in-tree driver programs WR2 with
BVME_IRQ_SCC_BASE = IRQ_USER = **8** (a bug: that is the Linux irq number, not
the CPU vector base), which would land on CPU vector 0x0C.  **This model
therefore forces the SCC vector base to 0x40** when computing the acknowledged
vector, matching the architecturally-intended BVME vectors regardless of the
driver's WR2 value.  Channel A Rx -> vector 0x4C at **IPL 4**.

## Timer tick — DP8570A RTC timer 1  (0xff900000, IRQ_AUTO_6 = IPL 6)
Registers are RtcRegs_t: byte at offset i*4+3, so reg index = off>>2:
  0 msr, 1 t0cr_rtmr, 2 t1cr_omr, 3 pfr_icr0, 4 irr_icr1, 5 bcd_tenms,
  6 bcd_sec, 7 bcd_min, 8 bcd_hr, 9 bcd_dom, 10 bcd_mth, 11 bcd_year,
  14 bcd_dow, 17 t1lsb, 18 t1msb, ...
config.c bvme6000_sched_init programs timer 1 for a 100 Hz (HZ=100) tick and
request_irq(BVME_IRQ_RTC = IRQ_AUTO_6).  bvme6000_timer_int acks by writing
`msr | 0x20`.  Model:
  * A periodic QEMU_CLOCK_VIRTUAL timer at HZ=100 (10 ms).  It is armed once
    the kernel writes pfr_icr0 with bit7 (0x80, "timer 1 int enable").
  * On expiry: set msr bit 0x20 (T1 interrupt) and raise autovector IPL 6
    (CPU vector VEC_SPUR+6 = 30, where vectors[30]=auto_inthandler ->
    do_IRQ(6) -> bvme6000_timer_int).
  * On a write to msr with bit 0x20 set: clear the pending T1 int, lower IPL 6.
  * Banking (msr selecting register windows) is ignored: every register is
    always readable at its fixed offset.
  * bcd_* return a fixed valid BCD date (2025-01-01 12:00:00, dow=Wed).
  * Clocksource read (bvme6000_read_clk) latches t1msb/t1lsb: we return a
    constant mid-range count (0x2710 = 10000, <= RTC_TIMER_COUNT-1% = 19800)
    so its convergence loop exits after 2-3 iterations; jiffies advance via the
    tick's clk_total, so the clocksource is still monotonic.

## MC68230 PIT stub (0xffa00000)
config_bvme6000 programs many PIT registers; all writes are accepted/ignored.
Only `pcdr` (port C data, reg index 11 -> byte off 0x2f) matters: read returns
0x04 (bit2 = timer-1 output high), which bvme6000_read_clk samples.  reset()
sets the watchdog via port C and spins -- not needed to reach a shell.

## Interrupt aggregation
Two active sources, each encoded (level<<8)|vector (0 = idle), higher level
wins, supplying the vector via m68k_set_irq_level():
  RTC timer : (6<<8) | 0x1E   (autovector level 6)
  SCC A Rx  : (4<<8) | 0x4C   (vectored user interrupt)
Abort (IPL 7) is not modelled.

## Bootinfo block (mvme16x.c layout)
BI_MACHTYPE=MACH_BVME6000, BI_CPUTYPE/MMUTYPE/FPUTYPE=68040,
BI_VME_TYPE=VME_TYPE_BVME4000, BI_MEMCHUNK(0,ram_size), BI_COMMAND_LINE,
BI_RAMDISK (from -initrd, placed at top of RAM), BI_LAST.  head.S consumes
BI_VME_TYPE into vme_brdtype (used by the console registration); BI_VME_BRDINFO
is only required by MVME16x, not by BVME6000, so it is omitted.

## Boot
  qemu-system-m68k -M bvme4000 -kernel vmlinux -append "console=ttyS0" \
      -initrd rootfs.cpio.gz -nographic -icount shift=7
Console device is **ttyS0** (channel A): the driver registers uart dev_name
"ttyS", console index 0 == channel A.

## Kernel-side changes required (linux-drmbounce, branch avme352-driver)
Two fixes were needed in the (WIP) in-tree BVME support to reach a shell:

1. arch/m68k/bvme6000/config.c: nothing instantiated a platform device for
   the SCC console driver, so its probe never ran and no ttyS port/console
   was created.  Added a device_initcall that does
   `platform_device_register_simple("mvme147-scc", -1, NULL, 0)` (the driver
   matches by name; bvme6000_scc_init() uses fixed addresses, no resources).

2. drivers/tty/serial/serial_mvme147_scc.c, bvme6000_scc_init():
   * both channel-init blocks wrote `port = &ports->scc_port` (== ports[0])
     instead of `&sccp->scc_port`, so channel B clobbered channel A's port and
     ttyS0 ended up pointing at channel B's registers (0xffb00003).  Console
     output appeared on the wrong SCC channel.  Fixed to `&sccp->scc_port`.
   * request_irq() was passed `port` (a struct scc_port *) but the handlers
     (scc_rx_int etc.) expect their dev_id to be a struct mvme147_scc_serial_port *
     (`sccp`).  With the wrong pointer, receive interrupts dereferenced garbage.
     Fixed all eight request_irq() calls to pass `sccp`.
   After these, ttyS0 == channel A (0xffb0000b), console output and
   interactive receive both work.

The SCC-vector-base note above (forcing 0x40) means no third kernel change was
needed for the driver's WR2 = IRQ_USER programming.
