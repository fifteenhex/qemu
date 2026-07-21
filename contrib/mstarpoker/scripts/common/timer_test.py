#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Sanity check of the PM timers the DDR settle delays rely on.

The infinity2m has three timers (0x1f006040, stride 0x40); the DDR-init blob
busy-waits on timer[0]'s free-running counter. If a timer is not running, or
runs at a rate we do not expect, every delay is wrong - so measure them
directly against the host clock:

  1. dump each timer's registers;
  2. confirm its counter advances; and
  3. time a known host-side interval to compute the tick rate, and how
     TIMER_DIVIDE scales it.

The model assumes a 432 MHz source (infinity2m.h); a measured ~12 MHz at
DIVIDE=0 would mean either a fixed prescaler or an unselected clock source -
this shows the truth per timer.

    timer_test.py --serial /dev/ttyUSB0                    # all timers, as-is
    timer_test.py --serial /dev/ttyUSB0 --set-divide 0x23  # apply /36 to each
    timer_test.py --serial /dev/ttyUSB0 --timer 0          # just timer[0]
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from mstarpoker import add_transport_args, open_link, parse_int  # noqa: E402

TIMER_BASE = 0x1f006040
TIMER_STRIDE = 0x40
NUM_TIMERS = 3
CTRL, MAX_L, MAX_H, CNT_L, CNT_H, DIV = 0x00, 0x08, 0x0c, 0x10, 0x14, 0x18
CTRL_EN = 1 << 0
CTRL_TRIG = 1 << 1

# The model assumes the block is clocked off 432 MHz (infinity2m.h); the 12 MHz
# crystal is the other candidate. rate*(DIVIDE+1) should reveal the source.
SRC_CANDIDATES = (432000000, 12000000)


def counter(lk, base):
    """Read the 32-bit counter the way the delay routine does: low16|high16."""
    lo = lk.read32(base + CNT_L) & 0xffff
    hi = lk.read32(base + CNT_H) & 0xffff
    return lo | (hi << 16)


def measure(lk, base, secs):
    c0 = counter(lk, base)
    t0 = time.time()
    time.sleep(secs)
    c1 = counter(lk, base)
    dt = time.time() - t0
    ticks = (c1 - c0) & 0xffffffff        # tolerate 32-bit wrap
    return ticks, dt


def dump_regs(lk, base, tag):
    print("  registers %s:" % tag)
    for name, off in [("CTRL", CTRL), ("MAX_L", MAX_L), ("MAX_H", MAX_H),
                      ("CNT_L", CNT_L), ("CNT_H", CNT_H), ("DIVIDE", DIV)]:
        print("    %-6s 0x%08x = 0x%04x"
              % (name, base + off, lk.read32(base + off) & 0xffff))


def start_timer(lk, base, divide=None):
    """Program a full-range free-running counter and (re)start it, applying
    `divide` first if given. TRIG reloads the divider so a divide change on an
    already-running timer actually takes effect."""
    if divide is not None:
        lk.write32(base + DIV, divide)
    lk.write32(base + MAX_L, 0xffff)
    lk.write32(base + MAX_H, 0xffff)
    lk.write32(base + CTRL, CTRL_EN | CTRL_TRIG)


def check_timer(lk, idx, secs, set_divide):
    base = TIMER_BASE + idx * TIMER_STRIDE
    print("=== timer[%d] @ 0x%08x ===" % (idx, base))
    dump_regs(lk, base, "before")

    running = counter(lk, base) != counter(lk, base)

    # Enable a frozen timer, or (re)start with the new divider so it applies.
    if not running or set_divide is not None:
        w = [("DIVIDE", base + DIV, set_divide)] if set_divide is not None else []
        w += [("MAX_L", base + MAX_L, 0xffff), ("MAX_H", base + MAX_H, 0xffff),
              ("CTRL", base + CTRL, CTRL_EN | CTRL_TRIG)]
        print("  [writing] " + ", ".join("0x%08x <- 0x%x" % (a, v)
                                         for _, a, v in w))
        for _, a, v in w:
            lk.write32(a, v)
        dump_regs(lk, base, "after write (did they stick?)")

    c0 = counter(lk, base)
    c1 = counter(lk, base)
    running = c1 != c0
    print("  counter back-to-back: 0x%08x -> 0x%08x  (running: %s)"
          % (c0, c1, "YES" if running else "NO - still frozen (clock gated?)"))
    if not running:
        print()
        return

    rates = []
    for _ in range(3):
        ticks, dt = measure(lk, base, secs)
        rates.append(ticks / dt if dt else 0)
    avg = sum(rates) / len(rates)
    div = lk.read32(base + DIV) & 0xffff
    print("  RATE: %.0f Hz (%.3f MHz)  [DIVIDE=0x%x]" % (avg, avg / 1e6, div))
    if avg > 1000:
        pre = avg * (div + 1)
        best = min(SRC_CANDIDATES, key=lambda s: abs(s - pre))
        print("  source clock (rate*(DIVIDE+1)) = %.3f MHz (nearest: %d MHz)"
              % (pre / 1e6, best // 1000000))
        one = 0x2ee0 / avg
        print("  one 0x2ee0-tick delay on this timer = %.3f ms" % (one * 1e3))
    print()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_transport_args(ap)
    ap.add_argument("--set-divide", type=parse_int,
                    help="write TIMER_DIVIDE on each timer before measuring")
    ap.add_argument("--timer", type=int, choices=range(NUM_TIMERS),
                    help="only check this timer (default: all)")
    ap.add_argument("--secs", type=float, default=0.5,
                    help="host-side interval per measurement (default 0.5)")
    args = ap.parse_args()

    lk = open_link(args)
    idxs = [args.timer] if args.timer is not None else range(NUM_TIMERS)
    for i in idxs:
        check_timer(lk, i, args.secs, args.set_divide)


if __name__ == "__main__":
    main()
