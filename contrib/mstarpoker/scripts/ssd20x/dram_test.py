#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Detect, initialise and test the in-package DDR on an ssd20x target.

Runs from the mstarpoker stub, which the mask ROM loads instead of the
vendor IPL - so DRAM is *not* up yet. This:

  1. detects the chip and its bonded DRAM (socid);
  2. brings the DDR up by replaying the vendor IPL's MIU register sequence
     (ddr_seq_ssd202d) and polling for init/BIST completion; and
  3. tests it - data-bus, address-bus (aliasing) and pattern/random tests -
     to confirm it is functional and the full size is addressable.

    dram_test.py --socket /tmp/s.ser
    dram_test.py --serial /dev/ttyUSB0 --test-size 0x40000
    dram_test.py --serial /dev/ttyUSB0 --no-init      # DRAM already up

Note: under QEMU, DRAM is plain modelled RAM (always up) and the MIU is a
stub, so the tests pass and the init replays cleanly - this validates the
flow; the init and tests do their real work on silicon.
"""
import argparse
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from mstarpoker import add_transport_args, open_link, parse_int  # noqa: E402
import socid  # noqa: E402

sys.path.insert(0, os.path.dirname(__file__))
import ddr  # noqa: E402

DRAM_BASE = ddr.DRAM_BASE


def dram_bytes(soc):
    try:
        return int(soc.memory.split()[0]) * 1024 * 1024
    except (ValueError, IndexError):
        return 0


# --- tests: each returns None on pass or an error string on failure --------

def data_bus_test(lk, addr):
    """Walk a 1 through every data bit at one address (stuck data lines)."""
    for b in range(32):
        v = 1 << b
        lk.write32(addr, v)
        r = lk.read32(addr)
        if r != v:
            return "data bus: @0x%08x wrote 0x%08x read 0x%08x" % (addr, v, r)
    return None


def addr_bus_test(lk, base, size):
    """Unique marker at base + each power-of-2 offset; detect aliasing."""
    offs = [0] + [1 << n for n in range(2, size.bit_length()) if (1 << n) < size]
    for i, o in enumerate(offs):
        lk.write32(base + o, 0xA5A50000 | i)
    for i, o in enumerate(offs):
        r = lk.read32(base + o)
        if r != (0xA5A50000 | i):
            return ("addr bus: @0x%08x read 0x%08x, expected 0x%08x "
                    "(aliasing / wrong size?)" % (base + o, r, 0xA5A50000 | i))
    return None


def pattern_test(lk, base, nbytes):
    """Fixed and random patterns over a region (stuck / weak cells)."""
    nw = nbytes // 4
    for pat in (0x00000000, 0xffffffff, 0xaaaaaaaa, 0x55555555):
        lk.write_block(base, [pat] * nw)
        rd = lk.read_block(base, nw)
        for i, v in enumerate(rd):
            if v != pat:
                return "pattern 0x%08x: @0x%08x read 0x%08x" % (pat, base + i * 4, v)
    words = [random.getrandbits(32) for _ in range(nw)]
    lk.write_block(base, words)
    rd = lk.read_block(base, nw)
    for i, (w, v) in enumerate(zip(words, rd)):
        if v != w:
            return "random: @0x%08x wrote 0x%08x read 0x%08x" % (base + i * 4, w, v)
    return None


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_transport_args(ap)
    ap.add_argument("--no-init", action="store_true",
                    help="skip DDR init (DRAM already up)")
    ap.add_argument("--test-size", type=parse_int, default=0x10000,
                    help="pattern-test region in bytes (default 64 KiB)")
    args = ap.parse_args()

    lk = open_link(args)

    soc = socid.identify(lk)
    size = dram_bytes(soc)
    print("[detect] %s, %s, DRAM @ 0x%08x (0x%x bytes)"
          % (soc.part, soc.memory, DRAM_BASE, size))
    if not size:
        sys.exit("[detect] unknown DRAM size - cannot size the address test")

    if args.no_init:
        print("[init] skipped (--no-init)")
    else:
        ddr.init(lk)

    print("[test] running...")
    tests = [
        ("data bus", lambda: data_bus_test(lk, DRAM_BASE)),
        ("address bus (%d MiB)" % (size // (1024 * 1024)),
         lambda: addr_bus_test(lk, DRAM_BASE, size)),
        ("patterns (%d KiB)" % (args.test_size // 1024),
         lambda: pattern_test(lk, DRAM_BASE, args.test_size)),
    ]
    failed = 0
    for name, fn in tests:
        err = fn()
        print("  %-24s %s" % (name, "PASS" if err is None else "FAIL - " + err))
        failed += err is not None

    print("[result] %s" % ("all tests passed - DRAM functional"
                           if not failed else "%d test(s) FAILED" % failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
