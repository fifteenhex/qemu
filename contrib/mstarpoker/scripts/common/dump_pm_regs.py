#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Dump the always-on (PM-domain) register banks over mstarpoker.

The PM ("always on") domain keeps power in standby; these banks hold
power-on defaults that the main domain's reset does not touch. This reads
them back as the boot ROM left them, so the values can be compared with a
model or across parts.

Best-effort list for the current target (MStar infinity2m / SSD202D);
edit PM_BANKS for other parts. Each entry is one 128-register RIU bank.

    dump_pm_regs.py --socket /tmp/s.ser
    dump_pm_regs.py --serial /dev/ttyUSB0 -o pm_regs.txt
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from mstarpoker import add_transport_args, open_link  # noqa: E402

# (base, name). One RIU bank = 128 16-bit regs on a 4-byte stride = 0x200.
PM_BANKS = [
    (0x1f001c00, "pm_clkgen  always-on clock gates (pwm/spi/ir/rtc/sar/pm_sleep)"),
    (0x1f001e00, "pm_gpio    always-on GPIO pads"),
    (0x1f002400, "rtc        second RTC (sstar,infinity-rtc)"),
    (0x1f006000, "wdt        watchdog"),
    (0x1f006800, "rtcpwc     RTC + power controller"),
    (0x1f007000, "pm_misc    chip id / boot-media strap (DID_KEY at +0x1c0)"),
]

BANK_WORDS = 0x80   # 128 registers


def format_bank(lk, base, name):
    before = lk.fault_count()
    words = lk.read_block(base, BANK_WORDS)
    faults = lk.fault_count() - before
    lines = ["\n== 0x%08x  %s ==%s" %
             (base, name, "   (%d faulted)" % faults if faults else "")]
    for i in range(0, BANK_WORDS, 4):
        lines.append("  0x%08x: %s" % (base + i * 4,
                     " ".join("%08x" % w for w in words[i:i + 4])))
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_transport_args(ap)
    ap.add_argument("-o", "--output",
                    help="also write the dump to this file")
    args = ap.parse_args()

    lk = open_link(args)
    fh = open(args.output, "w") if args.output else None
    for base, name in PM_BANKS:
        text = format_bank(lk, base, name)
        sys.stdout.write(text)
        if fh:
            fh.write(text)
    if fh:
        fh.close()
        print("\nwrote dump to %s" % args.output)


if __name__ == "__main__":
    main()
