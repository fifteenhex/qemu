Commodore Amiga (``a500``, ``a500plus``, ``a600``, ``a1000``, ``a1200``, ``a2000``, ``a3000``, ``a4000``, ``a4000t``, ``cd32``)
===============================================================================================================================

QEMU emulates eight classic Commodore Amiga models and the CD32
console.  They share a
common core (``hw/m68k/amiga.c``) — chip RAM, the Kickstart ROM with
its reset-time overlay at address 0, the two MOS 8520 CIAs and the
custom chip register block — and each board adds its own CPU, memory
layout and I/O:

.. list-table::
   :header-rows: 1

   * - Machine
     - CPU
     - Chipset
     - Chip RAM
     - ``-m`` expansion (default)
     - Kickstart
   * - ``a1000``
     - 68000
     - OCS
     - 256KB
     - front-panel chip RAM, max 256KB (fitted)
     - 1.3 (256KB image)
   * - ``a500``
     - 68000
     - ECS [1]_
     - 512KB
     - A501 trapdoor slow RAM, max 1.5MB (512KB)
     - 3.1 (A500/A600/A2000)
   * - ``a2000``
     - 68000
     - ECS Agnus 8372A, OCS Denise
     - 1MB
     - ranger slow RAM, max 1.5MB (none)
     - 1.3 or 3.1 (A500/A600/A2000)
   * - ``a500plus``
     - 68000
     - ECS (8375/8373)
     - 1MB
     - trapdoor chip RAM, max 1MB (fitted)
     - 2.04
   * - ``a600``
     - 68000
     - ECS (8375/8373)
     - 1MB
     - trapdoor chip RAM, max 1MB (fitted)
     - 2.05 or 3.1 (A500/A600/A2000)
   * - ``a1200``
     - 68020 [2]_
     - AGA
     - 2MB
     - none
     - 3.1 (A1200)
   * - ``a3000``
     - 68030
     - ECS
     - 2MB
     - motherboard fast RAM, max 16MB (8MB)
     - 3.1 (A3000)
   * - ``a4000``
     - 68040
     - AGA
     - 2MB
     - motherboard fast RAM, max 16MB (8MB)
     - 3.1 (A4000)
   * - ``a4000t``
     - 68040
     - AGA
     - 2MB
     - motherboard fast RAM, max 16MB (8MB)
     - 3.1 (A4000T)
   * - ``cd32``
     - 68020 [2]_
     - AGA
     - 2MB
     - none
     - 3.1 (CD32) + extended ROM

.. [1] The plain A500 machine inherits the shared base's ECS chipset
   IDs, modelling an A500 fitted with the ECS chip upgrade.
.. [2] The real A1200 has a 68EC020; QEMU has no EC020 variant, so
   the full 68020 core stands in and the whole 32-bit address space
   terminates in open bus (matching the EC020's must-not-bus-error
   behaviour as far as Kickstart's probes care).

Emulated hardware
-----------------

* The Kickstart ROM with the reset-time overlay at address 0
  (CIA-A PA0).  The A1000 has no ROM socket on real hardware; its
  machine maps the 256KB Kickstart image write-protected at the
  writable control store address, 0xfc0000.
* Both MOS 8520 CIAs: I/O ports, interval timers, TOD counters with
  alarms, interrupts.
* Paula interrupts (INTENA/INTREQ onto the 68k interrupt lines), the
  Agnus beam counters with PAL timing, and the vertical blank
  interrupt.
* The blitter, including line mode and area fill; blits complete
  instantly.
* A line-granular copper: display register writes are journalled by
  WAIT line and replayed by the renderer, so per-line palettes and
  mid-frame screen splits render.  Effects keyed to the horizontal
  beam position do not.
* Bitplane display: lores and hires, extra-half-brite, HAM6.  On the
  AGA machines it extends to eight bitplanes, the 256-entry 24-bit
  palette, HAM8 and the FMODE fetch widths, so AGA software displays
  in 256 colours.  DMA sprites are drawn in front of the playfield
  (mouse pointers and sprite-based games work).
* Paula disk DMA with two floppy drives (DF0/DF1) backed by plain
  880KB ADF images: tracks are encoded to AmigaDOS MFM on the fly and
  writes are decoded back, so both trackdisk.device and custom
  trackloaders work.  Disks can be swapped at runtime with
  ``blockdev-change-medium``.
* Mouse in gameport 0; the port 1 fire button is on the host middle
  mouse button.  Keyboard on CIA-A's serial port with the acknowledge
  handshake.
* Paula audio (four DMA channels, stereo; enable with
  ``-audiodev ...,id=snd0 -M <machine>,audiodev=snd0``) and the Paula
  serial port (``-serial``).
* On the A3000: the WD33C93A SCSI controller behind the SuperDMAC
  (``-drive if=scsi``), and the Ramsey/Fat Gary glue shared with the
  A4000.
* On the A600 and A1200: Gayle's IDE interface (``-drive if=ide``,
  master and slave), with the interrupt latching and the bit-serial
  Gayle ID register Kickstart's scsi.device probes for.
* On the CD32: the Akiko chip - identification, the chunky-to-planar
  conversion port and the battery-backed I2C NVRAM.  The CD drive
  itself is not modelled yet, so the console behaves as if no disc is
  inserted.
* Zorro II autoconfig with the Commodore A2065 Ethernet card (Am7990
  LANCE) as an autoconfigured board.

Firmware
--------

A Kickstart ROM image must be provided with ``-bios``.  Kickstart is
still under copyright; the images below (archive.org item
``commodore-amiga-firmware``, one zip per model) are the versions the
machines were developed and tested against:

.. list-table::
   :header-rows: 1

   * - Version
     - For machines
     - md5
   * - 1.3 r34.005 (256KB, A1000/A2000/CDTV)
     - ``a1000``, ``a2000``
     - 82a21c1890cae844b3df741f2762d48d
   * - 2.04 r37.175 (A500+)
     - ``a500plus``
     - dc10d7bdd1b6f450773dfb558477c230
   * - 2.05 r37.299 (A600)
     - ``a600``
     - 72ffce8541f100885da4b68a3bcf10f7
   * - 3.1 r40.063 (A500/A600/A2000)
     - ``a500``, ``a600``, ``a2000``
     - e40a5dfb3d017ba8779faba30cbd1c8e
   * - 3.1 r40.068 (A1200)
     - ``a1200``
     - 646773759326fbac3b2311fd8c8793ee
   * - 3.1 r40.068 (A3000)
     - ``a3000``
     - 413590e50098a056cfec418d3df0212d
   * - 3.1 r40.068 (A4000)
     - ``a4000``
     - 9bdedde6a4f33555b4a270c8ca53297d
   * - 3.1 r40.070 (A4000T)
     - ``a4000t``
     - e873c43040b4d7a9c65f37cf2da2158f
   * - 3.1 r40.060 (CD32)
     - ``cd32``
     - 5f8924d013dd57a89cf349f4cdedc6b1
   * - CD32 extended ROM r40.60
     - ``cd32``
     - bb72565701b1b6faece07d68ea5da639

The machines expect a plain ROM image.  "SuperKickstart" setups,
where a bonus ROM loads the real Kickstart from disk, are not
supported.  The 256KB Kickstart 1.x images are mirrored across the
512KB ROM window on the machines whose real socket decode does that.

The CD32 needs both ROMs: the Kickstart via ``-bios`` and the
extended ROM (cd.device and the boot user interface), which is
loaded from the file named by the ``extrom`` machine property
(default ``cd32_ext.rom``, looked up like ``-bios`` is, so ``-L`` is
the easiest way to point at a directory holding both).

Booting
-------

Kickstart boots to the insert-floppy screen (animated), and boots
bootable ADF images from the floppy::

   qemu-system-m68k -M a500 -bios kick31_a500.rom \
       -drive if=floppy,file=game.adf,format=raw

A second ``-drive if=floppy`` attaches DF1.  On the A3000, SCSI disks
attach with ``-drive if=scsi``; note that AmigaOS's HDToolBox and the
factory setup default to SCSI unit 6, so
``-drive if=scsi,unit=6,file=hd.img,format=raw`` is the natural
place for a hard disk.  A bootable RDB partition boots without a
floppy present.

On the A600 and A1200, hard disks attach to Gayle's IDE port::

   qemu-system-m68k -M a1200 -bios kick31_a1200.rom \
       -drive if=ide,file=hd.img,format=raw

The first ``-drive if=ide`` is the master, a second the slave.
Kickstart 3.1 (and 2.05 r37.300+ on the A600) boot-scans the RDB and
boots a Workbench installation from the disk with no floppy present;
Kickstart 2.05 r37.299 and older predate IDE support and ignore the
drive.

The CD32 boots to its animated "insert disc" startup screen::

   qemu-system-m68k -M cd32 -L /path/to/roms -bios kick31_cd32.rom

What works
----------

* Workbench 3.1 boots to the desktop from floppy on ``a500``,
  ``a600``, ``a1200``, ``a2000``, ``a3000`` and ``a4000``, and from
  an IDE hard disk on ``a600`` and ``a1200``; Kickstart 1.3 on
  ``a1000``/``a2000`` and 2.04/2.05 on ``a500plus``/``a600`` reach
  their insert-disk screens.
* The CD32 runs its boot chime and startup animation from the
  extended ROM, rendering through Akiko's chunky-to-planar port.
* Games run from ADF: Lemmings and The Secret of Monkey Island are
  playable, and Deluxe Galaga (AGA) runs in 256 colours on the
  ``a4000``.
* On the A3000, the guest can partition, format and mount SCSI disks
  end to end, and Kickstart boot-scans the RDB and boots from a
  bootable FFS partition.
* The 68030 PMMU is implemented far enough that Amiga UNIX (AMIX)
  runs its kernel in virtual memory on the A3000, though it does not
  yet complete booting.

Known limitations
-----------------

* Display: attached sprites, sprite/playfield priority (BPLCON2),
  BPLCON1 fine scroll, dual playfield and the AGA sprite widths are
  not implemented; horizontal-beam-keyed copper effects do not
  render.
* The battery-backed clock is not modelled, and Zorro III cards are
  not supported (the config space reads open bus).
* PCMCIA on the ``a600``/``a1200`` and the A4000's onboard IDE are
  not modelled, and the CD32 has no CD drive yet (the console runs
  its "no disc" startup screen).
* The A4000T's onboard NCR 53C710 SCSI is not modelled; its vacant
  slot latches a Fat Gary bus timeout so Kickstart 3.1 r40.070
  detects the absence and boots on.  The machine reaches the
  insert-disk screen, but booting Workbench from floppy currently
  stalls at an empty Workbench screen (under investigation; the
  ``a4000`` boots it fine).
* Floppies are 880KB double-density ADFs only.
* Joystick directions in gameport 1 are not wired (only the fire
  button is).

Adding other Amiga models
-------------------------

The boards share the abstract ``amiga-common`` machine class
(``hw/m68k/amiga.c``).  A board variant subclasses it, fills in the
class parameters (ROM base and size, chip RAM size, CIA clock, Agnus
and Denise IDs, open-bus extent) and creates its board-specific
devices in the ``board_init`` hook; ``hw/m68k/a500.c`` is the
minimal example and ``hw/m68k/a3000.c`` the fullest.
