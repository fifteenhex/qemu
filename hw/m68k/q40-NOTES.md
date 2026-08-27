# Q40 (Sinclair-QL-successor 68040 desktop) — QEMU model notes

Target: boot the Linux/m68k `q40` kernel (CONFIG_Q40, CONFIG_M68040) to an
interactive serial console (ttyS0) via initramfs.

Hardware contract reverse-engineered from the Linux q40 port:
`arch/m68k/q40/{config.c,q40ints.c}`,
`arch/m68k/include/asm/{q40_master.h,q40ints.h,io_mm.h,serial.h}`,
`arch/m68k/kernel/head.S`.

## CPU / RAM
* CPU: 68040 (`m68040`), autovectored interrupts (`m68k_setup_auto_interrupt`).
* RAM based at physical 0. Q40 max on-board DRAM is 32 MiB; default 32 MiB.
* head.S installs transparent translations (no page tables for I/O):
  * TT0: `0xfe000000` +16 MiB, cached  — screen + ROM
  * TT1: `0xff000000` +16 MiB, nocache/serialised — master chip, DAC, RTC, ISA

## Physical memory map
| base        | size      | what                                     |
|-------------|-----------|------------------------------------------|
| 0x00000000  | ram_size  | main DRAM                                |
| 0xfe000000  | 32 MiB    | I/O background absorber (screen/ROM/ISA) |
| 0xff000000  | 0x40      | Q40 master chip registers (byte @ +n*4)  |
| 0xff021fe0  | 0x20      | Q40 RTC (custom, byte @ 4-byte stride)   |
| 0xff400000  | ISA I/O window base (`q40_isa_io_base`)              |
| 0xff400fe1  | 32        | ISA 16550 UART COM1 / ttyS0 (see below)  |
| 0xff800000  | ISA memory window base (`q40_isa_mem_base`)          |

The absorber at 0xfe000000..0xffffffff (background, low priority) keeps the
kernel's unqualified ISA/screen probes from bus-faulting; real devices overlay
it at higher priority.

## Master chip (0xff000000, byte registers, offset = reg*4)
From `q40_master.h` / `q40ints.h`. Register offsets:
* IIRQ_REG 0x00 (r): internal IRQ status
  bit1 KEYB, bit2 SER, bit3 FRAME, bit4 EXT(=an EIRQ is pending)
* EIRQ_REG 0x04 (r): external ISA IRQ status
  bit0 irq3, bit1 irq4, bit2 irq5, bit3 irq6, bit4 irq7, bit5 irq10,
  bit6 irq14, bit7 irq15
* KEY_IRQ_ENABLE_REG 0x08 (w): keyboard IRQ enable
* EXT_ENABLE_REG 0x10 (w): master enable for ISA (external) IRQs
* SAMPLE_ENABLE_REG 0x14 (w): enable SAMPLE (audio) periodic int
* DISPLAY_CONTROL_REG 0x18 (w)
* KEYCODE_REG 0x1c (r): last keyboard scancode
* KEYBOARD_UNLOCK_REG 0x20 (w): ack/clear keyboard int
* FRAME_CLEAR_REG 0x24 (w): ack/clear FRAME int
* SAMPLE_CLEAR_REG 0x28 (w): ack/clear SAMPLE int
* SAMPLE_RATE_REG 0x2c (w)
* LED_REG 0x30 (w): front-panel LED
* FRAME_RATE_REG 0x38 (w): nonzero => enable 200 Hz FRAME periodic int

### Interrupt routing to the 68040 (autovector via M68K_IRQC)
The master chip drives two CPU IPL levels (see q40_init_IRQ which starts up
IRQ_AUTO_2 and IRQ_AUTO_4, and q40_irq_handler dispatch):
* **IPL 2 (IRQ_AUTO_2)**: FRAME timer, KEYBOARD, and all external ISA IRQs
  (SER/EXT). Asserted when
  `frame_pending | (keyb_pending & key_enable) | (ext_pending & ext_enable)`.
* **IPL 4 (IRQ_AUTO_4)**: SAMPLE timer (unused for serial-console bring-up).

FRAME timer: on `FRAME_RATE_REG != 0`, arm a periodic 200 Hz virtual timer.
Each expiry sets FRAME pending -> IPL2. Kernel's q40_timer_int acks with
`FRAME_CLEAR_REG` write. legacy_timer_tick runs every other FRAME (HZ=100).
Without this the scheduler / calibrate_delay hangs.

External ISA IRQ: the 16550's IRQ output (ISA IRQ 4 for COM1/ttyS0) feeds the
master chip. When asserted and EXT_ENABLE_REG!=0: set EIRQ bit1, IIRQ EXT bit
-> IPL2. `q40_irq_handler` reads EIRQ, maps bit1 -> Linux irq 4, dispatches the
8250 handler; needed for interactive (keyboard) input on the console.

## ISA I/O address computation (the tricky part), from io_mm.h
```
q40_isa_io_base  = 0xff400000
Q40_ISA_IO_B(a)  = 0xff400000 + 1 + 4*a   // byte port a -> odd byte lane
Q40_ISA_IO_W(a)  = 0xff400000 +     4*a   // word port a
```
Every ISA byte port `a` maps to a longword-spaced physical address; the byte
lives at the odd (+1) address (big-endian low byte of the 32-bit cell).

COM1 = ttyS0 = ISA port 0x3f8, IRQ 4, BASE_BAUD = 1843200/16 = 115200
(`arch/m68k/include/asm/serial.h`). UART register `r` (8250 uses byte inb/outb)
is at `Q40_ISA_IO_B(0x3f8 + r) = 0xff400000 + 1 + 4*(0x3f8+r) = 0xff400fe1 + 4*r`.
=> `serial_mm_init(base = 0xff400fe1, it_shift = 2 (4-byte stride),
    irq = master-chip ISA4, baudbase = 115200, DEVICE_BIG_ENDIAN)`.

## RTC (custom Q40 RTC, NOT MC146818), from q40_master.h
Byte registers near the top of the 0xff020000 SRAM, 4-byte stride, growing
*down* from `Q40_RTC_BASE = 0xff021ffc`:
* YEAR 0xff021ffc, MNTH -4, DATE -8, DOW -12, HOUR -16, MINS -20, SECS -24,
  CTRL 0xff021fe0.  Values are BCD.
* CTRL bit6 (0x40)=READ latch, bit7 (0x80)=WRITE latch; low bits = PLL trim.
Modelled directly from host wall-clock (read path); writes accepted/ignored.

## Bootinfo (built like hp300.c / mvme16x.c)
`BI_MACHTYPE=MACH_Q40(10)`, `BI_CPUTYPE=CPU_68040`, `BI_MMUTYPE=MMU_68040`,
`BI_FPUTYPE=FPU_68040`, `BI_MEMCHUNK(0, ram_size)`, `BI_COMMAND_LINE`,
optional `BI_RAMDISK(base,size)`, `BI_LAST`. Q40 bootinfo is otherwise minimal.

## Boot command
```
qemu-system-m68k -M q40 -kernel vmlinux -append "console=ttyS0" \
  -nographic -icount shift=7 -initrd initramfs.cpio.gz
```
