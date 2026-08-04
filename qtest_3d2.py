#!/usr/bin/env python3
"""Validate depth ordering and texturing on the voodoo3 3D engine."""
import json, os, socket, struct, subprocess, time
import qtest_3d as base

DIR = base.DIR
def f2i(f): return struct.unpack("<I", struct.pack("<f", f))[0]

def main():
    for p in (base.QTSOCK, base.QMPSOCK):
        if os.path.exists(p): os.unlink(p)
    err = open(DIR+"/q3b.err","wb")
    qemu = subprocess.Popen([base.QEMU,"-M","pc,accel=qtest","-display","none",
        "-qtest","unix:"+base.QTSOCK+",server=on,wait=off",
        "-qmp","unix:"+base.QMPSOCK+",server=on,wait=off",
        "-vga","none","-device","voodoo3"], stderr=err)
    try: run()
    finally: qemu.terminate()

def setup(q):
    slot=None
    for d in range(32):
        q.outl(0xcf8,0x80000000|(d<<11))
        if q.inl(0xcfc)==0x0005121a: slot=d;break
    base.cfg(q,slot,0x10,base.BAR0); base.cfg(q,slot,0x14,base.BAR1)
    base.cfg(q,slot,0x18,base.BAR2|1); base.cfg(q,slot,0x04,3)
    B=base.BAR0
    q.wl(B+base.PLL1,(44<<8)|(2<<2)); q.wl(B+base.DRAMINIT0,(1<<26)|(1<<27))
    q.wl(B+base.DRAMINIT1,(1<<30)); q.wl(B+base.DRAMCOMMAND,1)
    W,H=640,480
    q.wl(B+base.PLL0,(12<<8)|(1<<2)|1); q.wl(B+base.VGAINIT0,(1<<2))
    q.wl(B+base.VIDSCREENSIZE,W|(H<<12)); q.wl(B+base.VIDDESKSTRIDE,W*2)
    q.wl(B+base.VIDDESKSTART,0); q.wl(B+base.VIDPROCCFG,1|(1<<7)|(1<<18))
    D3=base.D3
    q.wl(D3+base.COLBUF,0); q.wl(D3+base.COLSTRIDE,W*2)
    q.wl(D3+base.AUXBUF,W*H*2); q.wl(D3+base.AUXSTRIDE,W*2)
    q.wl(D3+base.CLIPLR,W); q.wl(D3+base.CLIPBT,H)
    q.wl(D3+base.ALPHAMODE,0)
    return W,H

def px(m,x,y,W):
    m.cmd("screendump",filename=DIR+"/t2.ppm")
    d=open(DIR+"/t2.ppm","rb").read().split(b"\n",3); p=d[3]
    o=(y*W+x)*3; return (p[o],p[o+1],p[o+2])

def tri_setup(q,verts,mode=1|(1<<2)|(1<<3)):
    D3=base.D3; q.wl(D3+base.SSETUPMODE,mode)
    for i,v in enumerate(verts):
        q.wl(D3+base.SVX,f2i(float(v[0]))); q.wl(D3+base.SVY,f2i(float(v[1])))
        q.wl(D3+base.SVZ,f2i(float(v[2]))); q.wl(D3+base.SWOOW,f2i(v[3]))
        q.wl(D3+base.SARGB,v[4])
        if len(v)>5:
            q.wl(D3+0x28c,f2i(v[5])); q.wl(D3+0x290,f2i(v[6]))  # sSOW0,sTOW0
        q.wl(D3+(base.SBEGIN if i==0 else base.SDRAW),1)

def run():
    q=base.QT(base.QTSOCK); m=base.QMPc(base.QMPSOCK)
    W,H=setup(q); D3=base.D3
    # ---- depth test: far red, then near blue, zfunc=LESS ----
    q.wl(D3+base.FBZMODE,(1<<5)|(1<<4)|(1<<10))   # zfunc=LESS, depth on+write
    q.wl(D3+base.FBZCOLORPATH,0)
    q.wl(D3+base.C1,0x000000); q.wl(D3+base.ZACOLOR,0xffff); q.wl(D3+base.FASTFILL,1)
    # far red triangle (z=5000) covering centre
    tri_setup(q,[(100,100,5000,1.0,0xffff0000),(540,100,5000,1.0,0xffff0000),
                 (320,420,5000,1.0,0xffff0000)])
    # near blue triangle (z=1000) overlapping centre
    tri_setup(q,[(120,120,1000,1.0,0xff0000ff),(520,120,1000,1.0,0xff0000ff),
                 (320,400,1000,1.0,0xff0000ff)])
    q.wl(D3+base.SWAPBUF,1)
    c=px(m,320,250,W)
    print("depth centre pixel", c, "-> ", "PASS (near blue wins)" if c[2]>150 and c[0]<80 else "FAIL")
    # now reverse z: near triangle FARTHER should NOT overwrite
    q.wl(D3+base.C1,0x000000); q.wl(D3+base.FASTFILL,1)
    tri_setup(q,[(100,100,1000,1.0,0xffff0000),(540,100,1000,1.0,0xffff0000),
                 (320,420,1000,1.0,0xffff0000)])  # red near
    tri_setup(q,[(120,120,5000,1.0,0xff0000ff),(520,120,5000,1.0,0xff0000ff),
                 (320,400,5000,1.0,0xff0000ff)])  # blue far - should be hidden
    q.wl(D3+base.SWAPBUF,1)
    c=px(m,320,250,W)
    print("depth-occlude centre", c, "-> ", "PASS (red stays)" if c[0]>150 and c[2]<80 else "FAIL")

    # ---- texture test: 2x2 checker (red/green/blue/white) 565 ----
    texoff = W*H*2 + W*H*2   # after colour+depth
    def rgb565(r,g,b): return ((r>>3)<<11)|((g>>2)<<5)|(b>>3)
    checker=[rgb565(255,0,0),rgb565(0,255,0),rgb565(0,0,255),rgb565(255,255,255)]
    for i,t in enumerate(checker):
        q.wl(base.BAR1+texoff+i*2, t)   # write texels into vram via LFB window
    q.wl(D3+base.FBZMODE,(7<<5)|(1<<10))          # zfunc=always
    q.wl(D3+0x30c, texoff)                         # texBaseAddr
    q.wl(D3+0x304, (7<<2))                          # tLOD lodmin=7 -> dim=2
    q.wl(D3+0x300, 1 | (10<<8))                     # textureMode: enable, fmt=565
    q.wl(D3+base.FBZCOLORPATH,1)                    # rgbselect=texture
    q.wl(D3+base.C1,0x101010); q.wl(D3+base.FASTFILL,1)
    # big quad (two tris) with s,t 0..2 across it (perspective w=1)
    tri_setup(q,[(100,100,1000,1.0,0xffffffff,0.0,0.0),
                 (500,100,1000,1.0,0xffffffff,2.0,0.0),
                 (100,400,1000,1.0,0xffffffff,0.0,2.0)],
              mode=1|(1<<2)|(1<<3)|(1<<5))          # +ST0
    tri_setup(q,[(500,100,1000,1.0,0xffffffff,2.0,0.0),
                 (500,400,1000,1.0,0xffffffff,2.0,2.0),
                 (100,400,1000,1.0,0xffffffff,0.0,2.0)],
              mode=1|(1<<2)|(1<<3)|(1<<5))
    q.wl(D3+base.SWAPBUF,1)
    m.cmd("screendump",filename=DIR+"/tex.ppm")
    d=open(DIR+"/tex.ppm","rb").read().split(b"\n",3); p=d[3]
    cols={}
    for i in range(0,len(p)-2,3):
        c=(p[i]//64,p[i+1]//64,p[i+2]//64); cols[c]=cols.get(c,0)+1
    # expect red, green, blue, white regions from the checker
    def has(r,g,b): return any(n>500 and c==(r,g,b) for c,n in cols.items())
    ok = has(3,0,0) and has(0,3,0) and has(0,0,3)
    print("texture quadrants r/g/b present ->", "PASS" if ok else "FAIL",
          "(distinct:%d)"%len(cols))
    from PIL import Image
    Image.open(DIR+"/tex.ppm").save(DIR+"/tex.png")

main()
