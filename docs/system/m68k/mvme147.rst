Motorola MVME147 (``mvme147``)
==============================

The ``mvme147`` machine models the Motorola MVME147, a VMEbus 68030
single-board computer.

Emulated hardware
-----------------

* 68030 CPU with DRAM at 0 (``-m``, default 16MB).
* Two 2MB ROM banks at 0xff800000 and 0xffa00000; ``-bios`` loads
  bank 1.
* The PCC (Peripheral Channel Controller) ASIC: timers with
  programmable interrupt levels/vectors, interrupt routing to the
  CPU, and the DMA engine that services the SCSI chip
  (``hw/misc/mvme147_pcc.c``).
* VMEchip bus interface registers (``hw/misc/mvme147_vmechip.c``);
  the VMEbus itself is not populated.
* Two Z8530 ESCCs providing serial ports 1-4 (``-serial`` /
  ``-serial mon:stdio`` for the console on port 1).
* Am7990 LANCE Ethernet with the board's byte-swapped DMA path.
* WD33C93 SCSI controller (``-drive if=scsi`` /
  ``-device scsi-hd``).
* Two M48T02 battery-backed NVRAM/RTC devices at 0xfffe0000 and
  0xfffe0800.  Both can persist across runs:
  ``-drive if=mtd,format=raw,file=bbram.img`` for the first (147Bug's
  configuration), ``-drive if=mtd,index=1,...`` for the second, which
  the u-boot port uses for its environment.

Firmware
--------

The 147Bug 2.5 debugger/monitor: a single 256KB image combining the
board's EPROM pair, passed with ``-bios``.  The image used during
development has md5 ``152267fc670e8513ffc1a63da388f532`` (its
archive provenance was not recorded; MVME147 EPROM dumps circulate on
bitsavers and the usual retro archives).

Note that the machine's reset handler currently jumps to the 147Bug
2.5 entry point directly rather than reading the reset vector, so
other firmware revisions may need adjustment.

Running
-------

::

   qemu-system-m68k -M mvme147 -bios 147bug2.5-combined.bin \
       -display none -serial mon:stdio

What works
----------

147Bug boots to its ``147-Bug>`` monitor prompt on serial port 1.
Its self-test suite passes the hard parts: the MPU
exception-processing confidence test (68030 bus-error frames, trace
modes, format errors), the FPC test against the 68882 model (packed
decimal, fsave/frestore frame state, exception traps) and the MMU
test.  The IOP disk commands drive the WD33C93 through complete SCSI
transactions via the PCC DMA engine, and with persistent NVRAM images
the configured board boots without warnings.

Known limitations
-----------------

* The reset PC is hard-wired for the 147Bug 2.5 image (see above).
* The VMEchip is register-level only; no VMEbus devices or
  inter-board traffic.
* No OS boot has been verified on this machine yet, and the model
  retains some bring-up rough edges (e.g. the PCC's registered reset
  handler is never invoked).
