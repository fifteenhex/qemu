.. SPDX-License-Identifier: GPL-2.0-or-later

PWM
===

The PWM controller at ``0x1f003400`` (``sstar,infinity-pwm`` in the
vendor device tree, one channel). On the Miyoo Mini that channel drives
the LCD backlight: MainUI's Settings *Brightness* slider (levels 0 to
10) programs the duty cycle here, as ``level * 1500`` out of a fixed
period of ``15000``. The kernel exposes it as
``/sys/class/pwm/pwmchip0/pwm0`` (``period`` 800, ``duty_cycle`` =
level times 10).

Channel-0 registers, 16-bit on the usual 4-byte RIU stride:

.. list-table::
   :header-rows: 1

   * - Offset
     - Register
   * - ``0x08`` / ``0x0c``
     - duty count, low / high half
   * - ``0x10`` / ``0x14``
     - period count, low / high half
   * - ``0x18``
     - source-clock divider
   * - ``0x1c``
     - run/enable

Software programs these and reads them back (the pwm driver's
``get_state``), so the block is modelled as plain readback storage.
Nothing consumes the duty yet - the emulated panel is drawn at full
brightness regardless - so this captures the register state without
dimming the display.
