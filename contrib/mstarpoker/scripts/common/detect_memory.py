#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Detect the chip and its bonded in-package memory over mstarpoker.

MStar infinity2m parts bond the DDR die into the package; the chiptop
"bond" strap identifies the package (and with it the DRAM size), and the
chip-version register gives the revision. Both are pure ID reads - no
DRAM/MIU init needed - so they work straight from the stub.

Identification lives in the reusable socid library.

    detect_memory.py --socket /tmp/s.ser
    detect_memory.py --serial /dev/ttyUSB0
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from mstarpoker import add_transport_args, open_link  # noqa: E402
import socid  # noqa: E402


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_transport_args(ap)
    args = ap.parse_args()

    lk = open_link(args)
    soc = socid.identify(lk)
    if soc.faulted:
        sys.exit("an ID register faulted - wrong register base for this part?")
    print(socid.format_id(soc))


if __name__ == "__main__":
    main()
