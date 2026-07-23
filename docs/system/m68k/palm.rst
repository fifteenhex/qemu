Palm PDAs (``palmv``, ``palmm500``)
===================================

Two Palm handhelds built on Motorola DragonBall system-on-chips:

* ``palmv`` — Palm V: MC68EZ328 "DragonBall EZ" at 16.58MHz, 2MB RAM,
  runs PalmOS 3.x.
* ``palmm500`` — Palm m500: MC68VZ328 "DragonBall VZ" at 33.16MHz,
  8MB RAM, an SD card slot, runs PalmOS 4.x.

Emulated hardware
-----------------

* The DragonBall on-chip peripherals: PLL, interrupt controller,
  GPIO ports with the port D keyboard interrupt logic, timers (the VZ
  adds the second timer PalmOS 4 ticks on), SPI master, UART(s), LCD
  controller (1/2/4bpp greyscale with the gray palette register), RTC
  with alarm and watchdog, and PWM tone generator.
* ADS7843-style touchscreen ADC on the SPI bus, with the pen
  interrupt; pen taps come from the QEMU pointer (absolute
  coordinates).
* The hard buttons on the keypad matrix: F1-F4 launch Date
  Book/Address/To Do/Memo, Up/Down are the rocker, F5 power, F6
  contrast.  The four silkscreen icons (Applications/Menu/Calc/Find)
  are pen hotspots on F7-F10.
* Speaker via the PWM unit: enable with
  ``-audiodev <driver>,id=snd0 -M palmv,audiodev=snd0`` (PalmOS only
  sounds alarms and explicit tones by default, not UI taps).
* Serial cradle on the UART (``-serial``); on the m500 the cradle is
  UART2 and UART1 is the IR port.
* m500 only: the SD slot on the VZ's SPI1 unit
  (``-drive if=sd,format=raw,file=sd.img``).

Firmware
--------

``-bios`` takes a PalmOS ROM image (the "big ROM" dumps that Palm
backup tools produce).  Development used the PalmDB ROM set mirrored
as archive.org item ``20250707_20250707_0134``:

.. list-table::
   :header-rows: 1

   * - File
     - Machine
     - md5
   * - Palm-V-3.1-en.rom
     - ``palmv``
     - c575ebb95f736e389d9c29ad919b4753
   * - Palm-V-3.3-en.rom
     - ``palmv``
     - 6b347dada1c8b6bbc7546cc0f7281990
   * - Palm-m500-4.1-en.rom
     - ``palmm500``
     - dc8f0f8a6ffed58764065a7abe468ce4

Running
-------

::

   qemu-system-m68k -M palmv -bios Palm-V-3.3-en.rom \
       -display gtk,zoom-to-fit=on

   qemu-system-m68k -M palmm500 -bios Palm-m500-4.1-en.rom \
       -drive if=sd,format=raw,file=sd.img

What works
----------

Both machines boot PalmOS to the launcher with a working pen: the
whole Setup wizard completes, including the digitizer calibration
screen, and the built-in applications run (Memo Pad shows its
welcome memos on the V, Note Pad its handwritten note on the m500).
Hard buttons, silkscreen taps, the real-time clock, alarms through
the PWM speaker and the RTC watchdog all work.  On the m500, PalmOS
4.1 detects and mounts a FAT16 SD card image.

``palm-tools/`` in the source tree has a QMP driver (``palmctl.py``)
for scripting taps and buttons, deterministic qtest device tests, and
an SD image builder.

Known limitations
-----------------

* HotSync over the serial cradle has not been brought up.
* Sound is the PWM tone generator only; the sample-FIFO PCM path is
  not modelled.
* The m500's digitizer calibration is sensitive to exact tap
  coordinates and is fiddly to drive from scripts (the V is
  forgiving).
