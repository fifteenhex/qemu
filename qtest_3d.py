#!/usr/bin/env python3
"""Drive the voodoo3 3D engine over qtest: clear + Gouraud triangle."""
import json, os, socket, struct, subprocess, sys, time

QEMU = "/workspace/src/qemu-voodoo3/build/qemu-system-x86_64"
DIR = "/workspace/src/voodoo3-test"
QTSOCK = DIR + "/q3.sock"
QMPSOCK = DIR + "/q3qmp.sock"

BAR0 = 0xf0000000
BAR1 = 0xf4000000
BAR2 = 0xc000
D3 = BAR0 + 0x200000

# io regs
PLL0, PLL1 = 0x40, 0x44
DRAMINIT0, DRAMINIT1, DRAMCOMMAND = 0x18, 0x1c, 0x30
VGAINIT0, VIDPROCCFG, VIDSCREENSIZE = 0x28, 0x5c, 0x98
VIDDESKSTART, VIDDESKSTRIDE = 0xe4, 0xe8

# 3d regs (offset within 0x200000)
FBZMODE, FBZCOLORPATH, ALPHAMODE = 0x110, 0x104, 0x10c
CLIPLR, CLIPBT = 0x118, 0x11c
COLBUF, COLSTRIDE, AUXBUF, AUXSTRIDE = 0x1ec, 0x1f0, 0x1f4, 0x1f8
ZACOLOR, C1 = 0x130, 0x148
FASTFILL, SWAPBUF = 0x124, 0x128
SSETUPMODE = 0x260
SVX, SVY, SARGB, SVZ, SWOOW = 0x264, 0x268, 0x26c, 0x280, 0x284
SBEGIN, SDRAW = 0x2a4, 0x2a0


def f2i(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


class QT:
    def __init__(s, p):
        s.s = socket.socket(socket.AF_UNIX)
        for _ in range(50):
            try:
                s.s.connect(p); break
            except OSError:
                time.sleep(0.1)
        s.f = s.s.makefile("rw")
    def cmd(s, l):
        s.f.write(l + "\n"); s.f.flush()
        while True:
            r = s.f.readline().strip()
            if r.startswith("OK"):
                return r[2:].strip()
            if r.startswith("FAIL"):
                raise RuntimeError(l + " -> " + r)
    def wl(s, a, v): s.cmd("writel 0x%x 0x%x" % (a, v & 0xffffffff))
    def outl(s, p, v): s.cmd("outl 0x%x 0x%x" % (p, v))
    def inl(s, p): return int(s.cmd("inl 0x%x" % p), 16)


class QMPc:
    def __init__(s, p):
        s.s = socket.socket(socket.AF_UNIX); s.s.connect(p)
        s.f = s.s.makefile("rw"); s.f.readline(); s.cmd("qmp_capabilities")
    def cmd(s, n, **a):
        s.f.write(json.dumps({"execute": n, "arguments": a}) + "\n"); s.f.flush()
        while True:
            r = json.loads(s.f.readline())
            if "return" in r or "error" in r:
                return r


def cfg(q, slot, off, val):
    q.outl(0xcf8, 0x80000000 | (slot << 11) | (off & 0xfc)); q.outl(0xcfc, val)


def main():
    for p in (QTSOCK, QMPSOCK):
        if os.path.exists(p):
            os.unlink(p)
    errlog = open(DIR + "/q3.err", "wb")
    qemu = subprocess.Popen([
        QEMU, "-M", "pc,accel=qtest", "-display", "none",
        "-qtest", "unix:" + QTSOCK + ",server=on,wait=off",
        "-qmp", "unix:" + QMPSOCK + ",server=on,wait=off",
        "-vga", "none", "-device", "voodoo3"], stderr=errlog)
    try:
        run()
    finally:
        qemu.terminate()


def run():
    q = QT(QTSOCK); m = QMPc(QMPSOCK)
    # find card
    slot = None
    for d in range(32):
        q.outl(0xcf8, 0x80000000 | (d << 11));
        if q.inl(0xcfc) == 0x0005121a:
            slot = d; break
    assert slot is not None, "no voodoo3"
    cfg(q, slot, 0x10, BAR0); cfg(q, slot, 0x14, BAR1)
    cfg(q, slot, 0x18, BAR2 | 1); cfg(q, slot, 0x04, 0x0003)

    # bring up memory like the BIOS
    q.wl(BAR0 + PLL1, (44 << 8) | (2 << 2))
    q.wl(BAR0 + DRAMINIT0, (1 << 26) | (1 << 27))
    q.wl(BAR0 + DRAMINIT1, (1 << 30))
    q.wl(BAR0 + DRAMCOMMAND, 0x01)

    W, H = 640, 480
    # 16bpp desktop scanout of the front buffer (offset 0)
    q.wl(BAR0 + PLL0, (12 << 8) | (1 << 2) | 1)
    q.wl(BAR0 + VGAINIT0, (1 << 2))
    q.wl(BAR0 + VIDSCREENSIZE, W | (H << 12))
    q.wl(BAR0 + VIDDESKSTRIDE, W * 2)
    q.wl(BAR0 + VIDDESKSTART, 0)
    q.wl(BAR0 + VIDPROCCFG, 1 | (1 << 7) | (1 << 18))  # en|desk|16bpp

    # 3D render target = front buffer, depth after it
    q.wl(D3 + COLBUF, 0)
    q.wl(D3 + COLSTRIDE, W * 2)
    q.wl(D3 + AUXBUF, W * H * 2)
    q.wl(D3 + AUXSTRIDE, W * 2)
    q.wl(D3 + CLIPLR, (0 << 16) | W)
    q.wl(D3 + CLIPBT, (0 << 16) | H)
    q.wl(D3 + FBZMODE, (7 << 5) | (1 << 4) | (1 << 10))  # depth on, zfunc=always
    q.wl(D3 + FBZCOLORPATH, 0)                            # iterated colour
    q.wl(D3 + ALPHAMODE, 0)

    # clear to dark blue
    q.wl(D3 + C1, 0x000030)
    q.wl(D3 + ZACOLOR, 0xffff)
    q.wl(D3 + FASTFILL, 1)

    # Gouraud triangle via the setup unit (packed ARGB, fan)
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3))   # RGB | Z | Wfbi
    verts = [(320, 80, 0xffff0000),   # red top
             (90, 400, 0xff00ff00),   # green bottom-left
             (550, 400, 0xff0000ff)]  # blue bottom-right
    for i, (x, y, argb) in enumerate(verts):
        q.wl(D3 + SVX, f2i(float(x)))
        q.wl(D3 + SVY, f2i(float(y)))
        q.wl(D3 + SVZ, f2i(1000.0))
        q.wl(D3 + SWOOW, f2i(1.0))
        q.wl(D3 + SARGB, argb)
        q.wl(D3 + (SBEGIN if i == 0 else SDRAW), 1)

    q.wl(D3 + SWAPBUF, 1)

    m.cmd("screendump", filename=DIR + "/tri.ppm")
    data = open(DIR + "/tri.ppm", "rb").read().split(b"\n", 3)
    px = data[3]
    cols = {}
    for i in range(0, len(px) - 2, 3):
        c = (px[i], px[i + 1], px[i + 2])
        cols[c] = cols.get(c, 0) + 1
    top = sorted(cols.items(), key=lambda kv: -kv[1])[:8]
    print("size", data[1])
    print("distinct colours:", len(cols))
    for c, n in top:
        print("  %-16s %d" % (str(c), n))
    # expect: blue-ish background + red/green/blue triangle region
    reds = sum(n for c, n in cols.items() if c[0] > 150 and c[1] < 100 and c[2] < 100)
    greens = sum(n for c, n in cols.items() if c[1] > 150 and c[0] < 100 and c[2] < 120)
    print("PASS" if (reds > 50 and greens > 50 and len(cols) > 20)
          else "FAIL", "reds=%d greens=%d" % (reds, greens))
    from PIL import Image
    Image.open(DIR + "/tri.ppm").save(DIR + "/tri.png")


if __name__ == "__main__":
    main()
