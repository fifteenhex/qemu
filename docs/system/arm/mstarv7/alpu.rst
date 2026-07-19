.. SPDX-License-Identifier: GPL-2.0-or-later

ALPU-FA authentication chip
===========================

Not part of the SoC: a Neowine ALPU-FA i2c copy-protection chip fitted
to the board (on the Miyoo Mini, i2c bus 1 at address 0x3d). It is a
challenge-response crypto device that stops the firmware running on
cloned hardware. Both the vendor 4.9 kernel and the MainUI app
handshake with it during init and refuse to run without it: the
kernel's verifier dereferences NULL when the chip does not answer,
killing PID 1 and panicking with ``Attempted to kill init``.

The chip's crypto is modelled exactly, reverse-engineered from the
vendor kernel's own software verifier: the auth code around
``0xc01d0000``, the "transform" cipher at ``0xc01d05a4`` and its key
tables at ``0xc0345264``/``0xc0345274`` (``linux``, from the kernel
image in the firmware). The verifier is symmetric and deterministic,
so the whole secret lives in the firmware and the chip is reproduced
from it.

Protocol (the register is the first byte of each i2c write):

.. list-table::
   :header-rows: 1

   * - Access
     - Register
     - Meaning
   * - write
     - ``0x80`` / ``0x20`` / ``0x22``
     - init/config, ignored
   * - read
     - ``0x30`` (16B)
     - chip nonce; starts an auth round (reseeds the counter)
   * - read
     - ``0x73``-``0x76`` (8B)
     - feed the host's buffers; return zeros
   * - write
     - ``0x40`` (16B)
     - phase 3, ignored; advances the counter by 2
   * - write/read
     - ``0xe9`` (16B)
     - auth round 0: host writes arg0, reads back the response
   * - write/read
     - ``0x87`` (16B)
     - auth round 1

For each auth round the host writes a challenge ``arg0`` in the even
bytes, reads the response, extracts an 8-byte word W from bytes
``[0,1,4,5,8,9,12,13]``, computes ``transform(W)`` and requires it to
equal ``arg0``. So the model returns ``W = transform^-1(arg0)``. The
cipher's operation sequence depends only on the key tables and
external buffers, never on its input, which makes it a composition of
invertible byte operations and hence invertible. The model keeps the
external buffers and key byte K3 at zero (returning zeros for the
nonce and ``0x73``-``0x76`` reads), so only a deterministic counter
varies between rounds.

With the chip present the vendor kernel gets past the auth check into
full userspace: it mounts the squashfs rootfs, loads the Sigmastar
vendor modules and brings up the framebuffer.
