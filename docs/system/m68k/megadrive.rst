Sega Mega Drive (``megadrive``)
===============================

The ``megadrive`` machine models a Sega Mega Drive / Genesis console
far enough to play commercial cartridge games, and doubles as a
Linux-capable board via an Everdrive-style flash cartridge mapper.

Emulated hardware
-----------------

* 68000 main CPU with the console's 24-bit address bus behaviour
  (games keep tag bits in pointer high bytes).
* VDP (``hw/display/md_vdp.c``): planes A/B, window, sprites with
  priority, H32/H40 modes, scrolling, DMA (transfer/fill/copy), HV
  counter, scanline-timed vertical and horizontal interrupts.
* 64KB work RAM mirrored across 0xE00000-0xFFFFFF, and an open-bus
  background region (the real console has no bus-error generator).
* System control region (``hw/misc/md_sys.c``): Z80 sound RAM, the
  Z80 bus request/reset handshake and a YM2612 register stub — enough
  for games' sound drivers to load and run without hanging.
* Control pad (``hw/input/md_io.c``): 3-button pad on the keyboard
  (arrows = d-pad, ``A``/``S``/``D`` = A/B/C, Enter = Start).
* Everdrive mapper (``hw/misc/md_everdrive.c``): mailbox command FIFO
  bridged to a chardev and host directory, used by the Mega Drive
  Linux port.

The machine has a ``mapper`` option:

* ``-M megadrive`` (default, ``mapper=everdrive``): the ``-bios``
  image is loaded into 4MB of writable PSRAM at 0, modelling an
  Everdrive SSF2 cartridge; this is what the Linux port uses.
* ``-M megadrive,mapper=cart``: the image is mapped as read-only
  cartridge ROM at 0, as on a real cartridge.  Use this for games.

Firmware
--------

There is no console BIOS (no TMSS); ``-bios`` supplies the cartridge
image itself.  Any plain (non-interleaved) Mega Drive ROM dump works.
Development and playtesting used the No-Intro verified Sonic the
Hedgehog (USA, Europe) REV00 dump, 524288 bytes, md5
``1bc674be034e43c96b86487ac69d9293`` (archive.org item
``sega-genesis-romset-ultra-usa``).

Running
-------

::

   qemu-system-m68k -M megadrive,mapper=cart -bios sonic1.md

What works
----------

Sonic the Hedgehog 1 is fully playable: SEGA logo, title, attract
demo, controlled gameplay in Green Hill Zone, the level select cheat,
Labyrinth Zone with its per-scanline horizontal interrupt water
effects, Star Light Zone and the Special Stage all run with correct
graphics and sprite priorities.  The default Everdrive machine still
boots the Mega Drive Linux setup.

Known limitations
-----------------

* No audio: the YM2612 and PSG are register stubs (games run, but
  silently).
* The Z80 itself is not executed; only its RAM and the bus arbiter
  are modelled.
* Palette effects are applied per frame, not per scanline (e.g. the
  whole screen tints underwater in Labyrinth Zone instead of only
  below the waterline).
* Sprite masking (x=0) and shadow/highlight mode are not implemented.
* The Everdrive SSF2 bank registers are stored but do not remap
  anything (images larger than 4MB will not bank-switch).
