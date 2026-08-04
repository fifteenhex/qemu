/*
 * QEMU 3dfx Voodoo 3 (Avenger) PCI video card emulation
 *
 * Copyright (c) 2026 Daniel's intern <intern@thingy.jp>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 *
 * Models a Voodoo 3 3000 PCI board well enough for the Linux tdfxfb
 * driver and similar "Banshee compatible" 2D consumers: the VGA core
 * (reusing QEMU's standard VGA), the Avenger I/O register space (PLLs,
 * DRAM init, video processor, CLUT, hardware cursor, DDC), desktop
 * scanout in 8/16/24/32bpp and the bit of the 2D engine tdfxfb uses.
 *
 * The card comes up cold, as it is before the video BIOS has run:
 * until the guest programs the memory PLL (pllCtrl1), writes
 * dramInit0/1 and issues a dramCommand mode write, the framebuffer
 * aperture reads back 0xff and the 2D engine drops commands. Nothing is
 * scanned out until the video PLL (pllCtrl0) and either the video
 * processor or the VGA core are set up; otherwise the display is black,
 * like a monitor with no sync.
 *
 * Simplifications: memory is always 16 MB (dramInit sizing bits are
 * ignored); legacy VGA decode isn't gated on vgaInit0 bit 9; host blts
 * pack rows byte-aligned (what tdfxfb generates); big-endian swizzling
 * (miscInit0 30/31) is unimplemented.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/display/i2c-ddc.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "vga_int.h"
#include "vga_regs.h"

#define PCI_VENDOR_ID_3DFX              0x121a
#define PCI_DEVICE_ID_3DFX_VOODOO3      0x0005
/* subsystem id of the Voodoo 3 3000 PCI (SDRAM); the AGP board is 0x0036 */
#define PCI_SUBDEVICE_ID_3DFX_V3_3000   0x003a

#define TYPE_VOODOO3 "voodoo3"
OBJECT_DECLARE_SIMPLE_TYPE(Voodoo3State, VOODOO3)

/* I/O register space, mapped at BAR2 and at offset 0 of BAR0 */
#define V3_STATUS               0x00
#define V3_PCIINIT0             0x04
#define V3_SIPMONITOR           0x08
#define V3_LFBMEMORYCONFIG      0x0c
#define V3_MISCINIT0            0x10
#define V3_MISCINIT1            0x14
#define V3_DRAMINIT0            0x18
#define V3_DRAMINIT1            0x1c
#define V3_AGPINIT              0x20
#define V3_TMUGBEINIT           0x24
#define V3_VGAINIT0             0x28
#define V3_VGAINIT1             0x2c
#define V3_DRAMCOMMAND          0x30
#define V3_DRAMDATA             0x34
#define V3_PLLCTRL0             0x40
#define V3_PLLCTRL1             0x44
#define V3_PLLCTRL2             0x48
#define V3_DACMODE              0x4c
#define V3_DACADDR              0x50
#define V3_DACDATA              0x54
#define V3_RGBMAXDELTA          0x58
#define V3_VIDPROCCFG           0x5c
#define V3_HWCURPATADDR         0x60
#define V3_HWCURLOC             0x64
#define V3_HWCURC0              0x68
#define V3_HWCURC1              0x6c
#define V3_VIDINFORMAT          0x70
#define V3_VIDINSTATUS          0x74
#define V3_VIDSERPARPORT        0x78
#define V3_VIDCURLIN            0x94
#define V3_VIDSCREENSIZE        0x98
#define V3_VIDDESKSTART         0xe4
#define V3_VIDDESKSTRIDE        0xe8

#define V3_IO_REG_NB            (0x100 / 4)

/* vgaInit0 bits */
#define V3_VGAINIT0_VGA_DISABLE BIT(0)
#define V3_VGAINIT0_8BIT_DAC    BIT(2)

/* dacMode bits */
#define V3_DACMODE_2X           BIT(0)
#define V3_DACMODE_NO_HSYNC     BIT(1)
#define V3_DACMODE_NO_VSYNC     BIT(3)

/* vidProcCfg bits */
#define V3_VIDCFG_VIDPROC_EN    BIT(0)
#define V3_VIDCFG_CURS_X11      BIT(1)
#define V3_VIDCFG_INTERLACE     BIT(3)
#define V3_VIDCFG_HALF_MODE     BIT(4)
#define V3_VIDCFG_DESK_EN       BIT(7)
#define V3_VIDCFG_CLUT_BYPASS   BIT(10)
#define V3_VIDCFG_2X            BIT(26)
#define V3_VIDCFG_HWCURSOR_EN   BIT(27)
#define V3_VIDCFG_PIXFMT_SHIFT  18

/* miscInit1 bits */
#define V3_MISCINIT1_2DBLOCK_DIS BIT(15)

/* vidSerialParallelPort bits */
#define V3_VSP_DDC_EN           BIT(18)
#define V3_VSP_DDC_SCL_OUT      BIT(19)
#define V3_VSP_DDC_SDA_OUT      BIT(20)
#define V3_VSP_DDC_SCL_IN       BIT(21)
#define V3_VSP_DDC_SDA_IN       BIT(22)
#define V3_VSP_I2C_EN           BIT(23)
#define V3_VSP_I2C_SCL_OUT      BIT(24)
#define V3_VSP_I2C_SDA_OUT      BIT(25)
#define V3_VSP_I2C_SCL_IN       BIT(26)
#define V3_VSP_I2C_SDA_IN       BIT(27)

/* 2D register space at BAR0 + 0x100000 */
#define V3_2D_CLIP0MIN          0x08
#define V3_2D_CLIP0MAX          0x0c
#define V3_2D_DSTBASE           0x10
#define V3_2D_DSTFORMAT         0x14
#define V3_2D_SRCBASE           0x34
#define V3_2D_CLIP1MIN          0x4c
#define V3_2D_CLIP1MAX          0x50
#define V3_2D_SRCFORMAT         0x54
#define V3_2D_SRCSIZE           0x58
#define V3_2D_SRCXY             0x5c
#define V3_2D_COLORBACK         0x60
#define V3_2D_COLORFORE         0x64
#define V3_2D_DSTSIZE           0x68
#define V3_2D_DSTXY             0x6c
#define V3_2D_COMMAND           0x70
#define V3_2D_LAUNCH            0x80

#define V3_2D_REG_NB            (0x100 / 4)

#define V3_2D_OP_MASK           0x0f
#define V3_2D_OP_NOP            0x00
#define V3_2D_OP_S2S_BLT        0x01
#define V3_2D_OP_H2S_BLT        0x03
#define V3_2D_OP_RECTFILL       0x05
#define V3_2D_CMD_INITIATE      BIT(8)
#define V3_2D_CMD_X_REVERSE     BIT(14)
#define V3_2D_CMD_Y_REVERSE     BIT(15)

#define V3_ROP_COPY             0xcc
#define V3_ROP_XOR              0x66
#define V3_ROP_INVERT           0x55

/* PLL reference clock, kHz */
#define V3_PLL_REF_KHZ          14318

enum { V3_MODE_BLANK, V3_MODE_VGA, V3_MODE_DESKTOP };

struct Voodoo3State {
    PCIDevice dev;
    VGACommonState vga;

    MemoryRegion mmio;      /* BAR 0: 32 MB register space */
    MemoryRegion lfb;       /* BAR 1: 32 MB linear framebuffer window */
    MemoryRegion lfb_dead;  /* ... backing while DRAM is uninitialised */
    MemoryRegion io;        /* BAR 2: 256 byte I/O space */

    uint32_t io_regs[V3_IO_REG_NB];
    uint32_t d2_regs[V3_2D_REG_NB];

    bool draminit0_written;
    bool draminit1_written;
    bool dram_mode_set;

    /* host-to-screen blt in progress */
    struct {
        bool active;
        uint32_t x, y;
    } h2s;

    uint8_t mode;
    bool need_blank;

    QEMUCursor *cursor;
    bitbang_i2c_interface bbi2c;
    I2CDDCState i2cddc;
};

/*
 * PLL and initialisation state. The pllCtrl registers hold
 * (n << 8) | (m << 2) | k with fout = fref * (n + 2) / (m + 2) / 2^k.
 * Reset value 0 gives fref (14.3 MHz), which nothing would program, so
 * a frequency window is a good "has the PLL been set up?" test.
 */
static uint32_t voodoo3_pll_khz(uint32_t v)
{
    uint32_t n = (v >> 8) & 0xff, m = (v >> 2) & 0x3f, k = v & 3;

    return (V3_PLL_REF_KHZ * (n + 2) / (m + 2)) >> k;
}

static bool voodoo3_vidpll_ok(Voodoo3State *s)
{
    uint32_t f = voodoo3_pll_khz(s->io_regs[V3_PLLCTRL0 / 4]);

    return f >= 15000 && f <= 350000;
}

static bool voodoo3_mem_ok(Voodoo3State *s)
{
    uint32_t f = voodoo3_pll_khz(s->io_regs[V3_PLLCTRL1 / 4]);

    return f >= 50000 && f <= 250000 &&
           s->draminit0_written && s->draminit1_written && s->dram_mode_set;
}

static bool voodoo3_2d_ok(Voodoo3State *s)
{
    /*
     * The engine needs working memory but not pllCtrl2: the video BIOS
     * leaves the gfx PLL unprogrammed and drivers still use the 2D
     * engine, so at reset the gfx clock must fall back to a bypass
     * clock. Programming pllCtrl2 only makes it run at full speed.
     * (miscInit1 bit 15 is NOT an engine disable: it turns off the
     * SDRAM block write optimisation and is set on all SDRAM boards.)
     */
    return voodoo3_mem_ok(s);
}

/*
 * Scanline counter, approximated from the virtual clock. Enough for
 * guests polling for vertical retrace or reading vidCurrentLine.
 */
static uint32_t voodoo3_scanline(Voodoo3State *s, uint32_t *visible)
{
    uint32_t h = (s->io_regs[V3_VIDSCREENSIZE / 4] >> 12) & 0xfff;
    int64_t frame = 16666667; /* ~60 Hz */
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (h < 200 || h > 2048) {
        h = 480;
    }
    if (visible) {
        *visible = h;
    }
    /* ~5% blanking lines on top of the visible area */
    return (now % frame) * (h + h / 20 + 1) / frame;
}

static void voodoo3_update_cursor(Voodoo3State *s)
{
    uint32_t vidcfg = s->io_regs[V3_VIDPROCCFG / 4];
    uint32_t loc = s->io_regs[V3_HWCURLOC / 4];
    uint32_t pataddr = s->io_regs[V3_HWCURPATADDR / 4] & 0xffffff;
    bool en = s->mode == V3_MODE_DESKTOP && (vidcfg & V3_VIDCFG_HWCURSOR_EN);
    int x = (int)(loc & 0x7ff) - 63;
    int y = (int)((loc >> 16) & 0x7ff) - 63;

    if (en) {
        uint8_t plane0[64 * 8], plane1[64 * 8];
        int i;

        if (!(vidcfg & V3_VIDCFG_CURS_X11)) {
            qemu_log_mask(LOG_UNIMP,
                          "voodoo3: Windows cursor mode not implemented\n");
        }
        if (pataddr + 64 * 16 > s->vga.vram_size) {
            return;
        }
        /*
         * 64 lines of 8 bytes plane 0 (mask) + 8 bytes plane 1 (shape),
         * leftmost pixel in the msb of the first byte. X11 mode: mask 0
         * is transparent, mask 1 shows shape ? hwCurC1 : hwCurC0.
         * cursor_set_mono() wants an inverted mask for that.
         */
        for (i = 0; i < 64; i++) {
            int j;

            for (j = 0; j < 8; j++) {
                plane0[i * 8 + j] =
                    ~s->vga.vram_ptr[pataddr + i * 16 + j];
                plane1[i * 8 + j] =
                    s->vga.vram_ptr[pataddr + i * 16 + 8 + j];
            }
        }
        if (!s->cursor) {
            s->cursor = cursor_alloc(64, 64);
        }
        cursor_set_mono(s->cursor, s->io_regs[V3_HWCURC1 / 4] & 0xffffff,
                        s->io_regs[V3_HWCURC0 / 4] & 0xffffff,
                        plane1, 1, plane0);
        qemu_console_set_cursor(s->vga.con, s->cursor);
    }
    qemu_console_set_mouse(s->vga.con, x, y, en);
}

static void voodoo3_set_scanout_offset(Voodoo3State *s, uint32_t offs)
{
    VGACommonState *vga = &s->vga;
    int bypp = DIV_ROUND_UP(vga->vbe_regs[VBE_DISPI_INDEX_BPP], 8);

    if (!bypp ||
        vga->vbe_regs[VBE_DISPI_INDEX_YRES] *
        vga->vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] * bypp + offs >
        vga->vbe_size) {
        return;
    }
    vga->vbe_start_addr = offs / 4;
}

/*
 * Work out what the card would put on the wire and switch the display
 * accordingly. Reuses the VGA core's VBE machinery for the desktop
 * (video processor) scanout so that all pixel formats and dirty
 * tracking come for free.
 */
static void voodoo3_update_mode(Voodoo3State *s)
{
    VGACommonState *vga = &s->vga;
    uint32_t vidcfg = s->io_regs[V3_VIDPROCCFG / 4];
    uint8_t mode;

    if (!voodoo3_mem_ok(s)) {
        /* nothing to scan out of */
        mode = V3_MODE_BLANK;
    } else if (vidcfg & V3_VIDCFG_VIDPROC_EN) {
        /*
         * The video processor scans out the desktop surface with the
         * pixel clock from the video PLL, which has to be locked.
         */
        mode = (voodoo3_vidpll_ok(s) && (vidcfg & V3_VIDCFG_DESK_EN)) ?
               V3_MODE_DESKTOP : V3_MODE_BLANK;
    } else if (s->io_regs[V3_VGAINIT0 / 4] & V3_VGAINIT0_VGA_DISABLE) {
        mode = V3_MODE_BLANK;
    } else {
        /*
         * Legacy VGA modes run off the standard VGA clock selects (the
         * real video BIOS does not program pllCtrl0 for text modes).
         */
        mode = V3_MODE_VGA;
    }

    if (mode == V3_MODE_DESKTOP) {
        uint32_t w = s->io_regs[V3_VIDSCREENSIZE / 4] & 0xfff;
        uint32_t h = (s->io_regs[V3_VIDSCREENSIZE / 4] >> 12) & 0xfff;
        uint32_t stride = s->io_regs[V3_VIDDESKSTRIDE / 4] & 0x7fff;
        uint32_t offs = s->io_regs[V3_VIDDESKSTART / 4] & 0xffffff;
        uint32_t bpp = 8 * (((vidcfg >> V3_VIDCFG_PIXFMT_SHIFT) & 3) + 1);

        if (w < 64 || h < 64 || stride < w * bpp / 8 ||
            offs + h * stride > vga->vram_size) {
            mode = V3_MODE_BLANK;
        } else {
            if (vidcfg & (V3_VIDCFG_INTERLACE | V3_VIDCFG_HALF_MODE)) {
                qemu_log_mask(LOG_UNIMP, "voodoo3: interlaced/half mode "
                              "scanout not implemented\n");
            }
            vbe_ioport_write_index(vga, 0, VBE_DISPI_INDEX_ENABLE);
            vbe_ioport_write_data(vga, 0, VBE_DISPI_DISABLED);
            vga->vbe_regs[VBE_DISPI_INDEX_XRES] = w;
            vga->vbe_regs[VBE_DISPI_INDEX_YRES] = h;
            vga->vbe_regs[VBE_DISPI_INDEX_BPP] = bpp;
            vbe_ioport_write_index(vga, 0, VBE_DISPI_INDEX_ENABLE);
            vbe_ioport_write_data(vga, 0, VBE_DISPI_ENABLED |
                VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM |
                (s->io_regs[V3_VGAINIT0 / 4] & V3_VGAINIT0_8BIT_DAC ?
                 VBE_DISPI_8BIT_DAC : 0));
            vbe_ioport_write_index(vga, 0, VBE_DISPI_INDEX_VIRT_WIDTH);
            vbe_ioport_write_data(vga, 0, stride / (bpp / 8));
            voodoo3_set_scanout_offset(s, offs);
            /*
             * The video processor bypasses the VGA attribute
             * controller, whose "display enable" flag would otherwise
             * blank the VGA core.
             */
            vga->ar_index |= BIT(5);
        }
    }
    if (mode != V3_MODE_DESKTOP && s->mode == V3_MODE_DESKTOP) {
        vbe_ioport_write_index(vga, 0, VBE_DISPI_INDEX_ENABLE);
        vbe_ioport_write_data(vga, 0, VBE_DISPI_DISABLED);
    }
    if (mode != s->mode) {
        s->mode = mode;
        s->need_blank = true;
        qemu_console_hw_invalidate(vga->con);
        voodoo3_update_cursor(s);
    }
}

static void voodoo3_update_memory_gate(Voodoo3State *s)
{
    memory_region_set_enabled(&s->vga.vram, voodoo3_mem_ok(s));
    voodoo3_update_mode(s);
}

/*
 * Display update: gate the actual VGA/VBE rendering on the card being
 * initialised and unblanked so that an unconfigured card shows no
 * image, just like a monitor without a signal.
 */
static bool voodoo3_display_blanked(Voodoo3State *s)
{
    uint32_t dacmode = s->io_regs[V3_DACMODE / 4];

    if (s->mode == V3_MODE_BLANK) {
        return true;
    }
    if (dacmode & (V3_DACMODE_NO_HSYNC | V3_DACMODE_NO_VSYNC)) {
        return true;
    }
    /* SR01 bit 5 screen-off is bypassed by the VGA core in VBE mode */
    if (s->mode == V3_MODE_DESKTOP && (s->vga.sr[VGA_SEQ_CLOCK_MODE] & 0x20)) {
        return true;
    }
    return false;
}

static bool voodoo3_gfx_update(void *opaque)
{
    Voodoo3State *s = opaque;

    if (voodoo3_display_blanked(s)) {
        if (s->need_blank) {
            DisplaySurface *surface;
            int w = s->vga.last_scr_width ? s->vga.last_scr_width : 640;
            int h = s->vga.last_scr_height ? s->vga.last_scr_height : 480;

            surface = qemu_create_displaysurface(w, h);
            memset(surface_data(surface), 0,
                   (size_t)surface_stride(surface) * h);
            qemu_console_set_surface(s->vga.con, surface);
            qemu_console_update_full(s->vga.con);
            s->need_blank = false;
        }
        return true;
    }
    if (s->need_blank) {
        /* coming out of blank, force full redraw */
        s->vga.hw_ops->invalidate(&s->vga);
        s->need_blank = false;
    }
    return s->vga.hw_ops->gfx_update(&s->vga);
}

static void voodoo3_invalidate(void *opaque)
{
    Voodoo3State *s = opaque;

    s->need_blank = true;
    s->vga.hw_ops->invalidate(&s->vga);
}

static void voodoo3_text_update(void *opaque, uint32_t *chardata)
{
    Voodoo3State *s = opaque;

    if (s->mode == V3_MODE_VGA && s->vga.hw_ops->text_update) {
        s->vga.hw_ops->text_update(&s->vga, chardata);
    }
}

static const GraphicHwOps voodoo3_gfx_ops = {
    .invalidate = voodoo3_invalidate,
    .gfx_update = voodoo3_gfx_update,
    .text_update = voodoo3_text_update,
};

/* ---------------------------------------------------------------- */
/* 2D engine                                                        */

static void voodoo3_2d_dirty(Voodoo3State *s, uint32_t start, uint32_t end)
{
    if (start < end) {
        memory_region_set_dirty(&s->vga.vram, start, end - start);
    }
}

static uint8_t voodoo3_rop(uint8_t rop, uint8_t src, uint8_t dst)
{
    /* two operand subset of the ROP3 (pattern input tied low) */
    uint8_t r = 0;

    if (rop & 0x08) {
        r |= src & dst;
    }
    if (rop & 0x04) {
        r |= src & ~dst;
    }
    if (rop & 0x02) {
        r |= ~src & dst;
    }
    if (rop & 0x01) {
        r |= ~src & ~dst;
    }
    return r;
}

static uint32_t voodoo3_2d_bpp(uint32_t format)
{
    switch ((format >> 16) & 7) {
    case 1:
        return 1;
    case 3:
        return 2;
    case 4:
        return 3;
    case 5:
        return 4;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "voodoo3: bad 2D pixel format %d\n", (format >> 16) & 7);
        return 0;
    }
}

/*
 * Clip against clip0 and the framebuffer, then write one pixel.
 * Coordinates are absolute within the destination surface.
 */
static void voodoo3_2d_plot(Voodoo3State *s, uint32_t x, uint32_t y,
                            uint32_t color, uint8_t rop)
{
    uint32_t clipmin = s->d2_regs[V3_2D_CLIP0MIN / 4];
    uint32_t clipmax = s->d2_regs[V3_2D_CLIP0MAX / 4];
    uint32_t base = s->d2_regs[V3_2D_DSTBASE / 4] & 0xffffff;
    uint32_t fmt = s->d2_regs[V3_2D_DSTFORMAT / 4];
    uint32_t stride = fmt & 0x3fff;
    uint32_t bpp = voodoo3_2d_bpp(fmt);
    uint32_t off;
    unsigned int i;

    if (!bpp) {
        return;
    }
    if (x < (clipmin & 0xfff) || x >= (clipmax & 0xfff) ||
        y < ((clipmin >> 16) & 0xfff) || y >= ((clipmax >> 16) & 0xfff)) {
        return;
    }
    off = base + y * stride + x * bpp;
    if (off + bpp > s->vga.vram_size) {
        return;
    }
    for (i = 0; i < bpp; i++) {
        s->vga.vram_ptr[off + i] = voodoo3_rop(rop, color >> (i * 8),
                                               s->vga.vram_ptr[off + i]);
    }
}

static void voodoo3_2d_rectfill(Voodoo3State *s, uint32_t xy)
{
    uint32_t cmd = s->d2_regs[V3_2D_COMMAND / 4];
    uint32_t size = s->d2_regs[V3_2D_DSTSIZE / 4];
    uint32_t color = s->d2_regs[V3_2D_COLORFORE / 4];
    uint32_t dx = xy & 0x1fff, dy = (xy >> 16) & 0x1fff;
    uint32_t w = size & 0x1fff, h = (size >> 16) & 0x1fff;
    uint32_t base = s->d2_regs[V3_2D_DSTBASE / 4] & 0xffffff;
    uint32_t stride = s->d2_regs[V3_2D_DSTFORMAT / 4] & 0x3fff;
    uint8_t rop = cmd >> 24;
    uint32_t x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            voodoo3_2d_plot(s, dx + x, dy + y, color, rop);
        }
    }
    voodoo3_2d_dirty(s, base + dy * stride,
                     base + (dy + h) * stride + 4 * (dx + w));
}

static void voodoo3_2d_s2s_blt(Voodoo3State *s, uint32_t srcxy)
{
    uint32_t cmd = s->d2_regs[V3_2D_COMMAND / 4];
    uint32_t size = s->d2_regs[V3_2D_DSTSIZE / 4];
    uint32_t dstxy = s->d2_regs[V3_2D_DSTXY / 4];
    uint32_t srcbase = s->d2_regs[V3_2D_SRCBASE / 4] & 0xffffff;
    uint32_t dstbase = s->d2_regs[V3_2D_DSTBASE / 4] & 0xffffff;
    uint32_t sstride = s->d2_regs[V3_2D_SRCFORMAT / 4] & 0x3fff;
    uint32_t dstride = s->d2_regs[V3_2D_DSTFORMAT / 4] & 0x3fff;
    uint32_t bpp = voodoo3_2d_bpp(s->d2_regs[V3_2D_DSTFORMAT / 4]);
    uint32_t w = size & 0x1fff, h = (size >> 16) & 0x1fff;
    int sx = srcxy & 0x1fff, sy = (srcxy >> 16) & 0x1fff;
    int dx = dstxy & 0x1fff, dy = (dstxy >> 16) & 0x1fff;
    int xdir = (cmd & V3_2D_CMD_X_REVERSE) ? -1 : 1;
    int ydir = (cmd & V3_2D_CMD_Y_REVERSE) ? -1 : 1;
    uint8_t rop = cmd >> 24;
    uint32_t x, y;

    if (!bpp) {
        return;
    }
    /*
     * With reverse direction sx/sy/dx/dy name the bottom/right edge.
     * Copy pixel by pixel honouring the direction so that overlapping
     * areas work like on real hardware.
     */
    for (y = 0; y < h; y++) {
        int syy = sy + ydir * y;
        int dyy = dy + ydir * y;

        for (x = 0; x < w; x++) {
            int sxx = sx + xdir * x;
            uint32_t soff = srcbase + syy * sstride + sxx * bpp;
            uint32_t color = 0;
            unsigned int i;

            if (syy < 0 || sxx < 0 || soff + bpp > s->vga.vram_size) {
                continue;
            }
            for (i = 0; i < bpp; i++) {
                color |= s->vga.vram_ptr[soff + i] << (i * 8);
            }
            voodoo3_2d_plot(s, dx + xdir * x, dyy, color, rop);
        }
    }
    if (ydir > 0) {
        voodoo3_2d_dirty(s, dstbase + dy * dstride,
                         dstbase + (dy + h) * dstride + 4 * (dx + w));
    } else {
        voodoo3_2d_dirty(s, dstbase + (dy - (int)h + 1) * dstride,
                         dstbase + (dy + 1) * dstride + 4 * (dx + 1));
    }
}

/*
 * Host-to-screen blt: the host streams data into the launch area.
 * Only monochrome expansion (source format 0) is implemented; rows
 * are consumed byte aligned which matches how tdfxfb packs them.
 */
static void voodoo3_2d_h2s_data(Voodoo3State *s, uint32_t data)
{
    uint32_t cmd = s->d2_regs[V3_2D_COMMAND / 4];
    uint32_t size = s->d2_regs[V3_2D_DSTSIZE / 4];
    uint32_t dstxy = s->d2_regs[V3_2D_DSTXY / 4];
    uint32_t srcfmt = s->d2_regs[V3_2D_SRCFORMAT / 4];
    uint32_t w = size & 0x1fff, h = (size >> 16) & 0x1fff;
    uint32_t dx = dstxy & 0x1fff, dy = (dstxy >> 16) & 0x1fff;
    uint32_t fore = s->d2_regs[V3_2D_COLORFORE / 4];
    uint32_t back = s->d2_regs[V3_2D_COLORBACK / 4];
    uint32_t dstbase = s->d2_regs[V3_2D_DSTBASE / 4] & 0xffffff;
    uint32_t dstride = s->d2_regs[V3_2D_DSTFORMAT / 4] & 0x3fff;
    uint8_t rop = cmd >> 24;
    unsigned int byte, bit;

    if (((srcfmt >> 16) & 7) != 0) {
        qemu_log_mask(LOG_UNIMP,
                      "voodoo3: only monochrome host blts implemented\n");
        s->h2s.active = false;
        return;
    }
    if (!s->h2s.active) {
        if (!w || !h) {
            return;
        }
        s->h2s.active = true;
        s->h2s.x = 0;
        s->h2s.y = 0;
    }
    for (byte = 0; byte < 4 && s->h2s.active; byte++) {
        uint8_t b = data >> (byte * 8);

        for (bit = 0; bit < 8; bit++) {
            uint32_t color = (b & (0x80 >> bit)) ? fore : back;

            voodoo3_2d_plot(s, dx + s->h2s.x, dy + s->h2s.y, color, rop);
            if (++s->h2s.x >= w) {
                /* rows are byte aligned: drop the rest of this byte */
                s->h2s.x = 0;
                if (++s->h2s.y >= h) {
                    s->h2s.active = false;
                    voodoo3_2d_dirty(s, dstbase + dy * dstride,
                                     dstbase + (dy + h) * dstride +
                                     4 * (dx + w));
                }
                break;
            }
        }
    }
}

static void voodoo3_2d_launch(Voodoo3State *s, uint32_t data)
{
    uint32_t cmd = s->d2_regs[V3_2D_COMMAND / 4];

    if (!voodoo3_2d_ok(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "voodoo3: 2D engine command while "
                      "gfx/memory clocks are not initialised, ignored\n");
        return;
    }
    switch (cmd & V3_2D_OP_MASK) {
    case V3_2D_OP_NOP:
        break;
    case V3_2D_OP_RECTFILL:
        voodoo3_2d_rectfill(s, data);
        break;
    case V3_2D_OP_S2S_BLT:
        voodoo3_2d_s2s_blt(s, data);
        break;
    case V3_2D_OP_H2S_BLT:
        voodoo3_2d_h2s_data(s, data);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "voodoo3: unimplemented 2D op %d\n",
                      (int)(cmd & V3_2D_OP_MASK));
        break;
    }
}

static uint64_t voodoo3_2d_read(Voodoo3State *s, hwaddr addr, unsigned size)
{
    if (addr < 0x100) {
        return extract32(s->d2_regs[addr / 4], (addr & 3) * 8, size * 8);
    }
    return 0;
}

static void voodoo3_2d_write(Voodoo3State *s, hwaddr addr, uint64_t data,
                             unsigned size)
{
    uint32_t val;

    if (addr >= 0x100) {
        return;
    }
    if (addr >= V3_2D_LAUNCH) {
        voodoo3_2d_launch(s, data);
        return;
    }
    val = deposit32(s->d2_regs[addr / 4], (addr & 3) * 8, size * 8, data);
    s->d2_regs[addr / 4] = val;
    if ((addr & ~3) == V3_2D_COMMAND) {
        s->h2s.active = false;
        if (val & V3_2D_CMD_INITIATE) {
            switch (val & V3_2D_OP_MASK) {
            case V3_2D_OP_RECTFILL:
                voodoo3_2d_launch(s, s->d2_regs[V3_2D_DSTXY / 4]);
                break;
            case V3_2D_OP_S2S_BLT:
                voodoo3_2d_launch(s, s->d2_regs[V3_2D_SRCXY / 4]);
                break;
            default:
                break;
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/* I/O register space (BAR 2 and start of BAR 0)                    */

static void voodoo3_dac_write(Voodoo3State *s, uint32_t val)
{
    uint32_t idx = s->io_regs[V3_DACADDR / 4] & 0xff;

    /*
     * The CLUT is shared with the VGA DAC. Values are always 8 bit
     * wide here; the VGA core widens 6 bit values when dac_8bit is
     * clear, so narrow them in that case.
     */
    int shift = s->vga.dac_8bit ? 0 : 2;

    s->vga.palette[idx * 3 + 0] = ((val >> 16) & 0xff) >> shift;
    s->vga.palette[idx * 3 + 1] = ((val >> 8) & 0xff) >> shift;
    s->vga.palette[idx * 3 + 2] = (val & 0xff) >> shift;
    s->io_regs[V3_DACADDR / 4] = (idx + 1) & 0xff;
    s->vga.full_update_gfx = true;
}

static uint32_t voodoo3_dac_read(Voodoo3State *s)
{
    uint32_t idx = s->io_regs[V3_DACADDR / 4] & 0xff;
    int shift = s->vga.dac_8bit ? 0 : 2;

    return (uint32_t)(s->vga.palette[idx * 3 + 0] << shift) << 16 |
           (uint32_t)(s->vga.palette[idx * 3 + 1] << shift) << 8 |
           (uint32_t)(s->vga.palette[idx * 3 + 2] << shift);
}

static uint32_t voodoo3_vsp_update(Voodoo3State *s, uint32_t val)
{
    uint32_t in = 0;

    if (val & V3_VSP_DDC_EN) {
        bool scl = !!(val & V3_VSP_DDC_SCL_OUT);
        bool sda = !!(val & V3_VSP_DDC_SDA_OUT);

        bitbang_i2c_set(&s->bbi2c, BITBANG_I2C_SCL, scl);
        sda = bitbang_i2c_set(&s->bbi2c, BITBANG_I2C_SDA, sda);
        in |= scl ? V3_VSP_DDC_SCL_IN : 0;
        in |= sda ? V3_VSP_DDC_SDA_IN : 0;
    } else {
        /* lines released, pulled high */
        in |= V3_VSP_DDC_SCL_IN | V3_VSP_DDC_SDA_IN;
    }
    /* nothing lives on the second (monitor) I2C bus */
    if (val & V3_VSP_I2C_EN) {
        in |= (val & V3_VSP_I2C_SCL_OUT) ? V3_VSP_I2C_SCL_IN : 0;
        in |= (val & V3_VSP_I2C_SDA_OUT) ? V3_VSP_I2C_SDA_IN : 0;
    } else {
        in |= V3_VSP_I2C_SCL_IN | V3_VSP_I2C_SDA_IN;
    }
    return (val & ~(V3_VSP_DDC_SCL_IN | V3_VSP_DDC_SDA_IN |
                    V3_VSP_I2C_SCL_IN | V3_VSP_I2C_SDA_IN)) | in;
}

static uint64_t voodoo3_reg_read(Voodoo3State *s, hwaddr addr, unsigned size)
{
    uint32_t val;

    switch (addr & ~3) {
    case V3_STATUS:
    {
        uint32_t visible, line = voodoo3_scanline(s, &visible);

        /* command FIFO always has room, engine never busy */
        val = 0x1f;
        if (line >= visible) {
            val |= BIT(6); /* vertical retrace */
        }
        break;
    }
    case V3_VIDCURLIN:
        val = voodoo3_scanline(s, NULL);
        break;
    case V3_DACDATA:
        val = voodoo3_dac_read(s);
        break;
    case V3_VIDINSTATUS:
        val = 0;
        break;
    default:
        val = s->io_regs[(addr & ~3) / 4];
        break;
    }
    return extract32(val, (addr & 3) * 8, size * 8);
}

static void voodoo3_reg_write(Voodoo3State *s, hwaddr addr, uint64_t data,
                              unsigned size)
{
    unsigned int idx = (addr & ~3) / 4;
    uint32_t val = deposit32(s->io_regs[idx], (addr & 3) * 8, size * 8, data);

    switch (addr & ~3) {
    case V3_STATUS:
    case V3_VIDCURLIN:
    case V3_VIDINSTATUS:
        /* read only */
        return;
    case V3_DACDATA:
        voodoo3_dac_write(s, val);
        return;
    case V3_DACADDR:
        s->io_regs[idx] = val & 0x1ff;
        return;
    case V3_VIDSERPARPORT:
        s->io_regs[idx] = voodoo3_vsp_update(s, val);
        return;
    case V3_DRAMINIT0:
        s->io_regs[idx] = val;
        s->draminit0_written = true;
        voodoo3_update_memory_gate(s);
        return;
    case V3_DRAMINIT1:
        s->io_regs[idx] = val;
        s->draminit1_written = true;
        voodoo3_update_memory_gate(s);
        return;
    case V3_DRAMCOMMAND:
        s->io_regs[idx] = val;
        /* any mode register programming counts as DRAM init */
        s->dram_mode_set = true;
        voodoo3_update_memory_gate(s);
        return;
    case V3_PLLCTRL1:
    case V3_PLLCTRL2:
        s->io_regs[idx] = val;
        voodoo3_update_memory_gate(s);
        return;
    case V3_PLLCTRL0:
    case V3_VIDPROCCFG:
    case V3_VIDSCREENSIZE:
    case V3_VIDDESKSTART:
    case V3_VIDDESKSTRIDE:
    case V3_DACMODE:
        s->io_regs[idx] = val;
        voodoo3_update_mode(s);
        if ((addr & ~3) == V3_VIDPROCCFG) {
            voodoo3_update_cursor(s);
        }
        return;
    case V3_VGAINIT0:
        s->io_regs[idx] = val;
        s->vga.dac_8bit = !!(val & V3_VGAINIT0_8BIT_DAC);
        voodoo3_update_mode(s);
        return;
    case V3_HWCURPATADDR:
    case V3_HWCURLOC:
    case V3_HWCURC0:
    case V3_HWCURC1:
        s->io_regs[idx] = val;
        voodoo3_update_cursor(s);
        return;
    default:
        s->io_regs[idx] = val;
        return;
    }
}

/* ---------------------------------------------------------------- */
/* memory regions                                                   */

static uint64_t voodoo3_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Voodoo3State *s = opaque;

    if (addr < 0x100) {
        return voodoo3_reg_read(s, addr, size);
    }
    if (addr >= 0x100000 && addr < 0x200000) {
        return voodoo3_2d_read(s, addr - 0x100000, size);
    }
    return 0;
}

static void voodoo3_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                               unsigned size)
{
    Voodoo3State *s = opaque;

    if (addr < 0x100) {
        voodoo3_reg_write(s, addr, data, size);
    } else if (addr >= 0x100000 && addr < 0x200000) {
        voodoo3_2d_write(s, addr - 0x100000, data, size);
    } else if (addr >= 0x200000 && addr < 0x600000) {
        /* 3D engine: accept and ignore NOPs, tdfxfb uses it for sync */
        if ((addr & 0xfffff) != 0x120 || (data & 0xff) != 0) {
            qemu_log_mask(LOG_UNIMP,
                          "voodoo3: 3D engine not implemented "
                          "(write 0x%" PRIx64 " @ 0x%" HWADDR_PRIx ")\n",
                          data, addr);
        }
    }
}

static const MemoryRegionOps voodoo3_mmio_ops = {
    .read = voodoo3_mmio_read,
    .write = voodoo3_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static uint64_t voodoo3_io_read(void *opaque, hwaddr addr, unsigned size)
{
    Voodoo3State *s = opaque;
    uint64_t val = 0;
    unsigned int i;

    if (addr >= 0xb0 && addr < 0xe0) {
        /* VGA core registers, decoded like legacy 0x3b0..0x3df */
        for (i = 0; i < size; i++) {
            val |= (uint64_t)vga_ioport_read(&s->vga, 0x300 + addr + i) <<
                   (i * 8);
        }
        return val;
    }
    return voodoo3_reg_read(s, addr, size);
}

static void voodoo3_io_write(void *opaque, hwaddr addr, uint64_t data,
                             unsigned size)
{
    Voodoo3State *s = opaque;
    unsigned int i;

    if (addr >= 0xb0 && addr < 0xe0) {
        for (i = 0; i < size; i++) {
            vga_ioport_write(&s->vga, 0x300 + addr + i, (data >> (i * 8)) &
                             0xff);
        }
        return;
    }
    voodoo3_reg_write(s, addr, data, size);
}

static const MemoryRegionOps voodoo3_io_ops = {
    .read = voodoo3_io_read,
    .write = voodoo3_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/*
 * The linear framebuffer while the memory controller has not been set
 * up: reads float high, writes go nowhere.
 */
static uint64_t voodoo3_lfb_dead_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    return MAKE_64BIT_MASK(0, size * 8);
}

static void voodoo3_lfb_dead_write(void *opaque, hwaddr addr, uint64_t data,
                                   unsigned size)
{
}

static const MemoryRegionOps voodoo3_lfb_dead_ops = {
    .read = voodoo3_lfb_dead_read,
    .write = voodoo3_lfb_dead_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ---------------------------------------------------------------- */

static void voodoo3_realize(PCIDevice *dev, Error **errp)
{
    Voodoo3State *s = VOODOO3(dev);
    VGACommonState *vga = &s->vga;
    I2CBus *i2cbus;

    /*
     * A real Voodoo 3 3000 has 16 MB; the BARs are 32 MB regardless
     * of the amount fitted.
     */
    if (vga->vram_size_mb != 16) {
        error_setg(errp, "voodoo3 only supports vgamem_mb=16");
        return;
    }

    if (!vga_common_init(vga, OBJECT(s), errp)) {
        return;
    }
    vga_init(vga, OBJECT(s), pci_address_space(dev),
             pci_address_space_io(dev), true);
    vga->con = qemu_graphic_console_create(DEVICE(s), 0, &voodoo3_gfx_ops, s);

    /* DDC bus with the EDID eeprom of the attached monitor */
    i2cbus = i2c_init_bus(DEVICE(s), "voodoo3.ddc");
    bitbang_i2c_init(&s->bbi2c, i2cbus);
    i2c_slave_set_address(I2C_SLAVE(&s->i2cddc), 0x50);
    qdev_realize(DEVICE(&s->i2cddc), BUS(i2cbus), &error_abort);

    memory_region_init_io(&s->mmio, OBJECT(s), &voodoo3_mmio_ops, s,
                          "voodoo3.mmio", 32 * MiB);

    memory_region_init(&s->lfb, OBJECT(s), "voodoo3.lfb", 32 * MiB);
    memory_region_init_io(&s->lfb_dead, OBJECT(s), &voodoo3_lfb_dead_ops, s,
                          "voodoo3.lfb-uninit", 32 * MiB);
    memory_region_add_subregion(&s->lfb, 0, &s->lfb_dead);
    memory_region_add_subregion_overlap(&s->lfb, 0, &vga->vram, 1);
    memory_region_set_enabled(&vga->vram, false);

    memory_region_init_io(&s->io, OBJECT(s), &voodoo3_io_ops, s,
                          "voodoo3.io", 0x100);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_TYPE_32, &s->mmio);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->lfb);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_IO, &s->io);

    dev->config[PCI_INTERRUPT_PIN] = 1;
}

static void voodoo3_reset(DeviceState *dev)
{
    Voodoo3State *s = VOODOO3(dev);

    vga_common_reset(&s->vga);

    memset(s->io_regs, 0, sizeof(s->io_regs));
    memset(s->d2_regs, 0, sizeof(s->d2_regs));
    /*
     * Cold power-on state: PLLs unlocked, DRAM controller
     * unconfigured. The VGA BIOS (or a driver doing its job) has to
     * bring all of this up before the card produces a picture.
     */
    s->draminit0_written = false;
    s->draminit1_written = false;
    s->dram_mode_set = false;
    s->h2s.active = false;
    s->io_regs[V3_VIDSERPARPORT / 4] = voodoo3_vsp_update(s, 0);
    s->mode = V3_MODE_VGA; /* force transition in update_mode */
    voodoo3_update_memory_gate(s);
    voodoo3_update_cursor(s);
}

static void voodoo3_exit(PCIDevice *dev)
{
    Voodoo3State *s = VOODOO3(dev);

    qemu_graphic_console_close(s->vga.con);
    cursor_unref(s->cursor);
}

static int voodoo3_post_load(void *opaque, int version_id)
{
    Voodoo3State *s = opaque;

    s->vga.dac_8bit = !!(s->io_regs[V3_VGAINIT0 / 4] & V3_VGAINIT0_8BIT_DAC);
    memory_region_set_enabled(&s->vga.vram, voodoo3_mem_ok(s));
    s->need_blank = true;
    voodoo3_update_cursor(s);
    return 0;
}

static const VMStateDescription vmstate_voodoo3 = {
    .name = "voodoo3",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = voodoo3_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, Voodoo3State),
        VMSTATE_STRUCT(vga, Voodoo3State, 0, vmstate_vga_common,
                       VGACommonState),
        VMSTATE_UINT32_ARRAY(io_regs, Voodoo3State, V3_IO_REG_NB),
        VMSTATE_UINT32_ARRAY(d2_regs, Voodoo3State, V3_2D_REG_NB),
        VMSTATE_BOOL(draminit0_written, Voodoo3State),
        VMSTATE_BOOL(draminit1_written, Voodoo3State),
        VMSTATE_BOOL(dram_mode_set, Voodoo3State),
        VMSTATE_BOOL(h2s.active, Voodoo3State),
        VMSTATE_UINT32(h2s.x, Voodoo3State),
        VMSTATE_UINT32(h2s.y, Voodoo3State),
        VMSTATE_UINT8(mode, Voodoo3State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property voodoo3_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", Voodoo3State, vga.vram_size_mb, 16),
    DEFINE_EDID_PROPERTIES(Voodoo3State, i2cddc.edid_info),
};

static void voodoo3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, voodoo3_reset);
    device_class_set_props(dc, voodoo3_properties);
    dc->vmsd = &vmstate_voodoo3;
    dc->desc = "3dfx Voodoo 3 3000 PCI";
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    k->class_id = PCI_CLASS_DISPLAY_VGA;
    k->vendor_id = PCI_VENDOR_ID_3DFX;
    k->device_id = PCI_DEVICE_ID_3DFX_VOODOO3;
    k->subsystem_vendor_id = PCI_VENDOR_ID_3DFX;
    k->subsystem_id = PCI_SUBDEVICE_ID_3DFX_V3_3000;
    k->revision = 1;
    k->realize = voodoo3_realize;
    k->exit = voodoo3_exit;
}

static void voodoo3_init(Object *o)
{
    Voodoo3State *s = VOODOO3(o);

    object_initialize_child(o, "ddc", &s->i2cddc, TYPE_I2CDDC);
}

static const TypeInfo voodoo3_info = {
    .name = TYPE_VOODOO3,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Voodoo3State),
    .class_init = voodoo3_class_init,
    .instance_init = voodoo3_init,
    .interfaces = (const InterfaceInfo[]) {
          { INTERFACE_CONVENTIONAL_PCI_DEVICE },
          { },
    },
};

static void voodoo3_register_types(void)
{
    type_register_static(&voodoo3_info);
}

type_init(voodoo3_register_types)
