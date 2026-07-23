.. _ColdFire-System-emulator:

ColdFire System emulator
------------------------

Use the executable ``qemu-system-m68k`` to simulate a ColdFire machine.
The emulator is able to boot a uClinux kernel.

The M5208EVB emulation includes the following devices:

-  MCF5208 ColdFire V2 Microprocessor (ISA A+ with EMAC).

-  Three Two on-chip UARTs.

-  Fast Ethernet Controller (FEC)

The AN5206 emulation includes the following devices:

-  MCF5206 ColdFire V2 Microprocessor.

-  Two on-chip UARTs.

.. _Amiga-System-emulator:

Amiga System emulator
---------------------

Use the executable ``qemu-system-m68k`` to emulate machines from the
Commodore Amiga family.  Two boards are implemented: the Amiga 3000
(machine ``a3000``) and the Amiga 4000 (machine ``a4000``).

The emulation is at an early stage.  The following hardware is
modelled:

-  68030 CPU (68040 on the A4000).

-  2MB of chip RAM, and up to 16MB of motherboard fast RAM below
   0x08000000 (set with ``-m``, default 8MB).

-  Kickstart ROM at 0xf80000, including the reset-time overlay at
   address 0 controlled by CIA-A PA0.

-  Both MOS 8520 CIAs: I/O ports, interval timers, TOD counters with
   alarms, interrupts.

-  The Paula interrupt controller, with INTENA/INTREQ funnelled onto
   the 68k interrupt lines.

-  The Agnus beam counters (VPOSR/VHPOSR) with PAL timing, and the
   vertical blank interrupt.

-  The blitter, including line mode and area fill.  Blits complete
   instantly.

-  A line-granular copper: the list is executed once per displayed
   frame (at render time) with every WAIT considered satisfied, but
   display register writes are replayed by the renderer at the line of
   the preceding WAIT, so per-line palettes and mid-frame screen
   splits render.  Effects keyed to the horizontal beam position do
   not.

-  A bitplane display: lores and hires (mixable within a frame),
   extra-half-brite and hold-and-modify, rendered to a QEMU console.
   On the AGA machine it extends to eight bitplanes, the 256-entry
   24-bit palette (BPLCON3 bank and LOCT), HAM8 and the FMODE 32/64-bit
   fetch widths, so AGA software displays in 256 colours.  DMA sprites
   are drawn in front of the playfield (mouse pointers work); attached
   sprites, the AGA sprite widths and dual playfield are not.

-  Paula disk DMA and two floppy drives, DF0 and DF1, backed by plain
   880KB ADF images given with ``-drive if=floppy`` (once for each
   drive).  Reads encode the track into standard AmigaDOS MFM on the
   fly (both trackdisk.device and custom trackloaders work), writes
   decode it back into the image.  External drive detection through
   the drive ID shifter works, so DF1 is bootable too.  Disks can be
   swapped at runtime (``blockdev-change-medium``); the drive latches
   /CHNG so trackdisk.device notices.

-  A mouse in gameport 0: counters in JOY0DAT, left button on CIA-A
   PA6, right button on the POTGOR Y line.

-  A keyboard on CIA-A's serial port, with the acknowledge handshake;
   QEMU key events are mapped to Amiga raw keycodes.

-  Paula audio: the four DMA sample channels with per-channel period
   and volume, mixed to a stereo backend (0 and 3 left, 1 and 2
   right), with the AUDx interrupts on buffer loop.  Select a backend
   with ``-audiodev`` and ``-M a3000,audiodev=<id>``.  Manual (CPU
   driven) sample output and the AM/FM modulation modes are not
   implemented.

-  The Paula serial port, connected to the first QEMU serial device
   (``-serial``).

-  The WD33C93A SCSI controller behind the SuperDMAC, interrupting on
   INT2, with the SDMAC DMA engine.  ``-drive if=scsi`` disks appear
   on its bus.

-  The Ramsey memory controller and Fat Gary bus glue shared by the
   A3000 and A4000 (identification and control registers only).

-  Zorro II autoconfig, with the Commodore A2065 Ethernet card (an
   Am7990 LANCE) as an autoconfigured board.  Zorro III config space
   still reads open bus.

Notably missing: the battery-backed clock and Zorro expansion
boards.  Kickstart finds attached SCSI
disks but does not yet boot from them: the first TEST UNIT READY
fails with a power-on unit attention that scsi.device treats as "no
disk".

Booting
~~~~~~~

A Kickstart ROM image must be provided with ``-bios``::

   qemu-system-m68k -M a3000 -bios kick.rom -serial stdio

The Amiga 3000 machine expects a plain 512KB ROM image such as
Kickstart 3.1 (release 40.068 for the A3000).  "SuperKickstart"
setups, where a bonus ROM loads the real Kickstart from disk, are not
supported.

Kickstart 3.1 boots to the insert-floppy screen, with the animation
running on the QEMU console, and boots bootable ADF images from the
floppy::

   qemu-system-m68k -M a3000 -bios kick.rom \
       -drive if=floppy,file=game.adf,format=raw

The Amiga 4000
~~~~~~~~~~~~~~

The ``a4000`` machine is a 68040 with the AGA chipset IDs and 2MB of
chip RAM.  It needs the A4000 Kickstart (release 40.068 for the A4000,
also 512KB) and boots to the insert-floppy screen and Workbench in the
same way::

   qemu-system-m68k -M a4000 -bios kick_a4000.rom \
       -drive if=floppy,file=disk.adf,format=raw

The AGA display is modelled (eight bitplanes, the 256-entry 24-bit
palette, HAM8, FMODE fetch widths), so AGA software runs in 256
colours.  The onboard IDE interface is not modelled yet, so there is no
hard disk, and the AGA sprite fetch widths are not implemented.

Adding other Amiga models
~~~~~~~~~~~~~~~~~~~~~~~~~

The Amiga boards share the abstract ``amiga-common`` machine class
(``hw/m68k/amiga.c``), which instantiates the hardware every classic
Amiga has: chip RAM, the Kickstart ROM and its overlay, the CIA pair
and the custom chip register block.  A board variant subclasses it,
fills in the class parameters (ROM base and size, chip RAM size, CIA
clock, Agnus and Denise IDs, open-bus extent) and creates its
board-specific devices in the ``board_init`` hook.  See
``hw/m68k/a3000.c`` and ``hw/m68k/a4000.c`` for examples; the
Ramsey/Fat Gary glue they share lives in ``hw/m68k/amiga_mobo.c``.
