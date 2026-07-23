/*
 * Epson SED1376 embedded-memory color LCD controller, as used for
 * the color panel of the Palm m505/m515 (the DragonBall VZ's own
 * LCDC is unused there; this chip carries its own 80KB display
 * SRAM).
 *
 * Modelled from how the PalmOS 4.1 m515 ROM drives it (see
 * PALM-NOTES.md): the HAL writes the panel timings, programs the
 * 256-entry LUT through the address/data registers, and scans out
 * 8bpp (or 16bpp) from the internal SRAM.  Registers we don't
 * interpret are plain byte storage so read-modify-write sequences
 * still work.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/display/sed1376.h"
#include "migration/vmstate.h"
#include "ui/pixel_ops.h"

/* Registers (byte offsets) */
#define SED1376_REG_LUT_RED       0x08 /* LUT write data, red */
#define SED1376_REG_LUT_GREEN     0x09 /* LUT write data, green */
#define SED1376_REG_LUT_BLUE      0x0a /* LUT write data, blue */
#define SED1376_REG_LUT_ADDR      0x0b /* writing the index commits RGB */
#define SED1376_REG_HDP           0x14 /* horizontal display period /8 - 1 */
#define SED1376_REG_VDP0          0x1c /* vertical display period - 1, low */
#define SED1376_REG_VDP1          0x1d /* high */
#define SED1376_REG_MODE          0x70 /* display mode: bpp select */
#define SED1376_REG_SPECIAL       0x71 /* special effects: SwivelView */
#define SED1376_REG_START0        0x74 /* display start address, dwords */
#define SED1376_REG_START1        0x75
#define SED1376_REG_START2        0x76
#define SED1376_REG_LINE_OFFSET0  0x80 /* line address offset, dwords */
#define SED1376_REG_LINE_OFFSET1  0x81
/*
 * The PalmOS HAL polls bit 7 here before every LUT write (vertical
 * non-display period status); scan-out is instant for us, so the
 * "safe to touch the LUT" period is permanently active.
 */
#define SED1376_REG_VNDP_STATUS   0xa0
#define SED1376_VNDP_ACTIVE       0x80

#define SED1376_MODE_BPP_MASK     0x07
#define SED1376_MODE_BLANK        0x80
#define SED1376_SPECIAL_SWIVEL_MASK 0x30

static int sed1376_bpp(SED1376State *s)
{
    /* 0..4 -> 1/2/4/8/16 bpp */
    unsigned int sel = s->regs[SED1376_REG_MODE] & SED1376_MODE_BPP_MASK;

    return sel <= 4 ? 1 << sel : -1;
}

static unsigned int sed1376_width(SED1376State *s)
{
    return (s->regs[SED1376_REG_HDP] + 1) * 8;
}

static unsigned int sed1376_height(SED1376State *s)
{
    return ((s->regs[SED1376_REG_VDP1] << 8) | s->regs[SED1376_REG_VDP0]) + 1;
}

static bool sed1376_update_display(void *opaque)
{
    SED1376State *s = opaque;
    DisplaySurface *surface;
    const uint8_t *vmem = memory_region_get_ram_ptr(&s->vmem_mr);
    unsigned int width = sed1376_width(s);
    unsigned int height = sed1376_height(s);
    unsigned int start = ((s->regs[SED1376_REG_START2] << 16 |
                           s->regs[SED1376_REG_START1] << 8 |
                           s->regs[SED1376_REG_START0]) * 4) %
                         SED1376_VMEM_SIZE;
    unsigned int stride = (s->regs[SED1376_REG_LINE_OFFSET1] << 8 |
                           s->regs[SED1376_REG_LINE_OFFSET0]) * 4;
    /*
     * SwivelView: the panel is mounted rotated and the chip rescans
     * the (upright, linear) image accordingly; the start address
     * register then holds a value in the chip's rotated addressing
     * scheme, which we don't reproduce — but the net image the
     * PalmOS HAL produces is an upright frame at the start of the
     * SRAM, so scan out from there.  (Non-rotated addressing is
     * modelled straightforwardly.)
     */
    if (s->regs[SED1376_REG_SPECIAL] & SED1376_SPECIAL_SWIVEL_MASK) {
        start = 0;
    }
    int bpp = sed1376_bpp(s);
    unsigned int x, y;
    uint32_t *dest;

    if (bpp < 0 || !width || !height) {
        return false;
    }

    if (width > 1024 || height > 1024) {
        return false;
    }

    surface = qemu_console_surface(s->con);
    if (width != surface_width(surface) || height != surface_height(surface)) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
    }
    if (surface_bits_per_pixel(surface) != 32) {
        return false;
    }

    dest = surface_data(surface);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            unsigned int byte = (start + y * stride + x * bpp / 8) %
                                SED1376_VMEM_SIZE;
            uint8_t b = vmem[byte];
            uint32_t pixel;

            switch (bpp) {
            case 1:
                pixel = s->lut[(b >> (7 - x % 8)) & 1];
                break;
            case 2:
                pixel = s->lut[(b >> (6 - x % 4 * 2)) & 3];
                break;
            case 4:
                pixel = s->lut[x % 2 ? b & 0xf : b >> 4];
                break;
            case 8:
                pixel = s->lut[b];
                break;
            default: {
                /* 16bpp RGB565, big-endian halfwords */
                uint16_t v = b << 8 |
                             vmem[(byte + 1) % SED1376_VMEM_SIZE];
                pixel = rgb_to_pixel32((v >> 11) << 3,
                                       ((v >> 5) & 0x3f) << 2,
                                       (v & 0x1f) << 3);
                break;
            }
            }
            *dest++ = pixel;
        }
    }

    qemu_console_update_full(s->con);
    return true;
}

static uint64_t sed1376_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    SED1376State *s = opaque;

    if (addr == SED1376_REG_VNDP_STATUS) {
        return s->regs[addr] | SED1376_VNDP_ACTIVE;
    }
    return s->regs[addr];
}

static void sed1376_reg_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    SED1376State *s = opaque;

    s->regs[addr] = value;

    if (addr == SED1376_REG_LUT_ADDR) {
        s->lut[value & 0xff] =
            rgb_to_pixel32(s->regs[SED1376_REG_LUT_RED],
                           s->regs[SED1376_REG_LUT_GREEN],
                           s->regs[SED1376_REG_LUT_BLUE]);
    }
}

static const MemoryRegionOps sed1376_reg_ops = {
    .read = sed1376_reg_read,
    .write = sed1376_reg_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void sed1376_invalidate_display(void *opaque)
{
}

static const GraphicHwOps sed1376_gfx_ops = {
    .invalidate = sed1376_invalidate_display,
    .gfx_update = sed1376_update_display,
};

static void sed1376_reset(DeviceState *dev)
{
    SED1376State *s = SED1376(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->lut, 0, sizeof(s->lut));
}

static void sed1376_realize(DeviceState *dev, Error **errp)
{
    SED1376State *s = SED1376(dev);

    memory_region_init_io(&s->regs_mr, OBJECT(dev), &sed1376_reg_ops, s,
                          TYPE_SED1376 ".regs", SED1376_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs_mr);

    memory_region_init_ram(&s->vmem_mr, OBJECT(dev), TYPE_SED1376 ".vram",
                           SED1376_VMEM_SIZE, &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->vmem_mr);

    s->con = qemu_graphic_console_create(dev, 0, &sed1376_gfx_ops, s);
}

static const VMStateDescription vmstate_sed1376 = {
    .name = TYPE_SED1376,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, SED1376State, SED1376_REGS_SIZE),
        VMSTATE_UINT32_ARRAY(lut, SED1376State, 256),
        VMSTATE_END_OF_LIST()
    }
};

static void sed1376_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sed1376_reset);
    dc->realize = sed1376_realize;
    dc->vmsd = &vmstate_sed1376;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo sed1376_info = {
    .name          = TYPE_SED1376,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SED1376State),
    .class_init    = sed1376_class_init,
};

static void sed1376_register_types(void)
{
    type_register_static(&sed1376_info);
}

type_init(sed1376_register_types)
