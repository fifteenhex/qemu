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
 * The 3D (SST/Avenger) core models the common pixel pipeline: both
 * triangle paths (direct and the setup unit, fan/strip), Gouraud and
 * point/bilinear perspective-correct texturing in every non-palette
 * format, depth test/write, chroma-key, alpha test, fog (iterated
 * alpha/Z, and the fog table), alpha blend, fastfill and swapbuffer,
 * over a 565 colour and 16bpp depth buffer.
 *
 * Simplifications: memory is always 16 MB (dramInit sizing bits are
 * ignored); legacy VGA decode isn't gated on vgaInit0 bit 9; host blts
 * pack rows byte-aligned (what tdfxfb generates); big-endian swizzling
 * (miscInit0 30/31) is unimplemented. The colour path uses the
 * rgb/aselect mux rather than the full fbzColorPath/textureMode combine
 * units; palette/NCC textures, mipmap LOD selection, a second TMU, and
 * pixel-pipeline LFB writes are not modelled yet.
 *
 * Texture memory addressing (linear and 128x32 tiled, texBaseAddr bit 0
 * with the tile stride in [31:25]) is validated against hardware with
 * tdfx_texmap. texBaseAddr is taken as the corner of the sampled level;
 * the hardware treats it as the corner of the virtual 256x256 level, so
 * mip/sub-256 textures still need the per-level offset added (TODO,
 * pending a hardware probe of the smaller LODs).
 */

#include "qemu/osdep.h"
#include <math.h>
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

/*
 * 3D ("SST"/Avenger) register space at BAR0 + 0x200000.
 * Offsets and formats from the 3dfx Glide3 h3regs.h / sst.h spec.
 */
#define V3_3D_VA                0x008   /* vertex A: x,y at 0x08,0x0c (12.4) */
#define V3_3D_VB                0x010
#define V3_3D_VC                0x018
#define V3_3D_STARTR            0x020   /* iterated params at vertex A */
#define V3_3D_STARTG            0x024
#define V3_3D_STARTB            0x028
#define V3_3D_STARTZ            0x02c
#define V3_3D_STARTA            0x030
#define V3_3D_STARTS            0x034
#define V3_3D_STARTT            0x038
#define V3_3D_STARTW            0x03c
#define V3_3D_DPDX              0x040   /* 8 x/dx gradients 0x40..0x5c */
#define V3_3D_DPDY              0x060   /* 8 y/dy gradients 0x60..0x7c */
#define V3_3D_TRIANGLECMD       0x080
#define V3_3D_FVA               0x088   /* float vertex A */
#define V3_3D_FVB               0x090
#define V3_3D_FVC               0x098
#define V3_3D_FSTARTR           0x0a0
#define V3_3D_FDPDX             0x0c0
#define V3_3D_FDPDY             0x0e0
#define V3_3D_FTRIANGLECMD      0x100
#define V3_3D_FBZCOLORPATH      0x104
#define V3_3D_FOGMODE           0x108
#define V3_3D_ALPHAMODE         0x10c
#define V3_3D_FBZMODE           0x110
#define V3_3D_LFBMODE           0x114
#define V3_3D_CLIPLEFTRIGHT     0x118
#define V3_3D_CLIPBOTTOMTOP     0x11c
#define V3_3D_NOPCMD            0x120
#define V3_3D_FASTFILLCMD       0x124
#define V3_3D_SWAPBUFFERCMD     0x128
#define V3_3D_FOGCOLOR          0x12c
#define V3_3D_ZACOLOR           0x130
#define V3_3D_CHROMAKEY         0x134
#define V3_3D_C0                0x144
#define V3_3D_C1                0x148
#define V3_3D_RENDERMODE        0x1e0
#define V3_3D_COLBUFFERADDR     0x1ec
#define V3_3D_COLBUFFERSTRIDE   0x1f0
#define V3_3D_AUXBUFFERADDR     0x1f4
#define V3_3D_AUXBUFFERSTRIDE   0x1f8
#define V3_3D_SSETUPMODE        0x260
#define V3_3D_SVX               0x264
#define V3_3D_SVY               0x268
#define V3_3D_SARGB             0x26c
#define V3_3D_SRED              0x270
#define V3_3D_SGREEN            0x274
#define V3_3D_SBLUE             0x278
#define V3_3D_SALPHA            0x27c
#define V3_3D_SVZ               0x280
#define V3_3D_SWOOWFBI          0x284
#define V3_3D_SOOW0             0x288
#define V3_3D_SSOW0             0x28c
#define V3_3D_STOW0             0x290
#define V3_3D_SDRAWTRICMD       0x2a0
#define V3_3D_SBEGINTRICMD      0x2a4
#define V3_3D_TEXTUREMODE       0x300
#define V3_3D_TLOD              0x304
#define V3_3D_TEXBASEADDR       0x30c

#define V3_3D_INTRCTRL          0x004   /* interrupt control */

#define V3_3D_REG_NB            (0x400 / 4)

/* status register bits (BAR0 + 0x200000) */
#define V3_STATUS_VRETRACE      BIT(6)
#define V3_STATUS_BUSY          BIT(9)
#define V3_STATUS_SWAP_SHIFT    28      /* [30:28] pending swapbuffers */
#define V3_STATUS_PCIINT        BIT(31)

/* intrCtrl bits (our vsync-interrupt contract, see include/uapi tdfx3d.h) */
#define V3_INTR_VSYNC_ENABLE    BIT(0)  /* raise PCI IRQ on vertical blank */
#define V3_INTR_VSYNC_CLEAR     BIT(1)  /* write 1 to acknowledge the IRQ */
#define V3_INTR_VSYNC_PENDING   BIT(6)  /* read: a vblank IRQ is pending */

/* fbzMode bits */
#define V3_FBZ_ENCLIP           BIT(0)
#define V3_FBZ_ENCHROMAKEY      BIT(1)
#define V3_FBZ_ENDEPTH          BIT(4)
#define V3_FBZ_ZFUNC_SHIFT      5
#define V3_FBZ_ZFUNC_MASK       (0x7 << V3_FBZ_ZFUNC_SHIFT)
#define V3_FBZ_ENDITHER         BIT(8)
#define V3_FBZ_RGBWRMASK        BIT(9)
#define V3_FBZ_DEPTHWRMASK      BIT(10)
#define V3_FBZ_DRAWBUFFER_SHIFT 14
#define V3_FBZ_DRAWBUFFER_MASK  (0x3 << V3_FBZ_DRAWBUFFER_SHIFT)
#define V3_FBZ_ENWBUFFER        BIT(20)
#define V3_FBZ_YORIGIN          BIT(17)

/* zfunc sub-bits (LT/EQ/GT), test passes if (src REL dst) matches enabled set */
#define V3_ZF_LT                BIT(0)
#define V3_ZF_EQ                BIT(1)
#define V3_ZF_GT                BIT(2)

/* alphaMode bits */
#define V3_ALPHA_ENTEST         BIT(0)
#define V3_ALPHA_FUNC_SHIFT     1
#define V3_ALPHA_FUNC_MASK      (0x7 << V3_ALPHA_FUNC_SHIFT)
#define V3_ALPHA_ENBLEND        BIT(4)
#define V3_ALPHA_SRCFUNC_SHIFT  8
#define V3_ALPHA_DSTFUNC_SHIFT  12
#define V3_ALPHA_BLENDFUNC_MASK 0xf
#define V3_ALPHA_REF_SHIFT      24

/* alpha blend function codes */
#define V3_BLEND_ZERO           0x0
#define V3_BLEND_SRCALPHA       0x1
#define V3_BLEND_COLOR          0x2
#define V3_BLEND_DSTALPHA       0x3
#define V3_BLEND_ONE            0x4
#define V3_BLEND_OMSRCALPHA     0x5
#define V3_BLEND_OMCOLOR        0x6
#define V3_BLEND_OMDSTALPHA     0x7

/* fbzColorPath bits (subset) */
#define V3_CP_RGBSELECT_MASK    0x3     /* 0=iterated, 1=texture, 2=color1 */
#define V3_CP_ASELECT_SHIFT     2
#define V3_CP_ASELECT_MASK      (0x3 << V3_CP_ASELECT_SHIFT)
#define V3_CP_TEXTURE_EN        BIT(27) /* enable texture mapping (TREX->FBI) */

/* textureMode bits */
#define V3_TEX_ENABLE           BIT(0)   /* perspective/enable (tpersp) */
#define V3_TEX_MINFILTER        BIT(1)   /* 1 = bilinear minification */
#define V3_TEX_MAGFILTER        BIT(2)   /* 1 = bilinear magnification */
#define V3_TEX_CLAMPS           BIT(6)   /* 1 = clamp S (else wrap) */
#define V3_TEX_CLAMPT           BIT(7)   /* 1 = clamp T (else wrap) */
#define V3_TEX_TFORMAT_SHIFT    8
#define V3_TEX_TFORMAT_MASK     (0xf << V3_TEX_TFORMAT_SHIFT)
#define V3_TEX_TC_ADD_CLOCAL    BIT(18)  /* TMU RGB: add local texel */
#define V3_TEX_TCA_ADD_CLOCAL   BIT(27)  /* TMU alpha: add local texel */
/* texture formats (textureMode[11:8]) - values per the Avenger spec */
#define V3_TFMT_RGB332          0        /* 8bpp 3-3-2 */
#define V3_TFMT_YIQ             1        /* 8bpp NCC (table 0) */
#define V3_TFMT_A8              2        /* 8bpp alpha */
#define V3_TFMT_I8              3        /* 8bpp intensity */
#define V3_TFMT_AI44            4        /* 8bpp alpha(4) intensity(4) */
#define V3_TFMT_P8              5        /* 8bpp palette -> RGB */
#define V3_TFMT_P8_RGBA         6        /* 8bpp palette -> RGBA */
#define V3_TFMT_ARGB8332        8        /* 16bpp 8-3-3-2 */
#define V3_TFMT_AYIQ            9        /* 16bpp 8-4-2-2 NCC */
#define V3_TFMT_RGB565          10       /* 16bpp 5-6-5 */
#define V3_TFMT_ARGB1555        11       /* 16bpp 1-5-5-5 */
#define V3_TFMT_ARGB4444        12       /* 16bpp 4-4-4-4 */
#define V3_TFMT_AI88            13       /* 16bpp alpha(8) intensity(8) */
#define V3_TFMT_AP88            14       /* 16bpp alpha(8) palette(8) */

/* fogMode bits */
#define V3_FOG_ENABLE           BIT(0)
#define V3_FOG_ADD              BIT(1)   /* 0=fogColor, 1=zero */
#define V3_FOG_MULT             BIT(2)   /* 0=CCU rgb, 1=zero */
#define V3_FOG_ALPHA            BIT(3)   /* fog factor = iterated alpha */
#define V3_FOG_Z                BIT(4)   /* fog factor = iterated Z msb */
#define V3_FOG_CONSTANT         BIT(5)   /* add fogColor constant */

/* fog table: 32 32-bit words (64 entries), 3D reg index 0x60 */
#define V3_3D_FOGTABLE          0x180
#define V3_3D_FOGTABLE_NB       32

/* sSetupMode: which per-vertex params are valid */
#define V3_SSETUP_RGB           BIT(0)
#define V3_SSETUP_ALPHA         BIT(1)
#define V3_SSETUP_Z             BIT(2)
#define V3_SSETUP_WFBI          BIT(3)
#define V3_SSETUP_W0            BIT(4)
#define V3_SSETUP_ST0           BIT(5)
#define V3_SSETUP_CULLING_EN    BIT(16)
#define V3_SSETUP_CULL_SIGN     BIT(17)
#define V3_SSETUP_STRIP_MODE    BIT(18)

/* PLL reference clock, kHz */
#define V3_PLL_REF_KHZ          14318

enum { V3_MODE_BLANK, V3_MODE_VGA, V3_MODE_DESKTOP };

/* A rasteriser vertex, in screen space with iterated parameters. */
typedef struct V3Vertex {
    float x, y;         /* screen position (pixels, sub-pixel ok) */
    float z;            /* depth, 0..65535 */
    float oow;          /* 1/w for perspective-correct texturing */
    float r, g, b, a;   /* colour components, 0..255 */
    float sow, tow;     /* s/w, t/w */
} V3Vertex;

struct Voodoo3State {
    PCIDevice dev;
    VGACommonState vga;

    MemoryRegion mmio;      /* BAR 0: 32 MB register space */
    MemoryRegion lfb;       /* BAR 1: 32 MB linear framebuffer window */
    MemoryRegion lfb_dead;  /* ... backing while DRAM is uninitialised */
    MemoryRegion io;        /* BAR 2: 256 byte I/O space */
    MemoryRegion vbios;     /* ROM BAR: synthetic config-table VBIOS */

    bool fake_vbios;        /* synthesise a minimal VBIOS when none supplied */

    uint32_t io_regs[V3_IO_REG_NB];
    uint32_t d2_regs[V3_2D_REG_NB];
    uint32_t d3_regs[V3_3D_REG_NB];

    /*
     * Strip/fan setup-unit vertex accumulator. The Avenger setup unit
     * takes one vertex per sDrawTriCMD and emits a triangle once three
     * vertices are available (fan/strip per sSetupMode).
     */
    V3Vertex svtx[3];   /* setup-unit vertex window (strip/fan) */
    int snvert;         /* vertices accumulated since last Begin */

    /* vertical-blank / buffer-swap emulation */
    QEMUTimer vblank_timer;
    uint32_t vblank_count;      /* frames elapsed (also drives retrace) */
    uint32_t swap_pending;      /* queued swapbufferCMDs (status[30:28]) */
    uint32_t swap_buf;          /* vram offset queued for display */
    uint32_t swap_interval;     /* vblanks to wait before the queued swap */
    bool vsync_irq;             /* vsync interrupt asserted (status[31]) */

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
/* 3D engine (Avenger/SST core at BAR0 + 0x200000)                  */
/*                                                                  */
/* The SST rasteriser is fed a triangle either directly (vertices + */
/* per-parameter screen-space gradients, via [F]triangleCMD) or     */
/* through the on-chip setup unit (per-vertex data + sDrawTriCMD,    */
/* which is what Glide3/Mesa use). Both feed the same span walker.   */

static float v3_reg_f(Voodoo3State *s, unsigned off)
{
    uint32_t u = s->d3_regs[off / 4];
    float f;

    memcpy(&f, &u, sizeof(f));
    return f;
}

static float v3_reg_fx(Voodoo3State *s, unsigned off, int frac)
{
    return (float)(int32_t)s->d3_regs[off / 4] / (float)(1 << frac);
}

static inline float v3_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool voodoo3_zfunc_pass(unsigned func, uint32_t src, uint32_t dst)
{
    return ((func & V3_ZF_LT) && src < dst) ||
           ((func & V3_ZF_EQ) && src == dst) ||
           ((func & V3_ZF_GT) && src > dst);
}

static float voodoo3_blend_factor(unsigned f, float sa, float da, float comp)
{
    switch (f) {
    case V3_BLEND_ZERO:       return 0.0f;
    case V3_BLEND_ONE:        return 1.0f;
    case V3_BLEND_SRCALPHA:   return sa / 255.0f;
    case V3_BLEND_OMSRCALPHA: return 1.0f - sa / 255.0f;
    case V3_BLEND_DSTALPHA:   return da / 255.0f;
    case V3_BLEND_OMDSTALPHA: return 1.0f - da / 255.0f;
    case V3_BLEND_COLOR:      return comp / 255.0f;
    case V3_BLEND_OMCOLOR:    return 1.0f - comp / 255.0f;
    default:                  return 1.0f;
    }
}

/* Sample TMU0 (point sampled). Returns colour in r/g/b/a (0..255). */
/* bytes per texel for a texture format (fmt 0..6 = 8bpp, 8..14 = 16bpp) */
static unsigned voodoo3_tfmt_bpt(unsigned fmt)
{
    return fmt <= V3_TFMT_P8_RGBA ? 1 : 2;
}

/*
 * Texture memory address of texel (u,v). A linear texture is row-major with
 * a dim-wide pitch. A tiled texture (texBaseAddr bit 0) is stored in
 * 128-byte x 32-row tiles laid out tile-row-major, the tile stride coming
 * from texBaseAddr[31:25] in tiles. Both layouts verified against hardware
 * with tdfx_texmap: e.g. for a 256x256 16bpp tiled surface texel (64,0)
 * lands at slot 2048 and (0,32) at slot 8192, matching this arithmetic.
 */
#define V3_TILE_W 128u   /* bytes  */
#define V3_TILE_H 32u    /* rows   */

static uint32_t voodoo3_texel_off(uint32_t base, unsigned bpt, unsigned dim,
                                  bool tiled, unsigned stride_tiles,
                                  int u, int v)
{
    if (tiled) {
        unsigned xb = (unsigned)u * bpt;
        unsigned tx = xb / V3_TILE_W, ix = xb % V3_TILE_W;
        unsigned ty = (unsigned)v / V3_TILE_H, iy = (unsigned)v % V3_TILE_H;
        return base + (ty * stride_tiles + tx) * (V3_TILE_W * V3_TILE_H)
                    + iy * V3_TILE_W + ix;
    }
    return base + (unsigned)(v * (int)dim + u) * bpt;
}

/* fetch and decode one texel at integer (u,v) into 0..255 RGBA floats */
static void voodoo3_fetch_texel(Voodoo3State *s, uint32_t base, unsigned fmt,
                                unsigned dim, bool tiled, unsigned stride_tiles,
                                int u, int v,
                                float *r, float *g, float *b, float *a)
{
    unsigned bpt = voodoo3_tfmt_bpt(fmt);
    uint32_t off = voodoo3_texel_off(base, bpt, dim, tiled, stride_tiles, u, v);
    uint32_t t;

    *a = 255.0f;
    *r = *g = *b = 0.0f;
    if (off + bpt > s->vga.vram_size) {
        return;
    }
    if (bpt == 1) {
        uint8_t px = s->vga.vram_ptr[off];
        switch (fmt) {
        case V3_TFMT_RGB332:
            *r = ((px >> 5) & 7) * 255 / 7;
            *g = ((px >> 2) & 7) * 255 / 7;
            *b = (px & 3) * 255 / 3;
            break;
        case V3_TFMT_A8:
            *a = px; *r = *g = *b = px;
            break;
        case V3_TFMT_I8:
            *r = *g = *b = px;
            break;
        case V3_TFMT_AI44:
            *a = (px >> 4) * 255 / 15;
            *r = *g = *b = (px & 0xf) * 255 / 15;
            break;
        default: /* P8/P8_RGBA/YIQ: palette+NCC not modelled -> intensity */
            *r = *g = *b = px;
            break;
        }
        return;
    }
    t = lduw_le_p(s->vga.vram_ptr + off);
    switch (fmt) {
    case V3_TFMT_RGB565:
        *r = ((t >> 11) & 0x1f) * 255 / 31;
        *g = ((t >> 5) & 0x3f) * 255 / 63;
        *b = (t & 0x1f) * 255 / 31;
        break;
    case V3_TFMT_ARGB1555:
        *a = (t & 0x8000) ? 255 : 0;
        *r = ((t >> 10) & 0x1f) * 255 / 31;
        *g = ((t >> 5) & 0x1f) * 255 / 31;
        *b = (t & 0x1f) * 255 / 31;
        break;
    case V3_TFMT_ARGB4444:
        *a = ((t >> 12) & 0xf) * 255 / 15;
        *r = ((t >> 8) & 0xf) * 255 / 15;
        *g = ((t >> 4) & 0xf) * 255 / 15;
        *b = (t & 0xf) * 255 / 15;
        break;
    case V3_TFMT_ARGB8332:
        *a = (t >> 8) & 0xff;
        *r = ((t >> 5) & 7) * 255 / 7;
        *g = ((t >> 2) & 7) * 255 / 7;
        *b = (t & 3) * 255 / 3;
        break;
    case V3_TFMT_AI88:
        *a = (t >> 8) & 0xff;
        *r = *g = *b = t & 0xff;
        break;
    default: /* AYIQ/AP88: palette+NCC not modelled -> alpha+intensity */
        *a = (t >> 8) & 0xff;
        *r = *g = *b = t & 0xff;
        break;
    }
}

/* wrap or clamp an integer texel coordinate to [0, dim) */
static int voodoo3_wrap(int c, unsigned dim, bool clamp)
{
    if (clamp) {
        return c < 0 ? 0 : c >= (int)dim ? (int)dim - 1 : c;
    }
    return c & (int)(dim - 1);
}

/*
 * texBaseAddr points at the corner of the virtual 256x256 level, so when a
 * smaller level is sampled its texel(0,0) sits at base + lodOffset(L), where
 * lodOffset is the memory occupied by the larger levels (Glide's
 * _grTexCalcBaseAddress: sum of texels of levels 0..L-1, times bpt, aligned
 * down to 16). Square textures only, matching the rest of this sampler.
 */
static uint32_t voodoo3_lod_offset(unsigned lod, unsigned bpt)
{
    uint32_t texels = 0;
    unsigned i;
    for (i = 0; i < lod; i++) {
        unsigned d = 256u >> i;
        texels += d * d;
    }
    return (texels * bpt) & ~0xfu;
}

/* sample the texture at (sc,tc); point-sampled or bilinear per textureMode */
static void voodoo3_texel(Voodoo3State *s, float sc, float tc,
                          float *r, float *g, float *b, float *a)
{
    uint32_t tmode = s->d3_regs[V3_3D_TEXTUREMODE / 4];
    uint32_t texbase = s->d3_regs[V3_3D_TEXBASEADDR / 4];
    uint32_t base = texbase & 0xfffff0;   /* [23:4] byte address, 16-byte units */
    bool tiled = texbase & 1;             /* [0] SST_TEXTURE_IS_TILED */
    unsigned stride_tiles = (texbase >> 25) & 0x7f;  /* [31:25] tile stride */
    uint32_t lod = s->d3_regs[V3_3D_TLOD / 4];
    unsigned fmt = (tmode & V3_TEX_TFORMAT_MASK) >> V3_TEX_TFORMAT_SHIFT;
    unsigned lodmin = (lod & 0x3f) >> 2;  /* tLOD lodmin, 4.2 -> integer LOD */
    unsigned dim = 256 >> (lodmin > 8 ? 8 : lodmin);

    /*
     * Add the per-level offset for the linear layout (verified with
     * tdfx_texlod). The tiled layout's per-level offset is not applied yet
     * (pending the tiled column of that probe), so tiled sub-256 textures
     * are still addressed from base.
     */
    if (!tiled) {
        base += voodoo3_lod_offset(lodmin > 8 ? 8 : lodmin,
                                   voodoo3_tfmt_bpt(fmt));
    }
    bool clamps = tmode & V3_TEX_CLAMPS;
    bool clampt = tmode & V3_TEX_CLAMPT;
    /* no LOD gradient here, so use the magnification filter selector */
    bool bilinear = tmode & V3_TEX_MAGFILTER;

    if (!bilinear) {
        int u = voodoo3_wrap((int)floorf(sc), dim, clamps);
        int v = voodoo3_wrap((int)floorf(tc), dim, clampt);
        voodoo3_fetch_texel(s, base, fmt, dim, tiled, stride_tiles,
                            u, v, r, g, b, a);
        return;
    } else {
        float fs = sc - 0.5f, ft = tc - 0.5f;
        int u0 = (int)floorf(fs), v0 = (int)floorf(ft);
        float du = fs - u0, dv = ft - v0;
        float w00 = (1 - du) * (1 - dv), w10 = du * (1 - dv);
        float w01 = (1 - du) * dv,       w11 = du * dv;
        int ua = voodoo3_wrap(u0, dim, clamps);
        int ub = voodoo3_wrap(u0 + 1, dim, clamps);
        int va = voodoo3_wrap(v0, dim, clampt);
        int vb = voodoo3_wrap(v0 + 1, dim, clampt);
        float r00, g00, b00, a00, r10, g10, b10, a10;
        float r01, g01, b01, a01, r11, g11, b11, a11;

        voodoo3_fetch_texel(s, base, fmt, dim, tiled, stride_tiles,
                            ua, va, &r00, &g00, &b00, &a00);
        voodoo3_fetch_texel(s, base, fmt, dim, tiled, stride_tiles,
                            ub, va, &r10, &g10, &b10, &a10);
        voodoo3_fetch_texel(s, base, fmt, dim, tiled, stride_tiles,
                            ua, vb, &r01, &g01, &b01, &a01);
        voodoo3_fetch_texel(s, base, fmt, dim, tiled, stride_tiles,
                            ub, vb, &r11, &g11, &b11, &a11);
        *r = r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11;
        *g = g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11;
        *b = b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11;
        *a = a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11;
    }
}

static inline float v3_edge(const V3Vertex *a, const V3Vertex *b,
                            float px, float py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/*
 * Fog blending factor from the 64-entry fog table. The hardware indexes
 * it with the MSBs of a normalised float(1/w); lacking that here we index
 * by the iterated Z and interpolate with the low bits - good enough to
 * exercise the table, exact indexing to be confirmed on real hardware.
 */
static float voodoo3_fog_factor(Voodoo3State *s, uint32_t srcz)
{
    unsigned idx = (srcz >> 10) & 0x3f;
    unsigned frac = (srcz >> 2) & 0xff;
    uint32_t word = s->d3_regs[(V3_3D_FOGTABLE / 4) + (idx >> 1)];
    unsigned factor, delta;

    if (idx & 1) {
        factor = (word >> 24) & 0xff;
        delta = (word >> 16) & 0xff;
    } else {
        factor = (word >> 8) & 0xff;
        delta = word & 0xff;
    }
    return v3_clampf((factor + delta * frac / 256.0f) / 255.0f, 0, 1);
}

/*
 * Rasterise one triangle through the pixel pipeline: Gouraud colour or
 * texture, depth test/write, chroma-key, alpha test, fog and alpha
 * blending, honouring fbzMode/alphaMode/fbzColorPath/textureMode/fogMode.
 * 16bpp 565 colour buffer and 16bpp depth buffer.
 */
static void voodoo3_3d_triangle(Voodoo3State *s, const V3Vertex *v0,
                                const V3Vertex *v1, const V3Vertex *v2)
{
    uint32_t fbz = s->d3_regs[V3_3D_FBZMODE / 4];
    uint32_t alpham = s->d3_regs[V3_3D_ALPHAMODE / 4];
    uint32_t cpath = s->d3_regs[V3_3D_FBZCOLORPATH / 4];
    uint32_t tmode = s->d3_regs[V3_3D_TEXTUREMODE / 4];
    uint32_t fogm = s->d3_regs[V3_3D_FOGMODE / 4];
    uint32_t fogc = s->d3_regs[V3_3D_FOGCOLOR / 4];
    uint32_t ckey = s->d3_regs[V3_3D_CHROMAKEY / 4] & 0xffffff;
    uint32_t cbase = s->d3_regs[V3_3D_COLBUFFERADDR / 4] & 0xffffff;
    uint32_t cstride = s->d3_regs[V3_3D_COLBUFFERSTRIDE / 4] & 0xffff;
    uint32_t zbase = s->d3_regs[V3_3D_AUXBUFFERADDR / 4] & 0xffffff;
    uint32_t zstride = s->d3_regs[V3_3D_AUXBUFFERSTRIDE / 4] & 0xffff;
    uint32_t clr = s->d3_regs[V3_3D_CLIPLEFTRIGHT / 4];
    uint32_t cbt = s->d3_regs[V3_3D_CLIPBOTTOMTOP / 4];
    unsigned zfunc = (fbz & V3_FBZ_ZFUNC_MASK) >> V3_FBZ_ZFUNC_SHIFT;
    unsigned rgbsel = cpath & V3_CP_RGBSELECT_MASK;
    unsigned afunc = (alpham & V3_ALPHA_FUNC_MASK) >> V3_ALPHA_FUNC_SHIFT;
    unsigned sblend = (alpham >> V3_ALPHA_SRCFUNC_SHIFT) & V3_ALPHA_BLENDFUNC_MASK;
    unsigned dblend = (alpham >> V3_ALPHA_DSTFUNC_SHIFT) & V3_ALPHA_BLENDFUNC_MASK;
    unsigned aref = (alpham >> V3_ALPHA_REF_SHIFT) & 0xff;
    bool texen = (rgbsel == 1) && (cpath & V3_CP_TEXTURE_EN);
    int clx0, clx1, cly0, cly1;
    float area, minx, maxx, miny, maxy;
    int ix0, ix1, iy0, iy1, x, y;

    if (!voodoo3_2d_ok(s) || !cstride) {
        return;
    }

    area = v3_edge(v0, v1, v2->x, v2->y);
    if (area == 0.0f) {
        return; /* degenerate */
    }

    /*
     * Clip rectangle (left/right, top/bottom in packed form) - only
     * applied when fbzMode enables clipping, otherwise the whole buffer
     * is drawable, like the hardware.
     */
    clx0 = 0;
    clx1 = 2048;
    cly0 = 0;
    cly1 = 2048;
    if (fbz & V3_FBZ_ENCLIP) {
        clx0 = (clr >> 16) & 0x3ff;
        clx1 = clr & 0x3ff;
        cly0 = (cbt >> 16) & 0x3ff;
        cly1 = cbt & 0x3ff;
    }

    minx = MIN(v0->x, MIN(v1->x, v2->x));
    maxx = MAX(v0->x, MAX(v1->x, v2->x));
    miny = MIN(v0->y, MIN(v1->y, v2->y));
    maxy = MAX(v0->y, MAX(v1->y, v2->y));
    ix0 = MAX((int)floorf(minx), clx0);
    ix1 = MIN((int)ceilf(maxx), clx1);
    iy0 = MAX((int)floorf(miny), cly0);
    iy1 = MIN((int)ceilf(maxy), cly1);

    for (y = iy0; y < iy1; y++) {
        for (x = ix0; x < ix1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float e0 = v3_edge(v1, v2, px, py);
            float e1 = v3_edge(v2, v0, px, py);
            float e2 = v3_edge(v0, v1, px, py);
            float l0, l1, l2, sr, sg, sb, sa, zf;
            uint32_t coff, zoff, srcz;
            uint16_t pix;

            /* inside test, winding-agnostic */
            if (area > 0) {
                if (e0 < 0 || e1 < 0 || e2 < 0) {
                    continue;
                }
            } else if (e0 > 0 || e1 > 0 || e2 > 0) {
                continue;
            }
            l0 = e0 / area;
            l1 = e1 / area;
            l2 = e2 / area;

            zf = l0 * v0->z + l1 * v1->z + l2 * v2->z;
            srcz = (uint32_t)v3_clampf(zf, 0, 65535);

            zoff = zbase + y * zstride + x * 2;
            if ((fbz & V3_FBZ_ENDEPTH) && zstride &&
                zoff + 2 <= s->vga.vram_size) {
                uint32_t dstz = lduw_le_p(s->vga.vram_ptr + zoff);
                if (!voodoo3_zfunc_pass(zfunc, srcz, dstz)) {
                    continue;
                }
            }

            if (texen) {
                float oow = l0 * v0->oow + l1 * v1->oow + l2 * v2->oow;
                float sc = (l0 * v0->sow + l1 * v1->sow + l2 * v2->sow);
                float tc = (l0 * v0->tow + l1 * v1->tow + l2 * v2->tow);
                if (oow != 0) {
                    sc /= oow;
                    tc /= oow;
                }
                voodoo3_texel(s, sc, tc, &sr, &sg, &sb, &sa);
                /*
                 * Single-TMU combine: the texel reaches the TMU output
                 * only when textureMode adds the local texel; with the
                 * combine at reset the TMU emits zero (so a texture needs
                 * the combine set up, unlike plain iterated colour).
                 */
                if (!(tmode & V3_TEX_TC_ADD_CLOCAL)) {
                    sr = sg = sb = 0;
                }
                if (!(tmode & V3_TEX_TCA_ADD_CLOCAL)) {
                    sa = 0;
                }
            } else if (rgbsel == 2) {
                uint32_t c1 = s->d3_regs[V3_3D_C1 / 4];
                sr = (c1 >> 16) & 0xff;
                sg = (c1 >> 8) & 0xff;
                sb = c1 & 0xff;
                sa = (c1 >> 24) & 0xff;
            } else {
                sr = v3_clampf(l0 * v0->r + l1 * v1->r + l2 * v2->r, 0, 255);
                sg = v3_clampf(l0 * v0->g + l1 * v1->g + l2 * v2->g, 0, 255);
                sb = v3_clampf(l0 * v0->b + l1 * v1->b + l2 * v2->b, 0, 255);
                sa = v3_clampf(l0 * v0->a + l1 * v1->a + l2 * v2->a, 0, 255);
            }

            /* chroma-key: discard pixels whose colour matches chromaKey */
            if (fbz & V3_FBZ_ENCHROMAKEY) {
                if (((ckey >> 16) & 0xff) == (unsigned)sr &&
                    ((ckey >> 8) & 0xff) == (unsigned)sg &&
                    (ckey & 0xff) == (unsigned)sb) {
                    continue;
                }
            }

            /* alpha test */
            if (alpham & V3_ALPHA_ENTEST) {
                unsigned av = (unsigned)sa;
                if (!(((afunc & V3_ZF_LT) && av < aref) ||
                      ((afunc & V3_ZF_EQ) && av == aref) ||
                      ((afunc & V3_ZF_GT) && av > aref))) {
                    continue;
                }
            }

            /* fog */
            if (fogm & V3_FOG_ENABLE) {
                float fr = (fogc >> 16) & 0xff;
                float fg = (fogc >> 8) & 0xff;
                float fb = fogc & 0xff;

                if (fogm & V3_FOG_CONSTANT) {
                    sr = v3_clampf(sr + fr, 0, 255);
                    sg = v3_clampf(sg + fg, 0, 255);
                    sb = v3_clampf(sb + fb, 0, 255);
                } else {
                    float af;
                    float cr = (fogm & V3_FOG_MULT) ? 0.0f : sr;
                    float cg = (fogm & V3_FOG_MULT) ? 0.0f : sg;
                    float cb = (fogm & V3_FOG_MULT) ? 0.0f : sb;
                    float ar = (fogm & V3_FOG_ADD) ? 0.0f : fr;
                    float ag = (fogm & V3_FOG_ADD) ? 0.0f : fg;
                    float ab = (fogm & V3_FOG_ADD) ? 0.0f : fb;

                    if (fogm & V3_FOG_Z) {
                        af = (srcz >> 8) / 255.0f;
                    } else if (fogm & V3_FOG_ALPHA) {
                        af = sa / 255.0f;
                    } else {
                        af = voodoo3_fog_factor(s, srcz);
                    }
                    af = v3_clampf(af, 0, 1);
                    sr = v3_clampf(af * ar + (1 - af) * cr, 0, 255);
                    sg = v3_clampf(af * ag + (1 - af) * cg, 0, 255);
                    sb = v3_clampf(af * ab + (1 - af) * cb, 0, 255);
                }
            }

            coff = cbase + y * cstride + x * 2;
            if (coff + 2 > s->vga.vram_size) {
                continue;
            }

            if (alpham & V3_ALPHA_ENBLEND) {
                uint16_t d = lduw_le_p(s->vga.vram_ptr + coff);
                float dr = ((d >> 11) & 0x1f) * 255.0f / 31;
                float dg = ((d >> 5) & 0x3f) * 255.0f / 63;
                float db = (d & 0x1f) * 255.0f / 31;
                float da = 255.0f;

                sr = sr * voodoo3_blend_factor(sblend, sa, da, sr) +
                     dr * voodoo3_blend_factor(dblend, sa, da, dr);
                sg = sg * voodoo3_blend_factor(sblend, sa, da, sg) +
                     dg * voodoo3_blend_factor(dblend, sa, da, dg);
                sb = sb * voodoo3_blend_factor(sblend, sa, da, sb) +
                     db * voodoo3_blend_factor(dblend, sa, da, db);
                sr = v3_clampf(sr, 0, 255);
                sg = v3_clampf(sg, 0, 255);
                sb = v3_clampf(sb, 0, 255);
            }

            pix = (((uint16_t)sr >> 3) << 11) |
                  (((uint16_t)sg >> 2) << 5) |
                  ((uint16_t)sb >> 3);
            if (fbz & V3_FBZ_RGBWRMASK) {
                stw_le_p(s->vga.vram_ptr + coff, pix);
            }

            if ((fbz & V3_FBZ_ENDEPTH) && (fbz & V3_FBZ_DEPTHWRMASK) &&
                zstride && zoff + 2 <= s->vga.vram_size) {
                stw_le_p(s->vga.vram_ptr + zoff, srcz);
            }
        }
    }

    s->d3_regs[0x25c / 4]++;  /* fbiTrianglesOut */
    memory_region_set_dirty(&s->vga.vram, cbase, cstride * (iy1 - iy0 + 1));
}

/* Build a vertex from the setup-unit (s*) registers. */
static void voodoo3_setup_vertex(Voodoo3State *s, V3Vertex *v)
{
    uint32_t mode = s->d3_regs[V3_3D_SSETUPMODE / 4];
    uint32_t argb = s->d3_regs[V3_3D_SARGB / 4];

    v->x = v3_reg_f(s, V3_3D_SVX);
    v->y = v3_reg_f(s, V3_3D_SVY);
    v->z = v3_reg_f(s, V3_3D_SVZ);
    v->oow = v3_reg_f(s, V3_3D_SWOOWFBI);
    if (mode & V3_SSETUP_RGB) {
        /* packed ARGB is the common Glide layout */
        v->a = (argb >> 24) & 0xff;
        v->r = (argb >> 16) & 0xff;
        v->g = (argb >> 8) & 0xff;
        v->b = argb & 0xff;
    } else {
        v->r = v3_reg_f(s, V3_3D_SRED);
        v->g = v3_reg_f(s, V3_3D_SGREEN);
        v->b = v3_reg_f(s, V3_3D_SBLUE);
        v->a = v3_reg_f(s, V3_3D_SALPHA);
    }
    v->sow = v3_reg_f(s, V3_3D_SSOW0);
    v->tow = v3_reg_f(s, V3_3D_STOW0);
}

/* Setup unit: accumulate a vertex and emit triangles (strip or fan). */
static void voodoo3_setup_tri(Voodoo3State *s, bool begin)
{
    uint32_t mode = s->d3_regs[V3_3D_SSETUPMODE / 4];
    bool strip = mode & V3_SSETUP_STRIP_MODE;

    if (begin) {
        voodoo3_setup_vertex(s, &s->svtx[0]);
        s->snvert = 1;
        return;
    }
    if (s->snvert < 3) {
        voodoo3_setup_vertex(s, &s->svtx[s->snvert++]);
    } else if (strip) {
        s->svtx[0] = s->svtx[1];
        s->svtx[1] = s->svtx[2];
        voodoo3_setup_vertex(s, &s->svtx[2]);
    } else { /* fan: keep svtx[0] as the anchor */
        s->svtx[1] = s->svtx[2];
        voodoo3_setup_vertex(s, &s->svtx[2]);
    }
    if (s->snvert == 3) {
        voodoo3_3d_triangle(s, &s->svtx[0], &s->svtx[1], &s->svtx[2]);
    }
}

/*
 * Direct triangle: the driver has written the three vertices and the
 * per-parameter screen-space gradients; the value of each parameter at
 * pixel (x,y) is (value at vertex A) + dpdx*(x-Ax) + dpdy*(y-Ay). We
 * reconstruct per-vertex values from A + gradients and reuse the span
 * walker.
 */
/* fixed-point fraction bits for the iterated params: r,g,b,z,a,s,t,w */
static const int voodoo3_param_fracs[8] = { 12, 12, 12, 12, 12, 18, 18, 30 };

static void voodoo3_direct_tri(Voodoo3State *s, bool is_float)
{
    V3Vertex v[3];
    float ax, ay;
    float pA[8], dx[8], dy[8];   /* r,g,b,z,a,s,t,w */
    int i;

    if (is_float) {
        v[0].x = v3_reg_f(s, V3_3D_FVA);
        v[0].y = v3_reg_f(s, V3_3D_FVA + 4);
        v[1].x = v3_reg_f(s, V3_3D_FVB);
        v[1].y = v3_reg_f(s, V3_3D_FVB + 4);
        v[2].x = v3_reg_f(s, V3_3D_FVC);
        v[2].y = v3_reg_f(s, V3_3D_FVC + 4);
        for (i = 0; i < 8; i++) {
            pA[i] = v3_reg_f(s, V3_3D_FSTARTR + i * 4);
            dx[i] = v3_reg_f(s, V3_3D_FDPDX + i * 4);
            dy[i] = v3_reg_f(s, V3_3D_FDPDY + i * 4);
        }
    } else {
        v[0].x = v3_reg_fx(s, V3_3D_VA, 4);
        v[0].y = v3_reg_fx(s, V3_3D_VA + 4, 4);
        v[1].x = v3_reg_fx(s, V3_3D_VB, 4);
        v[1].y = v3_reg_fx(s, V3_3D_VB + 4, 4);
        v[2].x = v3_reg_fx(s, V3_3D_VC, 4);
        v[2].y = v3_reg_fx(s, V3_3D_VC + 4, 4);
        /* r,g,b,a: 12.12 ; z: 20.12 ; s,t: 14.18 ; w: 2.30 */
        for (i = 0; i < 8; i++) {
            int f = voodoo3_param_fracs[i];
            pA[i] = v3_reg_fx(s, V3_3D_STARTR + i * 4, f);
            dx[i] = v3_reg_fx(s, V3_3D_DPDX + i * 4, f);
            dy[i] = v3_reg_fx(s, V3_3D_DPDY + i * 4, f);
        }
    }

    ax = v[0].x;
    ay = v[0].y;
    for (i = 0; i < 3; i++) {
        float ox = v[i].x - ax, oy = v[i].y - ay;
        float p[8];
        int k;

        for (k = 0; k < 8; k++) {
            p[k] = pA[k] + dx[k] * ox + dy[k] * oy;
        }
        v[i].r = p[0];
        v[i].g = p[1];
        v[i].b = p[2];
        v[i].z = p[3];
        v[i].a = p[4];
        v[i].sow = p[5];
        v[i].tow = p[6];
        v[i].oow = p[7];
    }
    voodoo3_3d_triangle(s, &v[0], &v[1], &v[2]);
}

static void voodoo3_3d_fastfill(Voodoo3State *s)
{
    uint32_t fbz = s->d3_regs[V3_3D_FBZMODE / 4];
    uint32_t c1 = s->d3_regs[V3_3D_C1 / 4];
    uint32_t za = s->d3_regs[V3_3D_ZACOLOR / 4];
    uint32_t cbase = s->d3_regs[V3_3D_COLBUFFERADDR / 4] & 0xffffff;
    uint32_t cstride = s->d3_regs[V3_3D_COLBUFFERSTRIDE / 4] & 0xffff;
    uint32_t zbase = s->d3_regs[V3_3D_AUXBUFFERADDR / 4] & 0xffffff;
    uint32_t zstride = s->d3_regs[V3_3D_AUXBUFFERSTRIDE / 4] & 0xffff;
    uint32_t clr = s->d3_regs[V3_3D_CLIPLEFTRIGHT / 4];
    uint32_t cbt = s->d3_regs[V3_3D_CLIPBOTTOMTOP / 4];
    int x0 = (clr >> 16) & 0x3ff, x1 = clr & 0x3ff;
    int y0 = (cbt >> 16) & 0x3ff, y1 = cbt & 0x3ff;
    uint16_t col = ((((c1 >> 16) & 0xff) >> 3) << 11) |
                   ((((c1 >> 8) & 0xff) >> 2) << 5) |
                   ((c1 & 0xff) >> 3);
    uint16_t zval = za & 0xffff;
    int x, y;

    if (!voodoo3_2d_ok(s) || !cstride || x1 <= x0 || y1 <= y0) {
        return;
    }
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            uint32_t coff = cbase + y * cstride + x * 2;
            if ((fbz & V3_FBZ_RGBWRMASK) && coff + 2 <= s->vga.vram_size) {
                stw_le_p(s->vga.vram_ptr + coff, col);
            }
            if ((fbz & V3_FBZ_DEPTHWRMASK) && zstride) {
                uint32_t zoff = zbase + y * zstride + x * 2;
                if (zoff + 2 <= s->vga.vram_size) {
                    stw_le_p(s->vga.vram_ptr + zoff, zval);
                }
            }
        }
    }
    memory_region_set_dirty(&s->vga.vram, cbase, cstride * (y1 - y0 + 1));
}

/* Point the video scanout at a buffer and refresh the display. */
static void voodoo3_present(Voodoo3State *s, uint32_t addr)
{
    s->io_regs[V3_VIDDESKSTART / 4] = addr;
    if (s->mode == V3_MODE_DESKTOP) {
        voodoo3_set_scanout_offset(s, addr);
    } else {
        voodoo3_update_mode(s);
    }
    s->need_blank = false;
    qemu_console_hw_invalidate(s->vga.con);
    qemu_console_update_full(s->vga.con);
}

/*
 * swapbufferCMD: make colBufferAddr the displayed buffer. If the
 * command requests it (SST_SWAP_EN_WAIT_ON_VSYNC, bit 0), the swap is
 * deferred to the vertical-blank timer and reflected in the pending
 * count (status[30:28]); otherwise it happens immediately. cmd[8:1] is
 * the swap interval in vblanks.
 */
static void voodoo3_3d_swapbuffer(Voodoo3State *s, uint32_t cmd)
{
    uint32_t cbase = s->d3_regs[V3_3D_COLBUFFERADDR / 4] & 0xffffff;

    s->d3_regs[0x258 / 4]++;   /* fbiSwapHistory */
    if (cmd & 1) {
        s->swap_buf = cbase;
        s->swap_interval = (cmd >> 1) & 0xff;
        if (s->swap_pending < 7) {
            s->swap_pending++;
        }
    } else {
        voodoo3_present(s, cbase);
    }
}

/* Vertical-blank: perform any queued swap and raise the vsync IRQ. */
static void voodoo3_vblank(void *opaque)
{
    Voodoo3State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->vblank_count++;

    if (s->swap_pending) {
        if (s->swap_interval) {
            s->swap_interval--;
        } else {
            voodoo3_present(s, s->swap_buf);
            s->swap_pending--;
        }
    }

    if (s->d3_regs[V3_3D_INTRCTRL / 4] & V3_INTR_VSYNC_ENABLE) {
        s->vsync_irq = true;
        pci_set_irq(&s->dev, 1);
    }

    timer_mod(&s->vblank_timer, now + NANOSECONDS_PER_SECOND / 60);
}

static uint64_t voodoo3_3d_read(Voodoo3State *s, hwaddr addr, unsigned size)
{
    uint32_t val;

    if (addr >= 0x400) {
        return 0;
    }
    switch (addr & ~3) {
    case V3_STATUS:
    {
        uint32_t vis, line = voodoo3_scanline(s, &vis);
        val = 0x1f;                                 /* FIFO free */
        if (line >= vis) {
            val |= V3_STATUS_VRETRACE;
        }
        val |= (s->swap_pending & 7) << V3_STATUS_SWAP_SHIFT;
        if (s->vsync_irq) {
            val |= V3_STATUS_PCIINT;
        }
        break;
    }
    case V3_3D_INTRCTRL:
        val = s->d3_regs[V3_3D_INTRCTRL / 4] & V3_INTR_VSYNC_ENABLE;
        if (s->vsync_irq) {
            val |= V3_INTR_VSYNC_PENDING;
        }
        break;
    default:
        val = s->d3_regs[(addr & ~3) / 4];
        break;
    }
    return extract32(val, (addr & 3) * 8, size * 8);
}

static void voodoo3_3d_write(Voodoo3State *s, hwaddr addr, uint64_t data,
                             unsigned size)
{
    unsigned reg;

    if (addr >= 0x400) {
        return;
    }
    reg = (addr & ~3);
    s->d3_regs[reg / 4] = deposit32(s->d3_regs[reg / 4], (addr & 3) * 8,
                                    size * 8, data);

    switch (reg) {
    case V3_3D_TRIANGLECMD:
        voodoo3_direct_tri(s, false);
        break;
    case V3_3D_FTRIANGLECMD:
        voodoo3_direct_tri(s, true);
        break;
    case V3_3D_SBEGINTRICMD:
        voodoo3_setup_tri(s, true);
        break;
    case V3_3D_SDRAWTRICMD:
        voodoo3_setup_tri(s, false);
        break;
    case V3_3D_FASTFILLCMD:
        voodoo3_3d_fastfill(s);
        break;
    case V3_3D_SWAPBUFFERCMD:
        voodoo3_3d_swapbuffer(s, data);
        break;
    case V3_3D_INTRCTRL:
        if (data & V3_INTR_VSYNC_CLEAR) {
            s->vsync_irq = false;
            pci_set_irq(&s->dev, 0);
        }
        s->d3_regs[V3_3D_INTRCTRL / 4] = data & V3_INTR_VSYNC_ENABLE;
        break;
    case V3_3D_NOPCMD:
        break;
    default:
        break;
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
    if (addr >= 0x200000 && addr < 0x600000) {
        return voodoo3_3d_read(s, addr - 0x200000, size);
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
        voodoo3_3d_write(s, addr - 0x200000, data, size);
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

/*
 * Build a minimal PCI option ROM good enough for tdfxfb to cold-boot the
 * card without a real 3dfx VBIOS. A real card's VBIOS runs at POST and
 * leaves an "OEM config" table in the ROM that the driver reads (via
 * pci_map_rom) to program the PLLs and DRAM. We don't need executable BIOS
 * code - the driver does the init itself - only the ROM signature, a PCI
 * data structure so Linux accepts the image, and that config table pointed
 * to from ROM[0x50]. Values are chosen so the memory PLL lands ~100 MHz and
 * DRAM comes up, matching this model's memory gate.
 */
#define V3_VBIOS_SIZE 512
#define V3_VBIOS_ROMCFG 0x60    /* ROM config table */
#define V3_VBIOS_OEMCFG 0x70    /* OEM config table (struct tdfx_bios_cfg) */

static void voodoo3_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void voodoo3_build_vbios(uint8_t *rom)
{
    unsigned sum = 0, i;

    memset(rom, 0, V3_VBIOS_SIZE);

    /* option ROM header */
    rom[0x00] = 0x55;
    rom[0x01] = 0xaa;
    rom[0x02] = V3_VBIOS_SIZE / 512;   /* size in 512-byte blocks */
    rom[0x03] = 0xcb;                  /* entry point: retf (no-op init) */
    rom[0x18] = 0x1c; rom[0x19] = 0x00;/* pointer to PCI data structure */

    /* PCI data structure at 0x1c */
    rom[0x1c] = 'P'; rom[0x1d] = 'C'; rom[0x1e] = 'I'; rom[0x1f] = 'R';
    rom[0x20] = PCI_VENDOR_ID_3DFX & 0xff;
    rom[0x21] = PCI_VENDOR_ID_3DFX >> 8;
    rom[0x22] = PCI_DEVICE_ID_3DFX_VOODOO3 & 0xff;
    rom[0x23] = PCI_DEVICE_ID_3DFX_VOODOO3 >> 8;
    rom[0x26] = 0x18;                  /* PCIR struct length */
    rom[0x2b] = 0x03;                  /* class code: display controller */
    rom[0x2c] = V3_VBIOS_SIZE / 512;   /* image length in 512-byte blocks */
    rom[0x30] = 0x00;                  /* code type: x86 */
    rom[0x31] = 0x80;                  /* last image */

    /* ROM[0x50] -> ROM config table -> OEM config table */
    rom[0x50] = V3_VBIOS_ROMCFG & 0xff; rom[0x51] = V3_VBIOS_ROMCFG >> 8;
    rom[V3_VBIOS_ROMCFG] = V3_VBIOS_OEMCFG & 0xff;
    rom[V3_VBIOS_ROMCFG + 1] = V3_VBIOS_OEMCFG >> 8;

    /* struct tdfx_bios_cfg at OEMCFG */
    voodoo3_put_le32(rom + V3_VBIOS_OEMCFG + 0x00, 0x00000180); /* pciinit0 */
    voodoo3_put_le32(rom + V3_VBIOS_OEMCFG + 0x04, 0x00000000); /* miscinit0 */
    voodoo3_put_le32(rom + V3_VBIOS_OEMCFG + 0x08, 0x00000000); /* miscinit1 */
    voodoo3_put_le32(rom + V3_VBIOS_OEMCFG + 0x0c, 0x0817a100); /* draminit0 */
    voodoo3_put_le32(rom + V3_VBIOS_OEMCFG + 0x10, 0x00000000); /* draminit1 */
    voodoo3_put_le32(rom + V3_VBIOS_OEMCFG + 0x14, 0x00000000); /* agpinit0 */
    voodoo3_put_le32(rom + V3_VBIOS_OEMCFG + 0x18, 0x00001a01); /* pllctrl1 ~100MHz */
    voodoo3_put_le32(rom + V3_VBIOS_OEMCFG + 0x1c, 0x00000000); /* pllctrl2 */
    voodoo3_put_le32(rom + V3_VBIOS_OEMCFG + 0x20, 0x00000000); /* sgrammode */

    /* option ROM checksum: whole image must sum to 0 mod 256 */
    for (i = 0; i < V3_VBIOS_SIZE - 1; i++) {
        sum += rom[i];
    }
    rom[V3_VBIOS_SIZE - 1] = (uint8_t)(-sum);
}

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

    /*
     * With no external romfile, synthesise a minimal VBIOS carrying the OEM
     * config table so tdfxfb can cold-boot the card. Skip it if the user
     * supplied a real ROM (the generic PCI code registers that one).
     */
    if (s->fake_vbios && (!dev->romfile || !dev->romfile[0])) {
        memory_region_init_rom(&s->vbios, OBJECT(s), "voodoo3.vbios",
                               V3_VBIOS_SIZE, &error_fatal);
        voodoo3_build_vbios(memory_region_get_ram_ptr(&s->vbios));
        pci_register_bar(dev, PCI_ROM_SLOT, 0, &s->vbios);
    }

    dev->config[PCI_INTERRUPT_PIN] = 1;

    timer_init_ns(&s->vblank_timer, QEMU_CLOCK_VIRTUAL, voodoo3_vblank, s);
    timer_mod(&s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

static void voodoo3_reset(DeviceState *dev)
{
    Voodoo3State *s = VOODOO3(dev);

    vga_common_reset(&s->vga);

    s->swap_pending = 0;
    s->swap_interval = 0;
    s->vsync_irq = false;
    pci_set_irq(&s->dev, 0);

    memset(s->io_regs, 0, sizeof(s->io_regs));
    memset(s->d2_regs, 0, sizeof(s->d2_regs));
    memset(s->d3_regs, 0, sizeof(s->d3_regs));
    s->snvert = 0;
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

    timer_del(&s->vblank_timer);
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
        VMSTATE_UINT32_ARRAY(d3_regs, Voodoo3State, V3_3D_REG_NB),
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
    DEFINE_PROP_BOOL("fake-vbios", Voodoo3State, fake_vbios, true),
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
