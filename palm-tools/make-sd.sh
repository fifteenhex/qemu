#!/bin/sh
# Create a FAT16 SD-card image for the Palm m500 slot.
#
#   make-sd.sh out.img [size-mib]
#
# Both a "superfloppy" (filesystem at sector 0) and an MBR-partitioned
# image work with PalmOS 4.1; this makes the simpler superfloppy.
# Attach it with: qemu-system-m68k -M palmm500 ... \
#                   -drive if=sd,format=raw,file=out.img
set -e

img="${1:-palm-sd.img}"
size="${2:-64}"

truncate -s "${size}M" "$img"
mkfs.vfat -F 16 "$img" >/dev/null
echo "created ${size}MiB FAT16 SD image: $img"
