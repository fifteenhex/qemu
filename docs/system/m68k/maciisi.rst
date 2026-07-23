Apple Macintosh IIsi (``maciisi``)
==================================

The ``maciisi`` machine models the 1990 Macintosh IIsi: a 68030 with
the RBV (RAM-Based Video) chipset and onboard 640x480 video.

Emulated hardware
-----------------

* 68030 CPU, using the 68030 PMMU table walk (the IIsi ROM runs the
  real 24/32-bit MMU mappings).
* RAM at 0 (``-m``, default 8MB), 512KB ROM at 0x40800000 with the
  repeating I/O slice at 0x50F00000 that the ROM's machine
  identification probes.
* VIA1, and the RBV providing the VIA2-style interrupt registers,
  monitor sense and the pseudo-NuBus slot $E interrupts for the
  onboard video.
* Onboard video: 640x480 1-bit framebuffer with the VDAC CLUT and the
  slot $E declaration ROM, so the Slot Manager finds and initialises
  the video "card".
* Egret ADB/PRAM microcontroller, modelled at the protocol level on
  the VIA1 shift register (the startup exchange and the classic
  bit-banged RTC/PRAM protocol work).
* Z8530 SCC serial ports.
* NCR5380 SCSI (``hw/scsi/ncr5380.c``, written for this machine) with
  both pseudo-DMA windows, driven register-level by the ROM's SCSI
  Manager including its blind multi-byte reads.
* EASC sound chip and SWIM floppy controller (register-level;
  no media path yet).

Firmware
--------

The 512KB Mac IIsi ROM (checksum dword ``36B7FB6C``, universal ROM
version $067C), passed with ``-bios``.  The image used during
development came from archive.org item
``mac_rom_archive_-_as_of_8-19-2011``, in-zip file
``36B7FB6C - Mac IIsi.ROM``, md5
``373f0b2150bc391227b7a2e32ac5ff2c``.

Booting
-------

::

   qemu-system-m68k -M maciisi -bios maciisi.rom -icount shift=7 \
       -drive file=macos753.hda,format=raw,if=none,id=hd0 \
       -device scsi-hd,drive=hd0,scsi-id=0

``-icount shift=7`` is required: the ROM calibrates its TimeDBRA
delay-loop constants against the VIA timers, and only a realistically
paced CPU produces sane values (an uncalibrated boot dies with a
division by zero in the ADB stack).  Boot is correspondingly slow —
several minutes of real time to the first pixels.

A suitable disk image is any raw Apple-partitioned Mac OS disk, e.g.
the Mac OS 7.5.3 BlueSCSI image from archive.org item
``hd-0-imaged-001``.

What works
----------

The ROM passes its power-on tests, identifies the machine, completes
the Egret startup exchange, initialises the onboard video and reaches
the blinking insert-disk icon with a live cursor.  With a SCSI disk
attached it scans the bus, reads the driver and boot blocks, loads
the System and renders the "Welcome to Macintosh" startup screen.

Known limitations
-----------------

* The boot does not yet reach the Finder desktop; startup stops after
  the welcome screen.
* No ADB input: the Egret model covers the startup exchange and
  RTC/PRAM, but keyboard and mouse packets are not implemented.
* The SWIM floppy has no drive/media emulation.  The EASC can be
  given a backend (``-M maciisi,audiodev=<id>``) but sound has not
  been exercised.
