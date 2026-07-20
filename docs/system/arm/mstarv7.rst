.. SPDX-License-Identifier: GPL-2.0-or-later

MStar/SigmaStar ARMv7 SoCs (``miyoomini``)
==========================================

The MStar (now SigmaStar) ARMv7 SoCs are a range of Cortex-A7 based
SoCs with a shared lineage. The range is split into families that
share a die or are close derivatives of each other; the families in
turn share many hardware blocks with the rest of the range.

The QEMU models mirror that structure as a QOM class hierarchy:

* machine (``miyoomini``) -- the board or device the SoC is mounted in
* SoC (``ssd202d``) -- a concrete part number
* SoC family (``infinity2m``) -- parts sharing a die
* base (``mstarv7``) -- what the whole range has in common

Emulated devices and known limitations
--------------------------------------

* Two Cortex-A7 cores. The secondary is modelled as powered off and
  comes on when the kernel posts its entry address to the smpctrl
  mailbox (on real hardware it parks in the mask ROM instead).
* 128 MiB of in-package DDR3, mirrored through the MIU window so
  the IPL's wrap-around size probe works.
* 64 KiB of IMI SRAM.
* The PM UART, on the first serial port. No interrupt yet.
* The three timers, the DID boot-media strap (SPI NOR), the BDMA
  engine, the FSP flash sequencer, the SPI NOR XIP window (filled
  from the ``-drive if=mtd`` image), an MIU DDR controller stub and
  the l3bridge barrier.
* The two HWI2C masters, the SAR ADC (its channels the Miyoo Mini's
  keypad) and, on the Miyoo Mini, the board's ALPU-FA authentication
  chip (see :doc:`mstarv7/alpu`).
* The clkgen clock block (see :doc:`mstarv7/clocks`) and the chiptop
  pinctrl / strap block (see :doc:`mstarv7/pinctrl`), both readback
  register banks, the chiptop also returning the package bond strap.
* The PWM controller (see :doc:`mstarv7/pwm`), one channel of which is
  the Miyoo Mini's LCD backlight.
* The watchdog timer (see :doc:`mstarv7/watchdog`), left disabled by
  the vendor firmware.
* The FCIE SD/MMC host controller (see :doc:`mstarv7/fcie`), with a
  standard SD bus; attach a card with ``-drive if=sd``.
* The MIPI DSI output path (see :doc:`mstarv7/dsi`): the DSI
  controller, the D-PHY and, on the board, the panel it drives.
* The display controller (see :doc:`mstarv7/display`): the GOP
  graphics plane and MOP video plane, composited and scanned out to a
  QEMU console, and the display-top and GOP/fbdev vsync interrupts.
* The GE 2D graphics engine (see :doc:`mstarv7/ge`), which MainUI
  composites its UI through: bitblts with format conversion and
  180-degree rotation execute; blending, ROP and stretch do not.
* The BACH audio controller (see :doc:`mstarv7/bach`): the playback
  DMA sub-channel plays through QEMU's audio backend, with the
  period/underrun interrupt handshake the vendor ALSA driver needs.
* The boot ROM, loaded from ``ssd202d_bootrom.bin`` (a ``-bios``
  image overrides it). With no flash image it prints
  ``Check IPL Header failed! [HALT]`` and halts, matching the real
  ROM with nothing to boot.
* The GIC-400 and the Cortex-A7 generic timers, so the kernel gets
  timer interrupts, and the two mst-intc instances that funnel the
  peripheral interrupts onto GIC SPIs (the PM UART's line is routed
  through them).

With the Miyoo Mini firmware image attached the whole vendor boot
chain runs: mask ROM, IPL (DRAM sizing and memory BIST pass),
IPL_CUST (checksum passes, finds the MXP partition table), then
u-boot 2015.01 decompresses, detects the flash and reads its
environment. Known gaps at this point:

* Flash JEDEC ID reads zero (no SPI flash model behind the FSP), so
  u-boot complains and falls back to a default 16 MiB flash type.
* Flash writes/erases are not modelled: the erase path and the BDMA
  "write to flash" port (``0xb``) are still unimplemented.
* With the SAR ADC reading idle, u-boot takes the normal boot path:
  it JPEG-decodes the boot logo, shows it on the display (see below)
  and sends the panel its DCS init sequence over the modelled DSI
  link, then reads the kernel from flash, decompresses it and jumps
  to it.

The vendor 4.9 kernel then boots to userspace: it brings CPU1 online
through the smpctrl mailbox, mounts the squashfs root filesystem,
loads the Sigmastar vendor modules, and its init scripts run through
to launching the MainUI application.

The kernel's own log (dmesg) does not reach the serial port: the
vendor kernel's 8250 driver probes the PM UART, reports its type as
"unknown" and never attaches its printk console to it. Userspace,
however, opens the tty normally, so all of init's output and an
interactive shell **do** appear on the first serial port. To see the
kernel log, dump DRAM from the monitor (``pmemsave 0x20000000
0x8000000 mem.bin``) and read the printk ring buffer out of it. Why
the vendor driver rejects the UART has not been run down.

The display renders end to end: the vendor u-boot decodes its JPEG
boot logo into the MOP video plane (visible with a ``screendump``
during the u-boot window), the boot chime plays through the BACH
audio path, and MainUI comes up on the GOP plane - the full menu
(Game / RetroArch / App / Setting, with its header and footer) is on
the console about 90 seconds after power-on, with no extra options or
environment needed.

The menu can be driven from the keyboard: the Miyoo Mini's buttons are
wired to the SoC GPIO pads (see :doc:`mstarv7/gpio`), each mapped to a
host key.

.. list-table::
   :header-rows: 1

   * - Button
     - Key
   * - D-pad
     - arrow keys
   * - A / B / X / Y
     - ``A`` / ``B`` / ``X`` / ``Y``
   * - Start / Select
     - ``Enter`` / ``Right Shift``
   * - Menu
     - ``Esc``
   * - L1 / R1
     - ``Q`` / ``O``
   * - L2 / R2
     - ``W`` / ``P``

Remaining gaps on the way to a usable system: the power key and the
USB-plugged / charge state (the firmware's power-management daemon
watches these), and the SPI flash write/erase path.

Booting
-------

With no kernel argument the machine boots into the ROM, and with a
flash image attached the ROM loads the IPL out of it and runs it:

.. code-block:: bash

  $ qemu-system-arm -M miyoomini -drive if=mtd,format=raw,file=nor.bin \
      -nographic

A kernel can also be loaded directly into DRAM with the usual options:

.. code-block:: bash

  $ qemu-system-arm -M miyoomini -kernel zImage -dtb board.dtb -nographic

Hardware documentation
----------------------

These SoCs have no public documentation, so a goal of this emulation
is to double as documentation of the hardware: the memory map and the
registers of each block, at bit level, are documented alongside the
device models and every documented fact carries a tag saying where it
came from. The conventions and the tags are described at the top of
the memory map page. A block's register page is added or extended in
the same commit as the device model change it describes.

.. toctree::
   :maxdepth: 1

   mstarv7/memory-map
   mstarv7/bootrom
   mstarv7/timer
   mstarv7/isp
   mstarv7/bdma
   mstarv7/sar
   mstarv7/alpu
   mstarv7/dsi
   mstarv7/display
   mstarv7/ge
   mstarv7/bach
   mstarv7/gpio
   mstarv7/clocks
   mstarv7/pinctrl
   mstarv7/pwm
   mstarv7/watchdog
   mstarv7/fcie
