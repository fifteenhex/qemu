#!/usr/bin/env python3
"""
Verify the model's synthetic VBIOS: map the PCI expansion-ROM BAR, walk the
same pointer chain the tdfxfb driver uses (ROM[0x50] -> ROM cfg -> OEM cfg),
read the config table, run the driver's cold-boot register sequence with those
values, and confirm the memory gate opens (LFB writes reach VRAM).
"""
import os, socket, struct, subprocess, sys, time

QEMU = "/workspace/src/qemu-voodoo3/build/qemu-system-x86_64"
DIR = "/workspace/src/voodoo3-test"
SOCK = DIR + "/qv.sock"
BAR0, BAR1, BAR2, ROM = 0xf0000000, 0xf4000000, 0xc000, 0xf6000000

# init registers (BAR0 MMIO)
DRAMINIT0, DRAMINIT1, DRAMCOMMAND, DRAMDATA = 0x18, 0x1c, 0x30, 0x34
PLLCTRL1 = 0x44


class QT:
    def __init__(s, p):
        s.s = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try: s.s.connect(p); break
            except OSError: time.sleep(0.1)
        s.f = s.s.makefile("rw")
    def cmd(s, l):
        s.f.write(l + "\n"); s.f.flush()
        while True:
            r = s.f.readline().strip()
            if r.startswith("OK"): return r[2:].strip()
            if r.startswith("FAIL"): raise RuntimeError(r)
    def wl(s, a, v): s.cmd("writel 0x%x 0x%x" % (a, v & 0xffffffff))
    def rl(s, a): return int(s.cmd("readl 0x%x" % a), 16)
    def rw(s, a): return int(s.cmd("readw 0x%x" % a), 16)
    def rb(s, a): return int(s.cmd("readb 0x%x" % a), 16)
    def outl(s, p, v): s.cmd("outl 0x%x 0x%x" % (p, v))
    def inl(s, p): return int(s.cmd("inl 0x%x" % p), 16)


def main():
    if os.path.exists(SOCK): os.unlink(SOCK)
    err = open(DIR + "/qv.err", "wb")
    q = subprocess.Popen([QEMU, "-M", "pc,accel=qtest", "-display", "none",
        "-qtest", "unix:" + SOCK + ",server=on,wait=off",
        "-vga", "none", "-device", "voodoo3"], stderr=err)
    fails = []
    def check(name, cond, got=""):
        print(("PASS " if cond else "FAIL ") + name + ("  " + got if got else ""))
        if not cond: fails.append(name)
    try:
        t = QT(SOCK)
        for d in range(1, 32):
            t.outl(0xcf8, 0x80000000 | (d << 11))
            if t.inl(0xcfc) == 0x0005121a: slot = d; break
        def cfg(o, v):
            t.outl(0xcf8, 0x80000000 | (slot << 11) | (o & 0xfc)); t.outl(0xcfc, v)
        def cfgr(o):
            t.outl(0xcf8, 0x80000000 | (slot << 11) | (o & 0xfc)); return t.inl(0xcfc)
        cfg(0x10, BAR0); cfg(0x14, BAR1); cfg(0x18, BAR2 | 1); cfg(0x04, 3)
        # map + enable the expansion-ROM BAR (config 0x30, bit0 = enable)
        cfg(0x30, ROM | 1)

        # option ROM signature
        sig = t.rb(ROM) | (t.rb(ROM + 1) << 8)
        check("rom signature 0xaa55", sig == 0xaa55, hex(sig))

        # ROM[0x50] -> ROM cfg -> OEM cfg (exactly the driver's walk)
        romcfg = t.rb(ROM + 0x50) | (t.rb(ROM + 0x51) << 8)
        oemcfg = t.rb(ROM + romcfg) | (t.rb(ROM + romcfg + 1) << 8)
        def cfg32(o):
            b = ROM + oemcfg + o
            return t.rb(b) | (t.rb(b+1) << 8) | (t.rb(b+2) << 16) | (t.rb(b+3) << 24)
        pllctrl1 = cfg32(0x18)
        draminit0 = cfg32(0x0c)
        check("config pllctrl1 0x1a01", pllctrl1 == 0x1a01, hex(pllctrl1))
        check("config draminit0 != 0", draminit0 != 0, hex(draminit0))
        # driver sanity: 40MHz <= pll <= 250MHz
        khz = (14318 * (((pllctrl1 >> 8) & 0xff) + 2) //
               (((pllctrl1 >> 2) & 0x3f) + 2)) >> (pllctrl1 & 3)
        check("mem pll in range", 40000 <= khz <= 250000, "%d kHz" % khz)

        # memory gate is shut before init: an LFB write must not stick
        t.wl(BAR1 + 0x40, 0xdeadbeef)
        pre = t.rl(BAR1 + 0x40)
        check("vram gated before init", pre != 0xdeadbeef, hex(pre))

        # driver cold-boot sequence with the config-table values
        t.wl(BAR0 + PLLCTRL1, pllctrl1)
        t.wl(BAR0 + DRAMINIT0, draminit0)
        t.wl(BAR0 + DRAMINIT1, cfg32(0x10))
        t.wl(BAR0 + DRAMDATA, cfg32(0x20))
        t.wl(BAR0 + DRAMCOMMAND, 0x10d)

        # gate now open: LFB writes reach VRAM
        t.wl(BAR1 + 0x40, 0xcafe1234)
        post = t.rl(BAR1 + 0x40)
        check("vram live after init", post == 0xcafe1234, hex(post))

        print("----")
        print("FAILURES: %s" % fails if fails else "all vbios checks passed")
    finally:
        q.terminate()
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
