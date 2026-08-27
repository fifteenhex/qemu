# Apollo DN3000 display bring-up notes (framebuffer + Linux simpledrm/simplefb)

Status: **WORKING** — the `apollo-dn3000` machine now models a linear
framebuffer that is scanned out to the QEMU display, and Linux binds the
in-tree **simplefb** driver to it, bringing up **fbcon** (penguin logo +
kernel/console text) on the emulated display.

This is deliberately the *pragmatic* framebuffer path (option (a) in the
task brief): a linear xrgb8888 buffer presented to Linux as a generic
`simple-framebuffer`, rather than the faithful mono-MGM / 8-plane-CGM +
Bt458 blitter path (which simpledrm/simplefb cannot consume — they only
take linear RGB formats).

## 1. QEMU framebuffer device (`hw/display/apollo-fb.c`, `apollo-fb.h`)

- **Format / mode:** linear **xrgb8888**, fixed **1024x800** (the authentic
  DN3000 "15i" visible area, research §2.11), stride 4096, size 0x320000
  (3.125 MiB).
- **Physical placement:** the real MGM (`0xfa0000`, 256 KiB) and CGM
  (`0x0a0000`, 128 KiB) frame stores are far too small for a 3 MiB
  xrgb8888 buffer, so the linear scanout buffer lives in a dedicated **4 MiB
  window at physical `0x10000000`** — a BAR-like aperture well above the
  128 MiB RAM top (`0x100000..0x8100000`), in otherwise-unmapped high
  physical space. The MCR/Bt458 control window follows at `0x10400000`.
  Both addresses are `#define`d in `hw/display/apollo-fb.h` and mirrored by
  the Linux arch glue.
- **Scanout:** a `TYPE_APOLLO_FB` SysBusDevice with two MMIO regions
  (region 0 = linear FB RAM, region 1 = control). It registers a graphics
  console (`qemu_graphic_console_create`) with `invalidate`/`gfx_update`
  ops built on the `hw/display/framebuffer.c` helper (dirty-tracked
  `framebuffer_update_display`). The per-line draw function reads each
  source pixel as a **big-endian** 32-bit word `0x00RRGGBB` (`ldl_be_p`) —
  the 68k guest is big-endian — and writes it to the native
  `x8r8g8b8` QEMU surface, so colours/endianness map 1:1. Verified with a
  gradient test pattern screendump (correct R/G/B channels at all corners).
- **Control registers modelled (MCR + Bt458):** region 1 exposes
  - `0x00` MCR: bit0 = display enable (default **1**; scanout is blanked to
    black when cleared).
  - `0x04` Bt458 RAMDAC address register, `0x08` Bt458 CLUT data
    (auto-incrementing R/G/B triples, 256 entries).
  The Bt458 CLUT is **modelled but bypassed** in the truecolor xrgb8888
  scanout (documented in-code); simplefb runs the panel as direct-colour so
  the CLUT is unused. These registers are deliberately **NOT** wired to the
  real Apollo MCR/CCR probe addresses (`0x5d800`/`0x5e800`): those must keep
  floating `0xff` so the verified PROM graphics soft-probe / MD-prompt path
  (see APOLLO-NOTES.md) is left undisturbed.
- **Wiring:** instantiated in `apollo_dn3000_init()` (`hw/m68k/apollo.c`),
  mapped at `APOLLO_FB_BASE` / `APOLLO_FB_CTRL_BASE`. `config APOLLO` in
  `hw/m68k/Kconfig` now `select FRAMEBUFFER`; `hw/display/meson.build`
  builds `apollo-fb.c` when `CONFIG_APOLLO`.

## 2. Linux simplefb wiring (platform device, no DTB)

The current Apollo kernel tree (`/workspace/src/linux-m68kdt`,
`m68kdt-a500try`, 7.1.0) has **`CONFIG_OF` disabled**, so the device-tree
`simple-framebuffer` node route is unavailable. Instead we use the
**platform-device / platform_data** route that `simplefb.c` supports
(`simplefb_parse_pd()`): register a `"simple-framebuffer"` platform device
carrying a `struct simplefb_platform_data` (width/height/stride/format) and
an `IORESOURCE_MEM` for the FB. `simplefb`'s platform driver
(`.driver.name = "simple-framebuffer"`) binds by name and reads the params
from platform_data. No DTB, no `screen_info`, no sysfb needed.

- **Linux change (patch: `apollo-simplefb.patch`):** in
  `arch/m68k/apollo/config.c`, an `arch_initcall(apollo_init_simplefb)`
  registers the platform device with base `0x10000000`, 1024x800, stride
  4096, format `"x8r8g8b8"` (matching the QEMU `#define`s). Guarded by
  `MACH_IS_APOLLO`. (The dn_ints IRQ fix from Phase 2 is already committed
  in this tree.)
- **Kernel config:** `CONFIG_FB_SIMPLE=y` added on top of `apollo_defconfig`
  (which already sets `FB`, `VT`, `FRAMEBUFFER_CONSOLE`, `LOGO`,
  `FONT_8x16`). `FB_SIMPLE` pulls in `APERTURE_HELPERS`.

## 3. Boot / verification

```
qemu-system-m68k -M apollo-dn3000 -m 128M \
    -kernel /tmp/apollo-fb-kbuild/vmlinux \
    -initrd /workspace/files/rootfs.cpio.lz4.040 \
    -append "console=tty0 earlyprintk initcall_blacklist=dnfb_init" \
    -serial file:/tmp/apollo-fb-serial.log \
    -display none -monitor unix:/tmp/apollofb-mon.sock,server,nowait
```

`initcall_blacklist=dnfb_init` keeps the unmodelled native Apollo blitter fb
(`dnfb`) out of the way so `simplefb` owns `/dev/fb0`. Success = the QEMU
`screendump` shows the Linux penguin logo + fbcon text.

## 4. What was NOT done (scope)

- The faithful mono MGM / 8-plane CGM planar path + 2D blitter (research
  §2.11) is not modelled — not needed for simplefb (linear RGB only). The
  Bt458 CLUT is present as storage but bypassed.
- No native Apollo DRM/fbdev driver; simplefb is the whole Linux "driver".
