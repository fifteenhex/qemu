#!/usr/bin/env python3
"""
Generate the QEMU reference digest for tdfx_probe.c. Drives the model over
qtest with the *identical* register sequences and pixel samples that
tdfx_probe.c uses, and prints the same per-test lines. Run it and save the
output as tdfx_probe.expected; then diff the real-hardware tdfx_probe run
against that.
"""
import os, socket, struct, subprocess, sys, time

QEMU = "/workspace/src/qemu-voodoo3/build/qemu-system-x86_64"
DIR = "/workspace/src/voodoo3-test"
SOCK = DIR + "/qp.sock"
BAR0, BAR1, BAR2 = 0xf0000000, 0xf4000000, 0xc000
D3 = BAR0 + 0x200000
W, H = 320, 240

# 3D register offsets (== tdfx_probe.c)
FBZCOLORPATH, FOGMODE, ALPHAMODE, FBZMODE = 0x104, 0x108, 0x10c, 0x110
CLIPLR, CLIPBT = 0x118, 0x11c
FASTFILL, FOGCOLOR, ZACOLOR, CHROMAKEY, C1 = 0x124, 0x12c, 0x130, 0x134, 0x148
COLBUF, COLSTRIDE, AUXBUF, AUXSTRIDE = 0x1ec, 0x1f0, 0x1f4, 0x1f8
VA, VB, VC, STARTR, DPDX, DPDY, TRICMD = 0x008, 0x010, 0x018, 0x020, 0x040, 0x060, 0x080
FVA, FVB, FVC, FSTARTR, FDPDX, FDPDY, FTRICMD = 0x088, 0x090, 0x098, 0x0a0, 0x0c0, 0x0e0, 0x100
SSETUPMODE, SVX, SVY, SARGB, SVZ, SWOOW, SSOW0, STOW0 = 0x260, 0x264, 0x268, 0x26c, 0x280, 0x284, 0x28c, 0x290
SDRAW, SBEGIN = 0x2a0, 0x2a4
TEXMODE, TLOD, TEXBASE = 0x300, 0x304, 0x30c
RGBW = 1 << 9
ENCLIP = 1 << 0
TC = (1<<12)|(1<<18)|(1<<21)|(1<<27)


def f2i(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


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
    def ww(s, a, v): s.cmd("writew 0x%x 0x%x" % (a, v & 0xffff))
    def rw(s, a): return int(s.cmd("readw 0x%x" % a), 16)
    def outl(s, p, v): s.cmd("outl 0x%x 0x%x" % (p, v))
    def inl(s, p): return int(s.cmd("inl 0x%x" % p), 16)


def main():
    if os.path.exists(SOCK): os.unlink(SOCK)
    err = open(DIR + "/qp.err", "wb")
    q = subprocess.Popen([QEMU, "-M", "pc,accel=qtest", "-display", "none",
        "-qtest", "unix:" + SOCK + ",server=on,wait=off",
        "-vga", "none", "-device", "voodoo3"], stderr=err)
    try:
        run()
    finally:
        q.terminate()


def w3(t, off, v): t.wl(D3 + off, v)
def pix(t, x, y): return t.rw(BAR1 + (y * W + x) * 2)
def line(name, s): print("%-22s %s" % (name, s))


def vert(t, x, y, argb, first, z=1000.0, s=0.0, tt=0.0):
    w3(t, SVX, f2i(x)); w3(t, SVY, f2i(y)); w3(t, SVZ, f2i(z))
    w3(t, SWOOW, f2i(1.0)); w3(t, SARGB, argb)
    w3(t, SSOW0, f2i(s)); w3(t, STOW0, f2i(tt))
    w3(t, SBEGIN if first else SDRAW, 1)


def clear(t, argb):
    w3(t, C1, argb); w3(t, FBZMODE, (7 << 5) | RGBW); w3(t, FASTFILL, 1)


def run():
    t = QT(SOCK)
    # bring the card up like the driver/BIOS would (qtest only)
    slot = None
    for d in range(32):
        t.outl(0xcf8, 0x80000000 | (d << 11))
        if t.inl(0xcfc) == 0x0005121a: slot = d; break
    def cfg(o, v): t.outl(0xcf8, 0x80000000 | (slot << 11) | (o & 0xfc)); t.outl(0xcfc, v)
    cfg(0x10, BAR0); cfg(0x14, BAR1); cfg(0x18, BAR2 | 1); cfg(0x04, 3)
    t.wl(BAR0 + 0x44, (44 << 8) | (2 << 2)); t.wl(BAR0 + 0x18, (1 << 26) | (1 << 27))
    t.wl(BAR0 + 0x1c, (1 << 30)); t.wl(BAR0 + 0x30, 1)
    t.wl(BAR0 + 0x40, (12 << 8) | (1 << 2) | 1); t.wl(BAR0 + 0x28, (1 << 2))
    t.wl(BAR0 + 0x98, W | (H << 12)); t.wl(BAR0 + 0xe8, W * 2)
    t.wl(BAR0 + 0xe4, 0); t.wl(BAR0 + 0x5c, 1 | (1 << 7) | (1 << 18))

    print("# tdfx_probe QEMU reference (voodoo3 model)")
    w3(t, COLBUF, 0); w3(t, COLSTRIDE, W * 2)
    w3(t, AUXBUF, W * H * 2); w3(t, AUXSTRIDE, W * 2)
    w3(t, CLIPLR, W); w3(t, CLIPBT, H)
    w3(t, ALPHAMODE, 0); w3(t, FBZCOLORPATH, 0)
    w3(t, FBZMODE, (7 << 5) | RGBW)

    clear(t, 0x00204060); line("fastfill", "0x%04x" % pix(t, 10, 10))

    clear(t, 0x00ff0000)
    w3(t, C1, 0x000000ff); w3(t, FBZMODE, 7 << 5); w3(t, FASTFILL, 1)
    line("rgb-write-mask", "0x%04x" % pix(t, 10, 10))

    clear(t, 0); w3(t, FBZMODE, (7 << 5) | RGBW); w3(t, FBZCOLORPATH, 0)
    w3(t, VA, 30 << 4); w3(t, VA + 4, 30 << 4); w3(t, VB, 300 << 4); w3(t, VB + 4, 40 << 4)
    w3(t, VC, 60 << 4); w3(t, VC + 4, 210 << 4)
    for i in range(8): w3(t, DPDX + i * 4, 0); w3(t, DPDY + i * 4, 0)
    w3(t, STARTR, 0); w3(t, STARTR + 4, 220 << 12); w3(t, STARTR + 8, 0); w3(t, STARTR + 12, 1000 << 12)
    w3(t, TRICMD, 1); line("direct-int-tri", "0x%04x" % pix(t, 90, 90))

    clear(t, 0)
    w3(t, FVA, f2i(30)); w3(t, FVA + 4, f2i(30)); w3(t, FVB, f2i(300)); w3(t, FVB + 4, f2i(40))
    w3(t, FVC, f2i(60)); w3(t, FVC + 4, f2i(210))
    for i in range(8): w3(t, FDPDX + i * 4, 0); w3(t, FDPDY + i * 4, 0)
    w3(t, FSTARTR, f2i(230)); w3(t, FSTARTR + 4, f2i(0)); w3(t, FSTARTR + 8, f2i(0)); w3(t, FSTARTR + 12, f2i(1000))
    w3(t, FTRICMD, 1); line("direct-float-tri", "0x%04x" % pix(t, 90, 90))

    clear(t, 0); w3(t, SSETUPMODE, 1 | (1 << 2) | (1 << 3))
    vert(t, 160, 30, 0xffff0000, 1); vert(t, 40, 210, 0xff00ff00, 0); vert(t, 280, 210, 0xff0000ff, 0)
    line("setup-fan-gouraud", "0x%04x" % pix(t, 160, 150))

    clear(t, 0); w3(t, SSETUPMODE, 1 | (1 << 2) | (1 << 3))
    vert(t, 40, 40, 0xffff0000, 1); vert(t, 40, 200, 0xffff0000, 0); vert(t, 280, 120, 0xffff0000, 0)
    line("sargb-red", "0x%04x" % pix(t, 100, 110))
    clear(t, 0)
    vert(t, 40, 40, 0xff0000ff, 1); vert(t, 40, 200, 0xff0000ff, 0); vert(t, 280, 120, 0xff0000ff, 0)
    line("sargb-blue", "0x%04x" % pix(t, 100, 110))

    clear(t, 0); w3(t, SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 18))
    vert(t, 40, 40, 0xffffff00, 1); vert(t, 40, 200, 0xffffff00, 0)
    vert(t, 280, 40, 0xffffff00, 0); vert(t, 280, 200, 0xffffff00, 0)
    line("setup-strip", "0x%04x" % pix(t, 250, 180))

    clear(t, 0x000000ff); w3(t, FBZCOLORPATH, 0)
    w3(t, ALPHAMODE, (1 << 4) | (1 << 8) | (5 << 12))
    w3(t, SSETUPMODE, 1 | (1 << 1) | (1 << 2) | (1 << 3))
    vert(t, 40, 40, 0x80ffffff, 1); vert(t, 40, 200, 0x80ffffff, 0); vert(t, 280, 120, 0x80ffffff, 0)
    w3(t, ALPHAMODE, 0); line("alpha-blend", "0x%04x" % pix(t, 100, 110))

    toff = W * H * 2 * 2
    print("# toff = 0x%x" % toff)
    t.ww(BAR1 + toff, 0xabcd)
    line("vram-roundtrip", "0x%04x" % t.rw(BAR1 + toff))
    t.ww(BAR1 + toff, 0xf800)
    line("tex-vram-readback", "0x%04x" % t.rw(BAR1 + toff))
    for fmt, nbytes, val, nm in [(10, 2, 0xf800, "tex-RGB565"), (0, 1, 0xe0, "tex-RGB332"),
                                 (11, 2, 0x8000 | (31 << 10), "tex-ARGB1555"),
                                 (12, 2, 0xf000 | (0xf << 8), "tex-ARGB4444")]:
        clear(t, 0)
        if nbytes == 1: t.cmd("writeb 0x%x 0x%x" % (BAR1 + toff, val))
        else: t.ww(BAR1 + toff, val)
        w3(t, TEXBASE, toff); w3(t, TLOD, 0); w3(t, TEXMODE, (fmt << 8) | TC)
        w3(t, FBZCOLORPATH, 1 | (1 << 27))
        w3(t, SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
        vert(t, 40, 40, 0xffffffff, 1, s=0.0, tt=0.0); vert(t, 40, 200, 0xffffffff, 0, s=0.0, tt=0.0)
        vert(t, 280, 120, 0xffffffff, 0, s=0.0, tt=0.0)
        w3(t, FBZCOLORPATH, 0); w3(t, TEXMODE, 0)
        line(nm, "0x%04x" % pix(t, 100, 110))

    clear(t, 0)
    for slot, texv in [(0, 0xf800), (1, 0x07e0), (256, 0x001f), (257, 0xffff)]:
        t.ww(BAR1 + toff + slot * 2, texv)
    w3(t, TEXBASE, toff); w3(t, TLOD, 0); w3(t, TEXMODE, (10 << 8) | (1 << 2) | TC)
    w3(t, FBZCOLORPATH, 1 | (1 << 27)); w3(t, SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
    vert(t, 40, 40, 0xffffffff, 1, s=1.0, tt=1.0); vert(t, 40, 200, 0xffffffff, 0, s=1.0, tt=1.0)
    vert(t, 280, 120, 0xffffffff, 0, s=1.0, tt=1.0)
    w3(t, FBZCOLORPATH, 0); w3(t, TEXMODE, 0)
    line("bilinear", "0x%04x" % pix(t, 100, 110))

    clear(t, 0); t.ww(BAR1 + toff, 0xffff)
    w3(t, TEXBASE, toff); w3(t, TLOD, 0); w3(t, TEXMODE, (10 << 8) | TC)
    w3(t, FBZCOLORPATH, 1 | (1 << 27) | (1 << 10) | (1 << 13))
    w3(t, SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
    vert(t, 40, 40, 0xffff0000, 1); vert(t, 40, 200, 0xffff0000, 0); vert(t, 280, 120, 0xffff0000, 0)
    w3(t, FBZCOLORPATH, 0); w3(t, TEXMODE, 0)
    line("combine-modulate", "0x%04x" % pix(t, 100, 110))

    clear(t, 0x000000ff); w3(t, CHROMAKEY, 0x00ff00ff)
    w3(t, FBZMODE, (7 << 5) | RGBW | (1 << 1)); w3(t, FBZCOLORPATH, 0)
    w3(t, SSETUPMODE, 1 | (1 << 2) | (1 << 3))
    vert(t, 40, 40, 0xffff00ff, 1); vert(t, 40, 200, 0xffff00ff, 0); vert(t, 280, 120, 0xffff00ff, 0)
    keyed = pix(t, 100, 110)
    vert(t, 40, 40, 0xff00ff00, 1); vert(t, 40, 200, 0xff00ff00, 0); vert(t, 280, 120, 0xff00ff00, 0)
    nonkey = pix(t, 100, 110)
    w3(t, FBZMODE, (7 << 5) | RGBW)
    line("chroma-key", "keyed=0x%04x non=0x%04x" % (keyed, nonkey))

    clear(t, 0); w3(t, FBZCOLORPATH, 0); w3(t, FOGCOLOR, 0x000000ff); w3(t, FOGMODE, 1 | (1 << 3))
    w3(t, SSETUPMODE, 1 | (1 << 1) | (1 << 2) | (1 << 3))
    vert(t, 40, 40, 0x80ff0000, 1); vert(t, 40, 200, 0x80ff0000, 0); vert(t, 280, 120, 0x80ff0000, 0)
    w3(t, FOGMODE, 0); line("fog-alpha", "0x%04x" % pix(t, 100, 110))

    clear(t, 0x00202020); w3(t, CLIPLR, (100 << 16) | 200); w3(t, CLIPBT, (80 << 16) | 160)
    w3(t, FBZMODE, (7 << 5) | RGBW | ENCLIP)
    w3(t, FBZCOLORPATH, 0); w3(t, SSETUPMODE, 1 | (1 << 2) | (1 << 3))
    vert(t, 0, 0, 0xffff00ff, 1); vert(t, 319, 0, 0xffff00ff, 0); vert(t, 160, 239, 0xffff00ff, 0)
    inp = pix(t, 150, 120); out = pix(t, 20, 20)
    w3(t, CLIPLR, W); w3(t, CLIPBT, H)
    line("clip-rect", "in=0x%04x out=0x%04x" % (inp, out))


if __name__ == "__main__":
    main()
