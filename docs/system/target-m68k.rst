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
Commodore Amiga family.  The only board implemented so far is the
Amiga 3000 (machine ``a3000``).

The emulation is at an early stage.  The following hardware is
modelled:

-  68030 CPU.

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

-  The Paula serial port, connected to the first QEMU serial device
   (``-serial``).

-  The A3000's Ramsey memory controller and Fat Gary bus glue
   (identification and control registers only).

-  Empty Zorro II/III expansion space, reading as open bus so the
   expansion library sees "no board present".

Notably missing: the display (Denise), blitter, copper, audio and
floppy DMA, keyboard and mouse, the WD33C93A SCSI controller behind
the SuperDMAC, the battery-backed clock, and Zorro expansion boards.

Booting
~~~~~~~

A Kickstart ROM image must be provided with ``-bios``::

   qemu-system-m68k -M a3000 -bios kick.rom -serial stdio

The Amiga 3000 machine expects a plain 512KB ROM image such as
Kickstart 3.1 (release 40.068 for the A3000).  "SuperKickstart"
setups, where a bonus ROM loads the real Kickstart from disk, are not
supported.

With the display not yet modelled there is no video output; Kickstart
3.1 boots to a running exec with the boot process stalled on the
missing SCSI controller.

Adding other Amiga models
~~~~~~~~~~~~~~~~~~~~~~~~~

The Amiga boards share the abstract ``amiga-common`` machine class
(``hw/m68k/amiga.c``), which instantiates the hardware every classic
Amiga has: chip RAM, the Kickstart ROM and its overlay, the CIA pair
and the custom chip register block.  A board variant subclasses it,
fills in the class parameters (ROM base and size, chip RAM size, CIA
clock, Agnus ID, open-bus extent) and creates its board-specific
devices in the ``board_init`` hook; see ``hw/m68k/a3000.c`` for an
example.
