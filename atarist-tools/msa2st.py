#!/usr/bin/env python3
"""Convert an MSA (Magic Shadow Archiver) floppy image to a raw .ST image.

MSA layout (all values big-endian):
  +0  0x0E0F            magic
  +2  sectors per track
  +4  sides - 1
  +6  start track
  +8  end track
  then, per track and side (track-major, side-minor):
  +0  data length word
      == spt * 512: raw track data follows
      != spt * 512: RLE-compressed track; literal bytes, except 0xE5
                    introducing a run: 0xE5 <byte> <count.w>

A raw .ST image is just the 512-byte sectors in
track-major/side-minor/sector order, which is also the order MSA
stores tracks in, so conversion is a straight per-track pipe.
"""

import struct
import sys


def convert(msa: bytes) -> bytes:
    magic, spt, sides1, start, end = struct.unpack(">HHHHH", msa[:10])
    if magic != 0x0E0F:
        raise ValueError("not an MSA image (magic 0x%04X)" % magic)
    if not (1 <= spt <= 12 and sides1 in (0, 1) and start <= end <= 86):
        raise ValueError("implausible MSA geometry: spt=%d sides=%d "
                         "tracks=%d..%d" % (spt, sides1 + 1, start, end))
    track_bytes = spt * 512
    off = 10
    out = bytearray()
    for _track in range(start, end + 1):
        for _side in range(sides1 + 1):
            (dlen,) = struct.unpack(">H", msa[off:off + 2])
            off += 2
            data = msa[off:off + dlen]
            off += dlen
            if dlen == track_bytes:
                out += data
                continue
            # RLE
            tr = bytearray()
            i = 0
            while i < len(data):
                b = data[i]
                if b == 0xE5:
                    fill, count = data[i + 1], struct.unpack(
                        ">H", data[i + 2:i + 4])[0]
                    tr += bytes([fill]) * count
                    i += 4
                else:
                    tr.append(b)
                    i += 1
            if len(tr) != track_bytes:
                raise ValueError("track decompressed to %d bytes, "
                                 "expected %d" % (len(tr), track_bytes))
            out += tr
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        print("usage: msa2st.py input.msa output.st", file=sys.stderr)
        return 1
    with open(sys.argv[1], "rb") as f:
        msa = f.read()
    st = convert(msa)
    with open(sys.argv[2], "wb") as f:
        f.write(st)
    print("%s: %d bytes (%d sectors)" % (sys.argv[2], len(st),
                                         len(st) // 512))
    return 0


if __name__ == "__main__":
    sys.exit(main())
