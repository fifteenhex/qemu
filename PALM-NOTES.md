# Palm V (MC68EZ328 DragonBall EZ) emulation — working notes

Goal: reuse/fix the existing DragonBall EZ (MC68EZ328) support in this
tree to emulate a Palm PDA and boot real PalmOS ROMs.  Work happens on
the `palm` branch (based on `amiga`, which carries the m68k core fixes
and the DragonBall device models), working clone
`/workspace/src/qemu-palm`.

NOTE: other sessions work on the `amiga` branch — do not push to it
from here; push to `palm` only.

## Device choice: Palm V

Picked the Palm V as the machine to model:

  - MC68EZ328 DragonBall EZ @ 16.58 MHz (same SoC family the tree
    already models for the `mc68ez328` dev-board machine)
  - 2 MB pseudo-static RAM at chip-select DWE/CSD
  - 2 MB flash ROM
  - 160x160 mono/4-grey LCD driven by the EZ's internal LCDC
  - touchscreen ADC + battery sense on the SPI master; buttons on GPIO
  - serial cradle (RS232) on the EZ UART

Alternatives considered: Palm IIIx (same SoC, chunkier case, 4MB RAM),
Palm m100 (same SoC, OS 3.5), Palm Vx (20 MHz, 8MB).  The V was chosen
because it is the iconic EZ device, ROMs are plentiful, and OS 3.x is
the least demanding of the hardware.  Extending to Vx/IIIx later is
mostly a RAM-size and ROM-file matter.

## ROMs

From archive.org item `20250707_20250707_0134` ("Palm Pilot Roms", a
mirror of the PalmDB "complete" ROM set), downloaded 2026-07-20 into
`/workspace/src/palm-roms/` (not in git):

    Palm-V-3.3-en.rom       1277952  md5 6b347dada1c8b6bbc7546cc0f7281990
    Palm-V-3.3-en-dbg.rom   1343488  md5 be0593f087aa334800f3293161229d4e
    Palm-V-3.1-en.rom       2097152  md5 c575ebb95f736e389d9c29ad919b4753

    curl -sLO https://archive.org/download/20250707_20250707_0134/Palm-V-3.3-en.rom

Primary bring-up target: `Palm-V-3.3-en.rom`; the `-dbg` variant is
built with debugging enabled (extra serial chatter — useful against
our UART model).

### ROM image layout (from the 3.3-en image header)

    +0x000  00003000    initial SP (in the reset vector slot)
    +0x004  10C0823C    initial PC  -> ROM runs at 0x10C00000
    +0x008  FEEDBEEF    PalmOS card-header signature
    +0x010  "PalmCard", +0x030 "Palm Computing"
    +0x3000             big ROM starts (first 0x3000 bytes are the
                        "small ROM": boot + mini-debugger, strings
                        include "Welcome to the PalmOS Debugger!")

Conclusions for the machine model:

  - ROM must appear at 0x10C00000 (that is where CSA0 points after
    the small ROM programs the chip selects; the reset PC already
    assumes the ROM is visible there, because on real silicon CSA0
    responds to the *entire* address space until programmed).
  - RAM lives at 0x00000000.
  - Easiest reset scheme (matches what the existing `mc68ez328`
    machine does by hand): reset hook loads SP from ROM+0 and PC from
    ROM+4 instead of modelling the boot-time "CSA0 everywhere"
    aliasing.

## Existing DragonBall support in the tree

Machine `mc68ez328` (hw/m68k/mc68ez328.c) — a *dev board*, not a
Palm: 8MB RAM at 0, 8MB flash at 0x10000000, DS1305 RTC + SD card on
the SPI bus, and a hacked reset that jumps to flash+0x400 (real
reset-vector read is commented out).  Device models under
hw/*/dragonball_*.c.  Survey of each (2026-07-20), vs the MC68EZ328
user's manual:

  - PLL (hw/misc/dragonball_pll.c): PLLCR/PLLFSR r/w; PLLFSR read
    exposes the toggling CLK32 bit 15 off rtc_clock — that is the
    important part, PalmOS delay loops poll it.  Frequency calc is
    printf-only (and has a 32786-vs-32768 typo).  Good enough.
  - INTC (hw/intc/dragonball_intc.c): IMR/IPR/ISR/IVR modelled, 31
    "peripheral_interrupts" gpio-in lines, ISR = ~IMR & IPR.  The
    source->m68k-level table only knows 3 sources: SPI=4, TMR=6,
    UART=4 (dragonball_irq_levels[]); every other source is level 0
    and silently dropped.  ICR/IWR not implemented.  Needs the full
    EZ level map (PEN=5, RTC=4, KB=4, IRQ1/2/3/6, EMUIRQ...) for
    Palm.
  - GPIO (hw/gpio/dragonball_gpio.c): 7 ports A-G, dir/data/puden/sel
    modelled with per-port reset values; *outputs* work (used for SPI
    chip selects).  Input injection is a no-op (dragonball_gpio_set
    is empty) and the Port D interrupt/keyboard regs (PDPOL,
    PDIRQEN, PDKBEN, PDIRQEG) are #if 0 — so no buttons and no GPIO
    IRQs yet.  Both needed for Palm hard keys + pen-down.
  - Timer (hw/timer/dragonball_timer.c): timer 1 only (chip has 2 on
    EZ? no — EZ has 1, we're fine), TCTL/TPRER/TCMP/TCN/TSTAT via
    ptimer, IRQ wired to INTC line 1.  TCR capture missing.  TCN
    reads are an independent rtc_clock approximation.  Usable for
    the PalmOS system tick.
  - SPI (hw/ssi/dragonball_spi.c): SPIM master, 8/16-bit transfers
    on the QEMU SSI bus, XCH/IRQ semantics with a ptimer simulating
    transfer time.  Functional.  Palm V hangs its touchscreen ADC
    here.
  - UART (hw/char/dragonball_uart.c): RX FIFO from chardev, TX via a
    ptimer at hardcoded 9600 baud, FIFO-level IRQs wired.  TX write
    decoded at odd offset 0x7 only (byte writes to UTX low byte);
    UBAUD parsed but unused; UMISC/NIPR stubs.  Works as console.
  - LCDC (hw/display/dragonball_lcdc.c): framebuffer scanned out of
    system memory at LSSA via framebuffer_update_display; 1bpp only
    (2/4bpp draw fns are empty stubs), hardcoded greenish LCD
    palette; write decode covers only LSSA/LVPW/LXMAX/LYMAX — LPICF
    (bit depth) is *not writable*; printf spam on every access; reset
    registered twice.  OK for first boot (PalmOS 3.x boots in 1bpp),
    grayscale needs the 2bpp path.
  - RTC (hw/rtc/dragonball_rtc.c): complete stub — reads 0, ignores
    writes, IRQs never raised, not connected to INTC.  Palm V uses
    the on-chip RTC (no external one), so PalmOS needs at least
    RTCTIME/RTCCTL/RTCISR to keep time; watchdog too eventually.
  - No SCR (0xfffff000) or chip-select unit (0xfffff100) device —
    accesses fall to unassigned memory.  Need at least logging stubs
    so we can see what the small ROM pokes.
  - CPU: plain `m68000` core (no dragonball variant in target/m68k);
    correct family for the EZ's 68EC000 — fine.
  - Nothing Palm-specific anywhere in the tree.

## Journal

### 2026-07-20 (later) — PalmOS 3.3 boots to the Setup wizard

`palmv` machine added (hw/m68k/palm.c): 2MB RAM at 0, 4MB ROM window
at 0x10c00000 pre-filled with 0xff (erased flash), ROM image loaded
at +0x8000, reset SP/PC pulled from the big-ROM card header.  Run:

    qemu-system-m68k -M palmv -bios Palm-V-3.3-en.rom

ROM image layout — resolved.  The archive .rom files are *big ROM
only*: the card header at file offset 0 is the big ROM's own header
(bigROMOffset field = 0x10c08000 = where file offset 0 must land).
There is no small ROM in the files; the entry point at +0x23c is a
`jmp` followed by the ASCII tag "boot".  Verified the same for 3.1
(entry +0x22a).  First wrong theory (file = small ROM at 0 + big ROM
at 0x3000) put the reset PC mid-instruction — beware.

Boot-path findings, in order hit:

  1. Reset hook must not use load_image_targphys: the ROM-loader
     copies the image in from its own reset hook, *after* ours reads
     the vectors -> read 0xff, jumped to -1.  Load directly into the
     MemoryRegion instead.
  2. Unmapped on-chip regs (SCR 0xfffff000, chip selects 0xfffff100,
     DRAMC 0xfffffc00) caused bus-error -> double fault.  On silicon
     the fffff page never bus-errors, so the machine sets
     ignore_memory_transaction_failures.  The boot code writes SCR=0xf8,
     CSGBA=0x8600 (0x8600<<13 = 0x10c00000 — ROM base confirmed),
     CSD/DRAMC for RAM, reads the chip-ID at 0xfffff004 (we return 0,
     it takes the default EZ path — fine).
  3. dragonball_spi asserted on transfer widths other than 8/16 —
     PalmOS uses odd widths for the touchscreen ADC.  The EZ SPIM does
     1..16 bits; fixed the model to match (MSB-first byte chunks,
     result masked).
  4. PLL gets programmed to 16589716 Hz — the Palm V's real 16.58MHz.
     LCDC gets LSSA=0x25b0 (fb in RAM), LVPW=0xa, LXMAX=0xa0,
     LYMAX=0x9f -> 160x160x1bpp.  PCTLR (0xfffff207) written 0xc0,
     then the OS idles in PrvShutDownCPU via `stop #2000` — normal
     PalmOS doze, woken by interrupts.
  5. Screendump after ~8s: the OS 3.3 *Setup wizard page 1 of 4*
     renders fully.  Boot works end to end.

Known gaps after first boot:

  - LCD polarity inverted: model paints 1-bits light on dark; a Palm
    paints ink (1) dark on a pale LCD.  Swap the two hardcoded colours
    (proper palette/contrast handling later).
  - No input: pen needs the PENIRQ (IPR bit 20, level 5; INTC level
    table lacks it) + an ADS7843-style ADC on the SPI bus (QEMU has
    hw/display/ads7846.c, POSE models ADS784x for the Palm V — likely
    reusable).  Buttons need GPIO port D input + interrupt regs.
  - RTC still a stub; OS will lose time.  Timer tick evidently works
    (UI drew and updates).

### 2026-07-20 — project start

  - Cloned `amiga` -> `/workspace/src/qemu-palm`, created `palm`
    branch.
  - Chose Palm V, fetched ROMs from archive.org (see above), verified
    the card header and derived the ROM base address from the reset
    vectors.
  - Next: survey the DragonBall device models for gaps vs the EZ328
    user manual, then add a `palmv` machine with the Palm memory map.
