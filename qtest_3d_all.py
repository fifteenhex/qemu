#!/usr/bin/env python3
"""
Comprehensive feature test for the QEMU voodoo3 3D engine.

Exercises every implemented hardware feature and checks exact rendered
pixels (read back through the LFB), so the same sequences can be trusted
when validated against real Voodoo3 hardware:

  fastfill, direct integer triangleCMD, direct float FtriangleCMD,
  setup-unit fan and strip, all 8 depth-compare functions, depth write
  mask, alpha test, alpha blending, textures (RGB332/565/ARGB1555/4444,
  point-sampled perspective), constant colour, the clip rectangle, and
  vsync (swap-on-vblank pending count + vsync interrupt).
"""
import json, os, socket, struct, subprocess, sys, time

QEMU = "/workspace/src/qemu-voodoo3/build/qemu-system-x86_64"
DIR = "/workspace/src/voodoo3-test"
QTSOCK = DIR + "/qa.sock"
QMPSOCK = DIR + "/qaqmp.sock"
BAR0, BAR1, BAR2 = 0xf0000000, 0xf4000000, 0xc000
D3 = BAR0 + 0x200000
W, H = 320, 240

fails = []
def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (("  " + detail) if detail else ""))
    if not cond:
        fails.append(name)

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
            if r.startswith("FAIL"): raise RuntimeError(l + " -> " + r)
    def wl(s, a, v): s.cmd("writel 0x%x 0x%x" % (a, v & 0xffffffff))
    def rl(s, a): return int(s.cmd("readl 0x%x" % a), 16)
    def rw(s, a): return int(s.cmd("readw 0x%x" % a), 16)
    def ww(s, a, v): s.cmd("writew 0x%x 0x%x" % (a, v & 0xffff))
    def outl(s, p, v): s.cmd("outl 0x%x 0x%x" % (p, v))
    def inl(s, p): return int(s.cmd("inl 0x%x" % p), 16)
    def clock_step(s, ns): s.cmd("clock_step %d" % ns)

# 3D register offsets (relative to D3)
FBZMODE, FBZCOLORPATH, ALPHAMODE = 0x110, 0x104, 0x10c
RGBWRMASK = 1 << 9      # fbzMode: enable colour-buffer writes
ENCLIP = 1 << 0        # fbzMode: enable clip rectangle
TC = (1<<12)|(1<<18)|(1<<21)|(1<<27)   # textureMode: pass the texel
CLIPLR, CLIPBT = 0x118, 0x11c
COLBUF, COLSTRIDE, AUXBUF, AUXSTRIDE = 0x1ec, 0x1f0, 0x1f4, 0x1f8
ZACOLOR, C1, CHROMAKEY, CHROMARANGE = 0x130, 0x148, 0x134, 0x138
FOGMODE, FOGCOLOR = 0x108, 0x12c
FASTFILL, SWAPBUF, NOP, INTRCTRL = 0x124, 0x128, 0x120, 0x004
STATUS = 0x000
# direct int
VA, VB, VC = 0x008, 0x010, 0x018
STARTR, DPDX, DPDY, TRICMD = 0x020, 0x040, 0x060, 0x080
# direct float
FVA, FVB, FVC = 0x088, 0x090, 0x098
FSTARTR, FDPDX, FDPDY, FTRICMD = 0x0a0, 0x0c0, 0x0e0, 0x100
# setup unit
SSETUPMODE = 0x260
SVX, SVY, SARGB, SVZ, SWOOW, SSOW0, STOW0 = 0x264, 0x268, 0x26c, 0x280, 0x284, 0x28c, 0x290
SBEGIN, SDRAW = 0x2a4, 0x2a0
# texture
TEXMODE, TLOD, TEXBASE = 0x300, 0x304, 0x30c


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def pix(q, x, y, buf=0):
    return q.rw(BAR1 + buf + (y * W + x) * 2)

def near(v, r, g, b, tol=12):
    pr, pg, pb = ((v >> 11) & 0x1f) * 255 // 31, ((v >> 5) & 0x3f) * 255 // 63, (v & 0x1f) * 255 // 31
    return abs(pr - r) < tol + 8 and abs(pg - g) < tol + 8 and abs(pb - b) < tol + 8


def setup(q):
    slot = None
    for d in range(32):
        q.outl(0xcf8, 0x80000000 | (d << 11))
        if q.inl(0xcfc) == 0x0005121a: slot = d; break
    def cfg(o, v): q.outl(0xcf8, 0x80000000 | (slot << 11) | (o & 0xfc)); q.outl(0xcfc, v)
    cfg(0x10, BAR0); cfg(0x14, BAR1); cfg(0x18, BAR2 | 1); cfg(0x04, 3)
    # memory up (like BIOS)
    q.wl(BAR0 + 0x44, (44 << 8) | (2 << 2)); q.wl(BAR0 + 0x18, (1 << 26) | (1 << 27))
    q.wl(BAR0 + 0x1c, (1 << 30)); q.wl(BAR0 + 0x30, 1)
    # 16bpp desktop scanout of buffer 0
    q.wl(BAR0 + 0x40, (12 << 8) | (1 << 2) | 1); q.wl(BAR0 + 0x28, (1 << 2))
    q.wl(BAR0 + 0x98, W | (H << 12)); q.wl(BAR0 + 0xe8, W * 2)
    q.wl(BAR0 + 0xe4, 0); q.wl(BAR0 + 0x5c, 1 | (1 << 7) | (1 << 18))
    # 3D common state: render into buffer 0
    q.wl(D3 + COLBUF, 0); q.wl(D3 + COLSTRIDE, W * 2)
    q.wl(D3 + AUXBUF, W * H * 2); q.wl(D3 + AUXSTRIDE, W * 2)
    q.wl(D3 + CLIPLR, W); q.wl(D3 + CLIPBT, H)
    q.wl(D3 + ALPHAMODE, 0); q.wl(D3 + FBZCOLORPATH, 0)
    q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK)   # zfunc = ALWAYS, depth off


def clear(q, col565=0):
    q.wl(D3 + C1, 0); q.wl(D3 + ZACOLOR, 0xffff); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK)
    # C1 is ARGB; encode background as ARGB from a 565-ish value: just use black/blue
    q.wl(D3 + C1, col565)
    q.wl(D3 + FASTFILL, 1)

def svert(q, x, y, argb, z=1000.0, first=False, s=None, t=None):
    q.wl(D3 + SVX, f2i(float(x))); q.wl(D3 + SVY, f2i(float(y)))
    q.wl(D3 + SVZ, f2i(float(z))); q.wl(D3 + SWOOW, f2i(1.0))
    q.wl(D3 + SARGB, argb)
    if s is not None:
        q.wl(D3 + SSOW0, f2i(float(s))); q.wl(D3 + STOW0, f2i(float(t)))
    q.wl(D3 + (SBEGIN if first else SDRAW), 1)


def run():
    q = QT(QTSOCK)
    setup(q)

    # 1. fastfill
    q.wl(D3 + C1, 0x00204060); q.wl(D3 + FASTFILL, 1)
    check("fastfill", near(pix(q, 10, 10), 0x20, 0x40, 0x60), hex(pix(q, 10, 10)))

    # 1b. RGB write mask: a fill with RGBWRMASK clear must leave colour alone
    q.wl(D3 + C1, 0x00ff0000); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK)
    q.wl(D3 + FASTFILL, 1)                                  # red, mask on
    q.wl(D3 + C1, 0x000000ff); q.wl(D3 + FBZMODE, 7 << 5)   # blue, mask OFF
    q.wl(D3 + FASTFILL, 1)
    check("rgb write mask", near(pix(q, 10, 10), 255, 0, 0), hex(pix(q, 10, 10)))

    # 2. direct integer triangleCMD (flat green, gradients 0)
    q.wl(D3 + C1, 0); q.wl(D3 + FASTFILL, 1)
    q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK)
    q.wl(D3 + FBZCOLORPATH, 0)
    q.wl(D3 + VA, (30 << 4)); q.wl(D3 + VA + 4, (30 << 4))
    q.wl(D3 + VB, (300 << 4)); q.wl(D3 + VB + 4, (40 << 4))
    q.wl(D3 + VC, (60 << 4)); q.wl(D3 + VC + 4, (210 << 4))
    for i in range(8): q.wl(D3 + DPDX + i * 4, 0); q.wl(D3 + DPDY + i * 4, 0)
    q.wl(D3 + STARTR + 0, 0); q.wl(D3 + STARTR + 4, 220 << 12); q.wl(D3 + STARTR + 8, 0)  # g=220
    q.wl(D3 + STARTR + 12, 1000 << 12)  # z
    q.wl(D3 + TRICMD, 1)
    check("direct-int triangleCMD", near(pix(q, 90, 90), 0, 220, 0), hex(pix(q, 90, 90)))

    # 3. direct float FtriangleCMD (flat red)
    q.wl(D3 + C1, 0); q.wl(D3 + FASTFILL, 1)
    q.wl(D3 + FVA, f2i(30.0)); q.wl(D3 + FVA + 4, f2i(30.0))
    q.wl(D3 + FVB, f2i(300.0)); q.wl(D3 + FVB + 4, f2i(40.0))
    q.wl(D3 + FVC, f2i(60.0)); q.wl(D3 + FVC + 4, f2i(210.0))
    for i in range(8): q.wl(D3 + FDPDX + i * 4, 0); q.wl(D3 + FDPDY + i * 4, 0)
    q.wl(D3 + FSTARTR + 0, f2i(230.0)); q.wl(D3 + FSTARTR + 4, f2i(0.0))
    q.wl(D3 + FSTARTR + 8, f2i(0.0)); q.wl(D3 + FSTARTR + 12, f2i(1000.0))
    q.wl(D3 + FTRICMD, 1)
    check("direct-float FtriangleCMD", near(pix(q, 90, 90), 230, 0, 0), hex(pix(q, 90, 90)))

    # 4. setup-unit fan (gouraud), colours interpolate
    q.wl(D3 + C1, 0); q.wl(D3 + FASTFILL, 1)
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3))
    svert(q, 160, 30, 0xffff0000, first=True)
    svert(q, 40, 210, 0xff00ff00)
    svert(q, 280, 210, 0xff0000ff)
    c = pix(q, 160, 150)
    check("setup fan gouraud", not near(c, 0, 0, 0) and pix(q, 50, 205) != pix(q, 270, 205),
          hex(c))

    # 5. setup-unit strip (two triangles from 4 verts)
    q.wl(D3 + C1, 0); q.wl(D3 + FASTFILL, 1)
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 18))  # strip
    svert(q, 40, 40, 0xffffff00, first=True)
    svert(q, 40, 200, 0xffffff00)
    svert(q, 280, 40, 0xffffff00)
    svert(q, 280, 200, 0xffffff00)   # 2nd triangle
    check("setup strip", near(pix(q, 160, 120), 255, 255, 0) and
          near(pix(q, 250, 180), 255, 255, 0), hex(pix(q, 250, 180)))

    # 6. all 8 depth-compare functions
    def depth_case(func, srcz, dstz, expect_draw):
        q.wl(D3 + C1, 0); q.wl(D3 + ZACOLOR, dstz & 0xffff); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK)
        q.wl(D3 + FBZMODE, (1 << 4) | (1 << 10) | (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)  # clear depth=dstz
        q.wl(D3 + FBZMODE, (1 << 4) | (1 << 10) | (func << 5) | RGBWRMASK)  # depth on, this func
        q.wl(D3 + FBZCOLORPATH, 0)
        q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3))
        svert(q, 40, 40, 0xff00ffff, z=float(srcz), first=True)
        svert(q, 40, 200, 0xff00ffff, z=float(srcz))
        svert(q, 280, 120, 0xff00ffff, z=float(srcz))
        drew = near(pix(q, 100, 110), 0, 255, 255)
        return drew == expect_draw
    # func encoding: bit0=LT,bit1=EQ,bit2=GT (value in [5:7])
    cases = [
        (0, 100, 200, False),  # NEVER
        (1, 100, 200, True),   # LESS: 100<200
        (1, 200, 100, False),
        (2, 150, 150, True),   # EQUAL
        (2, 150, 151, False),
        (4, 200, 100, True),   # GREATER
        (3, 100, 100, True),   # LEQUAL (LT|EQ) equal
        (6, 200, 100, True),   # GEQUAL (EQ|GT)
        (5, 100, 200, True),   # NOTEQUAL (LT|GT)
        (5, 150, 150, False),
        (7, 200, 100, True),   # ALWAYS
    ]
    ok = all(depth_case(*c) for c in cases)
    check("depth compare (8 funcs)", ok)

    # 7. alpha test: ref=128, GREATER -> alpha 200 passes, 60 fails
    def alpha_test_case(alpha, expect):
        q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
        q.wl(D3 + FBZCOLORPATH, 0); q.wl(D3 + ALPHAMODE,
             1 | (4 << 1) | (128 << 24))   # entest, func=GT(bit2->value4), ref=128
        q.wl(D3 + SSETUPMODE, 1 | (1 << 1) | (1 << 2) | (1 << 3))
        argb = (alpha << 24) | 0x00ff00
        svert(q, 40, 40, argb, first=True); svert(q, 40, 200, argb)
        svert(q, 280, 120, argb)
        drew = near(pix(q, 100, 110), 0, 255, 0)
        q.wl(D3 + ALPHAMODE, 0)
        return drew == expect
    check("alpha test", alpha_test_case(200, True) and alpha_test_case(60, False))

    # 8. alpha blend: 50% white over blue -> light blue
    q.wl(D3 + C1, 0x000000ff); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)  # blue bg
    q.wl(D3 + FBZCOLORPATH, 0)
    q.wl(D3 + ALPHAMODE, (1 << 4) | (1 << 8) | (5 << 12))  # blend: src*srcA + dst*(1-srcA)
    q.wl(D3 + SSETUPMODE, 1 | (1 << 1) | (1 << 2) | (1 << 3))
    svert(q, 40, 40, 0x80ffffff, first=True); svert(q, 40, 200, 0x80ffffff)
    svert(q, 280, 120, 0x80ffffff)
    b = pix(q, 100, 110)
    q.wl(D3 + ALPHAMODE, 0)
    # expect ~ (128,128,255): white*0.5 + blue*0.5
    check("alpha blend", near(b, 128, 128, 255, 20), hex(b))

    # 9. textures - upload a distinct texel per format and sample it
    def tex_case(fmt, texel_bytes, expect_rgb):
        toff = W * H * 2 + W * H * 2   # after colour+depth
        for k, byteval in enumerate(texel_bytes):
            q.cmd("writeb 0x%x 0x%x" % (BAR1 + toff + k, byteval))
        q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
        q.wl(D3 + TEXBASE, toff); q.wl(D3 + TLOD, 0)   # 256x256: texel(0,0) at base
        q.wl(D3 + TEXMODE, (fmt << 8) | TC)             # point sample + pass texel
        q.wl(D3 + FBZCOLORPATH, 1 | (1 << 27))  # rgbselect=texture + texture enable
        q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
        svert(q, 40, 40, 0xffffffff, first=True, s=0.0, t=0.0)
        svert(q, 40, 200, 0xffffffff, s=0.0, t=0.0)
        svert(q, 280, 120, 0xffffffff, s=0.0, t=0.0)
        v = pix(q, 100, 110)
        q.wl(D3 + FBZCOLORPATH, 0); q.wl(D3 + TEXMODE, 0)
        return near(v, *expect_rgb, tol=24), hex(v)
    r565 = struct.pack("<H", rgb565(255, 0, 0))
    r332 = bytes([0b11100000])              # red in 3:3:2
    r1555 = struct.pack("<H", 0x8000 | (31 << 10))  # a=1,red
    r4444 = struct.pack("<H", 0xf000 | (0xf << 8))  # a=f,red
    ok565, d565 = tex_case(10, r565, (255, 0, 0))
    ok332, d332 = tex_case(0, r332, (255, 0, 0))
    ok1555, d1 = tex_case(11, r1555, (255, 0, 0))
    ok4444, d4 = tex_case(12, r4444, (255, 0, 0))
    check("texture RGB565", ok565, d565)
    check("texture RGB332", ok332, d332)
    check("texture ARGB1555", ok1555, d1)
    check("texture ARGB4444", ok4444, d4)

    # 9e. bilinear filtering: 256x256 texture, 4 distinct texels at (0,0),
    # (1,0),(0,1),(1,1); sample the centre -> equal 4-texel average
    q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    toff = W * H * 2 * 3
    for slot, texv in [(0, rgb565(255, 0, 0)), (1, rgb565(0, 255, 0)),
                       (256, rgb565(0, 0, 255)), (257, rgb565(255, 255, 255))]:
        q.ww(BAR1 + toff + slot * 2, texv)
    q.wl(D3 + TEXBASE, toff); q.wl(D3 + TLOD, 0)           # 256x256
    q.wl(D3 + TEXMODE, (10 << 8) | (1 << 2) | TC)          # RGB565 + magfilter + pass texel
    q.wl(D3 + FBZCOLORPATH, 1 | (1 << 27))
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
    svert(q, 40, 40, 0xffffffff, first=True, s=1.0, t=1.0)
    svert(q, 40, 200, 0xffffffff, s=1.0, t=1.0)
    svert(q, 280, 120, 0xffffffff, s=1.0, t=1.0)
    v = pix(q, 100, 110)
    q.wl(D3 + FBZCOLORPATH, 0); q.wl(D3 + TEXMODE, 0)
    check("bilinear filter", near(v, 127, 127, 127, tol=40), hex(v))

    # 9f. tiled texture addressing: 256x256 (TLOD 0), tiled base (bit0) with a
    # 4-tile stride. Hardware (tdfx_texmap) puts texel (64,0) at byte 4096 and
    # (0,32) at byte 16384 within the surface, vs 128 / 8192 if it were linear.
    # Plant the "tiled" colour at the tiled offset and a decoy at the linear
    # offset; a correct tiled fetch returns the tiled colour.
    def tiled_case(u, v_, tiled_byte, expect_rgb):
        toff = W * H * 2 * 4
        q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
        q.ww(BAR1 + toff + tiled_byte, rgb565(*expect_rgb))     # tiled location
        q.ww(BAR1 + toff + (v_ * 256 + u) * 2, rgb565(0, 0, 255))  # linear decoy
        q.wl(D3 + TEXBASE, toff | 1 | (4 << 25))   # tiled, stride 4 tiles
        q.wl(D3 + TLOD, 0)                          # 256x256
        q.wl(D3 + TEXMODE, (10 << 8) | TC)
        q.wl(D3 + FBZCOLORPATH, 1 | (1 << 27))
        q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
        svert(q, 40, 40, 0xffffffff, first=True, s=float(u), t=float(v_))
        svert(q, 40, 200, 0xffffffff, s=float(u), t=float(v_))
        svert(q, 280, 120, 0xffffffff, s=float(u), t=float(v_))
        val = pix(q, 100, 110)
        q.wl(D3 + FBZCOLORPATH, 0); q.wl(D3 + TEXMODE, 0)
        return near(val, *expect_rgb, tol=24), hex(val)
    okt1, dt1 = tiled_case(64, 0, 4096, (255, 0, 0))
    okt2, dt2 = tiled_case(64, 32, 20480, (0, 255, 0))
    check("tiled texture (64,0)", okt1, dt1)
    check("tiled texture (64,32)", okt2, dt2)

    # 9g. sub-256 LOD base offset: texBaseAddr is the virtual 256x256 corner,
    # so a 1x1 texture's texel(0,0) sits at base + lodOffset(8). Plant red
    # there and a blue decoy at base; a correct fetch returns red.
    def lodoff(L, bpt=2):
        return (sum((256 >> i) ** 2 for i in range(L)) * bpt) & ~0xf
    toff = W * H * 2 * 5
    q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    q.ww(BAR1 + toff + lodoff(8), rgb565(255, 0, 0))   # texel at base + offset
    q.ww(BAR1 + toff, rgb565(0, 0, 255))               # decoy at base
    q.wl(D3 + TEXBASE, toff); q.wl(D3 + TLOD, 8 << 2)  # 1x1 (lodmin=8)
    q.wl(D3 + TEXMODE, (10 << 8) | TC)
    q.wl(D3 + FBZCOLORPATH, 1 | (1 << 27))
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
    svert(q, 40, 40, 0xffffffff, first=True, s=0.0, t=0.0)
    svert(q, 40, 200, 0xffffffff, s=0.0, t=0.0)
    svert(q, 280, 120, 0xffffffff, s=0.0, t=0.0)
    v = pix(q, 100, 110)
    q.wl(D3 + FBZCOLORPATH, 0); q.wl(D3 + TEXMODE, 0)
    check("LOD offset (1x1)", near(v, 255, 0, 0, tol=24), hex(v))

    # 9h. fbzColorPath combine unit. Ctmu = texel (REPLACE); the fbz stage
    # then does out = (Cother - 0) * factor + 0. MODULATE = rgbselect texture,
    # mselect MCLOCAL, reverse_blend, local iterated => texel * iterated.
    def combine_case(cpath, texel, iterated_argb, expect_rgb):
        toff = W * H * 2 * 6
        q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
        q.ww(BAR1 + toff, texel)
        q.wl(D3 + TEXBASE, toff); q.wl(D3 + TLOD, 0)
        q.wl(D3 + TEXMODE, (10 << 8) | TC)          # REPLACE -> Ctmu = texel
        q.wl(D3 + FBZCOLORPATH, cpath)
        q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
        svert(q, 40, 40, iterated_argb, first=True, s=0.0, t=0.0)
        svert(q, 40, 200, iterated_argb, s=0.0, t=0.0)
        svert(q, 280, 120, iterated_argb, s=0.0, t=0.0)
        val = pix(q, 100, 110)
        q.wl(D3 + FBZCOLORPATH, 0); q.wl(D3 + TEXMODE, 0)
        return near(val, *expect_rgb, tol=24), hex(val)
    TEXEN = 1 << 27
    MODULATE = 1 | TEXEN | (1 << 10) | (1 << 13)   # rgbsel=tex, MCLOCAL, reverse
    REPLACE_TEX = 1 | TEXEN                         # rgbsel=tex, cc passes texel
    # white texel * red iterated -> red (a plain REPLACE would give white)
    okm, dm = combine_case(MODULATE, rgb565(255, 255, 255), 0xffff0000, (255, 0, 0))
    check("combine modulate white*red", okm, dm)
    # white texel * 50% grey iterated -> grey (scaling works)
    okg, dg = combine_case(MODULATE, rgb565(255, 255, 255), 0xff808080, (128, 128, 128))
    check("combine modulate white*grey", okg, dg)
    # rgbselect=texture, cc pass-through -> texel unchanged (green)
    okr, dr = combine_case(REPLACE_TEX, rgb565(0, 255, 0), 0xffff0000, (0, 255, 0))
    check("combine replace texel", okr, dr)

    # 9i. P8 palette texture: download a CLUT entry via nccTable0[4..11]
    # (bit31 | index<<24 | rgb), then index it from an 8bpp texel.
    def nccpal(idx, rgb):
        q.wl(D3 + 0x324 + 16 + (idx & 7) * 4,
             0x80000000 | ((idx & 0xFE) << 23) | (rgb & 0xFFFFFF))
    toff = W * H * 2 * 7
    q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    nccpal(5, 0xff0000)                              # palette[5] = red
    q.cmd("writeb 0x%x 0x5" % (BAR1 + toff))        # texel(0,0) = index 5
    q.wl(D3 + TEXBASE, toff); q.wl(D3 + TLOD, 0)
    q.wl(D3 + TEXMODE, (5 << 8) | TC)               # P8, REPLACE -> palette[5]
    q.wl(D3 + FBZCOLORPATH, 1 | (1 << 27))
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
    svert(q, 40, 40, 0xffffffff, first=True, s=0.0, t=0.0)
    svert(q, 40, 200, 0xffffffff, s=0.0, t=0.0)
    svert(q, 280, 120, 0xffffffff, s=0.0, t=0.0)
    v = pix(q, 100, 110)
    q.wl(D3 + FBZCOLORPATH, 0); q.wl(D3 + TEXMODE, 0)
    check("P8 palette texture", near(v, 255, 0, 0, tol=24), hex(v))

    # 9j. colour-source 2D host-to-screen blt: stream two 16bpp pixels into the
    # launch area and check it lands. (Single 1x1 pixel: the multi-pixel blt
    # geometry - where the 2nd pixel lands - is unresolved on hw, so validate
    # just the core colour write.)
    D2 = BAR0 + 0x100000
    def w2(off, val): q.wl(D2 + off, val)
    q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    w2(0x08, 0); w2(0x0c, 0x0fff0fff)         # clip0 min/max
    w2(0x10, 0); w2(0x14, (W * 2) | (3 << 16))  # dstbase, dstformat stride|16bpp
    w2(0x54, 3 << 16)                          # srcformat: 16bpp colour source
    w2(0x68, 1 | (1 << 16))                    # dstsize 1x1
    w2(0x6c, 10 | (20 << 16))                  # dstxy (10,20)
    w2(0x70, 0x03 | (0xcc << 24))              # command: H2S blt, ROP=SRCCOPY
    w2(0x80, rgb565(255, 0, 0))               # one pixel
    hb0 = pix(q, 10, 20)
    check("host blt colour", near(hb0, 255, 0, 0, tol=24), hex(hb0))

    # 9k. mipmap LOD selection: 2-level texture (L0 red, L1 green) mapped so
    # the texture is minified 2x (256 texels over 128 px -> 2 texels/pixel ->
    # LOD 1), so the sampled texel comes from L1 (green), not L0 (red).
    toff = W * H * 2 * 8
    l1off = (256 * 256 * 2) & ~0xf                # lodOffset(1) for 16bpp
    q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    for yy in range(4):
        for xx in range(4):
            q.ww(BAR1 + toff + (yy * 256 + xx) * 2, rgb565(255, 0, 0))          # L0
            q.ww(BAR1 + toff + l1off + (yy * 128 + xx) * 2, rgb565(0, 255, 0))  # L1
    q.wl(D3 + TEXBASE, toff); q.wl(D3 + TLOD, 0x100)   # lodmin=0, lodmax=1
    q.wl(D3 + TEXMODE, (10 << 8) | TC)
    q.wl(D3 + FBZCOLORPATH, 1 | (1 << 27))
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
    svert(q, 10, 10, 0xffffffff, first=True, s=0.0, t=0.0)
    svert(q, 138, 10, 0xffffffff, s=256.0, t=0.0)
    svert(q, 10, 138, 0xffffffff, s=0.0, t=256.0)
    mv = pix(q, 12, 12)
    q.wl(D3 + FBZCOLORPATH, 0); q.wl(D3 + TEXMODE, 0)
    check("mipmap LOD minified->L1", near(mv, 0, 255, 0, tol=48), hex(mv))

    # 9l. NCC (YIQ) texture: build a table so texel 0 -> green (Y0=0, I0=(0,255,0),
    # Q0=0), then sample a YIQ (fmt 1) texel of index 0.
    toff = W * H * 2 * 9
    q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    NCC0 = 0x324
    q.wl(D3 + NCC0 + 0, 0)                         # Y[0..3] = 0
    q.wl(D3 + NCC0 + 16, (0 << 18) | (255 << 9) | 0)  # I[0] = (r0,g255,b0)
    q.wl(D3 + NCC0 + 32, 0)                        # Q[0] = 0
    q.cmd("writeb 0x%x 0x0" % (BAR1 + toff))       # texel(0,0) = index 0
    q.wl(D3 + TEXBASE, toff); q.wl(D3 + TLOD, 0)
    q.wl(D3 + TEXMODE, (1 << 8) | TC)              # fmt 1 = YIQ
    q.wl(D3 + FBZCOLORPATH, 1 | (1 << 27))
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
    svert(q, 40, 40, 0xffffffff, first=True, s=0.0, t=0.0)
    svert(q, 40, 200, 0xffffffff, s=0.0, t=0.0)
    svert(q, 280, 120, 0xffffffff, s=0.0, t=0.0)
    nv = pix(q, 100, 110)
    q.wl(D3 + FBZCOLORPATH, 0); q.wl(D3 + TEXMODE, 0)
    check("NCC YIQ texture", near(nv, 0, 255, 0, tol=24), hex(nv))

    # 9m. dual-TMU multitexture: TMU1 REPLACE (Ctmu1 = texel1), TMU0 modulates
    # its texel by the TMU1 colour (MULT). white(TMU0) * red(TMU1) -> red,
    # proving the second TMU's colour feeds TMU0's combine.
    toff0, toff1 = W * H * 2 * 10, W * H * 2 * 11
    TMU1 = 0x800
    TC_MULT = (1 << 14) | (1 << 17) | (1 << 23) | (1 << 26)   # MCLOCAL|REVERSE
    q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    q.ww(BAR1 + toff0, 0xffff)          # TMU0 texel = white
    q.ww(BAR1 + toff1, 0xf800)          # TMU1 texel = red
    q.wl(D3 + TEXBASE, toff0); q.wl(D3 + TLOD, 0)
    q.wl(D3 + TEXMODE, (10 << 8) | TC_MULT)          # TMU0: texel0 * other
    q.wl(D3 + TMU1 + 0x30c, toff1); q.wl(D3 + TMU1 + 0x304, 0)   # TMU1 base,lod
    q.wl(D3 + TMU1 + 0x300, (10 << 8) | TC)          # TMU1: REPLACE -> texel1
    q.wl(D3 + 0x298, f2i(0.0)); q.wl(D3 + 0x29c, f2i(0.0))   # SSOW1/STOW1 = 0
    q.wl(D3 + FBZCOLORPATH, 1 | (1 << 27))
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
    svert(q, 40, 40, 0xffffffff, first=True, s=0.0, t=0.0)
    svert(q, 40, 200, 0xffffffff, s=0.0, t=0.0)
    svert(q, 280, 120, 0xffffffff, s=0.0, t=0.0)
    dv = pix(q, 100, 110)
    q.wl(D3 + FBZCOLORPATH, 0); q.wl(D3 + TEXMODE, 0); q.wl(D3 + TMU1 + 0x300, 0)
    check("dual-TMU multitexture", near(dv, 255, 0, 0, tol=24), hex(dv))

    # 9n. tiled sub-256 mip: sample level 1 (128) of a tiled texture (stride 6
    # tiles); texel(0,0) sits at base + tiled_lod_offset(1) via Glide's packing.
    def tiled_lodoff(lod, bpt, st):
        contrib = [(0, 2), (0, 4), (0, 8), (0, 16), (32, 0), (64, 0), (0, 128), (256, 0)]
        g = 0 if lod >= 8 else 8 - lod
        tx = sum(contrib[i][0] for i in range(g, 8))
        ty = sum(contrib[i][1] for i in range(g, 8))
        sb = st * 128
        bo = tx * bpt + ty * sb
        offy, offx = bo // sb, bo % sb
        a = ((offy >> 5) * st + (offx >> 7)) << 12
        if offx & 127: a += (1 << 12) - ((1 << 7) - (offx & 127))
        if offy & 31: a += (st << 12) - (((1 << 5) - (offy & 31)) << 7)
        return a
    toff = W * H * 2 * 12
    off1 = tiled_lodoff(1, 2, 6)
    q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    q.ww(BAR1 + toff + off1, rgb565(255, 0, 0))    # level-1 texel(0,0) = red
    q.wl(D3 + TEXBASE, toff | 1 | (6 << 25)); q.wl(D3 + TLOD, 1 << 2)
    q.wl(D3 + TEXMODE, (10 << 8) | TC)
    q.wl(D3 + FBZCOLORPATH, 1 | (1 << 27))
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3) | (1 << 5))
    svert(q, 40, 40, 0xffffffff, first=True, s=0.0, t=0.0)
    svert(q, 40, 200, 0xffffffff, s=0.0, t=0.0)
    svert(q, 280, 120, 0xffffffff, s=0.0, t=0.0)
    tv = pix(q, 100, 110)
    q.wl(D3 + FBZCOLORPATH, 0); q.wl(D3 + TEXMODE, 0)
    check("tiled sub-256 LOD", near(tv, 255, 0, 0, tol=24), hex(tv))

    # 9o. dithering (fbzMode ENDITHER): a flat R=4 (below one 565 step) is
    # ordered-dithered so some pixels round to r5=0 and others to r5=1
    # depending on the 4x4 matrix. Pixel (100,110) dith=3 -> 0x0000, pixel
    # (100,101) dith=12 -> 0x0800.
    q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK | (1 << 8))   # ENDITHER
    q.wl(D3 + FBZCOLORPATH, 0)
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3))
    svert(q, 40, 40, 0xff040000, first=True)
    svert(q, 40, 200, 0xff040000)
    svert(q, 280, 120, 0xff040000)
    di_lo, di_hi = pix(q, 100, 110), pix(q, 100, 101)
    q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK)
    check("dither 4x4 low", di_lo == 0x0000, hex(di_lo))
    check("dither 4x4 high", di_hi == 0x0800, hex(di_hi))

    # 9q. W-buffer: depth = float(w) encoding (hw: w=8 -> 0x3000)
    def dpix(x, y): return q.rw(BAR1 + W * H * 2 + (y * W + x) * 2)
    def vtxw(x, y, w, first):
        q.wl(D3 + SVX, f2i(float(x))); q.wl(D3 + SVY, f2i(float(y)))
        q.wl(D3 + SVZ, f2i(1000.0)); q.wl(D3 + SWOOW, f2i(w))
        q.wl(D3 + SARGB, 0xffffffff)
        q.wl(D3 + SSOW0, f2i(0.0)); q.wl(D3 + STOW0, f2i(0.0))
        q.wl(D3 + (SBEGIN if first else SDRAW), 1)
    q.wl(D3 + AUXBUF, W * H * 2); q.wl(D3 + AUXSTRIDE, W * 2)
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3))
    q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK | (1 << 4) | (1 << 10) | (1 << 3))
    vtxw(40, 40, 0.125, True); vtxw(40, 200, 0.125, False); vtxw(280, 120, 0.125, False)
    wb = dpix(100, 110)
    check("w-buffer w=8", wb == 0x3000, hex(wb))

    # 9r. depth bias: z=1000 + zaColor(+100) -> 0x044c
    q.wl(D3 + ZACOLOR, 0x0064)
    q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK | (1 << 4) | (1 << 10) | (1 << 16))
    svert(q, 40, 40, 0xffffffff, first=True); svert(q, 40, 200, 0xffffffff)
    svert(q, 280, 120, 0xffffffff)
    zb = dpix(100, 110)
    q.wl(D3 + ZACOLOR, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK)
    check("depth-bias +100", zb == 0x044c, hex(zb))

    # 9s. stipple 4x4 (0xaaaaaaaa): drawn if bit(31-((y&3)*4+(x&3))) set
    STIPPLE = 0x140
    q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    q.wl(D3 + STIPPLE, 0xaaaaaaaa)
    q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK | (1 << 2) | (1 << 12))
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3))
    svert(q, 40, 40, 0xffffffff, first=True); svert(q, 40, 200, 0xffffffff)
    svert(q, 280, 120, 0xffffffff)
    st_on, st_off = pix(q, 100, 110), pix(q, 101, 110)
    q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + STIPPLE, 0xffffffff)
    check("stipple 4x4 on", st_on == 0xffff, hex(st_on))
    check("stipple 4x4 off", st_off == 0x0000, hex(st_off))

    # 9f. chroma-key: matching colour discarded, non-matching drawn
    q.wl(D3 + C1, 0x000000ff)
    q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)   # blue bg
    q.wl(D3 + CHROMAKEY, 0x00ff00ff)                                   # key = magenta
    q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK | (1 << 1))                # enable chroma-key
    q.wl(D3 + FBZCOLORPATH, 0)
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3))
    svert(q, 40, 40, 0xffff00ff, first=True); svert(q, 40, 200, 0xffff00ff)
    svert(q, 280, 120, 0xffff00ff)
    keyed = pix(q, 100, 110)                                           # discarded -> blue
    svert(q, 40, 40, 0xff00ff00, first=True); svert(q, 40, 200, 0xff00ff00)
    svert(q, 280, 120, 0xff00ff00)
    nonkey = pix(q, 100, 110)                                          # green drawn
    q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK)
    check("chroma-key", near(keyed, 0, 0, 255) and near(nonkey, 0, 255, 0),
          "keyed=%s non=%s" % (hex(keyed), hex(nonkey)))

    # 9g. fog (iterated alpha): Cout = Afog*fogColor + (1-Afog)*Cin
    q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    q.wl(D3 + FBZCOLORPATH, 0)
    q.wl(D3 + FOGCOLOR, 0x000000ff)                 # fog blue
    q.wl(D3 + FOGMODE, 1 | (1 << 3))                # enable + fog_alpha
    q.wl(D3 + SSETUPMODE, 1 | (1 << 1) | (1 << 2) | (1 << 3))
    svert(q, 40, 40, 0x80ff0000, first=True); svert(q, 40, 200, 0x80ff0000)
    svert(q, 280, 120, 0x80ff0000)                  # red, alpha=128
    v = pix(q, 100, 110)
    q.wl(D3 + FOGMODE, 0)
    check("fog (alpha)", near(v, 127, 0, 127, tol=30), hex(v))

    # 10. constant colour (fbzColorPath rgbselect=2 -> color1)
    q.wl(D3 + C1, 0); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    q.wl(D3 + C1, 0x00ff8800)   # ARGB const
    q.wl(D3 + FBZCOLORPATH, 2)
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3))
    svert(q, 40, 40, 0xffffffff, first=True); svert(q, 40, 200, 0xffffffff)
    svert(q, 280, 120, 0xffffffff)
    v = pix(q, 100, 110); q.wl(D3 + FBZCOLORPATH, 0)
    check("constant colour (color1)", near(v, 0xff, 0x88, 0x00, 20), hex(v))

    # 11. clip rectangle: restrict to a box, draw full-screen tri, outside stays bg
    q.wl(D3 + C1, 0x00202020); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    q.wl(D3 + CLIPLR, (100 << 16) | 200); q.wl(D3 + CLIPBT, (80 << 16) | 160)
    q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK | ENCLIP)
    q.wl(D3 + FBZCOLORPATH, 0)
    q.wl(D3 + SSETUPMODE, 1 | (1 << 2) | (1 << 3))
    svert(q, 0, 0, 0xffff00ff, first=True); svert(q, 319, 0, 0xffff00ff)
    svert(q, 160, 239, 0xffff00ff)
    inside = near(pix(q, 150, 120), 255, 0, 255)
    outside = near(pix(q, 20, 20), 0x20, 0x20, 0x20)
    q.wl(D3 + CLIPLR, W); q.wl(D3 + CLIPBT, H)
    check("clip rectangle", inside and outside,
          "in=%s out=%s" % (hex(pix(q, 150, 120)), hex(pix(q, 20, 20))))

    # 12. vsync: swap-on-vblank pending count + vsync interrupt
    q.wl(D3 + INTRCTRL, 1)                 # enable vsync IRQ
    q.wl(D3 + COLBUF, W * H * 2 * 0)       # front = 0 (already)
    # render into a back buffer then queue a wait-on-vsync swap
    backoff = 0x300000
    q.wl(D3 + COLBUF, backoff); q.wl(D3 + COLSTRIDE, W * 2)
    q.wl(D3 + C1, 0x0000ff00); q.wl(D3 + FBZMODE, (7 << 5) | RGBWRMASK); q.wl(D3 + FASTFILL, 1)
    q.wl(D3 + SWAPBUF, 1)                  # wait-on-vsync swap
    st_before = q.rl(D3 + STATUS)
    pend = (st_before >> 28) & 7
    q.clock_step(20 * 1000 * 1000)         # advance ~1 vblank (60Hz)
    st_after = q.rl(D3 + STATUS)
    pciint = (st_after >> 31) & 1
    pend_after = (st_after >> 28) & 7
    deskstart = q.rl(BAR0 + 0xe4)
    check("vsync swap pending", pend == 1, "before=%d" % pend)
    check("vsync interrupt raised", pciint == 1, hex(st_after))
    check("vsync swap consumed+presented", pend_after == 0 and deskstart == backoff,
          "pend=%d desk=0x%x" % (pend_after, deskstart))
    q.wl(D3 + INTRCTRL, 2)                 # ack/clear
    check("vsync interrupt cleared", ((q.rl(D3 + STATUS) >> 31) & 1) == 0)

    print("----")
    if fails:
        print("FAILURES:", fails); sys.exit(1)
    print("all feature checks passed")


def main():
    for p in (QTSOCK, QMPSOCK):
        if os.path.exists(p): os.unlink(p)
    err = open(DIR + "/qa.err", "wb")
    qemu = subprocess.Popen([QEMU, "-M", "pc,accel=qtest", "-display", "none",
        "-qtest", "unix:" + QTSOCK + ",server=on,wait=off",
        "-qmp", "unix:" + QMPSOCK + ",server=on,wait=off",
        "-vga", "none", "-device", "voodoo3"], stderr=err)
    try: run()
    finally: qemu.terminate()

main()
