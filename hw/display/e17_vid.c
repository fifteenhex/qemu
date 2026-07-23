/*
 * ELTEC Eurocom E17 onboard video
 *
 * The chipset is not identified yet; everything here mirrors what the
 * RMON 3.1.3 firmware does with it (see E17-NOTES.md in the tree
 * root):
 *
 *  - RAMDAC at 0xfec40000: +0/+1 are a VGA-style palette address/data
 *    pair with byte autoincrement and readback, +2 an ID register
 *    that must read 0x3a (RMON also writes 0x63 there during mode
 *    set, VGA misc-output style), and an indexed register file
 *    (index to +0, data to +5 and +6) programming the pixel clock
 *    PLL from a 25.175MHz reference.  Palette entries are 8 bit.
 *  - CRTC at 0xfec48000: byte registers +0..+5 (pixel format, pitch
 *    and enable — meanings partly unknown), and a file of 16-bit
 *    timing registers behind index port +6 with an autoincrementing
 *    low/high byte data port at +7.  For the one mode RMON programs
 *    (800x600x8@60, pixel clock 40MHz) the registers hold horizontal
 *    positions in 2-pixel units and vertical positions in lines:
 *    reg 1 hsync start, reg 2 hsync end, reg 3 h active start,
 *    reg 4 h total, reg 7 v active start, reg 8 v total.
 *  - 4MB VRAM at 0x0fc00000, scanned out as 8bpp indexed from the
 *    start of the first bank.  (RMON tests 1MB banks at 0x0fc00000
 *    and 0x0fe00000; the layout in between is unknown.)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/display/e17_vid.h"
#include "migration/vmstate.h"
#include "ui/pixel_ops.h"
#include "trace.h"

#define E17_VID_DAC_ID      0x3a

/* CRTC timing register numbers (16-bit file behind index port +6) */
#define E17_CRTC_HSYNC_START    1
#define E17_CRTC_HSYNC_END      2
#define E17_CRTC_HACT_START     3
#define E17_CRTC_HTOTAL         4
#define E17_CRTC_VACT_START     7
#define E17_CRTC_VTOTAL         8

static void e17_vid_geometry(E17VidState *s, int *width, int *height)
{
    int w = (s->crtc[E17_CRTC_HTOTAL] - s->crtc[E17_CRTC_HACT_START]) * 2 + 2;
    int h = s->crtc[E17_CRTC_VTOTAL] - s->crtc[E17_CRTC_VACT_START] + 1;

    if (w < 64 || w > 2048 || h < 64 || h > 2048) {
        /* mode not (yet) programmed: RMON's single known mode */
        w = 800;
        h = 600;
    }
    *width = w;
    *height = h;
}

static uint64_t e17_vid_dac_read(void *opaque, hwaddr addr, unsigned size)
{
    E17VidState *s = opaque;
    uint8_t val;

    switch (addr & 7) {
    case 0:
        return s->dac_idx;
    case 1:
        val = s->pal[s->pal_addr % E17_VID_PAL_SIZE];
        s->pal_addr = (s->pal_addr + 1) % E17_VID_PAL_SIZE;
        return val;
    case 2:
        return s->dac_id;
    case 5:
        return s->dac5[s->dac_idx % E17_VID_NREGS];
    case 6:
        return s->dac6[s->dac_idx % E17_VID_NREGS];
    }
    return 0;
}

static void e17_vid_dac_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    E17VidState *s = opaque;

    trace_e17_vid_dac_write(addr & 7, val);
    switch (addr & 7) {
    case 0:
        s->dac_idx = val;
        s->pal_addr = (val * 3) % E17_VID_PAL_SIZE;
        break;
    case 1:
        s->pal[s->pal_addr % E17_VID_PAL_SIZE] = val;
        s->pal_addr = (s->pal_addr + 1) % E17_VID_PAL_SIZE;
        s->invalidate = true;
        break;
    case 2:
        /* the ID keeps reading 0x3a whatever is written here */
        break;
    case 5:
        s->dac5[s->dac_idx % E17_VID_NREGS] = val;
        break;
    case 6:
        s->dac6[s->dac_idx % E17_VID_NREGS] = val;
        break;
    }
}

static const MemoryRegionOps e17_vid_dac_ops = {
    .read = e17_vid_dac_read,
    .write = e17_vid_dac_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static uint64_t e17_vid_crtc_read(void *opaque, hwaddr addr, unsigned size)
{
    E17VidState *s = opaque;
    uint16_t val;

    switch (addr & 7) {
    case 0 ... 5:
        return s->crtc_direct[addr & 7];
    case 6:
        return s->crtc_idx;
    case 7:
        val = s->crtc[s->crtc_idx % E17_VID_NREGS];
        return s->crtc_high ? val >> 8 : val & 0xff;
    }
    return 0;
}

static void e17_vid_crtc_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    E17VidState *s = opaque;

    trace_e17_vid_crtc_write(addr & 7, val);
    switch (addr & 7) {
    case 0 ... 5:
        s->crtc_direct[addr & 7] = val;
        s->invalidate = true;
        break;
    case 6:
        s->crtc_idx = val;
        s->crtc_high = false;
        break;
    case 7: {
        /* low byte first, high byte second, then the index advances */
        uint16_t *reg = &s->crtc[s->crtc_idx % E17_VID_NREGS];

        if (s->crtc_high) {
            *reg = (*reg & 0x00ff) | (val << 8);
            trace_e17_vid_crtc_reg(s->crtc_idx, *reg);
            s->crtc_idx++;
        } else {
            *reg = (*reg & 0xff00) | (val & 0xff);
        }
        s->crtc_high = !s->crtc_high;
        s->invalidate = true;
        break;
    }
    }
}

static const MemoryRegionOps e17_vid_crtc_ops = {
    .read = e17_vid_crtc_read,
    .write = e17_vid_crtc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static bool e17_vid_update_display(void *opaque)
{
    E17VidState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    DirtyBitmapSnapshot *snap;
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    int width, height, y;
    bool dirty;

    e17_vid_geometry(s, &width, &height);

    if (width != surface_width(surface) ||
        height != surface_height(surface)) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
        s->invalidate = true;
    }

    snap = memory_region_snapshot_and_clear_dirty(&s->vram, 0,
                                                  width * height,
                                                  DIRTY_MEMORY_VGA);
    dirty = false;
    for (y = 0; y < height; y++) {
        if (!s->invalidate &&
            !memory_region_snapshot_get_dirty(&s->vram, snap,
                                              y * width, width)) {
            continue;
        }
        uint32_t *dst = (uint32_t *)(surface_data(surface) +
                                     y * surface_stride(surface));
        uint8_t *src = vram + y * width;
        int x;

        for (x = 0; x < width; x++) {
            uint8_t *rgb = &s->pal[src[x] * 3];

            dst[x] = rgb_to_pixel32(rgb[0], rgb[1], rgb[2]);
        }
        dirty = true;
    }
    g_free(snap);

    if (dirty || s->invalidate) {
        qemu_console_update(s->con, 0, 0, width, height);
    }
    s->invalidate = false;

    return true;
}

static void e17_vid_invalidate_display(void *opaque)
{
    E17VidState *s = opaque;

    s->invalidate = true;
}

static const GraphicHwOps e17_vid_ops = {
    .invalidate = e17_vid_invalidate_display,
    .gfx_update = e17_vid_update_display,
};

static void e17_vid_reset(DeviceState *dev)
{
    E17VidState *s = E17_VID(dev);

    memset(s->pal, 0, sizeof(s->pal));
    s->pal_addr = 0;
    s->dac_idx = 0;
    s->dac_id = E17_VID_DAC_ID;
    memset(s->dac5, 0, sizeof(s->dac5));
    memset(s->dac6, 0, sizeof(s->dac6));
    memset(s->crtc_direct, 0, sizeof(s->crtc_direct));
    s->crtc_idx = 0;
    s->crtc_high = false;
    memset(s->crtc, 0, sizeof(s->crtc));
    s->invalidate = true;
}

static void e17_vid_realize(DeviceState *dev, Error **errp)
{
    E17VidState *s = E17_VID(dev);

    memory_region_init_ram(&s->vram, OBJECT(dev), "e17-vid.vram",
                           E17_VID_VRAM_SIZE, errp);
    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->vram);

    s->con = qemu_graphic_console_create(dev, 0, &e17_vid_ops, s);
}

static void e17_vid_init(Object *obj)
{
    E17VidState *s = E17_VID(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->dac_mem, obj, &e17_vid_dac_ops, s,
                          "e17-vid.dac", 0x1000);
    sysbus_init_mmio(sbd, &s->dac_mem);
    memory_region_init_io(&s->crtc_mem, obj, &e17_vid_crtc_ops, s,
                          "e17-vid.crtc", 0x1000);
    sysbus_init_mmio(sbd, &s->crtc_mem);
}

static const VMStateDescription vmstate_e17_vid = {
    .name = "e17-vid",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(pal, E17VidState, E17_VID_PAL_SIZE),
        VMSTATE_UINT16(pal_addr, E17VidState),
        VMSTATE_UINT8(dac_idx, E17VidState),
        VMSTATE_UINT8(dac_id, E17VidState),
        VMSTATE_UINT8_ARRAY(dac5, E17VidState, E17_VID_NREGS),
        VMSTATE_UINT8_ARRAY(dac6, E17VidState, E17_VID_NREGS),
        VMSTATE_UINT8_ARRAY(crtc_direct, E17VidState, 8),
        VMSTATE_UINT8(crtc_idx, E17VidState),
        VMSTATE_BOOL(crtc_high, E17VidState),
        VMSTATE_UINT16_ARRAY(crtc, E17VidState, E17_VID_NREGS),
        VMSTATE_END_OF_LIST()
    }
};

static void e17_vid_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "ELTEC Eurocom E17 onboard video";
    dc->realize = e17_vid_realize;
    device_class_set_legacy_reset(dc, e17_vid_reset);
    dc->vmsd = &vmstate_e17_vid;
}

static const TypeInfo e17_vid_info = {
    .name = TYPE_E17_VID,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(E17VidState),
    .instance_init = e17_vid_init,
    .class_init = e17_vid_class_init,
};

static void e17_vid_register_types(void)
{
    type_register_static(&e17_vid_info);
}

type_init(e17_vid_register_types)
