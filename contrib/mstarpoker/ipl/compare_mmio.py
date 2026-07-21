# SPDX-License-Identifier: GPL-2.0-or-later
"""
Compare the RIU MMIO writes of the ported IPL against the stock IPL.

Both traces come from the mmiolog.so plugin ("pc addr size value").  The
compiled port lives at different addresses than the stock IPL, so the PC is
ignored; what must match is the sequence of (addr, size, value) writes the
IPL issues - filtered to writes made from the IPL (pc in 0xa0000000..) so the
shared mask-ROM writes are excluded.

Until the whole IPL is ported, the port reproduces a prefix of the stock
trace and then halts; this checks the port's writes are an exact prefix of
the stock writes and reports the first divergence.
"""
import sys

def load(path):
    out = []
    for line in open(path):
        line = line.split()
        if len(line) != 4:
            continue
        pc, addr, size, val = line
        if not pc.startswith("a000"):          # IPL-issued writes only
            continue
        out.append((addr, int(size), val.lstrip("0") or "0"))
    return out

def main():
    mine = load(sys.argv[1])
    gold = load(sys.argv[2])
    n = len(mine)
    print("port writes: %d   stock writes: %d" % (n, len(gold)))
    for i in range(n):
        if i >= len(gold):
            print("DIVERGE @%d: port has extra write %s (stock ended)" % (i, mine[i]))
            return 1
        if mine[i] != gold[i]:
            print("DIVERGE @%d:\n  port : %s\n  stock: %s" % (i, mine[i], gold[i]))
            return 1
    print("OK: %d/%d writes match as an exact prefix of the stock IPL" % (n, len(gold)))
    return 0

if __name__ == "__main__":
    sys.exit(main())
