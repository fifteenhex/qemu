Apple Macintosh 128K (``mac128k``)
==================================

The ``mac128k`` machine models the original 1984 Macintosh: a 68000
at 7.8336 MHz with 128KB of RAM and the 64KB Rev A ROM.

Emulated hardware
-----------------

* 68000 CPU with the 24-bit address bus behaviour (tagged Memory
  Manager pointers work as on the real machine).
* 128KB RAM, mirrored through the whole 4MB RAM window exactly as the
  ROM's memory sizing expects.  ``-m`` accepts any power of two up to
  4MB for aftermarket-style RAM upgrades; 128KB is the default.
* 64KB ROM at 0x400000, with the reset overlay (ROM aliased at 0
  until the ROM clears VIA PA4).
* VIA 6522: timers, the bit-banged RTC/PRAM protocol, screen/sound
  page and overlay bits, mouse button, 60.15Hz VBL.
* Z8530 SCC, with the mouse quadrature interrupts on the DCD pins.
* IWM floppy controller with one internal Sony 400K GCR drive, backed
  by a raw 400K (409600 byte) disk image (``-drive if=floppy``).
  The full boot-time drive probe is modelled: eject strobe timing,
  drive presence sense, and the PWM spindle speed control that the
  ROM calibrates against the tach.
* 512x342 1-bit video scanned directly out of main RAM.

Firmware
--------

The machine needs the 64KB Macintosh 128K Rev A ROM (checksum dword
``28BA61CE``), passed with ``-bios``.  The image used during
development came from archive.org item ``Macintosh-128K-ROM-Image``,
md5 ``1d7f52d2d490524954f6afce083d9593``.

Booting
-------

::

   qemu-system-m68k -M mac128k -bios Mac128K.ROM \
       -drive if=floppy,file=system11.img,format=raw \
       -icount shift=7

``-icount shift=7`` is required: the ROM calibrates the floppy
spindle speed and paces the mouse handlers with CPU-timed delay
loops, so the CPU must run at a realistic 68000 pace relative to the
virtual clock.  Without it the ROM's speed calibration divides by
zero and the machine sad-macs (error 0F0004).

Suitable boot floppies are raw 400K MFS system disks, e.g. System 1.1
/ Finder 1.1g (archive.org item ``system-1.1_apr1984``) or System 2.0
/ Finder 4.1 (item
``apple-mac-os-system-2.0-finder-4.1-macintosh-system-disk-apr-1985-3.5-400k``).

What works
----------

System 1.1 boots through the happy Mac and "Welcome to Macintosh" to
the Finder desktop, with a fully working mouse: clicking, menu
tracking and File > Open all behave.  System 2.0 boots to its
MiniFinder and on into Finder 4.1.  Without a disk the ROM shows the
blinking insert-disk icon.

Known limitations
-----------------

* The floppy is read-only: the disk always reports write-protected
  and there is no GCR write/decode path, so the Finder cannot save to
  disk (fine for locked-floppy boots).
* Ejecting a disk from the Finder re-inserts it about a second later
  when the image stays attached (a stand-in for the user pushing the
  disk back in).
* The keyboard is not modelled; the ROM's keyboard query times out
  harmlessly, like a real Mac with the keyboard unplugged.
* Sound is not modelled (the sound-page PWM bytes only feed the
  floppy speed control).
