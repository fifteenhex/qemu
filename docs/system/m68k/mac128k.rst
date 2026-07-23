Apple Macintosh 128K family (``mac128k``, ``mac512k``, ``macplus``)
===================================================================

These machines model the original 1984-1986 compact Macintoshes: a
68000 at 7.8336 MHz with the classic VIA + SCC + IWM chipset and the
512x342 1-bit display scanned out of main RAM.

* ``mac128k`` -- the original Macintosh: 128KB RAM, 64KB Rev A ROM.
* ``mac512k`` -- the same board with 512KB soldered on.
* ``macplus`` -- the Macintosh Plus: 128KB v3 ROM, SIMM sockets for
  up to 4MB RAM, NCR5380 SCSI and the 800K double-sided floppy drive.

Emulated hardware
-----------------

* 68000 CPU with the 24-bit address bus behaviour (tagged Memory
  Manager pointers work as on the real machine).
* RAM mirrored through the whole 4MB RAM window exactly as the ROM's
  memory sizing expects.  On ``mac128k``/``mac512k``, ``-m`` accepts
  any power of two up to 4MB for aftermarket-style RAM upgrades.  On
  ``macplus``, ``-m`` accepts the real SIMM configurations 1M, 2M,
  2.5M and 4M (the default); 2.5M is modelled as a full 2MB bank A
  plus a 512K bank B mirrored through its half of the window.
* ROM at 0x400000, with the reset overlay (ROM aliased at 0 until the
  ROM clears VIA PA4).  The Plus ROM repeats only through the 256KB
  ROM socket span: its startup code senses the SCSI hardware by
  checking that the ROM does *not* mirror at 0x440000.
* VIA 6522: timers, the bit-banged RTC/PRAM protocol, screen/sound
  page and overlay bits, mouse button, 60.15Hz VBL.
* Z8530 SCC, with the mouse quadrature interrupts on the DCD pins.
* IWM floppy controller with one internal Sony GCR drive, backed by a
  raw disk image (``-drive if=floppy``): 400K single-sided (409600
  bytes) on the 128K/512K, 800K double-sided (819200 bytes) on the
  Plus.  The full boot-time drive probe is modelled: eject strobe
  timing, drive presence sense, and (on the 400K drive) the PWM
  spindle speed control that the ROM calibrates against the tach.
* ``macplus`` only: NCR5380 SCSI at 0x580000 with the polled "DACK"
  data pages at +0x200 (the Plus has no DRQ-gated handshake aperture;
  its driver paces on the 5380's DRQ status bit).  Attach disks as
  SCSI devices, e.g. ``-device scsi-hd``.
* 512x342 1-bit video scanned directly out of main RAM.

Firmware
--------

``mac128k`` and ``mac512k`` need the 64KB Macintosh 128K Rev A ROM
(checksum dword ``28BA61CE``), passed with ``-bios``.  The image used
during development came from archive.org item
``Macintosh-128K-ROM-Image``, md5 ``1d7f52d2d490524954f6afce083d9593``.

``macplus`` needs the 128KB Macintosh Plus v3 "Loud Harmonicas" ROM
(checksum dword ``4D1F8172``), md5
``8a41e0754ffd1bb00d8183875c55164c`` (archive.org item ``macroms``).

Booting
-------

Floppy boot (mac128k/mac512k)::

   qemu-system-m68k -M mac128k -bios Mac128K.ROM \
       -drive if=floppy,file=system11.img,format=raw \
       -icount shift=7

SCSI boot (macplus)::

   qemu-system-m68k -M macplus -bios macplus_v3.rom \
       -drive file=system753.img,format=raw,if=none,id=hd0 \
       -device scsi-hd,drive=hd0,scsi-id=0 \
       -icount shift=7

``-icount shift=7`` is required on all three machines: the ROMs
calibrate the floppy spindle speed (400K drive), time SCSI and drive
handshakes, and pace the mouse handlers with CPU-timed delay loops,
so the CPU must run at a realistic 68000 pace relative to the virtual
clock.  Without it the 128K/512K ROM's speed calibration divides by
zero and sad-macs (error 0F0004), and the Plus ROM's timed loops
misbehave in subtler ways.

Suitable boot floppies for the 128K/512K are raw 400K MFS system
disks, e.g. System 1.1 / Finder 1.1g (archive.org item
``system-1.1_apr1984``) or System 2.0 / Finder 4.1 (item
``apple-mac-os-system-2.0-finder-4.1-macintosh-system-disk-apr-1985-3.5-400k``).
The Plus boots System 7.5.3 from a SCSI disk image (4MB RAM
recommended); System 1.x floppies are too old for the Plus ROM.

What works
----------

On ``mac128k``/``mac512k``, System 1.1 boots through the happy Mac
and "Welcome to Macintosh" to the Finder desktop, with a fully
working mouse: clicking, menu tracking and File > Open all behave.
System 2.0 boots to its MiniFinder and on into Finder 4.1.  Without a
disk the ROM shows the blinking insert-disk icon.

On ``macplus``, the ROM scans the SCSI bus (the drive-probe eject
dance on the 800K drive included), loads the disk's Apple driver
partition and boots System 7.5.3 through "Welcome to Macintosh" to
the Finder desktop with a working mouse.

Known limitations
-----------------

* The floppy is read-only: the disk always reports write-protected
  and there is no GCR write/decode path, so the Finder cannot save to
  floppy (fine for locked-floppy boots; the Plus boots from SCSI,
  which is fully read/write).
* Ejecting a floppy from the Finder re-inserts it about a second
  later when the image stays attached (a stand-in for the user
  pushing the disk back in).
* The keyboard is not modelled; the ROM's keyboard query times out
  harmlessly, like a real Mac with the keyboard unplugged.
* Sound is not modelled (the sound-page PWM bytes only feed the 400K
  floppy speed control).
