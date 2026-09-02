#!/usr/bin/env python3
"""
Build a bootable SunOS 4.1.1 sun3x SCSI disk image for the QEMU sun3x machine.

The install media miniroot (miniroot_sun3x) is a ready SunOS UFS partition
image: block 0 is the (to-be-written) disklabel slot, blocks 1-15 hold the
sun3 ufsboot boot block (block 1 begins with the m68k 'jmp' 0x4efa...), and
the UFS superblock is at byte 8192.  We only need to lay it at the start of a
disk and stamp a Sun disklabel (block 0) whose partitions point at it, so the
PROM 'b sd(0,30,N)' reads the boot block and boots the miniroot -> SunOS
install/miniroot shell on ttya.

Sun disklabel layout (Linux kernel block/partitions/sun.c):
  info[128]@0 ; rspeed@420 pcyl@422 sparecyl@424 ilfact@430 ncyl@432
  nacyl@434 ntrks@436 nsect@438 ; partitions[8]@444 (be32 start_cyl, be32
  num_sectors) ; magic@508 (0xDABE) ; csum@510 (xor of all 256 be16 == 0).
"""
import struct, sys, os

MINIROOT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/sunos-media/miniroot_sun3x"
OUT      = sys.argv[2] if len(sys.argv) > 2 else "/tmp/sunos-boot.img"

# Geometry: 16 heads x 32 sectors = 512 blocks/cyl; 1024 cyl = 256 MiB.
NTRKS, NSECT, NCYL = 16, 32, 1024
BPC   = NTRKS * NSECT               # blocks per cylinder
TOTAL = NCYL * BPC                  # total 512-byte blocks
SIZE  = TOTAL * 512

def build_label():
    b = bytearray(512)
    label = b"QEMU sun3x SunOS 4.1.1 miniroot\x00"
    b[0:len(label)] = label
    struct.pack_into(">H", b, 420, 3600)     # rspeed
    struct.pack_into(">H", b, 422, NCYL)     # pcyl
    struct.pack_into(">H", b, 424, 0)        # sparecyl
    struct.pack_into(">H", b, 430, 1)        # interleave
    struct.pack_into(">H", b, 432, NCYL)     # ncyl
    struct.pack_into(">H", b, 434, 0)        # nacyl
    struct.pack_into(">H", b, 436, NTRKS)    # ntrks
    struct.pack_into(">H", b, 438, NSECT)    # nsect
    # partitions: a,b,c,d all start at cyl 0 covering the whole disk so
    # 'b sd(0,30,N)' finds the boot block at disk block 1 for any N.
    for i in range(8):
        struct.pack_into(">II", b, 444 + i * 8, 0, TOTAL)
    struct.pack_into(">H", b, 508, 0xDABE)   # magic
    # checksum: xor of all 256 be16 words (incl. magic) must be 0, so the
    # csum word at 510 = xor of words at offsets 0..508.
    cs = 0
    for off in range(0, 510, 2):
        cs ^= struct.unpack_from(">H", b, off)[0]
    struct.pack_into(">H", b, 510, cs)
    return bytes(b)

def main():
    mr = open(MINIROOT, "rb").read()
    assert len(mr) <= SIZE, "miniroot larger than disk"
    with open(OUT, "wb") as f:
        f.truncate(SIZE)
        f.seek(0)
        f.write(mr)               # miniroot at LBA 0 (bootblk -> blk 1..15)
        f.seek(0)
        f.write(build_label())    # stamp disklabel over block 0
    print("wrote %s  (%d MiB, %dc/%dh/%ds, miniroot %d blocks)"
          % (OUT, SIZE >> 20, NCYL, NTRKS, NSECT, len(mr) // 512))

if __name__ == "__main__":
    main()
