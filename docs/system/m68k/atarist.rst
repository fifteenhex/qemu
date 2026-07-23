Atari 1040STF (``atarist``)
===========================

The ``atarist`` machine models the Atari 1040STF: a 68000 at 8 MHz
with up to 4MB of RAM behind the GLUE/MMU pair and TOS in ROM.

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

Firmware
--------

The machine wants a 192KB TOS 1.0x image, passed with ``-bios``
(default name ``tos104uk.rom``).  TOS 1.04 UK, md5
``036c5ae4f885cbf62c9bed651c6c58a8``, is the tested target; TOS 1.00
and 1.02 load at the same address.  256KB TOS 2.0x images map at
0xE00000 and are not supported yet.

Booting
-------

::

   qemu-system-m68k -M atarist -bios tos104uk.rom \
       -drive if=floppy,file=game.st,format=raw \
       -icount shift=7

With no floppy TOS boots to the GEM desktop.  A bootable .ST image
(boot sector word-checksum 0x1234) is executed, so game compilation
disks start their menus directly.  ``atarist-tools/msa2st.py``
converts MSA images to raw .ST.

``-icount shift=7`` is recommended: TOS times some floppy waits by
comparing against the 200Hz tick counter with an equality test, which
a host scheduling stall can step over when the virtual clock free-runs
at wall speed.
