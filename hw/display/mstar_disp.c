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
#include "ui/pixel_ops.h"
#include "hw/arm/mstar.h"
#include "trace.h"

/* --------------------------------------------------------- disp (top/vsync) */

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

/*
 * GE - the SoC 2D graphics engine (a bitblt/fill/line accelerator) at
 * 0x1f281200. MI_GFX/MainUI composites its UI through it, so nothing appears on
 * screen until the blits actually run. For now this stores the 16-bit registers
 * and reports the engine idle; the command-queue playback and the blit executor
 * are built on top of this. Register map: linux-chenxing ip/ge.md.
 */
#define GE_STATUS       0x1c        /* GE_WaitIdle polls bits[7:3] for 0x10 */
/*
 * Idle status: bit7 = CMDQ FIFO empty (GE_WaitIdle wants bits[7:3]==0x10),
 * bit0 = not busy, bits[15:11] = free CMDQ slots (GE_WaitCmdQAvail waits until
 * (status>>11) >= the number of commands it wants to push - report the FIFO as
 * fully free so it never blocks).
 */
#define GE_STATUS_IDLE  0xf880

/* GE pixel-format codes (ip/ge.md b_fm field) we handle. */
#define GE_FMT_RGB565   0x8
#define GE_FMT_ARGB1555 0x9
#define GE_FMT_ARGB4444 0xa
#define GE_FMT_ARGB8888 0xf

static unsigned ge_bpp(unsigned fmt)
{
    return fmt == GE_FMT_ARGB8888 ? 4 : 2;   /* the formats MainUI uses */
}

/* Decode one source pixel to 0xAARRGGBB. */
static uint32_t ge_to_argb(unsigned fmt, uint32_t px)
{
    unsigned a, r, g, b;

    switch (fmt) {
    case GE_FMT_ARGB8888:
        return px;                            /* stored B,G,R,A LE == ARGB32 */
    case GE_FMT_RGB565:
        r = (px >> 11) & 0x1f; g = (px >> 5) & 0x3f; b = px & 0x1f;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
        return 0xff000000 | (r << 16) | (g << 8) | b;
    case GE_FMT_ARGB1555:
        a = (px & 0x8000) ? 0xff : 0;
        r = (px >> 10) & 0x1f; g = (px >> 5) & 0x1f; b = px & 0x1f;
        r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
        return (a << 24) | (r << 16) | (g << 8) | b;
    case GE_FMT_ARGB4444:
        a = (px >> 12) & 0xf; r = (px >> 8) & 0xf; g = (px >> 4) & 0xf; b = px & 0xf;
        return ((a * 0x11) << 24) | ((r * 0x11) << 16) | ((g * 0x11) << 8) | (b * 0x11);
    default:
        return px;
    }
}

/* Encode 0xAARRGGBB into a destination pixel. */
static uint32_t ge_from_argb(unsigned fmt, uint32_t argb)
{
    unsigned a = (argb >> 24) & 0xff, r = (argb >> 16) & 0xff;
    unsigned g = (argb >> 8) & 0xff, b = argb & 0xff;

    switch (fmt) {
    case GE_FMT_ARGB8888:
        return argb;
    case GE_FMT_RGB565:
        return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
    case GE_FMT_ARGB1555:
        return (a ? 0x8000 : 0) | ((r & 0xf8) << 7) | ((g & 0xf8) << 2) | (b >> 3);
    case GE_FMT_ARGB4444:
        return ((a & 0xf0) << 8) | ((r & 0xf0) << 4) | (g & 0xf0) | (b >> 4);
    default:
        return argb;
    }
}

/*
 * Execute a bitblt (cmd 0x180 bit6). MI_GFX programs the GE with a source and
 * destination surface (MIU addresses, so physical = DRAM base + reg), pitches,
 * pixel formats and a destination rectangle whose size is srcw x srch. Copy the
 * pixels, converting the format, so MainUI's composited UI actually lands in the
 * framebuffer the GOP scans out. Alpha blending/ROP/stretch are not modelled yet.
 */
static void msc313_disp_ge_bitblt(Msc313DispState *s)
{
    uint16_t *r = s->geregs;
    uint32_t src = MSTAR_DRAM_BASE + (((uint32_t)r[0x84 / 4] << 16) | r[0x80 / 4]);
    uint32_t dst = MSTAR_DRAM_BASE + (((uint32_t)r[0x9c / 4] << 16) | r[0x98 / 4]);
    unsigned spit = r[0xc0 / 4], dpit = r[0xcc / 4];
    unsigned sfmt = r[0xd0 / 4] & 0xf, dfmt = (r[0xd0 / 4] >> 8) & 0xf;
    unsigned w = r[0x1b8 / 4], h = r[0x1bc / 4];
    unsigned rot = r[0x164 / 4] & 3;      /* 0/1/2/3 = 0/90/180/270 degrees */
    unsigned sbpp = ge_bpp(sfmt), dbpp = ge_bpp(dfmt);
    /*
     * The x0/y0 (v0) registers hold the destination rectangle's BOTTOM-RIGHT
     * corner, so the top-left position is (x0-(w-1), y0-(h-1)) - e.g. a
     * full-screen 640x480 blit programs (639,479) = position (0,0). MainUI
     * blits to the framebuffer with a 180-degree rotation (reg 0x164) to
     * pre-rotate for the upside-down panel, so honour that (reverse rows and
     * columns). 90/270-degree rotation is not modelled.
     */
    int dx = (int)r[0x1a0 / 4] - (int)w + 1;
    int dy = (int)r[0x1a4 / 4] - (int)h + 1;

    if (w == 0 || h == 0 || w > 4096 || h > 4096 || dx < 0 || dy < 0) {
        return;
    }

    for (unsigned row = 0; row < h; row++) {
        uint8_t sbuf[4096 * 4], dbuf[4096 * 4];
        unsigned orow = rot == 2 ? h - 1 - row : row;
        uint32_t srow = src + row * spit;
        uint32_t drow = dst + (dy + orow) * dpit + dx * dbpp;

        if (address_space_read(&address_space_memory, srow, MEMTXATTRS_UNSPECIFIED,
                               sbuf, w * sbpp) != MEMTX_OK) {
            return;
        }
        for (unsigned col = 0; col < w; col++) {
            uint32_t px = sbpp == 4 ? ldl_le_p(sbuf + col * 4)
                                    : lduw_le_p(sbuf + col * 2);
            uint32_t out = ge_from_argb(dfmt, ge_to_argb(sfmt, px));
            unsigned ocol = rot == 2 ? w - 1 - col : col;

            if (dbpp == 4) {
                stl_le_p(dbuf + ocol * 4, out);
            } else {
                stw_le_p(dbuf + ocol * 2, out);
            }
        }
        address_space_write(&address_space_memory, drow, MEMTXATTRS_UNSPECIFIED,
                            dbuf, w * dbpp);
    }
    /*
     * The blit lands in DRAM; the GOP device that scans that framebuffer out
     * picks the change up through the framebuffer dirty-page tracking, so there
     * is nothing to flag here.
     */
}

static uint64_t msc313_disp_ge_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313DispState *s = opaque;
    uint16_t val = s->geregs[addr / 4];

    if (addr == GE_STATUS) {
        val = GE_STATUS_IDLE;
    }
    mstar_iolog(MSTAR_DISP_GE_BASE + addr, false, val, size);
    return val;
}

static void msc313_disp_ge_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    Msc313DispState *s = opaque;

    mstar_iolog(MSTAR_DISP_GE_BASE + addr, true, val, size);
    s->geregs[addr / 4] = val;

    /* cmd register: bit6 kicks a bitblt. */
    if (addr == 0x180 && (val & 0x40)) {
        msc313_disp_ge_bitblt(s);
    }
}

static const MemoryRegionOps msc313_disp_ge_ops = {
    .read = msc313_disp_ge_read,
    .write = msc313_disp_ge_write,
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

    /*
     * The RGB primary plane is scanned out by the standalone TYPE_MSTAR_GOP
     * device (its own console). This "disp" console shows the mopg overlay
     * (video) plane: the vendor u-boot bootlogo decodes its JPEG splash into a
     * YUV420 surface and points this plane at it. Scan it out while enabled.
     */
    if (s->mopregs[MOP_REG(MOP_WIN0, MOP_WIN_EN)] & 1) {
        msc313_disp_scanout_mop(s);
        msc313_disp_apply_flip(s);
    }
    return true;
}

static const GraphicHwOps msc313_disp_gfx_ops = {
    .gfx_update = msc313_disp_gfx_update,
};

static void msc313_disp_reset_hold(Object *obj, ResetType type)
{
    Msc313DispState *s = MSC313_DISP(obj);

    memset(s->topregs, 0, sizeof(s->topregs));
    memset(s->mopregs, 0, sizeof(s->mopregs));
    memset(s->dsiregs, 0, sizeof(s->dsiregs));
    memset(s->geregs, 0, sizeof(s->geregs));
    s->width = 0;
    s->height = 0;
    s->topregs[TOP_VSYNC_MASK / 4] = TOP_VSYNC_BIT;     /* vblank starts masked */
    qemu_set_irq(s->irq, 0);
}

static void msc313_disp_realize(DeviceState *dev, Error **errp)
{
    Msc313DispState *s = MSC313_DISP(dev);

    memory_region_init_io(&s->top, OBJECT(dev), &msc313_disp_top_ops, s,
                          "mstar.disp-top", MSTAR_DISP_TOP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->top);
    memory_region_init_io(&s->mop, OBJECT(dev), &msc313_disp_mop_ops, s,
                          "mstar.disp-mop", MSTAR_DISP_MOP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mop);
    memory_region_init_io(&s->dsi, OBJECT(dev), &msc313_disp_dsi_ops, s,
                          "mstar.disp-dsi", MSTAR_DISP_DSI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->dsi);
    memory_region_init_io(&s->ge, OBJECT(dev), &msc313_disp_ge_ops, s,
                          "mstar.disp-ge", MSTAR_DISP_GE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->ge);
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
