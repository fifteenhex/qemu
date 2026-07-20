# Booting the Miyoo Mini (MStar SSD202D) to a rendered display

This documents how to boot the Miyoo Mini vendor firmware on the `miyoomini`
machine and reproduce the display coming up, so the shared GOP scanout can be
verified end-to-end.

## What renders

The vendor firmware boots mask-ROM bootrom -> IPL -> U-Boot 2015.01 -> Linux
4.9 -> the vendor **MainUI** menu. MainUI draws through SDL 1.2 -> `/dev/fb0`,
which the kernel scans out via the **GOP primary plane** at `0x1f246800`. In QEMU
that plane is the standalone `TYPE_MSTAR_GOP` device (`hw/display/mstar_gop.c`),
created as graphic console index 0, so a `screendump` captures the UI.

Two facts make the stock image bootable in QEMU:

* The i2c1 `@0x3d` security element ("alpu-fa") is emulated
  (`hw/i2c/mstar_secelem.c`), so the kernel no longer `BUG()`s when MainUI runs
  its crypto challenge. The **unpatched** vendor kernel boots as-is.
* `MSTAR_SECURE_KERNEL=1` must be set in the environment, or the kernel hangs in
  `calibrate_delay`.

The one wrinkle: the vendor NOR image's U-Boot environment partition (flash
`0x5F000`) is erased, so U-Boot drops to its prompt with no `bootcmd`. Drive it
by hand (or via the helper script) to load and boot the NOR kernel.

## Firmware images

These are the retail images; supply your own copies (not in-tree):

| role            | example filename            |
|-----------------|-----------------------------|
| SSD202D bootrom | `ssd202_bootrom_16kb.bin`   |
| 16MiB NOR flash | `MiYoo283v1.1.bin`          |

The NOR kernel is a uImage at flash offset `0x60000` (load/entry `0x20008000`,
LZMA), and the root filesystem is the squashfs at `0x290000`
(`root=/dev/mtdblock4`).

## One-shot: the helper script

```
scripts/mstar/miyoo-render.py \
    --qemu build/qemu-system-arm \
    --bootrom /path/to/ssd202_bootrom_16kb.bin \
    --flash   /path/to/MiYoo283v1.1.bin \
    --out     /tmp/miyoo.ppm
```

It boots QEMU with the monitor and a serial socket, waits for the U-Boot prompt,
sets `bootargs`, loads and boots the kernel, then screendumps. On success it
prints e.g.:

```
wrote /tmp/miyoo.ppm  dims=640 480  non-black=99.1%
```

A near-100% non-black 640x480 frame is the pass signal (a blank screen would be
~0%). View it with e.g. `python3 -c "from PIL import Image;
Image.open('/tmp/miyoo.ppm').save('/tmp/miyoo.png')"`. Expected: the MainUI menu
- Miyoo logo, battery, Game / RetroArch / App / Setting icons, and an
"A OPEN / B BACK" footer.

## By hand

Start QEMU (serial and monitor on TCP so you can drive u-boot and screendump):

```
MSTAR_SECURE_KERNEL=1 build/qemu-system-arm -M miyoomini -m 128M \
    -bios /path/to/ssd202_bootrom_16kb.bin \
    -drive if=mtd,format=raw,file=/path/to/MiYoo283v1.1.bin \
    -display none \
    -serial  tcp:127.0.0.1:55812,server=on,wait=off \
    -monitor tcp:127.0.0.1:55813,server=on,wait=off
```

Connect to the serial port (e.g. `nc 127.0.0.1 55812`); at the `SigmaStar #`
prompt (~15s) enter:

```
setenv bootargs 'console=ttyS0,115200 root=/dev/mtdblock4 rootfstype=squashfs ro init=/linuxrc LX_MEM=0x7f00000 mma_heap=mma_heap_name0,miu=0,sz=0x1500000 mma_memblock_remove=1 highres=off mmap_reserved=fb,miu=0,sz=0x300000,max_start_off=0x7C00000,max_end_off=0x7F00000'
sf probe 0; sf read 0x22000000 0x60000 0x200000; bootm 0x22000000
```

MainUI reaches its menu ~75s after `bootm`. Connect to the monitor port
(`nc 127.0.0.1 55813`) and run `screendump /tmp/miyoo.ppm`.

## Notes

* The GOP is created before the "disp" device so its console is index 0 (the one
  `screendump` / `-display` grabs). The disp's own console carries the mopg
  video/overlay plane (the u-boot bootlogo).
* The same `TYPE_MSTAR_GOP` device is used by the mercury5 `70mai` machine; see
  the block-level notes in the source and the project memories.
