/*
 * Sony CLIE color HiRes companion LCD controller.
 *
 * The color-capable VZ328 CLIEs do not scan the panel out through the
 * DragonBall's own on-chip LCDC (the way the mono HiRes T400/S300 do);
 * a companion "MediaQ" MQ11xx-class graphics chip sits on an external
 * chip select and owns the 320x320 color panel instead -- Palm's own
 * HAL source calls it "MediaQ 1100/1132".  Register map (byte offsets,
 * 32-bit registers) is the one Cloudpilot-emu's EmRegsMediaQ11xx.cpp
 * and EmPalmStructs.i's "HwrMediaQ11xxType"/FOR_HwrMediaQ11xxType_FIELDS
 * struct document (fetched from github.com/cloudpilot-emu/cloudpilot-emu
 * this session; see CLIE-POC-NOTES.md's SJ33 section for the register
 * derivation and the two base addresses (T_BASE/MMIO_BASE) Cloudpilot's
 * EmDevice.cpp hard-codes for every MediaQ-equipped Sony VZ328 board:
 * N600C, T600 and N700C all call `new EmRegsMediaQ11xx(*framebuffer,
 * MMIO_BASE, T_BASE)` with the exact same two constants).
 *
 * This model only implements the *scanout* path this tree needs to get
 * a picture on screen -- enable/bpp/geometry/start-address/stride and
 * the 6:6:6 palette -- plus the one status register ("CC01", a.k.a.
 * GraphicEngineStatus) the Palm HAL polls for "command/source FIFO
 * empty, engine not busy" before touching other registers.  Scope is
 * deliberately the same as SED1376 (the color controller this tree
 * already models for the Palm m515): the MediaQ's hardware BitBLT/
 * line-draw graphics-accelerator engine (registers 0x200-0x27f) is
 * *not* implemented -- writes there are accepted and stored (so
 * read-modify-write register sequences still work) but produce no
 * drawing side effect, matching Cloudpilot's own CC01Read, which
 * always reports the accelerator idle regardless of what's queued.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/display/clie_lcd.h"
#include "migration/vmstate.h"
#include "ui/pixel_ops.h"

/* Register byte offsets -- see EmPalmStructs.i "HwrMediaQ11xxType". */
#define CLIE_LCD_REG_CC_STATUS      0x004  /* GraphicEngineStatus ("CC01") */
#define CLIE_LCD_REG_GC_CONTROL     0x180  /* mode: enable / bpp / doubling */
#define CLIE_LCD_REG_GC_HWINDOW     0x1a0  /* horizontal window: width-1 */
#define CLIE_LCD_REG_GC_VWINDOW     0x1a4  /* vertical window: height-1 */
#define CLIE_LCD_REG_GC_START_ADDR  0x1b0  /* window start, byte offset into vmem */
#define CLIE_LCD_REG_GC_STRIDE      0x1b8  /* window stride, bytes/line */
#define CLIE_LCD_REG_PALETTE        0x800  /* 256 * 4-byte 6:6:6 palette entries */
#define CLIE_LCD_PALETTE_ENTRIES    256
#define CLIE_LCD_REG_DEVCFG0        0x380  /* DeviceConfig0 ("DC00"), chip-ID/present */

#define CLIE_LCD_GC_ENABLE          0x00000001
#define CLIE_LCD_GC_BPP_SHIFT       4
#define CLIE_LCD_GC_BPP_MASK        (0x7 << CLIE_LCD_GC_BPP_SHIFT)

/*
 * CC01 ("GraphicEngineStatus"): command-FIFO depth bits [4:0], source-
 * FIFO depth bits [12:8], "engine busy" bit 16.  Cloudpilot's CC01Read
 * forces "FIFO empty (0x10/0x1000), not busy" on every read since it
 * never actually runs an accelerator queue either -- do the same here,
 * or a HAL "wait for idle" poll loop never sees what it's waiting for.
 */
#define CLIE_LCD_CC01_FIFO_EMPTY    0x00001010
#define CLIE_LCD_CC01_MASK          0x00011f1f

/*
 * MediaQ register byte-lane / endianness bridge (traced first-hand this
 * session against the live T600C boot; see CLIE-POC-NOTES.md).  The
 * chip presents its 32-bit registers to the big-endian DragonBall such
 * that the guest writes them as two 16-bit big-endian halfwords, but
 * the chip's *logical* 32-bit value orders those halfwords
 * little-endian (low address = low 16 bits).  So the logical value
 * Cloudpilot's EmRegsMediaQ11xx code (and the real silicon) works with
 * is:
 *     logical(O) = be16(O) | (be16(O+2) << 16)
 * where be16(x) = regs[x]<<8 | regs[x+1] (a plain big-endian halfword).
 *
 * Verified live against all five registers this device reads:
 *   GC_CONTROL@0x180 bytes 00 39 06 01 -> logical 0x06010039
 *     -> enable (bit0)=1, bpp = 1<<((v&0x70)>>4) = 1<<3 = 8bpp
 *   GC_HWINDOW@0x1a0 bytes 00 00 01 3f -> logical 0x013f0000
 *     -> width  = (v>>16)+1 = 320
 *   GC_VWINDOW@0x1a4 bytes 00 00 01 3f -> logical 0x013f0000 -> height 320
 *   GC_STRIDE@0x1b8 bytes 01 40 00 00 -> logical 0x00000140 -> stride 320
 *   GC_START@0x1b0  bytes 00 00 00 00 -> logical 0 -> framebuffer at vmem 0
 * All of which match a 320x320 8bpp-palettised framebuffer, exactly the
 * mode the T600C HAL programs for the Setup UI.
 */
static uint32_t clie_lcd_logical(ClieLcdState *s, hwaddr off)
{
    uint32_t lo = (s->regs[off] << 8) | s->regs[off + 1];
    uint32_t hi = (s->regs[off + 2] << 8) | s->regs[off + 3];

    return lo | (hi << 16);
}

static bool clie_lcd_update_display(void *opaque)
{
    ClieLcdState *s = opaque;
    DisplaySurface *surface;
    const uint8_t *vmem = memory_region_get_ram_ptr(&s->vmem_mr);
    uint32_t gc = clie_lcd_logical(s, CLIE_LCD_REG_GC_CONTROL);
    unsigned int bpp_shift, bpp, width, height, start, stride, x, y;
    uint32_t palette[CLIE_LCD_PALETTE_ENTRIES];
    uint32_t *dest;

    /*
     * NOTE on the "return false" cases below: qemu_console_hw_update()
     * (ui/console.c) only wakes coroutines blocked in
     * qemu_console_co_wait_update() -- which "screendump" and the
     * VNC/Spice refresh paths use -- when gfx_update() returns *true*.
     * Before the guest programs the controller (GC_ENABLE clear at
     * reset, as it is through most of early boot) this callback used
     * to return false, which permanently wedges any such waiter the
     * very first time it's called -- e.g. a screendump taken before
     * Setup starts drawing hangs QMP forever.  So: fall through to a
     * blank/best-effort paint and always return true; only bail out
     * (still returning true) when there is nothing sane to paint at
     * all (surface not yet 32bpp, which "can't happen" in this tree's
     * consoles but is kept as a guard).
     */
    if (!(gc & CLIE_LCD_GC_ENABLE)) {
        goto blank;
    }

    bpp_shift = (gc & CLIE_LCD_GC_BPP_MASK) >> CLIE_LCD_GC_BPP_SHIFT;
    if (bpp_shift > 4) {
        goto blank;
    }
    bpp = 1u << bpp_shift;

    width = ((clie_lcd_logical(s, CLIE_LCD_REG_GC_HWINDOW) >> 16) & 0xfff) + 1;
    height = ((clie_lcd_logical(s, CLIE_LCD_REG_GC_VWINDOW) >> 16) & 0xfff) + 1;
    if (!width || !height || width > 1024 || height > 1024) {
        goto blank;
    }

    start = clie_lcd_logical(s, CLIE_LCD_REG_GC_START_ADDR) &
            (CLIE_LCD_VMEM_SIZE - 1);
    stride = clie_lcd_logical(s, CLIE_LCD_REG_GC_STRIDE) & 0xffff;
    if (!stride) {
        stride = width * bpp / 8;
    }

    surface = qemu_console_surface(s->con);
    if (width != surface_width(surface) || height != surface_height(surface)) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
    }
    if (surface_bits_per_pixel(surface) != 32) {
        goto blank;
    }

    if (bpp <= 8) {
        /*
         * Palette (cpREG at 0x800), one 4-byte entry per index, read
         * through the same logical() byte-lane bridge as the control
         * registers.  Cloudpilot's PrvUpdatePalette:
         *   palette[i] = 0xff000000 | (logical & (RED|GREEN|BLUE_MASK))
         * with RED_MASK=0x0000FC, GREEN_MASK=0x00FC00, BLUE_MASK=0xFC0000
         * -- i.e. R in bits[7:2], G in bits[15:10], B in bits[23:18],
         * each a 6-bit channel already left-shifted into place.
         */
        for (x = 0; x < CLIE_LCD_PALETTE_ENTRIES; x++) {
            uint32_t v = clie_lcd_logical(s, CLIE_LCD_REG_PALETTE + x * 4);

            palette[x] = rgb_to_pixel32(v & 0xfc,
                                        (v >> 8) & 0xfc,
                                        (v >> 16) & 0xfc);
        }
    }

    dest = surface_data(surface);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            unsigned int byte = (start + y * stride + x * bpp / 8) %
                                CLIE_LCD_VMEM_SIZE;
            uint8_t b = vmem[byte];
            uint32_t pixel;

            switch (bpp) {
            case 1:
                pixel = palette[(b >> (7 - x % 8)) & 1];
                break;
            case 2:
                pixel = palette[(b >> (6 - x % 4 * 2)) & 3];
                break;
            case 4:
                pixel = palette[x % 2 ? b & 0xf : b >> 4];
                break;
            case 8:
                pixel = palette[b];
                break;
            default: {
                /*
                 * 16bpp RGB565.  The MediaQ video aperture is a
                 * little-endian device (same byte-lane bridge as the
                 * registers, see clie_lcd_logical()), so a 16-bit pixel
                 * reads low byte first.  (Not exercised while the HAL
                 * runs the Setup UI in 8bpp; correct for the color mode
                 * it switches to afterwards.)
                 */
                uint16_t v = b | (vmem[(byte + 1) % CLIE_LCD_VMEM_SIZE] << 8);

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

blank:
    /*
     * Nothing sane to scan out yet (controller disabled, or geometry/
     * depth not programmed to something valid): make sure the console
     * has *a* surface -- 320x320 is the SJ33's native panel size and a
     * reasonable default for any color-HiRes CLIE this device backs --
     * and leave it as-is (blank/whatever it last held) rather than
     * bailing out without ever completing the update.
     */
    surface = qemu_console_surface(s->con);
    if (!surface || surface_width(surface) == 0 || surface_height(surface) == 0) {
        qemu_console_resize(s->con, 320, 320);
    }
    qemu_console_update_full(s->con);
    return true;
}

static uint64_t clie_lcd_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    ClieLcdState *s = opaque;
    uint64_t val = 0;
    unsigned int i;

    for (i = 0; i < size; i++) {
        val = (val << 8) | s->regs[addr + i];
    }

    if (addr == CLIE_LCD_REG_CC_STATUS && size == 4) {
        val = (val & ~(uint64_t)CLIE_LCD_CC01_MASK) | CLIE_LCD_CC01_FIFO_EMPTY;
    }

    if (addr == CLIE_LCD_REG_DEVCFG0) {
        /*
         * DeviceConfig0 ("DC00"): a chip-present/ID style read-only
         * register (Cloudpilot's T2-chip sibling class hard-codes 0
         * here; the plain MediaQ11xx class this device models leaves
         * it as default -- i.e. also 0 -- storage with no override).
         * Traced this session (see CLIE-POC-NOTES.md's SJ33 section):
         * the T600C HAL busy-polls exactly this address (870 reads in
         * 10s of real time) before it will program anything else on
         * the controller, and reading back 0 there never satisfies it.
         * No datasheet value is available, so this returns all-ones
         * (a conventional "unimplemented/floating bus" pattern) as a
         * best-effort unblock, which was verified to work: the HAL
         * proceeds to genuine mode/geometry/palette programming once
         * this stops reading 0.  Flagged as unconfirmed against real
         * silicon -- a good target to replace with the real value if
         * ever sourced from a MediaQ 1100/1132 datasheet or a working
         * Cloudpilot trace against real T600C hardware.
         */
        val = 0xffffffffULL;
    }
    return val;
}

static void clie_lcd_regs_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    ClieLcdState *s = opaque;
    unsigned int i;

    for (i = 0; i < size; i++) {
        s->regs[addr + i] = value >> (8 * (size - 1 - i));
    }
}

static const MemoryRegionOps clie_lcd_regs_ops = {
    .read = clie_lcd_regs_read,
    .write = clie_lcd_regs_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void clie_lcd_invalidate_display(void *opaque)
{
}

static const GraphicHwOps clie_lcd_gfx_ops = {
    .invalidate = clie_lcd_invalidate_display,
    .gfx_update = clie_lcd_update_display,
};

static void clie_lcd_reset(DeviceState *dev)
{
    ClieLcdState *s = CLIE_LCD(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void clie_lcd_realize(DeviceState *dev, Error **errp)
{
    ClieLcdState *s = CLIE_LCD(dev);

    memory_region_init_ram(&s->vmem_mr, OBJECT(dev), TYPE_CLIE_LCD ".vram",
                           CLIE_LCD_VMEM_SIZE, &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->vmem_mr);

    memory_region_init_io(&s->regs_mr, OBJECT(dev), &clie_lcd_regs_ops, s,
                          TYPE_CLIE_LCD ".regs", CLIE_LCD_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs_mr);

    s->con = qemu_graphic_console_create(dev, 0, &clie_lcd_gfx_ops, s);
}

static const VMStateDescription vmstate_clie_lcd = {
    .name = TYPE_CLIE_LCD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, ClieLcdState, CLIE_LCD_REGS_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static void clie_lcd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, clie_lcd_reset);
    dc->realize = clie_lcd_realize;
    dc->vmsd = &vmstate_clie_lcd;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo clie_lcd_info = {
    .name          = TYPE_CLIE_LCD,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ClieLcdState),
    .class_init    = clie_lcd_class_init,
};

static void clie_lcd_register_types(void)
{
    type_register_static(&clie_lcd_info);
}

type_init(clie_lcd_register_types)
