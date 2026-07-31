.. _M68K-System-emulator:

m68k System emulator
--------------------

Use the executable ``qemu-system-m68k`` to emulate Motorola 68000
family machines, from ColdFire microcontroller boards to classic
68k desktops, consoles and handhelds.

Board-specific documentation
============================

..
   This table of contents should be kept sorted alphabetically
   by the title text of each file, which isn't the same ordering
   as an alphabetical sort by filename.

.. toctree::
   :maxdepth: 1

   m68k/mac128k
   m68k/maciisi
   m68k/amiga
   m68k/e17
   m68k/mc68ez328
   m68k/mvme147
   m68k/palm
   m68k/megadrive

.. _ColdFire-System-emulator:

ColdFire System emulator
========================

The emulator is able to boot a uClinux kernel on the ColdFire
machines.

The M5208EVB emulation includes the following devices:

-  MCF5208 ColdFire V2 Microprocessor (ISA A+ with EMAC).

-  Three Two on-chip UARTs.

-  Fast Ethernet Controller (FEC)

The AN5206 emulation includes the following devices:

-  MCF5206 ColdFire V2 Microprocessor.

-  Two on-chip UARTs.
