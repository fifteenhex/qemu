# SPDX-License-Identifier: GPL-2.0-or-later
"""
Cross-check the decompiled IPL against the model's ground-truth register
writes.

  mmiolog.txt   from the mmiolog.so TCG plugin: "pc addr size value" for
                every store into the RIU MMIO window (0x1f000000..).
  ipl_decompiled.c  Ghidra output; function starts are read from its
                "@ aXXXXXXX" comments.

It reports, for the writes issued by the IPL (pc in 0xa0000000..0xa000ffff),
what fraction land inside a decompiled function (structural coverage) and
which registers each function drives.
"""
import re
import sys
import collections

IPL_LO, IPL_HI = 0xa0000000, 0xa0010000


def main():
    c_path = sys.argv[1] if len(sys.argv) > 1 else "ipl_decompiled.c"
    log_path = sys.argv[2] if len(sys.argv) > 2 else "mmiolog.txt"

    funcs = sorted({int(m, 16) for m in
                    re.findall(r'@ (a[0-9a-f]{7}) \*/', open(c_path).read())})

    def fn_of(pc):
        lo = None
        for f in funcs:
            if f <= pc:
                lo = f
            else:
                break
        return lo

    ipl = []
    for line in open(log_path):
        if not line.strip():
            continue
        pc, addr, sz, val = line.split()
        if pc.startswith("a000"):
            ipl.append((int(pc, 16), int(addr, 16), int(sz), val))

    sites = collections.OrderedDict()
    for pc, addr, sz, val in ipl:
        sites.setdefault((pc, addr, sz), set()).add(val)

    write_pcs = sorted({pc for pc, _, _, _ in ipl})
    uncovered = [pc for pc in write_pcs if fn_of(pc) is None]

    print("IPL functions decompiled : %d" % len(funcs))
    print("Total IPL MMIO writes     : %d" % len(ipl))
    print("Distinct (pc,addr,size)   : %d" % len(sites))
    print("Distinct writing PCs      : %d" % len(write_pcs))
    print("  inside a function       : %d (%.1f%%)" %
          (len(write_pcs) - len(uncovered),
           100.0 * (len(write_pcs) - len(uncovered)) / len(write_pcs)))
    print("  not in any function     : %s" % [hex(p) for p in uncovered])

    byfn = collections.defaultdict(set)
    for (pc, addr, sz) in sites:
        byfn[fn_of(pc)].add(addr)
    print("\nDistinct MMIO registers written, by function:")
    for f in sorted(byfn):
        regs = sorted(byfn[f])
        print("  FUN_%08x : %3d regs  e.g. %s" %
              (f, len(regs), " ".join("%08x" % r for r in regs[:6])))

    return 1 if uncovered else 0


if __name__ == "__main__":
    sys.exit(main())
