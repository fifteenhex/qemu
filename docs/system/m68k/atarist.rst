Atari 1040STF (``atarist``) and 1040STE (``atariste``)
======================================================

The ``atarist`` machine models the Atari 1040STF: a 68000 at 8 MHz
with up to 4MB of RAM behind the GLUE/MMU pair and TOS in ROM.  The
``atariste`` machine models the 1040STE on top of the same chip set;
its additions are listed in their own section below.

Emulated hardware
-----------------

* 68000 CPU with the 24-bit address bus behaviour (TOS addresses the
  hardware through sign-extended short absolute addresses).
* Two DRAM banks decoded by the MMU memory config register,
  including the column-fold aliasing TOS's cold-boot RAM sizing
  probes for; ``-m`` accepts 256K, 512K, 1M (default), 2M, 2.5M and
  4M.
* 192KB TOS ROM at 0xFC0000, with the reset vectors readable at
  address 0.  Unassigned addresses deliver bus errors (TOS probes
  for a blitter, a Mega ST RTC and ACSI devices this way); the empty
  cartridge slot reads as floating 0xFF.
* Shifter video as a framebuffer console: 320x200 in sixteen
  colours, 640x200 in four, and the 640x400 monochrome mode, with
  the 512-colour STF palette.  A colour monitor is attached (MFP
  GPIP7 high).
* MFP 68901: GPIP edge interrupts, the four timers (delay modes plus
  timer B's display-enable event counting, which TOS polls to find
  the vertical blank before it even sizes RAM), vectored to the CPU
  at IPL6 through a GLUE priority encoder that also latches the
  50/60Hz VBL at IPL4 and HBL at IPL2 until interrupt acknowledge.
* The two MC6850 ACIAs with an HD6301 IKBD modelled at protocol
  level behind the keyboard one: relative mouse packets, Atari
  make/break scancodes, joystick and clock commands.  MIDI transmit
  is discarded.
* YM2149 PSG as a register file (no sound), with port A driving the
  floppy drive/side select lines.
* WD1772 FDC behind the ST DMA controller: seek/restore, read/write
  sector including multiple-sector transfers against the DMA sector
  counter, read address, force interrupt.  Two drives
  (``-drive if=floppy``), raw .ST images; geometry is taken from the
  boot sector BPB when sane, else derived from the image size.

The STE additions (``atariste``)
--------------------------------

* 256KB TOS 2.0x ROM at 0xE00000; 0xFC0000 is left open and
  bus-errors like on the real machine.
* STE MCU RAM aliasing: an undersized bank mirrors every chip-size
  bytes instead of the ST MMU's column fold.  TOS 2.06's STE-specific
  cold-boot sizing (taken once a write to the video base low byte
  0xFF820D sticks) probes for mirrors at +0x40000/+0x80000.
* Shifter STE extensions: the 4096-colour palette (bit 3 of each
  nibble is the added *least* significant intensity bit), video base
  low byte 0xFF820D (cleared again whenever the pre-STE hi/mid bytes
  are written), the line-width register 0xFF820F and the horizontal
  fine-scroll register 0xFF8264/65 including the extra per-line
  prefetch words while scrolling.
* The BLiTTER at 0xFF8A00: complete register file and operation
  (halftone/HOP/OP, endmasks, skew, FXSR/NFSR, smudge, the
  ascending/descending source FIFO).  Blits complete within the
  register write that sets the busy bit, so hog and shared mode
  behave identically and restart loops terminate immediately.  The
  GEM desktop's Options->Blitter toggle works and routes the VDI
  through it.
* DMA sound as a register model at 0xFF8900: the frame counter
  advances at the programmed sample rate between the frame base and
  end (single-shot end clears the enable bit; repeat wraps), but no
  audio is produced.  The Microwire mixer interface completes its
  transfer instantly: the data register reads back zero (TOS 2.06's
  boot polls for exactly that) and the mask register keeps its value.
* Enhanced joystick ports at 0xFF9200 reading as "no buttons".

TOS 2.06 idles on the cold-boot memory-test screen until the 200Hz
counter reaches 16000 (80 seconds); pressing any key skips the wait,
and warm boots skip the test entirely.

Firmware
--------

The ``atarist`` machine wants a 192KB TOS 1.0x image, passed with
``-bios`` (default name ``tos104uk.rom``).  TOS 1.04 UK, md5
``036c5ae4f885cbf62c9bed651c6c58a8``, is the tested target; TOS 1.00
and 1.02 load at the same address.

The ``atariste`` machine wants a 256KB TOS 2.0x image mapping at
0xE00000 (default name ``tos206.rom``).  TOS 2.06, md5
``e690bec90d902024beed549d22150755``, is the tested target.

Booting
-------

::

   qemu-system-m68k -M atarist -bios tos104uk.rom \
       -drive if=floppy,file=game.st,format=raw \
       -icount shift=7

With no floppy TOS boots to the GEM desktop.  A bootable .ST image
(boot sector word-checksum 0x1234) is executed, so game compilation
disks start their menus directly.  ``atarist-tools/msa2st.py``
converts MSA images to raw .ST.  The same applies to ``atariste``
with a TOS 2.06 image, though period disks that stop MFP timer C and
then call TOS floppy routines hang on TOS 2.06 (its FDC micro-delay
polls the timer C data register), exactly as on real STE hardware.

``-icount shift=7`` is recommended: TOS times some floppy waits by
comparing against the 200Hz tick counter with an equality test, which
a host scheduling stall can step over when the virtual clock free-runs
at wall speed.
