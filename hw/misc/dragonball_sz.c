/*
 * MC68SZ328 "DragonBall Super VZ" SoC register block + on-chip enhanced
 * TFT color LCD controller (Sony CLIE PEG-SJ33 / NR70V).
 *
 * Register map and reset values from Cloudpilot-emu's
 * hardware/EmRegsSZ.{h,cpp}, hardware/clie/EmRegsSZRedwood.cpp and the
 * HwrM68SZ328Type struct in palm/.../IncsPrv/M68SZ328Hwr.h (fetched
 * this session; see CLIE-POC-NOTES.md's SJ33/SZ328 section).  Register
 * offsets below are struct-offsets from the 0xFFFE0000 base.
 *
 * Scope: this models enough of the SZ328 to load and run the Palm OS
 * 4.1 SoC bring-up off a real SZ328 ROM (the NR70V image, halID
 * sonyrdwd -- the same SoC as the SJ33) and to scan out the enhanced
 * on-chip LCD controller.  The registers are plain big-endian storage
 * with the documented reset values seeded (identity + PLL), so the
 * guest's read-modify-write and chip-detection sequences behave; the
 * LCD controller reads its framebuffer straight out of main DRAM at
 * the guest-programmed lcdStartAddr.  The INTC / timer / UART / ADC
 * dynamic behaviour is NOT modelled (that is the remaining work to
 * reach the launcher; see the notes) -- this is the SoC-bring-up and
 * display-scanout foundation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "exec/cpu-common.h"
#include "system/address-spaces.h"
#include "hw/misc/dragonball_sz.h"
#include "migration/vmstate.h"
#include "ui/pixel_ops.h"

/* Register struct offsets (from the 0xFFFE0000 window base). */
#define SZ_LCD_START_ADDR   0x00800  /* u32: framebuffer base (in DRAM) */
#define SZ_LCD_SCREEN_SIZE  0x00804  /* u16: [15:9]=width/8, [8:0]=height */
#define SZ_LCD_PAGE_WIDTH   0x00806  /* u16: [9:0]=virtual page width /2 */
#define SZ_LCD_PANEL_CTL1   0x00814  /* u16: [11:9]=bpp selector */
#define SZ_LCD_PANNING      0x0081E  /* u16: [3:0]=panning offset */
#define SZ_LCD_CLUT         0x00A00  /* u16[256]: 12-bit colour LUT */
#define SZ_SCR              0x10000  /* u8:  system control */
#define SZ_CHIP_ID          0x10004  /* u8:  chip ID */
#define SZ_MASK_ID          0x10005  /* u8:  mask ID */
#define SZ_PLL_CONTROL      0x10200  /* u16 */
#define SZ_PLL_FREQSEL0     0x10202  /* u16 */
#define SZ_PLL_FREQSEL1     0x10204  /* u16 */
#define SZ_PWR_CONTROL      0x10207  /* u8 */
#define SZ_CLOCK_SRC_CTL    0x1020C  /* u16 */

static void sz_put16(DragonBallSZState *s, unsigned int off, uint16_t v)
{
    s->regs[off] = v >> 8;
    s->regs[off + 1] = v;
}

static uint16_t sz_get16(DragonBallSZState *s, unsigned int off)
{
    return (s->regs[off] << 8) | s->regs[off + 1];
}

static uint32_t sz_get32(DragonBallSZState *s, unsigned int off)
{
    return (s->regs[off] << 24) | (s->regs[off + 1] << 16) |
           (s->regs[off + 2] << 8) | s->regs[off + 3];
}

/* CLUT entry: 12-bit 0x0RGB -> XRGB8888 (EmRegsSZ::convertColor_12bit). */
static uint32_t sz_clut_pixel(DragonBallSZState *s, unsigned int idx)
{
    uint16_t v = sz_get16(s, SZ_LCD_CLUT + idx * 2);
    uint8_t r = (v >> 8) & 0xf;
    uint8_t g = (v >> 4) & 0xf;
    uint8_t b = v & 0xf;

    return rgb_to_pixel32(r | (r << 4), g | (g << 4), b | (b << 4));
}

static bool dragonball_sz_update_display(void *opaque)
{
    DragonBallSZState *s = opaque;
    DisplaySurface *surface;
    uint32_t fb = sz_get32(s, SZ_LCD_START_ADDR) & ~1u;
    uint16_t screen = sz_get16(s, SZ_LCD_SCREEN_SIZE);
    unsigned int width = (screen >> 9) * 8;
    unsigned int height = screen & 0x1ff;
    unsigned int vpw = (sz_get16(s, SZ_LCD_PAGE_WIDTH) & 0x3ff) * 2; /* bytes/line */
    unsigned int bpp = 1u << ((sz_get16(s, SZ_LCD_PANEL_CTL1) >> 9) & 0x7);
    uint32_t palette[256];
    uint8_t *line;
    uint32_t *dest;
    size_t linelen;
    unsigned int x, y;

    /*
     * Always finish the update (return true) even when there is nothing
     * to scan out yet -- otherwise a screendump / VNC refresh coroutine
     * parked in qemu_console_co_wait_update() wedges forever (the same
     * gfx_update() contract fixed for clie_lcd.c, see that file).
     */
    if (!width || !height || width > 1024 || height > 1024) {
        goto blank;
    }
    if (!vpw) {
        vpw = width * bpp / 8;
    }

    surface = qemu_console_surface(s->con);
    if (width != surface_width(surface) || height != surface_height(surface)) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
    }
    if (surface_bits_per_pixel(surface) != 32) {
        goto blank;
    }

    if (bpp > 16) {
        /* only 1/2/4/8/16bpp are real LCD modes; anything else is a
         * mid-programming transient -- don't try to scan it out */
        goto blank;
    }

    if (bpp <= 8) {
        for (x = 0; x < 256; x++) {
            palette[x] = sz_clut_pixel(s, x);
        }
    }

    /*
     * Allocate the line buffer big enough for every pixel access this
     * scanline makes (width * bytes-per-pixel, +2 slack for the 16bpp
     * high-byte read), independent of the programmed virtual page width
     * -- a too-small vpw (transient, mid-programming) must not cause an
     * out-of-bounds read.  Zero-fill, then copy in the real framebuffer
     * bytes (vpw of them) capped to the buffer.
     */
    linelen = (size_t)width * ((bpp <= 8) ? 1 : 2) + 2;
    if (vpw > linelen) {
        linelen = vpw;
    }
    line = g_malloc0(linelen);
    dest = surface_data(surface);
    for (y = 0; y < height; y++) {
        /* the framebuffer lives in main DRAM at the programmed address */
        address_space_read(&address_space_memory, fb + (hwaddr)y * vpw,
                           MEMTXATTRS_UNSPECIFIED, line, vpw ? vpw : linelen);
        for (x = 0; x < width; x++) {
            uint32_t pixel;

            switch (bpp) {
            case 1:
                pixel = palette[(line[x / 8] >> (7 - x % 8)) & 1];
                break;
            case 2:
                pixel = palette[(line[x / 4] >> (6 - x % 4 * 2)) & 3];
                break;
            case 4:
                pixel = palette[x % 2 ? line[x / 2] & 0xf : line[x / 2] >> 4];
                break;
            case 8:
                pixel = palette[line[x]];
                break;
            default: {
                /* 16bpp big-endian RGB565 (EmMemDoGet16 is big-endian) */
                uint16_t v = (line[x * 2] << 8) | line[x * 2 + 1];

                pixel = rgb_to_pixel32((v >> 11) << 3,
                                       ((v >> 5) & 0x3f) << 2,
                                       (v & 0x1f) << 3);
                break;
            }
            }
            *dest++ = pixel;
        }
    }
    g_free(line);

    qemu_console_update_full(s->con);
    return true;

blank:
    /*
     * Nothing valid to scan out yet (LCD controller not programmed, or
     * mid-programming transient): present a clean black 320x480 frame
     * (the SJ33/NR70V panel size) instead of leaving QEMU's default
     * "guest has not initialized the display" placeholder.  Always
     * return true so the screendump / VNC waiter is released.
     */
    surface = qemu_console_surface(s->con);
    if (!surface || surface_width(surface) != 320 ||
        surface_height(surface) != 480) {
        qemu_console_resize(s->con, 320, 480);
        surface = qemu_console_surface(s->con);
    }
    if (surface && surface_bits_per_pixel(surface) == 32) {
        memset(surface_data(surface), 0,
               (size_t)surface_width(surface) * surface_height(surface) * 4);
    }
    qemu_console_update_full(s->con);
    return true;
}

static uint64_t dragonball_sz_read(void *opaque, hwaddr addr, unsigned size)
{
    DragonBallSZState *s = opaque;
    uint64_t val = 0;
    unsigned int i;

    for (i = 0; i < size; i++) {
        val = (val << 8) | s->regs[addr + i];
    }
    return val;
}

static void dragonball_sz_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    DragonBallSZState *s = opaque;
    unsigned int i;

    for (i = 0; i < size; i++) {
        s->regs[addr + i] = value >> (8 * (size - 1 - i));
    }
}

static const MemoryRegionOps dragonball_sz_ops = {
    .read = dragonball_sz_read,
    .write = dragonball_sz_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void dragonball_sz_invalidate(void *opaque)
{
}

static const GraphicHwOps dragonball_sz_gfx_ops = {
    .invalidate = dragonball_sz_invalidate,
    .gfx_update = dragonball_sz_update_display,
};

static void dragonball_sz_reset(DeviceState *dev)
{
    DragonBallSZState *s = DRAGONBALL_SZ(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* Identity + PLL reset values (EmRegsSZ kInitial68SZ328RegisterValues). */
    s->regs[SZ_SCR] = 0x1c;
    s->regs[SZ_CHIP_ID] = 0x56;
    s->regs[SZ_MASK_ID] = 0x01;
    s->regs[SZ_PWR_CONTROL] = 0x1f;
    sz_put16(s, SZ_PLL_CONTROL, 0x2414);
    sz_put16(s, SZ_PLL_FREQSEL0, 0x3ce8);
    sz_put16(s, SZ_PLL_FREQSEL1, 0x0900);
    sz_put16(s, SZ_CLOCK_SRC_CTL, 0x8a03);
}

static void dragonball_sz_realize(DeviceState *dev, Error **errp)
{
    DragonBallSZState *s = DRAGONBALL_SZ(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &dragonball_sz_ops, s,
                          TYPE_DRAGONBALL_SZ, DRAGONBALL_SZ_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    s->con = qemu_graphic_console_create(dev, 0, &dragonball_sz_gfx_ops, s);
}

static const VMStateDescription vmstate_dragonball_sz = {
    .name = TYPE_DRAGONBALL_SZ,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, DragonBallSZState, DRAGONBALL_SZ_REGS_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static void dragonball_sz_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, dragonball_sz_reset);
    dc->realize = dragonball_sz_realize;
    dc->vmsd = &vmstate_dragonball_sz;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo dragonball_sz_info = {
    .name          = TYPE_DRAGONBALL_SZ,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DragonBallSZState),
    .class_init    = dragonball_sz_class_init,
};

static void dragonball_sz_register_types(void)
{
    type_register_static(&dragonball_sz_info);
}

type_init(dragonball_sz_register_types)
