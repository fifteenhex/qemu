/* SPDX-License-Identifier: MIT */
/* QEMU Sega MegaDrive machine / Everdrive / VDP */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"
#include "migration/vmstate.h"
#include "system/reset.h"
#include "ui/console.h"

#include "hw/display/md_vdp.h"

static void md_vdp_reset(void *opaque);

#define VDP_STATUS_FIFO_EMPTY   (1 << 9)
#define VDP_STATUS_VINT         (1 << 7)
#define VDP_STATUS_VBLANK       (1 << 3)
#define VDP_STATUS_STUB         (VDP_STATUS_FIFO_EMPTY | VDP_STATUS_VBLANK)

#define VDP_REG1_IE0            0x20

#define VDP_VINT_PERIOD_NS      (NANOSECONDS_PER_SECOND / 60)
//#define VDP_VINT_PERIOD_NS      (NANOSECONDS_PER_SECOND / 120)

#define VDP_FB_WIDTH            320
#define VDP_FB_HEIGHT           224

/* CRAM entry (0000 BBB0 GGG0 RRR0) -> XRGB8888 */
static uint32_t vdp_cram_to_rgb(uint16_t c)
{
    unsigned r = (c >> 1) & 7;
    unsigned g = (c >> 5) & 7;
    unsigned b = (c >> 9) & 7;

    r = (r << 5) | (r << 2) | (r >> 1);
    g = (g << 5) | (g << 2) | (g >> 1);
    b = (b << 5) | (b << 2) | (b >> 1);

    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static uint16_t vdp_status(MDVDPState *s)
{
    return VDP_STATUS_STUB | (s->vint_pending ? VDP_STATUS_VINT : 0);
}

/* Level-6 VINT is level-triggered: asserted while pending and enabled (IE0),
 * cleared only when the guest reads the status register. */
static void md_vdp_update_irq(MDVDPState *s)
{
    int level = (s->vint_pending && (s->regs[1] & VDP_REG1_IE0)) ? 1 : 0;

    qemu_set_irq(s->irq_vint, level);
}

static void md_vdp_vint(void *opaque)
{
    MDVDPState *s = opaque;


    timer_mod(s->vint_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VDP_VINT_PERIOD_NS);

    s->vint_pending = true;
    md_vdp_update_irq(s);
}

static void vdp_control_write(MDVDPState *s, uint16_t val)
{
    if (!s->pending_cmd) {

        if ((val & 0xC000) == 0x8000) {

            uint8_t reg = (val >> 8) & 0x1F;
            uint8_t data = val & 0xFF;
            if (reg < MD_VDP_NUM_REGS) {
                qemu_log_mask(LOG_UNIMP,
                    "md_vdp: reg[%u] = 0x%02x\n", reg, data);
                s->regs[reg] = data;
                if (reg == 1) {
                    /* IE0 may have changed - re-evaluate the VINT line */
                    md_vdp_update_irq(s);
                }
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                    "md_vdp: write to out-of-range register %u\n", reg);
            }
            s->pending_cmd = false;
        } else {

            s->cmd_first   = val;
            s->pending_cmd = true;
        }
    } else {

        uint16_t first = s->cmd_first;
        uint8_t  code  = ((first >> 14) & 0x03) | ((val >> 2) & 0x3C);
        uint32_t addr  = (first & 0x3FFF) | ((val & 0x0003) << 14);

        s->code        = code;
        s->addr        = addr;
        s->pending_cmd = false;

        qemu_log_mask(LOG_UNIMP,
            "md_vdp: control cmd code=0x%02x addr=0x%04x\n", code, addr);
    }
}

static uint16_t vdp_control_read(MDVDPState *s)
{
    uint16_t status = vdp_status(s);

    s->pending_cmd = false;

    /* reading the status register clears the VINT flag and drops level 6 */
    s->vint_pending = false;
    md_vdp_update_irq(s);

    return status;
}

/* CD codes (low nibble) selecting the data-port target */
#define VDP_CD_VRAM_READ    0x0
#define VDP_CD_VRAM_WRITE   0x1
#define VDP_CD_CRAM_WRITE   0x3
#define VDP_CD_VSRAM_READ   0x4
#define VDP_CD_VSRAM_WRITE  0x5
#define VDP_CD_CRAM_READ    0x8

static void vdp_data_write(MDVDPState *s, uint16_t val)
{
    uint32_t a = s->addr;

    switch (s->code & 0x0F) {
    case VDP_CD_VRAM_WRITE:
        s->vram[a & 0xFFFF]       = val >> 8;
        s->vram[(a + 1) & 0xFFFF] = val & 0xFF;
        break;
    case VDP_CD_CRAM_WRITE:
        s->cram[(a >> 1) & (MD_VDP_CRAM_SIZE - 1)] = val;
        break;
    case VDP_CD_VSRAM_WRITE:
        if ((a >> 1) < MD_VDP_VSRAM_SIZE) {
            s->vsram[a >> 1] = val;
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
            "md_vdp: data write code=0x%02x addr=0x%04x\n", s->code, a);
        break;
    }

    s->addr = (a + s->regs[15]) & 0xFFFF;
}

static uint16_t vdp_data_read(MDVDPState *s)
{
    uint32_t a = s->addr;
    uint16_t val = 0;

    switch (s->code & 0x0F) {
    case VDP_CD_VRAM_READ:
        val = (s->vram[a & 0xFFFF] << 8) | s->vram[(a + 1) & 0xFFFF];
        break;
    case VDP_CD_CRAM_READ:
        val = s->cram[(a >> 1) & (MD_VDP_CRAM_SIZE - 1)];
        break;
    case VDP_CD_VSRAM_READ:
        if ((a >> 1) < MD_VDP_VSRAM_SIZE) {
            val = s->vsram[a >> 1];
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
            "md_vdp: data read code=0x%02x addr=0x%04x\n", s->code, a);
        break;
    }

    s->addr = (a + s->regs[15]) & 0xFFFF;
    return val;
}

static uint64_t md_vdp_read(void *opaque, hwaddr offset, unsigned size)
{
    MDVDPState *s = MD_VDP(opaque);
    uint16_t val  = 0;

    offset &= 0x1F;

    switch (offset & ~0x01) {
    case VDP_PORT_DATA_0:
    case VDP_PORT_DATA_1:
        val = vdp_data_read(s);
        break;

    case VDP_PORT_CTRL_0:
    case VDP_PORT_CTRL_1:
        val = vdp_control_read(s);
        break;

    case VDP_PORT_HV_0:
    case 0x0A:
    case 0x0C:
    case 0x0E:

        val = 0x0000;
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
            "md_vdp: unhandled read at offset 0x%02" HWADDR_PRIx "\n",
            offset);
        break;
    }

    if (size == 1) {
        val = (offset & 1) ? (val & 0xFF) : (val >> 8);
    }

    return val;
}

static void md_vdp_write(void *opaque, hwaddr offset, uint64_t val,
                         unsigned size)
{
    MDVDPState *s = MD_VDP(opaque);
    uint16_t w;

    offset &= 0x1F;

    if (size == 1) {

        w = (offset & 1) ? (uint16_t)(val & 0xFF)
                         : (uint16_t)(val << 8);
    } else {
        w = (uint16_t)(val & 0xFFFF);
    }

    switch (offset & ~0x01) {
    case VDP_PORT_DATA_0:
    case VDP_PORT_DATA_1:
        vdp_data_write(s, w);
        break;

    case VDP_PORT_CTRL_0:
    case VDP_PORT_CTRL_1:
        vdp_control_write(s, w);
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
            "md_vdp: unhandled write 0x%04x at offset 0x%02" HWADDR_PRIx "\n",
            w, offset);
        break;
    }
}

static const MemoryRegionOps md_vdp_ops = {
    .read       = md_vdp_read,
    .write      = md_vdp_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

static const VMStateDescription vmstate_md_vdp = {
    .name    = "md-vdp",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs,        MDVDPState, MD_VDP_NUM_REGS),
        VMSTATE_UINT8_ARRAY(vram,        MDVDPState, MD_VDP_VRAM_SIZE),
        VMSTATE_UINT16_ARRAY(cram,       MDVDPState, MD_VDP_CRAM_SIZE),
        VMSTATE_UINT16_ARRAY(vsram,      MDVDPState, MD_VDP_VSRAM_SIZE),
        VMSTATE_BOOL(pending_cmd,        MDVDPState),
        VMSTATE_UINT16(cmd_first,        MDVDPState),
        VMSTATE_UINT32(addr,             MDVDPState),
        VMSTATE_UINT8(code,              MDVDPState),
        VMSTATE_BOOL(vint_pending,       MDVDPState),
        VMSTATE_TIMER_PTR(vint_timer,    MDVDPState),
        VMSTATE_END_OF_LIST()
    },
};

/* Register helpers */
#define VDP_REG1_DISP_EN   0x40

static int vdp_plane_dim(unsigned bits)
{
    switch (bits & 3) {
    case 1:  return 64;
    case 3:  return 128;
    default: return 32;
    }
}

/*
 * Sample one plane pixel, applying horizontal (HSCROLL table) and vertical
 * (VSRAM) scroll.  plane: 0 = A, 1 = B.  Returns a CRAM index, or -1 if the
 * pixel is transparent.
 *
 * reg11 selects scroll granularity:
 *   bits 0-1: H scroll - 0=whole screen, 2=per-cell (8 lines), 3=per-line
 *   bit 2:    V scroll - 0=whole screen, 1=per 16px column
 * reg13 holds the HSCROLL table base in VRAM.
 */
static int vdp_plane_pixel(MDVDPState *s, int plane, uint32_t ntbase,
                           int pw, int ph, int x, int y)
{
    int pwpx = pw * 8, phpx = ph * 8;
    int hs, vs, px, py, tcol, trow, tx, ty, tile, pal, idx;
    uint32_t nt, paddr, hbase, ha;
    uint16_t entry;
    uint8_t byte;
    int L, vcol, vi;

    /* horizontal scroll value for this line */
    hbase = (s->regs[13] & 0x3F) << 10;
    switch (s->regs[11] & 3) {
    case 0:  L = 0;      break;     /* whole screen */
    case 2:  L = y & ~7; break;     /* per-cell     */
    case 3:  L = y;      break;     /* per-line     */
    default: L = y & 7;  break;     /* 1 = prohibited */
    }
    ha = (hbase + L * 4 + (plane ? 2 : 0)) & 0xFFFF;
    hs = ((s->vram[ha] << 8) | s->vram[(ha + 1) & 0xFFFF]) & 0x3FF;

    /* vertical scroll value for this column */
    vcol = (s->regs[11] & 4) ? (x >> 4) : 0;
    vi = vcol * 2 + (plane ? 1 : 0);
    vs = (vi < MD_VDP_VSRAM_SIZE) ? (s->vsram[vi] & 0x3FF) : 0;

    /* scrolled pixel within the plane (planes wrap; dims are powers of two) */
    px = (x - hs) & (pwpx - 1);
    py = (y + vs) & (phpx - 1);

    tcol = px >> 3;
    trow = py >> 3;
    tx = px & 7;
    ty = py & 7;

    nt = (ntbase + ((trow * pw + tcol) << 1)) & 0xFFFF;
    entry = (s->vram[nt] << 8) | s->vram[(nt + 1) & 0xFFFF];
    tile = entry & 0x07FF;
    pal  = (entry >> 13) & 3;

    if (entry & 0x0800) {           /* hflip */
        tx = 7 - tx;
    }
    if (entry & 0x1000) {           /* vflip */
        ty = 7 - ty;
    }

    paddr = (tile * 32 + ty * 4 + (tx >> 1)) & 0xFFFF;
    byte = s->vram[paddr];
    idx = (tx & 1) ? (byte & 0x0F) : (byte >> 4);

    return idx ? (pal * 16 + idx) : -1;
}

/*
 * Sample one window-plane pixel at screen position (x, y).  The window plane
 * replaces plane A inside its region; it uses screen coordinates (no scroll)
 * and its own nametable (reg3).  ww is the window width in cells (32 for H32,
 * 64 for H40).  Returns a CRAM index, or -1 if the pixel is transparent.
 */
static int vdp_window_pixel(MDVDPState *s, uint32_t wbase, int ww, int x, int y)
{
    int col = x >> 3, row = y >> 3;
    int tx = x & 7, ty = y & 7;
    uint32_t nt, paddr;
    uint16_t entry;
    int tile, pal, idx;
    uint8_t byte;

    nt = (wbase + ((row * ww + col) << 1)) & 0xFFFF;
    entry = (s->vram[nt] << 8) | s->vram[(nt + 1) & 0xFFFF];
    tile = entry & 0x07FF;
    pal  = (entry >> 13) & 3;

    if (entry & 0x0800) {           /* hflip */
        tx = 7 - tx;
    }
    if (entry & 0x1000) {           /* vflip */
        ty = 7 - ty;
    }

    paddr = (tile * 32 + ty * 4 + (tx >> 1)) & 0xFFFF;
    byte = s->vram[paddr];
    idx = (tx & 1) ? (byte & 0x0F) : (byte >> 4);

    return idx ? (pal * 16 + idx) : -1;
}

/* Draw the sprite layer on top of the planes. */
static void vdp_draw_sprites(MDVDPState *s, uint8_t *fb, int stride,
                             int nw, int nh, unsigned scale)
{
    uint32_t sat = (s->regs[5] & 0x7F) << 9;
    int idx = 0;
    int processed = 0;

    /* Follow the link chain (max 80 sprites) */
    while (processed < 80) {
        uint32_t e = (sat + idx * 8) & 0xFFFF;
        int yraw  = ((s->vram[e] << 8) | s->vram[(e + 1) & 0xFFFF]) & 0x3FF;
        uint8_t sz = s->vram[(e + 2) & 0xFFFF];
        int link  = s->vram[(e + 3) & 0xFFFF] & 0x7F;
        uint16_t attr = (s->vram[(e + 4) & 0xFFFF] << 8) |
                         s->vram[(e + 5) & 0xFFFF];
        int xraw  = ((s->vram[(e + 6) & 0xFFFF] << 8) |
                      s->vram[(e + 7) & 0xFFFF]) & 0x3FF;

        int wt = ((sz >> 2) & 3) + 1;   /* width  in tiles */
        int ht = (sz & 3) + 1;          /* height in tiles */
        int wpx = wt * 8, hpx = ht * 8;
        int base  = attr & 0x07FF;
        int pal   = (attr >> 13) & 3;
        bool hflip = attr & 0x0800;
        bool vflip = attr & 0x1000;
        int x0 = xraw - 128;            /* 128 = left/top of screen */
        int y0 = yraw - 128;
        int tx, ty;

        /* draw if any part is on-screen */
        if (x0 < nw && x0 + wpx > 0 && y0 < nh && y0 + hpx > 0) {
            for (ty = 0; ty < hpx; ty++) {
                int py = y0 + ty;
                int fy = vflip ? (hpx - 1 - ty) : ty;

                if (py < 0 || py >= nh) {
                    continue;
                }
                for (tx = 0; tx < wpx; tx++) {
                    int px = x0 + tx;
                    int fx = hflip ? (wpx - 1 - tx) : tx;
                    int tile, ci;
                    uint32_t pa, color;
                    uint8_t byte;
                    unsigned bx, by;

                    if (px < 0 || px >= nw) {
                        continue;
                    }

                    /* column-major tile order within the sprite */
                    tile = base + (fx >> 3) * ht + (fy >> 3);
                    pa = (tile * 32 + (fy & 7) * 4 + ((fx & 7) >> 1)) & 0xFFFF;
                    byte = s->vram[pa];
                    ci = (fx & 1) ? (byte & 0x0F) : (byte >> 4);
                    if (!ci) {
                        continue;
                    }

                    color = vdp_cram_to_rgb(s->cram[(pal * 16 + ci) & 0x3F]);
                    for (by = 0; by < scale; by++) {
                        uint32_t *r = (uint32_t *)(fb + (py * scale + by) * stride);
                        for (bx = 0; bx < scale; bx++) {
                            r[px * scale + bx] = color;
                        }
                    }
                }
            }
        }

        processed++;
        if (link == 0) {
            break;
        }
        idx = link;
    }
}

/* Render planes B then A (A over B), upscaled by s->scale. */
static bool md_vdp_gfx_update(void *opaque)
{
    MDVDPState *s = opaque;
    DisplaySurface *surface;
    uint8_t *fb;
    int stride;
    unsigned scale = s->scale ? s->scale : 1;
    /* reg12: RS1|RS0 (0x81) selects H40 (320px), else H32 (256px) */
    int nw = ((s->regs[12] & 0x81) == 0x81) ? 320 : 256;
    int nh = VDP_FB_HEIGHT;
    int sw = nw * scale, sh = nh * scale;

    surface = qemu_console_surface(s->con);
    if (surface_width(surface) != sw || surface_height(surface) != sh) {
        qemu_console_resize(s->con, sw, sh);
        surface = qemu_console_surface(s->con);
    }
    fb = surface_data(surface);
    stride = surface_stride(surface);

    int pw = vdp_plane_dim(s->regs[16] & 3);
    int ph = vdp_plane_dim((s->regs[16] >> 4) & 3);
    uint32_t b_base = (s->regs[4] & 0x07) << 13;
    uint32_t a_base = (s->regs[2] & 0x38) << 10;
    uint32_t backdrop = vdp_cram_to_rgb(s->cram[s->regs[7] & 0x3F]);
    bool display_on = s->regs[1] & VDP_REG1_DISP_EN;

    /* Window plane: own nametable (reg3), no scroll, region from reg17/reg18. */
    int ww = nw / 8;                              /* window width in cells */
    uint32_t w_base = (nw == 320) ? (s->regs[3] & 0x3C) << 10
                                  : (s->regs[3] & 0x3E) << 10;
    int  win_hp    = (s->regs[17] & 0x1F) * 2;    /* horiz split, in cells */
    bool win_rigt  = s->regs[17] & 0x80;          /* window on right side  */
    int  win_vp    = (s->regs[18] & 0x1F);        /* vert split, in cells  */
    bool win_down  = s->regs[18] & 0x80;          /* window on bottom side */
    int x, y;
    unsigned sx, sy;

    for (y = 0; y < nh; y++) {
        int wrow = y >> 3;
        bool in_v = win_down ? (wrow >= win_vp) : (wrow < win_vp);

        for (x = 0; x < nw; x++) {
            int ci = -1;
            uint32_t color;

            if (display_on) {
                int wcol = x >> 3;
                bool in_h = win_rigt ? (wcol >= win_hp) : (wcol < win_hp);
                int a;

                ci = vdp_plane_pixel(s, 1, b_base, pw, ph, x, y);
                if (in_v || in_h) {
                    a = vdp_window_pixel(s, w_base, ww, x, y);
                } else {
                    a = vdp_plane_pixel(s, 0, a_base, pw, ph, x, y);
                }
                if (a >= 0) {
                    ci = a;
                }
            }

            color = (ci >= 0) ? vdp_cram_to_rgb(s->cram[ci & 0x3F]) : backdrop;

            for (sy = 0; sy < scale; sy++) {
                uint32_t *row = (uint32_t *)(fb + (y * scale + sy) * stride);
                for (sx = 0; sx < scale; sx++) {
                    row[x * scale + sx] = color;
                }
            }
        }
    }

    if (display_on) {
        vdp_draw_sprites(s, fb, stride, nw, nh, scale);
    }

    qemu_console_update_full(s->con);

    return true;
}

static const GraphicHwOps md_vdp_gfx_ops = {
    .gfx_update = md_vdp_gfx_update,
};

static void md_vdp_realize(DeviceState *dev, Error **errp)
{
    MDVDPState  *s   = MD_VDP(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &md_vdp_ops, s,
                          "md-vdp", 0x20);
    sysbus_init_mmio(sbd, &s->iomem);

    sysbus_init_irq(sbd, &s->irq_vint);
    sysbus_init_irq(sbd, &s->irq_hint);

    s->vint_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, md_vdp_vint, s);
    timer_mod(s->vint_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VDP_VINT_PERIOD_NS);

    qemu_register_reset(md_vdp_reset, s);

    s->con = qemu_graphic_console_create(dev, 0, &md_vdp_gfx_ops, s);
    qemu_console_resize(s->con, VDP_FB_WIDTH * (s->scale ? s->scale : 1),
                        VDP_FB_HEIGHT * (s->scale ? s->scale : 1));
}

static void md_vdp_reset(void *opaque)
{
    MDVDPState *s = opaque;

    memset(s->regs,  0, sizeof(s->regs));
    memset(s->vram,  0, sizeof(s->vram));
    memset(s->cram,  0, sizeof(s->cram));
    memset(s->vsram, 0, sizeof(s->vsram));

    s->pending_cmd = false;
    s->cmd_first   = 0;
    s->addr        = 0;
    s->code        = 0;

    s->vint_pending = false;
    md_vdp_update_irq(s);
    timer_mod(s->vint_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VDP_VINT_PERIOD_NS);
}

static const Property md_vdp_properties[] = {
    DEFINE_PROP_UINT32("scale", MDVDPState, scale, 4),
};

static void md_vdp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize       = md_vdp_realize;
    dc->vmsd          = &vmstate_md_vdp;
    dc->desc          = "Sega MegaDrive VDP";
    device_class_set_props(dc, md_vdp_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo md_vdp_info = {
    .name          = TYPE_MD_VDP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MDVDPState),
    .class_init    = md_vdp_class_init,
};

static void md_vdp_register_types(void)
{
    type_register_static(&md_vdp_info);
}

type_init(md_vdp_register_types)
