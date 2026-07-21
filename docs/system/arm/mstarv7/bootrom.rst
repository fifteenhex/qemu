.. SPDX-License-Identifier: GPL-2.0-or-later

Boot ROM
========

What is known about the mask ROM, from static analysis of the 16 KiB
SSD202D dump (``rom``) and from watching it run under the model with
``-d unimp`` (``model``). File offsets below are also offsets from
the ROM base.

Boot flow as observed under the model
-------------------------------------

In ARM state the ROM masks interrupts, enables the I-cache, splits the
cores off (below) and, on CPU0, sets up the watchdog and timer, then
``blx``\ es into a Thumb loader (file offset ``0x2dc``). The loader
brings up uart0 for its messages, sets pad-mux registers
(``0x1f2070c4``, ``0x1f203d4c``, ``0x1f001c24``), reads the boot-media
strap (``0x1f0071c0`` plus ``0x1f00700c`` / ``0x1f004014``), loads the
IPL from the strapped medium, verifies (and optionally authenticates)
it, and jumps to it at ``0xa0000000``. With the strap registers
reading zero in the model no medium matches, so the loader falls
through to ``Check IPL Header failed! [HALT]`` on uart0 - the correct
outcome for nothing-attached; on the real chip the strap will select
SPI NOR (``0x20``).

Progress code register
----------------------

The ROM reports its progress as 32-bit writes to ``0x1f200800``
(bank ``0x1004`` register ``0x00``). Codes observed in order, one run
(``model``):

``0xa01 0xa02 0xbabe 0xa03 0xa04 0xa06 0xb01 0xb04 0xb02 0xbf7``

The ``0xa..`` codes bracket early init, the ``0xb..`` codes the boot
media path, and ``0xbabe`` is written just before the timer setup.
It is not known whether the register is a plain scratch register or
something a debugger can watch (``rom``).

Exception handlers
------------------

The vector table sits at the start of the ROM. Each handler writes an
identifying code to ``0x1f200800`` and spins; the undefined
instruction vector branches straight to a spin without a code:

========  ====================  ==========================
Code      Exception             Source
========  ====================  ==========================
none      undefined instr       ``rom``
``0xe02``  supervisor call      ``rom``
``0xe03``  prefetch abort       ``rom``
``0xe04``  data abort           ``rom``, seen under model
``0xe05``  IRQ                  ``rom``
``0xe06``  FIQ                  ``rom``
========  ====================  ==========================

The ``0xe04`` handler is how the missing IMI SRAM was found: the ROM
keeps its stack in IMI (literals up to ``0xa000f8a5``) and data
aborts on the first pop if it is not there.

Timer setup
-----------

Write sequence observed at bank ``0x30`` (``0x1f006000``), matching
the timer node at ``0x6040`` in the mainline device trees (``dts``):

======================  ==========  ==================================
Address                 Written     Guess
======================  ==========  ==================================
``0x1f006040``          ``0x0``     control, disable
``0x1f006058``          ``0x0``     counter clear?
``0x1f006048``          ``0xffff``  max value low 16 bits
``0x1f00604c``          ``0xffff``  max value high 16 bits
``0x1f006040``          ``0x1``     control, enable
======================  ==========  ==================================

Earlier in init the ROM also writes zero to ``0x1f006010`` and
``0x1f006014``: those are ``WDT_MAX_PRD_L``/``_H`` of the watchdog at
``0x1f006000`` (``linux``, from the mainline ``msc313e_wdt`` driver),
so this is the ROM disabling the watchdog.

Secondary core mailbox (smpctrl)
--------------------------------

Around file offset ``0xd0`` the ROM has paths that write
``0xaaaa``/``0xbbbb`` to ``0x1f200804``, poll a register for the
magic ``0xbabe``, assemble a 32-bit entry address from two 16-bit
registers and ``bx`` to it (``rom``). This was first read as a debug
hand-off, but it is the secondary core's parking loop: the registers
are the "smpctrl" bank at ``0x1f204000``, and mainline Linux's
``mstar,smp`` enable-method releases CPU1 by writing its entry
address to ``+0x50`` (low half) and ``+0x4c`` (high half) and then
``0xbabe`` to ``+0x58`` (``linux``, ``prev``). So CPU1 is not held
in reset by hardware as first assumed: it runs the mask ROM too and
spins in this loop until the kernel posts an address. The model
keeps CPU1 powered off and instead powers it on at the posted
address when the unlock magic is written; that matches what software
observes, but not the mechanism - there is no parked core executing
ROM code in the model (``model``).

The split between the two cores is right at the top of the ROM
(``rom``): after the progress-code setup the reset path reads the core
number from ``MPIDR`` (``mrc p15, 0, r0, c0, c0, 5``; ``r0 & 3``) at
file offset ``0x64`` and branches on it. CPU0 (``r0 == 0``) continues
to the watchdog/timer setup and the boot-media path; every other core
falls through to its own parking code, which reports progress to a
**separate** register at ``0x1f200808`` (not CPU0's ``0x1f200800``)
and sleeps in ``wfi``/``wfe`` loops until it is released through the
smpctrl bank above. So ``0x1f200808`` is the secondary-core progress
register.

Early strap and config
----------------------

Before the core split, in the first dozen instructions after the
cache setup, the ROM does two things not tied to any block modelled
yet (``rom``):

* It writes ``1`` to ``0x16001000`` and ``0xf0`` to ``0x16001004`` -
  an address region outside the RIU (the RIU is ``0x1f000000``, the
  flash XIP window ``0x14000000``); ``0x16000000`` is otherwise unseen
  and its meaning is unknown.
* It reads ``0x1f00401c`` and tests bit 6 (``& 0x40``). With the bit
  set it goes on to write the ``0xaaaa`` smpctrl handshake; with it
  clear it takes a different path. So ``0x1f00401c`` bit 6 is a
  config/strap that gates the multi-core mailbox setup - it reads zero
  in the model, so that path is skipped.

ARM entry and the Thumb loader
------------------------------

The reset path up to here is 32-bit ARM code. At file offset ``0x290``,
after setting the stack to IMI (``sp = 0xa000d000``) and zeroing the
registers, CPU0 does ``blx 0x2dc`` - switching to **Thumb** for the
whole boot-media loader (``rom``). This is why an ARM-only disassembly
(and a literal-pool scan) sees none of the boot logic: the loader is
Thumb, and it builds its register bases and constants with
``movw``/``movt`` immediates rather than literal pools. If the loader
ever returns, the ARM caller writes progress ``0xaff`` and spins.

Console UART
------------

The loader's first act (its ``0x62c`` init, progress ``0xb01``) is to
bring up **uart0** at ``0x1f221000`` for its messages (``rom``): it
sets the UART pad-mux (writes ``9`` to ``0x1f2070c4`` and ``0x3210``
to ``0x1f203d4c``), pokes ``0x1f001c24`` and the uart0 config
registers ``0x1f221008``/``0x221038``/``0x221070``. The print routine
at ``0x748`` polls the TX-ready bit (bit 5 of ``0x1f221028``) and
writes each byte to the data register ``0x1f221000`` until the NUL.
So the "PM UART" guess was wrong - it is uart0, and there is no banner
simply because the routine is only ever called on a failure path. The
UART is **output only**; nothing reads it back.

Boot-media dispatch
-------------------

The loader reads ``DID_KEY`` (``0x1f0071c0``) and masks it with
``0x24`` (bits 2 and 5), progress ``0xb04``/``0xb02`` (``rom``):

.. list-table::
   :header-rows: 1

   * - ``DID_KEY & 0x24``
     - Medium
     - Path
   * - ``0x20``
     - SPI NOR
     - progress ``0xb07``; FSP reset then XIP load (below)
   * - ``0x04``
     - SPI NAND
     - progress ``0xb0e``; SPI-NAND init at ``0xd2c`` (``0x1f00080c``
       status, IMI ``0xa000c000`` buffer); on init failure prints
       ``SPINAND init failed! [HALT]``
   * - other
     - none
     - falls through to the common header check, which fails ->
       ``0xbf7`` ``Check IPL Header failed! [HALT]``

Only these two media exist in this ROM; the ``0x10``/``0x08`` variants
the previous branch listed are not decoded here. The ROM also reads
``0x1f00700c`` and bit 10 of ``0x1f004014`` at dispatch (they steer
minor sub-paths).

**SPI NOR** (``rom``): the IPL header magic ``"IPL_"``
(``0x5f4c5049``) is checked at XIP ``0x14000004``; the FSP NOR reset
(``66 99``) is fired (``0x1f200800`` scratch ``0x6000``/``0x6001``,
FSP registers ``0x1f002d84``-``0x1f002dd8``, timed against the timer
counter ``0x1f006050``/``0x006054`` - hence ``SPINOR reset
timeout!``); the image is copied through BDMA (``0x1f200404``-``424``)
into IMI at ``0xa0000000``. A bad magic gives ``Check Header
failed!``; a failed load ``Load IPL from SPINOR failed!``.

Common verify and authentication
--------------------------------

After the copy, all media converge (``0x424``) on the IPL header at
IMI ``0xa0000000`` (``rom``): the ``"IPL_"`` magic is re-checked
(fail -> ``0xbf7`` ``Check IPL Header failed!``), then a signed-image
flag byte is tested (``== 0xfa``). If the image is marked signed, the
ROM authenticates it **in hardware** using the crypto engine at
``0x1f224400`` (see :doc:`vendor-modules`): SHA over the image
(``0x1f224420`` control, ``0x224430`` length, ``0x22443c`` status,
``0x224440`` data) and an RSA signature check (``0x1f224484``-
``0x2244a4``), plus a chip-ID (CID) and magic-data check. Failures
print ``Authenticate failed!`` / ``CID check failed!`` /
``CHK_MAGICDATA_ERR`` and halt. (An earlier note here claimed the ROM
never touches the crypto engine - that was from an ARM-only scan that
missed the Thumb ``movw``/``movt`` constants; it does.) The Miyoo
Mini's IPL is unsigned, so the ``0xfa`` test fails and the whole
authentication path is skipped - which is why a crypto access is never
*seen* at runtime even though the code is present.

On success the ROM jumps to the IPL entry at ``0xa0000000``.

No recovery / backup loader
---------------------------

This ROM has **no serial or USB download fallback** (``rom``). There
is no USB-controller base (nothing in ``0x1f2c/0x1f2d``) anywhere in
the image, the UART is output-only, and every failure path ends in a
print followed by ``b .`` (spin). So a board that cannot load a valid,
authenticated IPL from its strapped medium simply halts with the
matching ``[HALT]`` message; there is no ROM-level recovery mode here.
(A larger mask-ROM region, if one exists beyond this 16 KiB image,
could hold one, but this dump does not - its code ends by ``0x1370``,
the rest is zero padding.)

Error strings
-------------

The string table (file offset ``0x124c``) shows the failure modes the
ROM knows about (``rom``)::

  Check Header failed! [HALT]
  SPINAND init failed! [HALT]
  Load IPL from SPINOR failed! [HALT]
  Check IPL Header failed! [HALT]
  CID check failed! [HALT]
  Authenticate failed! [HALT]
  CHK_MAGICDATA_ERR
  SPINOR reset timeout!

together with ``MSTARSEMIUSFDCIS``, ``[ROM] SPINAND_ID`` and an
``I2m_ROM`` signature at the very end of the image (file offset
``0x3fe0``): ``MVX4######g833de85I2m_ROM####XVM``. The ``g833de85``
looks like the ROM's build/commit tag (``rom``).

Each string is loaded with a ``movw`` immediate of its file offset
(e.g. ``0x126c`` for ``SPINAND init failed!``) and passed to the
uart0 print routine at ``0x748`` (see `Console UART`_), so the offsets
map straight to the failure branches: ``0x124c`` NOR bad-magic,
``0x126c`` SPI-NAND init, ``0x128c`` NOR load, ``0x12b4`` IPL header
(``0xbf7``), ``0x12d8`` CID, ``0x12f4`` authenticate.

Open questions
--------------

Resolved by the Thumb disassembly (``rom``): the ROM prints over
**uart0** (``0x1f221000``), not a PM UART; ``0x1f2070c4``\ =\ ``9`` and
``0x1f203d4c``\ =\ ``0x3210`` are its UART pad-mux; ``0x1f002d84``-
``0x1f002dd8`` (including ``0x1f002db8``) are the FSP command/data
registers of the NOR read; and the authentication *is* hardware
(crypto engine), just gated behind the ``0xfa`` signed-image flag.

Still open:

* Where the ROM really lives in the address map (modelled at ``0x0``;
  code occupies only the first ``0x1370`` bytes of the 16 KiB image).
* What ``0x16001000``/``0x16001004`` are (written ``1``/``0xf0`` at
  the very start), what ``0x1f00401c`` bit 6 selects, and what the
  dispatch reads in ``0x1f00700c`` / ``0x1f004014`` bit 10 steer.
* What ``0x1f203d40`` bit 15 (cleared at progress ``0xa04``) controls.
* Whether the ROM is identical across the infinity2m family, and
  whether a larger mask-ROM region beyond this dump adds a download
  mode (this 16 KiB image has none).
