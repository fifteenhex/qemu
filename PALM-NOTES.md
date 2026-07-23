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

### 2026-07-20 — project start

  - Cloned `amiga` -> `/workspace/src/qemu-palm`, created `palm`
    branch.
  - Chose Palm V, fetched ROMs from archive.org (see above), verified
    the card header and derived the ROM base address from the reset
    vectors.
  - Next: survey the DragonBall device models for gaps vs the EZ328
    user manual, then add a `palmv` machine with the Palm memory map.
