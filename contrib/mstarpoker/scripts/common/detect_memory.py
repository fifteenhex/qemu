#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Detect the bonded in-package memory over mstarpoker.

MStar infinity2m parts bond the DDR die into the package; the "bond" strap
in the chiptop block identifies the package, and with it the memory size.
Reading it needs no DRAM to be up (it is a pure strap), so it works from
the stub before any MIU/DDR init.

    detect_memory.py --socket /tmp/s.ser
    detect_memory.py --serial /dev/ttyUSB0
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from mstarpoker import add_transport_args, open_link  # noqa: E402

# chiptop block base 0x1f203c00, bond strap at +0x120.
BOND_REG = 0x1f203d20

# Known bond values -> (part, bonded DRAM). Extend as parts are confirmed.
BOND_TABLE = {
    0x1d: ("SSD201",  "64 MiB DDR3"),
    0x1e: ("SSD202D", "128 MiB DDR3"),
}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_transport_args(ap)
    args = ap.parse_args()

    lk = open_link(args)
    val, faulted = lk.probe(BOND_REG)
    if faulted:
        sys.exit("bond strap 0x%08x faulted - wrong chiptop base for this part?"
                 % BOND_REG)

    bond = val & 0xff
    part, mem = BOND_TABLE.get(bond, ("unknown", "unknown - add to BOND_TABLE"))
    print("bond strap  0x%08x = 0x%02x (raw 0x%08x)" % (BOND_REG, bond, val))
    print("part        %s" % part)
    print("bonded DRAM %s" % mem)


if __name__ == "__main__":
    main()
