#!/usr/bin/env python3
"""Wrap a raw m68k binary in the ELTEC RMON netboot header.

Format (RE'd from RMON 3.1.3, block callback fe819016, big-endian):

    +0  u32  magic 0x134FEE73
    +4  u16  cpu type (not read by the TFTPBOOT loader; covered by cksum)
    +6  u32  image size in bytes (100 < size <= 0x1FE0000)
    +10 u32  load address (must be inside RAM bounds, +size+2 must fit)
    +14 u32  entry address (0 -> firmware takes u32 at image offset 4;
             must satisfy load <= entry < load+size)
    +18 u16  image checksum: firmware stores this word at load+size and
             the inet checksum over (load, size+2) must verify
    +20 u16  header checksum: inet checksum over the 22-byte header
             must verify (routine fe819bf8 = RFC1071 sum, returns 0 if ok)

Image data follows the header immediately (TFTP stream = header + data).
"""
import struct
import sys


def fold(s):
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return s


def cksum16(data):
    """One's-complement sum of big-endian 16-bit words (RFC1071 style;
    a trailing odd byte counts as its high byte, like fe819bf8)."""
    if len(data) & 1:
        data += b'\0'
    s = 0
    for i in range(0, len(data), 2):
        s += struct.unpack_from('>H', data, i)[0]
    return fold(s)


def wrap(payload, load, entry, cputype=0):
    if len(payload) & 1:
        payload += b'\0'          # keep the appended cksum word aligned
    while len(payload) <= 100:    # firmware requires size > 100
        payload += b'\0\0'
    img_ck = 0xffff - cksum16(payload)
    hdr = struct.pack('>IHIIIH', 0x134FEE73, cputype, len(payload),
                      load, entry, img_ck)
    hdr_ck = 0xffff - cksum16(hdr)
    return hdr + struct.pack('>H', hdr_ck) + payload


if __name__ == '__main__':
    if len(sys.argv) < 5:
        sys.exit('usage: mke17boot.py IN OUT LOADADDR ENTRY [CPUTYPE]')
    payload = open(sys.argv[1], 'rb').read()
    load = int(sys.argv[3], 0)
    entry = int(sys.argv[4], 0)
    cputype = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0
    out = wrap(payload, load, entry, cputype)
    open(sys.argv[2], 'wb').write(out)
    print(f'{sys.argv[2]}: {len(out)} bytes, load {load:#x} entry {entry:#x}')
