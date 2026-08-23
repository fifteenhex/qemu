#!/usr/bin/env python3
"""Turn a raw NuBus DP8390 ("mac8390") card ROM dump into a QEMU bus image.

The physical dump (e.g. mx98803afc_nic.rom, a Macronix MX98803 "Apple TFL"
card) is the logical declaration-ROM byte stream.  The card drives NuBus byte
lanes 0 and 2 (byteLanes 0xA5), so on the bus each logical byte occupies one
useful lane and the 64K image spans the top 128K of slot space with data on
the even addresses (addr % 4 in {0, 2}).  QEMU's nubus-device maps a romfile
verbatim at the top of slot space, so emit that spread "bus image".

  usage: mk-mac8390-busrom.py IN.rom OUT.bus
"""
import sys

LANES = 0xA5  # byte lanes 0 and 2


def useful(off):
    return (LANES >> (off & 3)) & 1


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    raw = open(sys.argv[1], 'rb').read()
    bus = bytearray(b'\xff' * (len(raw) * 2))
    off = 0
    for b in raw:
        while not useful(off):
            off += 1
        bus[off] = b
        off += 1
    # the format block's byte-lanes marker must be the top useful byte
    assert bus[-2] == LANES, "unexpected byte-lanes marker 0x%02x" % bus[-2]
    open(sys.argv[2], 'wb').write(bytes(bus))
    print("wrote %s (%d bytes) from %s (%d bytes)"
          % (sys.argv[2], len(bus), sys.argv[1], len(raw)))


if __name__ == '__main__':
    main()
