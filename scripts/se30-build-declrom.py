#!/usr/bin/env python3
# Build a minimal Macintosh SE/30 pseudo-slot ($E) video declaration ROM.
# byteLanes = 0x0F (all four lanes -> contiguous byte layout, so the blob is
# laid out linearly and its last byte lands at 0x00FFFFFF in slot space).
#
# Layout (low address -> high address):
#   [ sResource data / directory ... ]
#   [ 20-byte format block, byteLanes at the very top ]
#
# The whole blob is loaded so that blob[-1] == physical 0x00FFFFFF.
import struct, sys

TESTPATTERN = 0x5A932BC7

# ---- we assemble the body first, then prepend nothing; format block last ----
# We build using absolute "blob offsets"; blob is placed so that
#   phys(blob_off) = 0x01000000 - BLOB_LEN + blob_off
# The format block occupies the final 20 bytes.

def rol32(v, n=1):
    v &= 0xFFFFFFFF
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFF

def apple_crc(data, crc_off):
    # Documented Apple declaration-ROM CRC: for each byte (ascending address),
    # sum = ROL(sum,1) + byte, with the 4 CRC bytes treated as 0.
    s = 0
    for i, b in enumerate(data):
        s = rol32(s, 1)
        if crc_off <= i < crc_off + 4:
            b = 0
        s = (s + b) & 0xFFFFFFFF
    return s

# ---------- sResource encoding helpers ----------
# An sResource is a list of entries; each entry is one long:
#   (id << 24) | (offset & 0x00FFFFFF)  where offset is signed, relative to
#   the entry's own address, pointing at the data for that id.
# The list terminates with an "end" entry id=0xFF, offset=0.
#
# We lay out all data blobs, then the sResource lists, then the directory,
# then the format block, computing offsets against final positions.

class Blob:
    def __init__(self):
        self.buf = bytearray()
    def tell(self): return len(self.buf)
    def emit(self, b): self.buf += b; return len(self.buf) - len(b)
    def long(self, v): return self.emit(struct.pack(">I", v & 0xFFFFFFFF))
    def word(self, v): return self.emit(struct.pack(">H", v & 0xFFFF))
    def byte(self, v): return self.emit(bytes([v & 0xFF]))
    def pstr(self, s):
        d = s.encode('mac_roman')
        off = self.emit(bytes([len(d)]) + d)
        # word-align
        while len(self.buf) & 1:
            self.buf += b'\0'
        return off
    def cstr(self, s):
        # sResource names are C strings (Slot Manager sGetCString/sReadDrvrName
        # convention) -- a Pascal string here makes the ROM's sReadDrvrName
        # embed the length byte in the derived driver name, so its by-name SRT
        # search never matches and ROM sGetDriver fails with -345.
        d = s.encode('mac_roman')
        off = self.emit(d + b'\0')
        while len(self.buf) & 1:
            self.buf += b'\0'
        return off

b = Blob()

# ---- data records (emit first so we know their offsets) ----
# Board sResource type record: catBoard, 0, 0, 0
off_board_type = b.tell(); b.long(0x00010000); b.long(0x00000000)
off_board_name = b.cstr("Macintosh SE/30 Video Card")
# Video sResource type record: catDisplay(3), typVideo(1), drSwApple(1), drHwXXX(1)
off_vid_type = b.tell(); b.long(0x00030001); b.long(0x00010001)
off_vid_name = b.cstr("Display_Video_Apple_SE30")

# vidMode params (mVidParams) for the 1-bit 512x342 mode, emitted as an
# sBlock (leading physical length long, then the VPBlock) because the ROM's
# screen-init fetches it with sGetBlock (Slot Manager selector 5), which
# expects a length-prefixed block -- same convention that already works for
# the sMacOS68000 driver sBlock below.
# VPBlock: {vpBaseOffset(l), vpRowBytes(w), vpBounds(t,l,b,r w), vpVersion(w),
#   vpPackType(w), vpPackSize(l), vpHRes(l), vpVRes(l), vpPixelType(w),
#   vpPixelSize(w), vpCmpCount(w), vpCmpSize(w), vpPlaneBytes(l) }
def emit_vidparams(base_off, rowbytes, w, h, depth):
    body = struct.pack(">IHHHHHHHI II HHHH I",
                       base_off,          # vpBaseOffset ($8040)
                       rowbytes,          # vpRowBytes   (64)
                       0, 0, h, w,        # vpBounds t,l,b,r
                       0,                 # vpVersion
                       0,                 # vpPackType
                       0,                 # vpPackSize
                       0x00480000,        # vpHRes 72dpi
                       0x00480000,        # vpVRes 72dpi
                       0,                 # vpPixelType (0=chunky)
                       depth,             # vpPixelSize
                       1,                 # vpCmpCount
                       depth,             # vpCmpSize
                       0)                 # vpPlaneBytes
    off = b.tell()
    # sBlock physical length INCLUDES the length field itself (Designing
    # Cards & Drivers; the ROM's sGetDriver does spSize -= 4 after
    # sReadPBSize before allocating/copying, confirmed live at 0x40804df2).
    b.long(len(body) + 4)
    b.emit(body)
    return off
off_vidparams1 = emit_vidparams(0x8040, 64, 512, 342, 1)

# long data records for MinorBaseOS / MinorLength.  The ROM's screen-init
# reads these via sFindDevBase (sel 27) / sReadLong (sel 2), which follow the
# entry OFFSET to a long data record -- an inline value does not work here.
# MinorBaseOS = 0 so sFindDevBase returns slot base 0xFE000000; the vidMode's
# vpBaseOffset ($8040) is then added by the ROM, landing ScrnBase exactly on
# the fb scanout at 0xFE008040.
off_minorbase = b.tell(); b.long(0x00000000)
off_minorlen  = b.tell(); b.long(0x00010000)   # 64KB VRAM aperture

# ---- slot video driver (sMacOS68000) ----
# Load the flat driver binary (assembled from se30-video-driver.s with
# m68k-linux-gnu-as + objcopy).  Emit it as an sBlock: a leading physical
# length (the byte count of the driver that follows) then the driver bytes.
import os, subprocess
_here = os.path.dirname(os.path.abspath(__file__))
_drvsrc = os.path.join(_here, "se30-video-driver.s")
_drvpath = os.environ.get("SE30_DRIVER_BIN",
                          os.path.join(_here, "se30-video-driver.bin"))
# Assemble the driver from source if the flat binary is absent/stale.
if os.path.exists(_drvsrc) and (not os.path.exists(_drvpath) or
        os.path.getmtime(_drvsrc) > os.path.getmtime(_drvpath)):
    _o = _drvpath + ".o"
    subprocess.check_call(["m68k-linux-gnu-as", "-m68030", _drvsrc, "-o", _o])
    subprocess.check_call(["m68k-linux-gnu-objcopy", "-O", "binary",
                           "-j", ".text", _o, _drvpath])
    os.unlink(_o)
with open(_drvpath, "rb") as _f:
    driver_bytes = _f.read()

off_driver_block = b.tell()
b.long(len(driver_bytes) + 4)      # sBlock physical length (INCLUDES itself)
b.emit(driver_bytes)
while len(b.buf) & 1:               # word align
    b.buf += b'\0'

# sDriver directory: sMacOS68000 (id 1) and sMacOS68020 (id 2) both point at
# the same driver block -- the ROM's sGetDriver (0x40804dbc) asks for the
# 68020 driver (spID 2) FIRST and only falls back to spID 1, and a failing
# first lookup nils spsPointer in the spBlock so the fallback then dies with
# -335 (smsPointerNil); serving id 2 directly avoids that entirely.
off_drvr_dir = b.tell()
_p1 = b.tell(); b.long(0)           # placeholder sMacOS68000 entry
_p2 = b.tell(); b.long(0)           # placeholder sMacOS68020 entry
b.long(0xFF000000)                  # end-of-list (id 0xFF in the TOP byte)
struct.pack_into(">I", b.buf, _p1,
                 (0x01 << 24) | ((off_driver_block - _p1) & 0x00FFFFFF))
struct.pack_into(">I", b.buf, _p2,
                 (0x02 << 24) | ((off_driver_block - _p2) & 0x00FFFFFF))

# ---- sResource lists ----
# helper to build an sResource list at the current position; entries is a list
# of (id, kind, value) where kind in {'off','long','word','byte'}.
def emit_sresource(entries):
    start = b.tell()
    positions = []
    for i,(rid,kind,val) in enumerate(entries):
        positions.append(b.tell())
        b.long(0)  # placeholder
    # end marker: id 0xFF lives in the TOP byte of the entry long.  (The
    # original 0x000000FF encoded id 0x00, which made every FAILED id search
    # walk past the last real entry into an "id 0 after id N" state ->
    # smBadsList (-331) instead of a clean not-found; successful searches
    # never noticed because they find their target before the terminator.)
    end_pos = b.tell(); b.long(0xFF000000)
    # backfill
    for (rid,kind,val),pos in zip(entries,positions):
        if kind == 'off':
            rel = (val - pos) & 0x00FFFFFF
            word = ((rid & 0xFF) << 24) | rel
        elif kind == 'longinline':
            # id with an inline long value stored right after? Not used; treat as off
            word = ((rid & 0xFF) << 24) | (val & 0x00FFFFFF)
        else:
            word = ((rid & 0xFF) << 24) | (val & 0x00FFFFFF)
        struct.pack_into(">I", b.buf, pos, word)
    return start

# Board sResource (id in directory = 1)
off_board_sr = emit_sresource([
    (0x01, 'off', off_board_type),   # sRsrcType
    (0x02, 'off', off_board_name),   # sRsrcName
    (0x20, 'inline', 0x000C),        # boardId  (inline word value)
])

# vidMode $80 directory: the ROM's screen-init does sFindStruct($80) and then
# sGetBlock(1) INSIDE it, so mode $80 must itself be an sResource list:
#   1 = mVidParams (sBlock), 3 = mPageCnt (inline), 4 = mDevType (inline,
#   1 = fixed device: the SE/30's onboard 1-bit video has no writable CLUT).
off_mode80_dir = emit_sresource([
    (0x01, 'off', off_vidparams1),   # mVidParams sBlock
    (0x03, 'inline', 1),             # mPageCnt
    (0x04, 'inline', 1),             # mDevType (fixed)
])

# Video sResource (id in directory = 0x80)
off_vid_sr = emit_sresource([
    (0x01, 'off', off_vid_type),     # sRsrcType (catDisplay/typVideo)
    (0x02, 'off', off_vid_name),     # sRsrcName
    (0x04, 'off', off_drvr_dir),     # sRsrcDrvrDir -> sMacOS68000 driver
    (0x0A, 'off', off_minorbase),    # minorBaseOS -> long data record
    (0x0B, 'off', off_minorlen),     # minorLength -> long data record
    (0x80, 'off', off_mode80_dir),   # first vidMode ($80) -> mode directory
])

# ---- sResource directory ----
dir_pos = b.tell()
dir_entries = [(0x01, off_board_sr), (0x80, off_vid_sr)]
positions = []
for rid,_ in dir_entries:
    positions.append(b.tell()); b.long(0)
b.long(0xFF000000)  # end-of-list (id 0xFF in the top byte)
for (rid,target),pos in zip(dir_entries,positions):
    rel = (target - pos) & 0x00FFFFFF
    struct.pack_into(">I", b.buf, pos, ((rid & 0xFF) << 24) | rel)

# ---- format block (last 20 bytes) ----
fb_pos = b.tell()
b.long(0)   # directoryOffset (fix later)
b.long(0)   # length (fix later)
b.long(0)   # crc (fix later)
b.byte(0x01)  # romRevision
b.byte(0x01)  # format
b.long(TESTPATTERN)  # testPattern
b.byte(0x00)  # reserved
b.byte(0x0F)  # byteLanes

BLOB_LEN = b.tell()
# directoryOffset: 24-bit signed offset from the directoryOffset field to the
# directory.  The ROM's format-validate (0x4080427c) requires the top byte to
# be 0, so store only the low 24 bits (a negative offset becomes 0x00FFFFxx).
dir_off = (dir_pos - fb_pos) & 0x00FFFFFF
struct.pack_into(">I", b.buf, fb_pos + 0, dir_off)
struct.pack_into(">I", b.buf, fb_pos + 4, BLOB_LEN)
crc = apple_crc(bytes(b.buf), fb_pos + 8)
struct.pack_into(">I", b.buf, fb_pos + 8, crc)

out = bytes(b.buf)
open(sys.argv[1] if len(sys.argv)>1 else "/workspace/src/qemu-q630/se30_declrom.bin","wb").write(out)
print("decl ROM: %d bytes, dir_pos=0x%x fb_pos=0x%x crc=0x%08x dirOff=%d" %
      (len(out), dir_pos, fb_pos, crc, dir_pos-fb_pos))
print("last byte (byteLanes) will be at 0x00FFFFFF")

# Optional: also emit a C array for embedding as the machine's built-in
# default decl ROM (hw/m68k/macse30.c macse30_declrom_default[]).
if "--c-array" in sys.argv:
    lines = []
    for i in range(0, len(out), 12):
        lines.append("    " + " ".join("0x%02x," % byte
                                       for byte in out[i:i+12]))
    print("/* generated by scripts/se30-build-declrom.py --c-array */")
    print("static const uint8_t macse30_declrom_default[%d] = {" % len(out))
    print("\n".join(lines))
    print("};")
