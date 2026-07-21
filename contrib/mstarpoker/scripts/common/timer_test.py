#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Basic sanity check of the PM timer[0] the DDR settle delays rely on.

The DDR-init blob busy-waits on timer[0] (free-running counter at 0x1f006050),
whose rate the IPL sets via TIMER_DIVIDE (0x1f006058). If that timer is not
running, or runs at a rate we do not expect, every delay is wrong - so before
trusting the delays, measure the timer directly against the host clock:

  1. dump timer[0]'s registers;
  2. confirm the counter actually advances; and
  3. time a known host-side interval and compute the tick rate, so we can see
     the real frequency and how TIMER_DIVIDE changes it.

    timer_test.py --serial /dev/ttyUSB0                 # measure as-is
    timer_test.py --serial /dev/ttyUSB0 --set-divide 0x23   # like the IPL (/36)
    timer_test.py --serial /dev/ttyUSB0 --set-divide 0x00   # no divide (/1)
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from mstarpoker import add_transport_args, open_link, parse_int  # noqa: E402

TIMER0 = 0x1f006040
CTRL, MAX_L, MAX_H, CNT_L, CNT_H, DIV = 0x00, 0x08, 0x0c, 0x10, 0x14, 0x18

# The block is documented as clocked off the 12 MHz crystal (see
# hw/timer/mstar_timer.c); with DIVIDE=D the counter ticks at 12 MHz / (D + 1).
XTAL_HZ = 12000000


def counter(lk):
    """Read the 32-bit counter the way the delay routine does: low16|high16."""
    lo = lk.read32(TIMER0 + CNT_L) & 0xffff
    hi = lk.read32(TIMER0 + CNT_H) & 0xffff
    return lo | (hi << 16)


def measure(lk, secs):
    c0 = counter(lk)
    t0 = time.time()
    time.sleep(secs)
    c1 = counter(lk)
    dt = time.time() - t0
    ticks = (c1 - c0) & 0xffffffff        # tolerate 32-bit wrap
    return ticks, dt


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_transport_args(ap)
    ap.add_argument("--set-divide", type=parse_int,
                    help="write TIMER_DIVIDE (0x1f006058) before measuring")
    ap.add_argument("--secs", type=float, default=0.5,
                    help="host-side interval per measurement (default 0.5)")
    args = ap.parse_args()

    lk = open_link(args)

    print("timer[0] registers (0x%08x):" % TIMER0)
    for name, off in [("CTRL", CTRL), ("MAX_L", MAX_L), ("MAX_H", MAX_H),
                      ("CNT_L", CNT_L), ("CNT_H", CNT_H), ("DIVIDE", DIV)]:
        print("  %-6s 0x%08x = 0x%04x"
              % (name, TIMER0 + off, lk.read32(TIMER0 + off) & 0xffff))

    if args.set_divide is not None:
        lk.write32(TIMER0 + DIV, args.set_divide)
        print("\n[set] TIMER_DIVIDE = 0x%x -> expect ~%.0f Hz (%.3f MHz)"
              % (args.set_divide, XTAL_HZ / (args.set_divide + 1),
                 XTAL_HZ / (args.set_divide + 1) / 1e6))

    # does it run at all?
    c0 = counter(lk)
    c1 = counter(lk)
    print("\ncounter back-to-back: 0x%08x -> 0x%08x  (running: %s)"
          % (c0, c1, "YES" if c1 != c0 else "NO - counter is frozen!"))

    # measure the rate a few times
    print("\nrate (timed against the host clock):")
    rates = []
    for _ in range(3):
        ticks, dt = measure(lk, args.secs)
        hz = ticks / dt if dt else 0
        rates.append(hz)
        print("  %8d ticks in %.3f s -> %10.0f Hz (%.3f MHz)"
              % (ticks, dt, hz, hz / 1e6))

    avg = sum(rates) / len(rates)
    div = lk.read32(TIMER0 + DIV) & 0xffff
    print("\naverage: %.0f Hz (%.3f MHz), DIVIDE=0x%x" % (avg, avg / 1e6, div))
    if avg > 1000:
        implied_xtal = avg * (div + 1)
        print("implied source clock (rate*(DIVIDE+1)): %.3f MHz "
              "(expected ~12 MHz)" % (implied_xtal / 1e6))
        # the DDR blob uses 0x2ee0 (12000) ticks per delay, x11 delays
        one = 0x2ee0 / avg
        print("=> one 0x2ee0-tick DDR delay = %.3f ms (%.0f us); "
              "the blob's 11 delays total ~%.0f ms"
              % (one * 1e3, one * 1e6, one * 11 * 1e3))
    else:
        print("counter barely moved - the timer is not clocking as expected.")


if __name__ == "__main__":
    main()
