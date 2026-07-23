Sun-3/60 (``sun3-60``)
======================

The ``sun3-60`` machine models the Sun-3/60 "Ferrari": a 68020
workstation built around the discrete Sun-3 MMU.  The 3/60 boot PROM
runs its full power-up self-test and drops to the interactive ``>``
monitor prompt on ttya, and the NetBSD/sun3 1.5.2 RAMDISK kernel
boots to userland.

Emulated hardware
-----------------

* 68020 CPU (``-m`` up to 24MB, default 16MB).
* The Sun-3 MMU: 8 contexts, per-context segment map (2048 segments
  of 128KB) selecting one of 256 PMEGs of 16 8KB page table entries,
  with valid/write/system protection, the four PTE type spaces
  (on-board memory, on-board I/O, VME16, VME32) and hardware
  accessed/modified bits.  The MMU control space (IDPROM, page and
  segment maps, context register, system enable register, bus error
  and diagnostic registers) is reached through ``MOVES`` with
  SFC/DFC=3, using the CPU's board function-code hooks.
* The boot state: while the system enable register's NOTBOOT bit is
  clear, every supervisor program fetch is redirected to the boot
  EPROM regardless of address (this also supplies the reset vectors).
* Bus timeouts: accesses that decode to nothing latch the TIMEOUT bit
  in the bus error register and raise a 68020 bus error, which is how
  the PROM sizes memory and probes for missing devices.
* Two Z8530 zs SCCs: ttya/ttyb (``-serial``) and the
  keyboard/mouse ports (not yet populated with a keyboard).
* Intersil ICM7170 RTC, the 100Hz system clock on interrupt
  level 5/7.
* The interrupt register with its soft interrupt and clock gates.
* Memory error (parity) registers, with enough forced-parity
  behaviour to satisfy the PROM's level-7 NMI self-test.
* 2KB configuration EEPROM (initialised to select the ttya console).
* On-board video RAM (the monitor runs its early stack there); the
  bwtwo framebuffer itself is not rendered yet.

Not yet modelled: LANCE Ethernet, the "si" NCR5380 SCSI with its
DMA engine, DVMA, the bwtwo display and the Sun keyboard/mouse.  The
PROM's auto-boot probe of the missing LANCE bus-errors exactly as on
hardware and falls back to the monitor prompt.

Firmware
--------

``-bios`` (default name ``3.60_v3.0.1_rom``) takes the 64KB Sun-3/60
boot PROM v3.0.1 (27C512 image, md5
``28597f8a7f59b44395a398e152203505``, from the ``sunshack_bootroms``
collection on archive.org).

Running
-------

.. code-block:: shell

   qemu-system-m68k -M sun3-60 \
     -bios 3.60_v3.0.1_rom \
     -display none -serial mon:stdio

After the self-test (the full memory test takes a while) the PROM
prints its banner and the ``>`` monitor prompt on ttya.

Booting NetBSD
--------------

There is no boot device yet, so the pragmatic path loads the
NetBSD/sun3 1.5.2 RAMDISK kernel (an a.out image linked at
0xE004000, i.e. physical 0x4000) into RAM through the gdbstub once
the monitor is up, and starts it with the monitor's ``g`` command:

1. Strip the 32-byte a.out header from ``netbsd.RAMDISK`` (text +
   data, 0x11b980 bytes).
2. Run with ``-gdb tcp:...``, wait for the ``>`` prompt.
3. ``gdb-multiarch``: ``target remote ...``, ``restore netbsd.flat
   binary 0x4000``, ``detach``.
4. Type ``g 4000`` at the monitor.

The kernel is entered exactly as the PROM's own boot path would
enter it (low virtual addresses identity-mapped, the PROM vector at
0x0FEF0000 live), boots to userland and prints the RAMDISK welcome
banner on ttya.  ``sun3-tools/boot-netbsd-ramdisk.py`` automates the
sequence.  A residual guest-visible issue is still open: once per
shell cycle an ``open(2)`` fails with ``EFAULT`` on the namei
pathname buffer, so the installer shell currently loops (see
SUN3-NOTES.md).
