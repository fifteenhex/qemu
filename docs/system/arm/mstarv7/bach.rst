.. SPDX-License-Identifier: GPL-2.0-or-later

BACH (audio controller)
=======================

The SoC audio block, in two banks: the "bach" controller at
``0x1f2a0400`` (DMA sub-channels moving PCM between DRAM and the
codec) and the "audiotop" analog codec syscon at ``0x1f206800``,
which the driver reaches through the ``mstar,audiotop`` phandle
(``linux``, ``prev``). The vendor boot flow plays a chime through it
before MainUI starts, and MainUI opens its SDL audio through the same
path, so the playback handshake has to complete for the boot to
proceed.

The codec registers are accessed *bytewise* by the vendor driver; a
model that only accepts 16-bit accesses makes the access fault and
the audio task die with an external abort at the audiotop base
(``hw``).

Reader (playback) DMA sub-channel
---------------------------------

16-bit registers on the usual 4-byte RIU stride, byte offsets from the
bach base (``prev``, register meaning from the mainline msc313-bach
ALSA driver, ``linux``):

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Bits
   * - ``0x100``
     - CTRL0
     - bit8 INT_CLEAR (ack: write 1 then 0), bit10 empty interrupt
       enable, bit13 underrun interrupt enable
   * - ``0x104``
     - EN
     - bits[11:0] ring address low, bit13 trigger (self-clearing:
       queue one TRIGGER of data), bit14 init (reset the write
       pointer), bit15 channel enable
   * - ``0x108``
     - ADDR
     - bits[14:0] ring address high
   * - ``0x10c``
     - SIZE
     - ring size, in 8 byte MIU units
   * - ``0x110``
     - TRIGGER
     - bytes queued per trigger, in MIU units
   * - ``0x118``
     - UNDERRUN
     - underrun threshold, in MIU units
   * - ``0x11c``
     - LEVEL
     - bytes queued but not yet consumed, in MIU units
   * - ``0x120``
     - CTRL8
     - status flags: bit2 underrun, bit4 empty

The ring's DRAM address is ``((ADDR << 12) | EN[11:0]) << 3``, a MIU
(DRAM bus) address. The driver queues a period of PCM, sets the
trigger bit and arms the underrun interrupt; the hardware raises the
shared bach interrupt ("IRQ" mst-intc line 42) - latching the CTRL8
flag - once the DMA has drained the queue to the underrun threshold.
The interrupt handler acks with CTRL0.INT_CLEAR and queues the next
period. The Miyoo Mini firmware runs this at 44100 Hz stereo S16_LE.

The model snapshots each triggered period out of the ring and plays
it through QEMU's audio backend, reporting the backend's drain
progress in LEVEL and raising the underrun interrupt on the falling
edge across the threshold - one interrupt per period, which is the
"period elapsed" the ALSA machinery advances on.

The audiotop bank is modelled as plain read-back storage; nothing
decodes it yet beyond keeping the codec bring-up writes readable.
