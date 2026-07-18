/*
 * MStar/SigmaStar GOP (Graphics Output Plane)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The GOP is the MStar RGB primary/overlay plane: it scans a linear framebuffer
 * (from DRAM, at a MIU-relative address) out to the display. The same block sits
 * at 0x1f246800 on the SSD20xD (mainline DRM "gop" plane, drivers/gpu/drm/mstar/
 * mstar_gop.c) and on mercury5 (the 70mai's UI is composited through it - SDK
 * gop_hal.lib HalGopUpdateBase/HalGopUpdateGwinParam), with the same register
 * layout, so it is modelled once here and instantiated per-SoC.
 *
 * Register layout (byte offsets within the window's register bank; the RIU is a
 * 16-bit lane bus at a 4-byte stride, so register N is stored at index N/4):
 *   0xc0 STRETCH_W   bits[11:0] = crtc_w >> 1
 *   0xc4 STRETCH_H   bits[11:0] = crtc_h
 *   0x200 WIN0       bit0 = enable, bits[7:4] = source pixel format
 *   0x204 WIN0_ADDRL framebuffer address, low 16 bits  (>> 4)
 *   0x208 WIN0_ADDRH framebuffer address, high 12 bits
 *   0x224 WIN0_PITCH stride, in 16-byte units
 * On SoCs whose GOP is one of several instances the "winoff" property gives the
 * byte offset of this window's bank within the mmio region.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "ui/pixel_ops.h"
#include "hw/display/mstar_gop.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

#define GOP_STRETCH_W       0xc0        /* bits 0-11 = crtc_w >> 1 */
#define GOP_STRETCH_H       0xc4        /* bits 0-11 = crtc_h */
#define GOP_WIN0            0x200       /* bit0 = enable, bits 4-7 = format */
#define GOP_WIN0_ADDRL      0x204       /* bits 0-15 */
#define GOP_WIN0_ADDRH      0x208       /* bits 0-11 */
#define GOP_WIN0_PITCH      0x224       /* bits 0-10, in addr_shift units */
#define GOP_ADDR_SHIFT      4

#define MSTAR_GOP_REFRESH_NS (NANOSECONDS_PER_SECOND / 60)

/* Read a 16-bit lane register at (window base + off). */
static inline uint16_t gop_reg(MStarGopState *s, unsigned off)
{
    return s->regs[(s->winoff + off) / 4];
}

static void mstar_gop_off(void *opaque)
{
    MStarGopState *s = opaque;

    qemu_set_irq(s->gop_irq, 0);
}

static void mstar_gop_vblank(void *opaque)
{
    MStarGopState *s = opaque;

    /*
     * The GOP/fbdev vsync (SSD20xD _sstar_FB_VsyncISR) has no status register -
     * it just bumps a counter and wakes a waitqueue - so pulse it once per frame
     * (the mst-intc is level-based, so assert it and lower it a short while
     * later). On SoCs that don't wire the line this is a harmless no-op.
     */
    qemu_set_irq(s->gop_irq, 1);
    timer_mod(s->gop_off, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 200 * 1000);
    timer_mod(s->vblank,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MSTAR_GOP_REFRESH_NS);
}

static uint64_t mstar_gop_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarGopState *s = opaque;

    if (addr / 4 >= s->regsize / 4) {
        return 0;
    }
    return s->regs[addr / 4];
}

static void mstar_gop_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MStarGopState *s = opaque;

    if (addr / 4 >= s->regsize / 4) {
        return;
    }
    s->regs[addr / 4] = val;
}

static const MemoryRegionOps mstar_gop_ops = {
    .read = mstar_gop_read,
    .write = mstar_gop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

/*
 * Convert one GOP source row to the console surface. The source pixel format is
 * the WIN0 format field (drm_color_to_gop): 0x1 = RGB565, 0x5 = ARGB8888,
 * 0x7 = ABGR8888 (the vendor mi_disp/SDL fbdev runs the primary plane as 32bpp).
 * Other 16bpp codes decode as RGB565.
 */
static void mstar_gop_draw_row(void *opaque, uint8_t *dst, const uint8_t *src,
                               int width, int deststep)
{
    MStarGopState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int bpp = surface_bits_per_pixel(surface);
    unsigned int bl = s->brightness;
    unsigned int fmt = (gop_reg(s, GOP_WIN0) >> 4) & 0xf;
    bool is32 = (fmt == 0x5 || fmt == 0x7);     /* ARGB8888 / ABGR8888 */
    bool bgr = (fmt == 0x7);

    while (width--) {
        uint8_t r, g, b;

        if (is32) {
            uint32_t px = ldl_le_p(src);
            uint8_t c0 = px & 0xff, c1 = (px >> 8) & 0xff, c2 = (px >> 16) & 0xff;

            if (bgr) {
                r = c0; g = c1; b = c2;
            } else {
                b = c0; g = c1; r = c2;
            }
            src += 4;
        } else {
            uint16_t px = lduw_le_p(src);

            r = ((px >> 11) & 0x1f) << 3;
            g = ((px >> 5) & 0x3f) << 2;
            b = (px & 0x1f) << 3;
            src += 2;
        }
        r = r * bl >> 8;
        g = g * bl >> 8;
        b = b * bl >> 8;
        switch (bpp) {
        case 16:
            *(uint16_t *)dst = rgb_to_pixel16(r, g, b);
            dst += 2;
            break;
        case 32:
            *(uint32_t *)dst = rgb_to_pixel32(r, g, b);
            dst += 4;
            break;
        default:
            return;
        }
    }
}

static void mstar_gop_invalidate(void *opaque)
{
    MStarGopState *s = opaque;

    s->invalidate = true;
}

/*
 * The Miyoo Mini's panel is mounted upside down, so the firmware writes the
 * framebuffer already rotated 180deg. Rotate the rendered surface back (a 180deg
 * rotation of a row-major image is a reversal of its pixels) when "flip" is set.
 */
static void mstar_gop_apply_flip(MStarGopState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    int w = surface_width(surface);
    int h = surface_height(surface);
    int bpp = (surface_bits_per_pixel(surface) + 7) / 8;
    int stride = surface_stride(surface);
    uint8_t *data = surface_data(surface);
    int n = w * h;
    int i;

    if (!s->flip) {
        return;
    }
    for (i = 0; i < n / 2; i++) {
        int j = n - 1 - i;
        uint8_t *a = data + (i / w) * stride + (i % w) * bpp;
        uint8_t *b = data + (j / w) * stride + (j % w) * bpp;
        uint8_t tmp[4];

        memcpy(tmp, a, bpp);
        memcpy(a, b, bpp);
        memcpy(b, tmp, bpp);
    }
    qemu_console_update(s->con, 0, 0, w, h);
}

static bool mstar_gop_gfx_update(void *opaque)
{
    MStarGopState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t win = gop_reg(s, GOP_WIN0);
    uint32_t w = (gop_reg(s, GOP_STRETCH_W) & 0xfff) << 1;
    uint32_t h = gop_reg(s, GOP_STRETCH_H) & 0xfff;
    uint32_t addrl = gop_reg(s, GOP_WIN0_ADDRL);
    uint32_t addrh = gop_reg(s, GOP_WIN0_ADDRH) & 0xfff;
    unsigned int fmt = (win >> 4) & 0xf;
    bool is32 = (fmt == 0x5 || fmt == 0x7);
    /*
     * The GOP address register holds the DRAM-relative (MIU) framebuffer address
     * (drm_gem_dma dma_addr = phys - MSTAR_DRAM_BASE), so add the DRAM base.
     */
    hwaddr fbaddr = MSTAR_DRAM_BASE +
                    (((hwaddr)((addrh << 16) | addrl)) << GOP_ADDR_SHIFT);
    uint32_t pitch = (gop_reg(s, GOP_WIN0_PITCH) & 0x7ff) << GOP_ADDR_SHIFT;
    unsigned int bl = s->backlight ? msc313_pwm_brightness(s->backlight, 0) : 256;
    int first = 0, last = 0;

    if (bl != s->brightness) {
        s->brightness = bl;
        s->invalidate = true;
    }
    if (s->flip) {
        s->invalidate = true;
    }

    /* Nothing to show until the plane is enabled and sized by the driver. */
    if (!(win & 1) || w == 0 || h == 0) {
        return true;
    }
    if (pitch == 0) {
        pitch = w * (is32 ? 4 : 2);
    }
    if (w != s->width || h != s->height) {
        qemu_console_resize(s->con, w, h);
        surface = qemu_console_surface(s->con);
        s->width = w;
        s->height = h;
        s->invalidate = true;
    }
    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection, get_system_memory(),
                                          fbaddr, h, pitch);
    }
    framebuffer_update_display(surface, &s->fbsection, w, h, pitch,
                               surface_stride(surface), 0, s->invalidate,
                               mstar_gop_draw_row, s, &first, &last);
    if (first >= 0) {
        qemu_console_update(s->con, 0, first, w, last - first + 1);
    }
    s->invalidate = false;
    mstar_gop_apply_flip(s);
    return true;
}

static const GraphicHwOps mstar_gop_gfx_ops = {
    .invalidate = mstar_gop_invalidate,
    .gfx_update = mstar_gop_gfx_update,
};

static void mstar_gop_reset_hold(Object *obj, ResetType type)
{
    MStarGopState *s = MSTAR_GOP(obj);

    memset(s->regs, 0, s->regsize / 4 * sizeof(uint16_t));
    s->width = 0;
    s->height = 0;
    s->brightness = 256;
    s->invalidate = true;
    qemu_set_irq(s->gop_irq, 0);
}

static void mstar_gop_realize(DeviceState *dev, Error **errp)
{
    MStarGopState *s = MSTAR_GOP(dev);

    if (s->regsize < 0x400) {
        s->regsize = 0x400;
    }
    if (s->winoff + 0x400 > s->regsize) {
        error_setg(errp, "mstar-gop: winoff 0x%x outside region 0x%x",
                   s->winoff, s->regsize);
        return;
    }
    s->regs = g_malloc0(s->regsize / 4 * sizeof(uint16_t));
    s->nregs = s->regsize / 4;

    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_gop_ops, s,
                          "mstar.gop", s->regsize);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->gop_irq);

    s->con = qemu_graphic_console_create(dev, 0, &mstar_gop_gfx_ops, s);
    s->gop_off = timer_new_ns(QEMU_CLOCK_VIRTUAL, mstar_gop_off, s);
    s->vblank = timer_new_ns(QEMU_CLOCK_VIRTUAL, mstar_gop_vblank, s);
    timer_mod(s->vblank,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MSTAR_GOP_REFRESH_NS);
}

static const Property mstar_gop_props[] = {
    DEFINE_PROP_UINT32("regsize", MStarGopState, regsize, 0x400),
    DEFINE_PROP_UINT32("winoff", MStarGopState, winoff, 0),
    DEFINE_PROP_BOOL("flip", MStarGopState, flip, false),
    DEFINE_PROP_LINK("backlight", MStarGopState, backlight,
                     TYPE_MSC313_PWM, Msc313PwmState *),
};

/*
 * The framebuffer MemoryRegionSection and the QemuConsole are not migrated:
 * post_load just flags a full invalidate so the next gfx_update re-derives
 * the section from the migrated registers and repaints.
 */
static int mstar_gop_post_load(void *opaque, int version_id)
{
    MStarGopState *s = opaque;

    s->invalidate = true;
    return 0;
}

static const VMStateDescription vmstate_mstar_gop = {
    .name = "mstar-gop",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mstar_gop_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_VARRAY_UINT32(regs, MStarGopState, nregs, 0,
                              vmstate_info_uint16, uint16_t),
        VMSTATE_UINT32(width, MStarGopState),
        VMSTATE_UINT32(height, MStarGopState),
        VMSTATE_TIMER_PTR(vblank, MStarGopState),
        VMSTATE_TIMER_PTR(gop_off, MStarGopState),
        VMSTATE_END_OF_LIST()
    },
};

static void mstar_gop_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_gop_realize;
    device_class_set_props(dc, mstar_gop_props);
    rc->phases.hold = mstar_gop_reset_hold;
    dc->vmsd = &vmstate_mstar_gop;
}

static const TypeInfo mstar_gop_types[] = {
    {
        .name           = TYPE_MSTAR_GOP,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarGopState),
        .class_init     = mstar_gop_class_init,
    },
};

DEFINE_TYPES(mstar_gop_types)
