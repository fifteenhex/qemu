#!/usr/bin/env python3
"""
Boot the Miyoo Mini (MStar SSD202D / infinity2m) vendor firmware in QEMU and
capture a screendump, to reproduce the display coming up through the GOP plane.

Boot path: mask-ROM bootrom -> IPL -> U-Boot -> Linux 4.9 -> the vendor "MainUI"
menu, rendered through the standalone TYPE_MSTAR_GOP device (console index 0).

The stock flash image boots the *unpatched* kernel because the i2c1 @0x3d
security element is emulated (no more BUG()); its u-boot environment partition is
erased, so this script drives u-boot over a serial socket to load and boot the
NOR kernel, then screendumps via the QEMU monitor.

Firmware images (supply these paths, not in-tree):
  --bootrom  SSD202D 16KiB mask ROM   (e.g. ssd202_bootrom_16kb.bin)
  --flash    16MiB vendor NOR image   (e.g. MiYoo283v1.1.bin)

Usage:
  scripts/mstar/miyoo-render.py \
      --qemu build/qemu-system-arm \
      --bootrom /path/ssd202_bootrom_16kb.bin \
      --flash /path/MiYoo283v1.1.bin \
      --out /tmp/miyoo.ppm

Expected result: a 640x480 screendump of the MainUI menu (Miyoo logo, battery,
Game / RetroArch / App / Setting icons, "A OPEN / B BACK"), ~99% non-black.
"""
import argparse
import os
import signal
import socket
import subprocess
import sys
import time

# The vendor bootcmd lives in the (erased) env, so pass its bootargs by hand.
BOOTARGS = (
    "console=ttyS0,115200 root=/dev/mtdblock4 rootfstype=squashfs ro "
    "init=/linuxrc LX_MEM=0x7f00000 "
    "mma_heap=mma_heap_name0,miu=0,sz=0x1500000 mma_memblock_remove=1 "
    "highres=off "
    "mmap_reserved=fb,miu=0,sz=0x300000,max_start_off=0x7C00000,"
    "max_end_off=0x7F00000"
)
# Load the NOR kernel uImage (flash offset 0x60000) to DRAM and boot it.
UBOOT_CMDS = "sf probe 0; sf read 0x22000000 0x60000 0x200000; bootm 0x22000000"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default="build/qemu-system-arm")
    ap.add_argument("--bootrom", required=True)
    ap.add_argument("--flash", required=True)
    ap.add_argument("--out", default="/tmp/miyoo.ppm")
    ap.add_argument("--serial-port", type=int, default=55812)
    ap.add_argument("--monitor-port", type=int, default=55813)
    ap.add_argument("--boot-wait", type=int, default=78,
                    help="seconds to wait for MainUI after issuing bootm")
    args = ap.parse_args()

    q = subprocess.Popen(
        [args.qemu, "-M", "miyoomini", "-m", "128M",
         "-bios", args.bootrom,
         "-drive", "if=mtd,format=raw,file=" + args.flash,
         "-display", "none",
         "-serial", "tcp:127.0.0.1:%d,server=on,wait=off" % args.serial_port,
         "-monitor", "tcp:127.0.0.1:%d,server=on,wait=off" % args.monitor_port],
        # MSTAR_SECURE_KERNEL=1 is required or the kernel hangs in calibrate_delay.
        env=dict(os.environ, MSTAR_SECURE_KERNEL="1"),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(6)
        s = socket.create_connection(("127.0.0.1", args.serial_port))
        s.settimeout(1)

        def drain(secs):
            end = time.time() + secs
            while time.time() < end:
                try:
                    s.recv(4096)
                except OSError:
                    pass

        drain(15)                       # reach the "SigmaStar #" u-boot prompt
        s.sendall(b"\n")
        drain(1)
        s.sendall(("setenv bootargs '%s'\n" % BOOTARGS).encode())
        drain(1)
        s.sendall((UBOOT_CMDS + "\n").encode())
        drain(args.boot_wait)           # let Linux boot to the MainUI menu
        s.close()

        m = socket.create_connection(("127.0.0.1", args.monitor_port))
        m.settimeout(3)
        time.sleep(0.5)
        try:
            m.recv(4096)
        except OSError:
            pass
        m.sendall(("screendump %s\n" % args.out).encode())
        time.sleep(3)
        m.close()
    finally:
        q.send_signal(signal.SIGKILL)
        q.wait()

    if not os.path.exists(args.out):
        print("FAILED: no screendump written to %s" % args.out, file=sys.stderr)
        return 1

    # Report how much of the frame is non-black (a blank screen is the failure).
    data = open(args.out, "rb").read()
    hdr = data.split(b"\n", 3)
    px = hdr[3]
    nz = sum(1 for i in range(0, len(px), 3) if px[i:i + 3] != b"\x00\x00\x00")
    print("wrote %s  dims=%s  non-black=%.1f%%"
          % (args.out, hdr[1].decode(), 100.0 * nz / (len(px) / 3)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
