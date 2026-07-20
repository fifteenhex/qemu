/*
 * MStar/SigmaStar GE (2D graphics engine)
 *
 * The GE is a bitblt/fill/line accelerator at 0x1f281200. The MI_GFX
 * middleware and MainUI composite the on-screen UI through it, so nothing
 * appears on the panel until its blits run: MI_GFX programs a source and
 * destination surface, kicks the engine and waits for it to go idle.
 *
 * This models the completion status (the engine always reads back idle, with
 * its command FIFO reported empty and fully free so the driver's wait loops
 * never block) and executes bitblt copies so the composited UI actually lands
 * in the framebuffer the GOP scans out. Register map: linux-chenxing ip/ge.md.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/display/mstar_ge.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"

/*
 * Status register. GE_WaitIdle polls bits[7:3] for 0x10; GE_WaitCmdQAvail
 * waits until (status >> 11) is at least the number of commands it wants to
 * push. Report bit7 (CMDQ FIFO empty), bit0 (not busy) and a full set of free
 * FIFO slots [15:11] so neither wait ever blocks.
 */
#define GE_STATUS       0x1c
#define GE_STATUS_IDLE  0xf880

/* Command register: bit6 kicks a bitblt */
#define GE_CMD          0x180
#define GE_CMD_BITBLT   (1 << 6)

/* Source/destination surface base addresses (MIU offsets), low/high halves */
#define GE_SRC_ADDR_L   0x80
#define GE_SRC_ADDR_H   0x84
#define GE_DST_ADDR_L   0x98
#define GE_DST_ADDR_H   0x9c
/* Source/destination pitch, in bytes */
#define GE_SRC_PITCH    0xc0
#define GE_DST_PITCH    0xcc
/* Pixel formats: source in bits[3:0], destination in bits[11:8] */
#define GE_FMT          0xd0
/* Rotation in bits[1:0]: 0/1/2/3 = 0/90/180/270 degrees */
#define GE_ROTATE       0x164
/* Destination rectangle bottom-right corner (v0) */
#define GE_DST_X0       0x1a0
#define GE_DST_Y0       0x1a4
/* Blit width/height (the source rectangle size) */
#define GE_WIDTH        0x1b8
#define GE_HEIGHT       0x1bc

/* Pixel-format codes (ip/ge.md b_fm field) MainUI uses */
#define GE_FMT_RGB565   0x8
#define GE_FMT_ARGB1555 0x9
#define GE_FMT_ARGB4444 0xa
#define GE_FMT_ARGB8888 0xf

#define GE_ROTATE_180   2
#define GE_MAX_DIM      4096

static unsigned ge_bpp(unsigned fmt)
{
    return fmt == GE_FMT_ARGB8888 ? 4 : 2;
}

/* Decode one source pixel to 0xAARRGGBB */
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
        return ((a * 0x11) << 24) | ((r * 0x11) << 16) | ((g * 0x11) << 8) |
               (b * 0x11);
    default:
        return px;
    }
}

/* Encode 0xAARRGGBB into a destination pixel */
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
 * Execute a bitblt. MI_GFX programs a source and destination surface (MIU
 * addresses, so physical = DRAM base + reg), pitches, pixel formats and a
 * destination rectangle whose size is width x height. Copy the pixels,
 * converting the format, so the composited UI lands in the framebuffer.
 * The x0/y0 registers hold the destination rectangle's BOTTOM-RIGHT corner, so
 * the top-left is (x0-(w-1), y0-(h-1)); a full-screen 640x480 blit programs
 * (639, 479) = position (0, 0). MainUI pre-rotates 180 degrees for the
 * upside-down panel; honour that. Alpha blending, ROP, stretch and 90/270
 * rotation are not modelled.
 */
static void mstar_ge_bitblt(MStarGeState *s)
{
    const uint16_t *r = s->regs;
    uint32_t src = s->dram_base +
                   (((uint32_t)r[GE_SRC_ADDR_H / 4] << 16) | r[GE_SRC_ADDR_L / 4]);
    uint32_t dst = s->dram_base +
                   (((uint32_t)r[GE_DST_ADDR_H / 4] << 16) | r[GE_DST_ADDR_L / 4]);
    unsigned spit = r[GE_SRC_PITCH / 4], dpit = r[GE_DST_PITCH / 4];
    unsigned sfmt = r[GE_FMT / 4] & 0xf, dfmt = (r[GE_FMT / 4] >> 8) & 0xf;
    unsigned w = r[GE_WIDTH / 4], h = r[GE_HEIGHT / 4];
    unsigned rot = r[GE_ROTATE / 4] & 3;
    unsigned sbpp = ge_bpp(sfmt), dbpp = ge_bpp(dfmt);
    int dx = (int)r[GE_DST_X0 / 4] - (int)w + 1;
    int dy = (int)r[GE_DST_Y0 / 4] - (int)h + 1;
    unsigned row, col;

    if (w == 0 || h == 0 || w > GE_MAX_DIM || h > GE_MAX_DIM || dx < 0 || dy < 0) {
        return;
    }

    for (row = 0; row < h; row++) {
        uint8_t sbuf[GE_MAX_DIM * 4], dbuf[GE_MAX_DIM * 4];
        unsigned orow = rot == GE_ROTATE_180 ? h - 1 - row : row;
        uint32_t srow = src + row * spit;
        uint32_t drow = dst + (dy + orow) * dpit + dx * dbpp;

        if (address_space_read(&address_space_memory, srow,
                               MEMTXATTRS_UNSPECIFIED, sbuf,
                               w * sbpp) != MEMTX_OK) {
            return;
        }
        for (col = 0; col < w; col++) {
            uint32_t px = sbpp == 4 ? ldl_le_p(sbuf + col * 4)
                                    : lduw_le_p(sbuf + col * 2);
            uint32_t out = ge_from_argb(dfmt, ge_to_argb(sfmt, px));
            unsigned ocol = rot == GE_ROTATE_180 ? w - 1 - col : col;

            if (dbpp == 4) {
                stl_le_p(dbuf + ocol * 4, out);
            } else {
                stw_le_p(dbuf + ocol * 2, out);
            }
        }
        address_space_write(&address_space_memory, drow,
                            MEMTXATTRS_UNSPECIFIED, dbuf, w * dbpp);
    }
    /*
     * The blit lands in DRAM; the GOP scanout picks it up through framebuffer
     * dirty-page tracking, so there is nothing to flag here.
     */
}

static uint64_t mstar_ge_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarGeState *s = MSTAR_GE(opaque);

    if (addr == GE_STATUS) {
        return GE_STATUS_IDLE;
    }
    return s->regs[addr / 4];
}

static void mstar_ge_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    MStarGeState *s = MSTAR_GE(opaque);

    s->regs[addr / 4] = val;

    if (addr == GE_CMD && (val & GE_CMD_BITBLT)) {
        mstar_ge_bitblt(s);
    }
}

static const MemoryRegionOps mstar_ge_ops = {
    .read = mstar_ge_read,
    .write = mstar_ge_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void mstar_ge_reset(DeviceState *dev)
{
    MStarGeState *s = MSTAR_GE(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mstar_ge_init(Object *obj)
{
    MStarGeState *s = MSTAR_GE(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_ge_ops, s,
                          "mstar-ge", MSTAR_GE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property mstar_ge_properties[] = {
    DEFINE_PROP_UINT32("dram-base", MStarGeState, dram_base, 0),
};

static void mstar_ge_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_ge_reset);
    device_class_set_props(dc, mstar_ge_properties);
}

static const TypeInfo mstar_ge_types[] = {
    {
        .name           = TYPE_MSTAR_GE,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarGeState),
        .instance_init  = mstar_ge_init,
        .class_init     = mstar_ge_class_init,
    },
};

DEFINE_TYPES(mstar_ge_types)
