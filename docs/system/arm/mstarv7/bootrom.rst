.. SPDX-License-Identifier: GPL-2.0-or-later

Boot ROM
========

What is known about the mask ROM, from static analysis of the 16 KiB
SSD202D dump (``rom``) and from watching it run under the model with
``-d unimp`` (``model``). File offsets below are also offsets from
the ROM base.

Boot flow as observed under the model
-------------------------------------

The ROM masks interrupts, enables the I-cache, then works through its
early init writing progress codes (see below) to ``0x1f200800``. It
sets up the timer at ``0x1f006040``, does a read-modify-write of
``0x1f203d40``, pokes what looks like pin or boot media setup
(``0x1f2070c4``, ``0x1f203d4c``, ``0x1f001c24``), reads three
registers that presumably describe the boot source (``0x1f0071c0``,
``0x1f00700c``, ``0x1f004014``, all reading zero in the model), tries
to load an IPL, then prints ``Check IPL Header failed! [HALT]`` on
the PM UART and hangs. With nothing backing the boot media that is
the correct outcome; on the real chip the strap registers will not
read zero, so the path taken here is not necessarily the SPI NOR one.

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
``0x1f006014``, in the same bank but before the timer block.

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
currently keeps CPU1 powered off instead; that is wrong in detail
and needs revisiting when SMP is tackled (``model``).

Boot-media strap
----------------

The two reads of ``0x1f0071c0`` (``DID_KEY``, vendor register
``0x70`` of the DID block) during the ``0xb..`` phase select the
boot medium from bits[5:2]: ``0x20`` SPI NOR, ``0x10`` NAND,
``0x08`` SPI NAND/eMMC (``prev``). Forcing the register to ``0x20``
in the model was confirmed to send the ROM down its SPI NOR path: it
starts polling the ISP flash sequencer at ``0x1f002db8``, timing the
poll against the timer counter at ``0x1f006050``/``0x1f006054``
(``model``) - which also explains the ``SPINOR reset timeout!``
string. With the strap reading zero the ROM instead falls through to
the ``Check IPL Header failed! [HALT]`` outcome.

On the SPI NOR path the previous branch describes the rest of the
flow (``prev``, msc313e): the IPL is read through the XIP window at
``0x14000000``, copied into IMI at ``0xa0000000``, its ``IPL_``
magic at offset ``4`` is checked and the ROM jumps to it.

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
``I2m_ROM`` signature at the end of the image.

Open questions
--------------

* Where the ROM really lives in the address map (modelled at ``0x0``;
  the previous branch used a 32 KiB window there, our dump is 16 KiB).
* What the real chip reads in ``DID_KEY`` bits[5:2] (the Miyoo Mini
  boots from SPI NOR so probably ``0x20``), and what ``0x1f00700c``
  and ``0x1f004014`` hold.
* What the chiptop pokes ``0x1f203d40``/``0x1f203d4c`` and
  ``0x1f2070c4`` control.
* Whether the ROM is identical across the infinity2m family.
* The PM UART clock (172 MHz per ``prev``), and why no banner is
  printed before the failure message.
* Which FSP register/bit ``0x1f002db8`` is, and what the ROM needs
  from it to move on to reading the XIP window.
