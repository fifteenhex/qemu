.. SPDX-License-Identifier: GPL-2.0-or-later

FCIE SD/MMC host controller
===========================

The SD/MMC host controller ("sdio"/FCIE) at ``0x1f282000``. The
register map is from the mainline driver ``drivers/mmc/host/
mstar-fcie.c`` (``linux``): a bank of 16-bit registers plus a small
command/response FIFO. Software loads a command into the FIFO,
programs ``SD_CTL`` and fires the job (``JOBSTART``); the model runs
the request on a QEMU SD card over the standard SD bus and posts the
completion. Data moves by a DMA engine that either transfers one
contiguous run or walks a list of 16-byte ADMA descriptors.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Bits
   * - ``0x00``
     - INT
     - 0 data-end, 1 cmd-end, 4 busy-end, 5 r2n-ready (write-1-clear)
   * - ``0x04``
     - INTMASK
     - interrupt enable, same layout
   * - ``0x0c`` / ``0x10``
     - DMA_ADDR low / high
     - bus address of the transfer (or ADMA descriptor list)
   * - ``0x14`` / ``0x18``
     - DMA_LEN low / high
     - transfer length
   * - ``0x30``
     - SD_CTL
     - 1 rsp-en, 2 cmd-en, 3 data-en, 4 dir (0 read/1 write),
       5 adma-en, 6 job-start, 8 busy-detect-en
   * - ``0x34``
     - SD_STS
     - 5..0 errors (CRC/timeout/no-response, write-1-clear),
       8 D0 line level
   * - ``0x80``-``0xbc``
     - FIFO
     - command/response FIFO, 16-bit words on a 4 byte stride
   * - ``0xfc``
     - RST
     - bit0 nRST. Reset done reads back nRST; while asserted it
       reads ``0xe``, which the vendor driver polls for ("IP Reset
       Switch Low Fail" if it never changes)

With no ``-drive if=sd`` image there is no card: the driver's reset
now succeeds, CMD0 gets no response and it reports no card, instead of
retrying the reset forever.
