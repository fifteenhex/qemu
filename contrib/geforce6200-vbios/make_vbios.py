#!/usr/bin/env python3
# Minimal synthetic VBIOS for nouveau on emulated NV44A (chipset 0x4a).
# Derived from nouveau source (kernel 7.2-rc4); see NOTES.md.
import sys

img = bytearray(0x1000)

def w8(o, v): img[o] = v & 0xff
def w16(o, v): img[o:o+2] = (v & 0xffff).to_bytes(2, 'little')
def w32(o, v): img[o:o+4] = (v & 0xffffffff).to_bytes(4, 'little')

# ROM header
w16(0x0000, 0xaa55)         # ROM signature
w8(0x0002, 8)               # size in 512B units (cosmetic)
w16(0x0018, 0x001c)         # PCIR pointer

# PCIR at 0x001c
img[0x001c:0x0020] = b'PCIR'
w16(0x0020, 0x10de)         # vendor
w16(0x0022, 0x0221)         # device (NV44A GeForce 6200)
w16(0x0024, 0x0000)         # VPD
w16(0x0026, 0x0018)         # PCIR header length
w8(0x0028, 0x00)            # PCIR revision
img[0x0029:0x002c] = bytes([0x00, 0x00, 0x03])  # class code: display
w16(0x002c, 8)              # image size in 512B units = 4096
w16(0x002e, 0x0000)         # image revision
w8(0x0030, 0x00)            # image type: x86 BIOS
w8(0x0031, 0x80)            # last image

# DCB pointer at 0x36 stays 0 (graceful "DCB table not found")
# 0x40 must not read 'NPDE' - stays zero.

# BMP structure at 0x60: "\xff\x7fNV\0", major 0 minor 1 -> nouveau's
# legacy parser succeeds without checksum or init tables, and
# bios->version.major = rd08(bmp+13) = 0 skips DRM init scripts.
img[0x0060:0x0065] = b'\xff\x7fNV\x00'
w8(0x0065, 0x00)            # BMP struct major
w8(0x0066, 0x01)            # BMP struct minor

# 8-bit checksum over the whole image must be 0
img[0x0fff] = (-sum(img)) & 0xff

out = sys.argv[1] if len(sys.argv) > 1 else 'nv44a_vbios.rom'
open(out, 'wb').write(img)
print(f"{out}: {len(img)} bytes, checksum {sum(img) & 0xff}")
