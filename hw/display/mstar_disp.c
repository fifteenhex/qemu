/*
 * MStar/SigmaStar SSD20xD display pipeline
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/loader.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "ui/pixel_ops.h"
#include "hw/arm/mstar.h"
#include "trace.h"

/* ------------------------------------------------------------- disp (GOP) */

/* gop1 primary-plane registers (drivers/gpu/drm/mstar/mstar_gop.c). */
#define GOP_STRETCH_W       0xc0        /* bits 0-11 = crtc_w >> 1 */
#define GOP_STRETCH_H       0xc4        /* bits 0-11 = crtc_h */
#define GOP_WIN0            0x200       /* bit0 = enable, bits 4-7 = format */
#define GOP_WIN0_ADDRL      0x204       /* bits 0-15 */
#define GOP_WIN0_ADDRH      0x208       /* bits 0-11 */
#define GOP_WIN0_PITCH      0x224       /* bits 0-10, in addr_shift units */
#define GOP_ADDR_SHIFT      4

/* display-top registers (drivers/gpu/drm/mstar/mstar_top.c). */
#define TOP_VSYNC_FLAG      0x08        /* bit3, write-1-to-clear */
#define TOP_VSYNC_MASK      0x0c        /* bit3, 0 = vblank interrupt enabled */
#define TOP_VSYNC_BIT       (1 << 3)

/*
 * mopg overlay-plane window 0 (drivers/gpu/drm/mstar/mstar_mop.c). The video
 * planes carry YUV420 (semi-planar NV12): a luma plane plus a half-resolution
 * interleaved chroma plane. Windows start at 0x200 with a 0x40 stride.
 */
#define MOP_WIN0            0x200
#define MOP_WIN_EN          0x00        /* bit0 = enable */
#define MOP_WIN_YADDRL      0x08        /* luma addr, low 16 bits */
#define MOP_WIN_YADDRH      0x0c        /* luma addr, high 12 bits */
#define MOP_WIN_CADDRL      0x10        /* chroma addr, low 16 bits */
#define MOP_WIN_CADDRH      0x14        /* chroma addr, high 12 bits */
#define MOP_WIN_PITCH       0x28        /* luma stride, in 16-byte units */
#define MOP_WIN_SRCW        0x2c        /* source width - 1 */
#define MOP_WIN_SRCH        0x30        /* source height - 1 */
#define MOP_ADDR_SHIFT      4           /* addresses/pitch are >> 4 */
#define MOP_REG(win, r)     (((win) + (r)) / 4)

/*
 * MIPI DSI controller (drivers/gpu/drm/mstar/mstar_dsi.c). This is a MediaTek
 * DSI clone; the register map matches mtk_dsi/mstar_dsi. The layout below was
 * confirmed against the running Miyoo Mini firmware, whose panel bring-up
 * programs a 640x480 24bpp RGB888 video-mode panel:
 *
 *   0x00 START      bit0 = kick the queued command-mode packet
 *   0x0c INTSTA     bit1 = CMD_DONE (polled), bit2 = TE_RDY, bit31 = BUSY
 *   0x10 CON_CTRL   bit0 DSI_RESET, bit1 DSI_EN, bit2 DPHY_RESET
 *   0x14 MODE_CTRL  0 = command mode, 1 = sync-pulse video, 2/3 = event/burst
 *   0x1c PSCTRL     [13:0] word count = width*bpp, [17:16] pixel select
 *                   (3 = packed 24bit RGB888)
 *   0x20-0x2c       VSA/VBP/VFP/VACT line counts (video mode)
 *   0x50-0x58       HSA/HBP/HFP word counts
 *   0x60 CMDQ_SIZE  [5:0] number of 32-bit CMDQ words in the queued packet
 *   0x104-0x11c     D-PHY LCCON + HS lane TIMECON0..3 (in-controller PHY)
 *   0x200 CMDQ0..   command queue: word0 = MIPI packet header
 *                   (config | data_id<<8 | data0<<16 | data1<<24); for a long
 *                   packet data0|data1<<8 is the payload word count and the
 *                   bytes follow in CMDQ1.. .
 *
 * The queued packet is sent when the guest writes START; hardware streams it
 * over the D-PHY and raises CMD_DONE. We have no panel, so complete instantly.
 * Each kicked packet is decoded to a trace event (msc313_dsi_cmd) so a driver's
 * panel init sequence is self-documenting.
 */
#define DSI_START           0x00        /* bit0 = kick the queued command */
#define DSI_INTSTA          0x0c        /* interrupt status (polled) */
#define DSI_CMD_DONE_FLAG   (1 << 1)
#define DSI_TE_RDY_FLAG     (1 << 2)
#define DSI_BUSY_FLAG       (1u << 31)
#define DSI_MODE_CTRL       0x14
#define DSI_CMDQ_SIZE       0x60        /* [5:0] queued packet word count */
#define DSI_CMDQ            0x200        /* command queue base */

#define MSTAR_DISP_REFRESH_NS (NANOSECONDS_PER_SECOND / 60)

static void msc313_disp_update_irq(Msc313DispState *s)
{
    bool flag = s->topregs[TOP_VSYNC_FLAG / 4] & TOP_VSYNC_BIT;
    bool masked = s->topregs[TOP_VSYNC_MASK / 4] & TOP_VSYNC_BIT;

    qemu_set_irq(s->irq, flag && !masked);
}

static void msc313_disp_vblank(void *opaque)
{
    Msc313DispState *s = opaque;

    /* Pulse the vsync interrupt each frame; the handler acks it (W1C). */
    if (!(s->topregs[TOP_VSYNC_MASK / 4] & TOP_VSYNC_BIT)) {
        s->topregs[TOP_VSYNC_FLAG / 4] |= TOP_VSYNC_BIT;
        msc313_disp_update_irq(s);
    }
    timer_mod(s->vblank,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MSTAR_DISP_REFRESH_NS);
}

static uint64_t msc313_disp_gop_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313DispState *s = opaque;

    mstar_iolog(MSTAR_DISP_GOP_BASE + addr, false, s->gopregs[addr / 4], size);
    return s->gopregs[addr / 4];
}

static void msc313_disp_gop_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    Msc313DispState *s = opaque;

    mstar_iolog(MSTAR_DISP_GOP_BASE + addr, true, val, size);
    s->gopregs[addr / 4] = val;
}

static uint64_t msc313_disp_top_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313DispState *s = opaque;

    mstar_iolog(MSTAR_DISP_TOP_BASE + addr, false, s->topregs[addr / 4], size);
    return s->topregs[addr / 4];
}

static void msc313_disp_top_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    Msc313DispState *s = opaque;

    mstar_iolog(MSTAR_DISP_TOP_BASE + addr, true, val, size);
    if (addr == TOP_VSYNC_FLAG) {
        s->topregs[TOP_VSYNC_FLAG / 4] &= ~(uint16_t)val;   /* write-1-clear */
        msc313_disp_update_irq(s);
        return;
    }
    s->topregs[addr / 4] = val;
    if (addr == TOP_VSYNC_MASK) {
        msc313_disp_update_irq(s);
    }
}

static const MemoryRegionOps msc313_disp_gop_ops = {
    .read = msc313_disp_gop_read,
    .write = msc313_disp_gop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps msc313_disp_top_ops = {
    .read = msc313_disp_top_read,
    .write = msc313_disp_top_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static uint64_t msc313_disp_mop_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313DispState *s = opaque;

    mstar_iolog(MSTAR_DISP_MOP_BASE + addr, false, s->mopregs[addr / 4], size);
    return s->mopregs[addr / 4];
}

static void msc313_disp_mop_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    Msc313DispState *s = opaque;

    mstar_iolog(MSTAR_DISP_MOP_BASE + addr, true, val, size);
    s->mopregs[addr / 4] = val;
}

static const MemoryRegionOps msc313_disp_mop_ops = {
    .read = msc313_disp_mop_read,
    .write = msc313_disp_mop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static uint64_t msc313_disp_dsi_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313DispState *s = opaque;

    mstar_iolog(MSTAR_DISP_DSI_BASE + addr, false, s->dsiregs[addr / 4], size);
    return s->dsiregs[addr / 4];
}

/* Decode the queued command-mode packet to a trace event. */
static void msc313_disp_dsi_decode(Msc313DispState *s)
{
    uint32_t hdr = s->dsiregs[DSI_CMDQ / 4];
    unsigned int data_id = (hdr >> 8) & 0xff;
    unsigned int data0 = (hdr >> 16) & 0xff;
    unsigned int data1 = (hdr >> 24) & 0xff;
    bool is_long = (data_id & 0x0f) == 0x09;    /* DT[3:0]=9: long packet */

    if (is_long) {
        unsigned int wc = data0 | (data1 << 8);
        /* First payload byte (in CMDQ1) is the DCS command for a DCS write. */
        unsigned int cmd = s->dsiregs[(DSI_CMDQ + 4) / 4] & 0xff;

        trace_msc313_dsi_cmd("long", data_id, cmd, wc);
    } else {
        /* Short packet: data0 = DCS command, data1 = parameter. */
        trace_msc313_dsi_cmd("short", data_id, data0, data1);
    }
}

static void msc313_disp_dsi_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    Msc313DispState *s = opaque;

    mstar_iolog(MSTAR_DISP_DSI_BASE + addr, true, val, size);
    s->dsiregs[addr / 4] = val;
    /*
     * Kicking DSI_START runs the queued command-mode packet. On real hardware
     * this streams over the D-PHY to the panel and raises CMD_DONE when the
     * transfer finishes; we have no panel, so complete it instantly and clear
     * BUSY. The vendor u-boot/kernel poll DSI_INTSTA for CMD_DONE (otherwise
     * they spin until "CMD Done Time Out").
     */
    if (addr == DSI_START && (val & 1)) {
        msc313_disp_dsi_decode(s);
        s->dsiregs[DSI_INTSTA / 4] =
            (s->dsiregs[DSI_INTSTA / 4] | DSI_CMD_DONE_FLAG) & ~DSI_BUSY_FLAG;
    }
    /*
     * In video mode the controller free-runs, so a tear-effect poll must make
     * progress: latch TE_RDY whenever the driver selects a display mode.
     */
    if (addr == DSI_MODE_CTRL) {
        s->dsiregs[DSI_INTSTA / 4] |= DSI_TE_RDY_FLAG;
    }
}

static const MemoryRegionOps msc313_disp_dsi_ops = {
    .read = msc313_disp_dsi_read,
    .write = msc313_disp_dsi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * Convert one GOP source row to the console surface. The source pixel format is
 * taken from the WIN0 format field (drm_color_to_gop): 0x1 = RGB565 (mainline
 * DRM fbcon), 0x5 = ARGB8888, 0x7 = ABGR8888 (the vendor mi_disp/SDL fbdev runs
 * the primary plane as 32bpp ARGB8888). Other 16bpp codes decode as RGB565.
 */
static void msc313_disp_draw_row(void *opaque, uint8_t *dst, const uint8_t *src,
                                 int width, int deststep)
{
    Msc313DispState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int bpp = surface_bits_per_pixel(surface);
    unsigned int bl = s->brightness;
    unsigned int fmt = (s->gopregs[GOP_WIN0 / 4] >> 4) & 0xf;
    bool is32 = (fmt == 0x5 || fmt == 0x7);     /* ARGB8888 / ABGR8888 */
    bool bgr = (fmt == 0x7);

    while (width--) {
        uint8_t r, g, b;

        if (is32) {
            uint32_t px = ldl_le_p(src);
            uint8_t c0 = px & 0xff, c1 = (px >> 8) & 0xff, c2 = (px >> 16) & 0xff;

            /* ARGB8888 stores B,G,R,A in memory; ABGR8888 stores R,G,B,A. */
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

static void msc313_disp_invalidate(void *opaque)
{
    Msc313DispState *s = opaque;

    s->invalidate = true;
}

static inline uint8_t msc313_clamp_u8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/*
 * Scan out the mopg overlay plane. The vendor u-boot bootlogo decodes its JPEG
 * splash into a semi-planar YUV420 (NV12) surface in DRAM and points this plane
 * at it. We read the luma and chroma planes and convert to the console surface;
 * with no real panel there is no backlight PWM at this stage, so full
 * brightness is used (unlike the GOP path, which the kernel dims via the PWM).
 */
static void msc313_disp_scanout_mop(Msc313DispState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t yl = s->mopregs[MOP_REG(MOP_WIN0, MOP_WIN_YADDRL)];
    uint32_t yh = s->mopregs[MOP_REG(MOP_WIN0, MOP_WIN_YADDRH)] & 0xfff;
    uint32_t cl = s->mopregs[MOP_REG(MOP_WIN0, MOP_WIN_CADDRL)];
    uint32_t ch = s->mopregs[MOP_REG(MOP_WIN0, MOP_WIN_CADDRH)] & 0xfff;
    uint32_t w = (s->mopregs[MOP_REG(MOP_WIN0, MOP_WIN_SRCW)] & 0xfff) + 1;
    uint32_t h = (s->mopregs[MOP_REG(MOP_WIN0, MOP_WIN_SRCH)] & 0xfff) + 1;
    uint32_t pitch = (s->mopregs[MOP_REG(MOP_WIN0, MOP_WIN_PITCH)] & 0x1fff)
                     << MOP_ADDR_SHIFT;
    hwaddr yaddr = MSTAR_DRAM_BASE +
                   (((hwaddr)((yh << 16) | yl)) << MOP_ADDR_SHIFT);
    hwaddr caddr = MSTAR_DRAM_BASE +
                   (((hwaddr)((ch << 16) | cl)) << MOP_ADDR_SHIFT);
    uint8_t *yplane, *cplane, *dst;
    int bpp;
    uint32_t x, y;

    if (w == 0 || h == 0) {
        return;
    }
    if (pitch == 0) {
        pitch = w;
    }
    if (w != s->width || h != s->height) {
        qemu_console_resize(s->con, w, h);
        surface = qemu_console_surface(s->con);
        s->width = w;
        s->height = h;
    }
    bpp = surface_bits_per_pixel(surface);
    if (bpp != 16 && bpp != 32) {
        return;
    }

    yplane = g_malloc(pitch * h);
    cplane = g_malloc(pitch * (h / 2));
    address_space_read(&address_space_memory, yaddr, MEMTXATTRS_UNSPECIFIED,
                       yplane, pitch * h);
    address_space_read(&address_space_memory, caddr, MEMTXATTRS_UNSPECIFIED,
                       cplane, pitch * (h / 2));

    dst = surface_data(surface);
    for (y = 0; y < h; y++) {
        const uint8_t *yr = yplane + y * pitch;
        const uint8_t *cr = cplane + (y / 2) * pitch;
        uint8_t *d = dst + y * surface_stride(surface);

        for (x = 0; x < w; x++) {
            int yy = yr[x];
            int uu = cr[x & ~1u] - 128;         /* Cb */
            int vv = cr[(x & ~1u) + 1] - 128;   /* Cr */
            /* Full-range (JFIF) BT.601 YCbCr -> RGB. */
            uint8_t r = msc313_clamp_u8(yy + ((91881 * vv) >> 16));
            uint8_t g = msc313_clamp_u8(yy -
                            ((22554 * uu + 46802 * vv) >> 16));
            uint8_t b = msc313_clamp_u8(yy + ((116130 * uu) >> 16));

            if (bpp == 16) {
                *(uint16_t *)d = rgb_to_pixel16(r, g, b);
                d += 2;
            } else {
                *(uint32_t *)d = rgb_to_pixel32(r, g, b);
                d += 4;
            }
        }
    }
    g_free(yplane);
    g_free(cplane);
    qemu_console_update(s->con, 0, 0, w, h);
}

/*
 * The Miyoo Mini's panel is mounted upside down, so the firmware writes the
 * framebuffer already rotated 180 degrees. Rotate the rendered surface back so
 * a screendump shows what is physically on the panel (a 180 rotation of a
 * row-major image is just a reversal of its pixels).
 */
static void msc313_disp_apply_flip(Msc313DispState *s)
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

static bool msc313_disp_gfx_update(void *opaque)
{
    Msc313DispState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t win = s->gopregs[GOP_WIN0 / 4];
    uint32_t w = (s->gopregs[GOP_STRETCH_W / 4] & 0xfff) << 1;
    uint32_t h = s->gopregs[GOP_STRETCH_H / 4] & 0xfff;
    uint32_t addrl = s->gopregs[GOP_WIN0_ADDRL / 4];
    uint32_t addrh = s->gopregs[GOP_WIN0_ADDRH / 4] & 0xfff;
    unsigned int fmt = (win >> 4) & 0xf;
    bool is32 = (fmt == 0x5 || fmt == 0x7);
    /*
     * The GOP address register holds the DRAM-relative (MIU) address of the
     * framebuffer (drm_gem_dma dma_addr, i.e. phys - MSTAR_DRAM_BASE), so add
     * the DRAM base to reach it in system memory.
     */
    hwaddr fbaddr = MSTAR_DRAM_BASE +
                    (((hwaddr)((addrh << 16) | addrl)) << GOP_ADDR_SHIFT);
    uint32_t pitch = (s->gopregs[GOP_WIN0_PITCH / 4] & 0x7ff) << GOP_ADDR_SHIFT;
    unsigned int bl = s->backlight ? msc313_pwm_brightness(s->backlight, 0) : 256;
    int first = 0, last = 0;

    /* A backlight change needs a full redraw even if the framebuffer didn't. */
    if (bl != s->brightness) {
        s->brightness = bl;
        s->invalidate = true;
    }
    /* The flip rotates the whole surface, so it needs the full frame each time. */
    if (s->flip) {
        s->invalidate = true;
    }

    /*
     * The vendor u-boot bootlogo drives the MOP video plane (YUV420); the
     * mainline DRM fbcon drives the GOP plane (RGB). Scan out whichever is
     * enabled, MOP taking priority when it is active.
     */
    if (s->mopregs[MOP_REG(MOP_WIN0, MOP_WIN_EN)] & 1) {
        msc313_disp_scanout_mop(s);
        msc313_disp_apply_flip(s);
        return true;
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
                               msc313_disp_draw_row, s, &first, &last);
    if (first >= 0) {
        qemu_console_update(s->con, 0, first, w, last - first + 1);
    }
    s->invalidate = false;
    msc313_disp_apply_flip(s);
    return true;
}

static const GraphicHwOps msc313_disp_gfx_ops = {
    .invalidate = msc313_disp_invalidate,
    .gfx_update = msc313_disp_gfx_update,
};

static void msc313_disp_reset_hold(Object *obj, ResetType type)
{
    Msc313DispState *s = MSC313_DISP(obj);

    memset(s->gopregs, 0, sizeof(s->gopregs));
    memset(s->topregs, 0, sizeof(s->topregs));
    memset(s->mopregs, 0, sizeof(s->mopregs));
    memset(s->dsiregs, 0, sizeof(s->dsiregs));
    s->width = 0;
    s->height = 0;
    s->brightness = 256;
    s->invalidate = true;
    s->topregs[TOP_VSYNC_MASK / 4] = TOP_VSYNC_BIT;     /* vblank starts masked */
    qemu_set_irq(s->irq, 0);
}

static void msc313_disp_realize(DeviceState *dev, Error **errp)
{
    Msc313DispState *s = MSC313_DISP(dev);

    memory_region_init_io(&s->gop, OBJECT(dev), &msc313_disp_gop_ops, s,
                          "mstar.disp-gop", MSTAR_DISP_GOP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->gop);
    memory_region_init_io(&s->top, OBJECT(dev), &msc313_disp_top_ops, s,
                          "mstar.disp-top", MSTAR_DISP_TOP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->top);
    memory_region_init_io(&s->mop, OBJECT(dev), &msc313_disp_mop_ops, s,
                          "mstar.disp-mop", MSTAR_DISP_MOP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mop);
    memory_region_init_io(&s->dsi, OBJECT(dev), &msc313_disp_dsi_ops, s,
                          "mstar.disp-dsi", MSTAR_DISP_DSI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->dsi);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->con = qemu_graphic_console_create(dev, 0, &msc313_disp_gfx_ops, s);
    s->vblank = timer_new_ns(QEMU_CLOCK_VIRTUAL, msc313_disp_vblank, s);
    timer_mod(s->vblank,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MSTAR_DISP_REFRESH_NS);
}

static void msc313_disp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313_disp_realize;
    rc->phases.hold = msc313_disp_reset_hold;
}

static const TypeInfo mstar_disp_types[] = {
    {
        .name           = TYPE_MSC313_DISP,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313DispState),
        .class_init     = msc313_disp_class_init,
    },
};

DEFINE_TYPES(mstar_disp_types)
