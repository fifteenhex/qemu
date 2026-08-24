/*
 * QEMU Macintosh LC 475 / Quadra 605 hardware system emulator
 *
 * 68LC040 machine: MEMCjr memory controller + PrimeTime I/O combo
 * (Quadra-class VIA1/VIA2), Cuda ADB/RTC/PRAM MCU on the VIA1 shift
 * register, 53C96 ESP SCSI with VIA2-DRQ pseudo-DMA, ESCC serial,
 * SWIM II floppy, EASC sound and Valkyrie/CSC-class onboard video.
 * Modelled on q800.c and maciisi.c; device set grown empirically
 * against the Quadra 605 ROM.
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
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/datadir.h"
#include "system/system.h"
#include "target/m68k/cpu.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/escc.h"
#include "hw/misc/mos6522.h"
#include "hw/misc/mac_via.h"
#include "hw/m68k/q800-glue.h"
#include "hw/audio/asc.h"
#include "hw/block/swim.h"
#include "hw/scsi/esp.h"
#include "hw/scsi/scsi.h"
#include "hw/input/adb.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "qemu/cutils.h"
#include "system/rtc.h"

#define LC475_ROM_ADDR        0x40800000
#define LC475_ROM_SIZE        0x00100000
#define LC475_ROM_FILENAME    "quadra605.rom"

#define IO_BASE               0x50000000
#define IO_SLICE              0x00040000
#define IO_SLICE_MASK         (IO_SLICE - 1)
#define IO_SIZE               0x04000000

/* offsets within the repeating I/O slice */
#define VIA1_OFS              0x00000
#define VIA2_OFS              0x02000
#define SCC_OFS               0x0c020
#define ESP_OFS               0x10000
#define ESP_PDMA_OFS          0x10100
#define ASC_OFS               0x14000
#define MEMCJR_OFS            0x0e000
#define PRIMETIME_OFS         0x18000
#define SWIM_OFS              0x1e000

#define MAC_CLOCK             3686418

/* Size of whole RAM area (unbacked reads return 0 for RAM sizing) */
#define RAM_SIZE              0x40000000

/* Onboard video: physical VRAM base the ROM's page tables resolve to */
#define LC475_VRAM_BASE       0xf9000000
#define LC475_VRAM_SIZE       0x00100000    /* 1 MiB */

#define VIA_TIMER_FREQ        783360
#define VIA_60HZ_TIMER_PERIOD_NS   16625800

/*
 * Machine ID register at 0x5FFFFFFC: read-only, upper word must be 0xA55A;
 * the ROM then disambiguates the djMEMC-class machines by the VIA1 port A
 * straps (see below).
 */
#define LC475_MACHINE_ID      0xa55a2bad

/*
 * VIA1 port A board straps, read by the ROM's machine identification with
 * DDRA forced to input: the box is selected by (PA & 0x56).  In the
 * Quadra 605 ROM's decoder table:
 *   0x42 -> box 0x34 (LC 475),   0x56/0x16 -> box 0x35 (P475 w/FPU),
 *   0x50 -> box 0x39 (Quadra 605), ...
 * Bits 1,2 low and 4,6 high give 0x50 = Quadra 605.
 */
#define LC475_VIA1_PINS_A     0xf9


/*
 * VIA1 subclass: on the LC475 the VIA1 fronts the Cuda MCU.  The Cuda
 * emulates the classic 343-0042 RTC/PRAM serial protocol on PB0 (data),
 * PB1 (clock), PB2 (/enable) for the ROM, and carries ADB and system
 * commands over the shift register with a TIP/TACK/TREQ handshake on
 * PB5/PB4/PB3 (all active low).  RTC engine ported from maciisi.c.
 */

#define VIA1B_vRTCEnb  0x04
#define VIA1B_vRTCClk  0x02
#define VIA1B_vRTCData 0x01

/* Cuda handshake lines on VIA1 port B (all active low) */
#define CUDA_TIP       0x20    /* out: transfer in progress */
#define CUDA_TACK      0x10    /* out: byte acknowledge */
#define CUDA_TREQ      0x08    /* in:  Cuda transfer request / session ack */

/* Cuda packet type bytes */
#define CUDA_PKT_ADB     0
#define CUDA_PKT_PSEUDO  1
#define CUDA_PKT_ERROR   2

/* VIA returns time offset from Jan 1, 1904, not 1970 */
#define RTC_OFFSET 2082844800

enum {
    REG_0,
    REG_1,
    REG_2,
    REG_3,
    REG_TEST,
    REG_WPROTECT,
    REG_PRAM_ADDR,
    REG_PRAM_ADDR_LAST = REG_PRAM_ADDR + 19,
    REG_PRAM_SECT,
    REG_PRAM_SECT_LAST = REG_PRAM_SECT + 7,
    REG_INVALID,
    REG_EMPTY = 0xff,
};

#define TYPE_MOS6522_LC475 "mos6522-lc475"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522Lc475State, MOS6522_LC475)

struct LC475MachineState;

struct MOS6522Lc475State {
    MOS6522State parent_obj;

    struct LC475MachineState *machine;
    ADBBusState adb_bus;
    uint8_t last_b;

    /* RTC */
    uint8_t PRAM[256];
    uint32_t tick_offset;
    uint8_t data_out;
    int data_out_cnt;
    uint8_t data_in;
    int data_in_cnt;
    uint8_t cmd;
    uint8_t alt;
    uint8_t wprotect;
};

static int lc475_cuda_log_budget = 6000;

#define lc475_cuda_log(...) do {                    \
        if (lc475_cuda_log_budget > 0) {            \
            lc475_cuda_log_budget--;                \
            qemu_log_mask(LOG_UNIMP, __VA_ARGS__);  \
        }                                           \
    } while (0)

static int via1_rtc_compact_cmd(uint8_t value)
{
    uint8_t read = value & 0x80;

    value &= 0x7f;

    /* the last 2 bits of a command byte must always be 0b01 ... */
    if ((value & 0x78) == 0x38) {
        /* except for the extended memory designator */
        return read | (REG_PRAM_SECT + (value & 0x07));
    }
    if ((value & 0x03) == 0x01) {
        value >>= 2;
        if ((value & 0x18) == 0) {
            /* seconds registers */
            return read | (REG_0 + (value & 0x03));
        } else if ((value == 0x0c) && !read) {
            return REG_TEST;
        } else if ((value == 0x0d) && !read) {
            return REG_WPROTECT;
        } else if ((value & 0x1c) == 0x08) {
            /* RAM address 0x10 to 0x13 */
            return read | (REG_PRAM_ADDR + 0x10 + (value & 0x03));
        } else if ((value & 0x10) == 0x10) {
            /* RAM address 0x00 to 0x0f */
            return read | (REG_PRAM_ADDR + (value & 0x0f));
        }
    }
    return REG_INVALID;
}

static void via1_rtc_update(MOS6522Lc475State *v1s)
{
    MOS6522State *s = MOS6522(v1s);
    int cmd, sector, addr;
    uint32_t time;

    if (s->b & VIA1B_vRTCEnb) {
        return;
    }

    if (s->dirb & VIA1B_vRTCData) {
        /* send bits to the RTC */
        if (!(v1s->last_b & VIA1B_vRTCClk) && (s->b & VIA1B_vRTCClk)) {
            v1s->data_out <<= 1;
            v1s->data_out |= s->b & VIA1B_vRTCData;
            v1s->data_out_cnt++;
        }
    } else {
        /* receive bits from the RTC */
        if ((v1s->last_b & VIA1B_vRTCClk) &&
            !(s->b & VIA1B_vRTCClk) &&
            v1s->data_in_cnt) {
            s->b = (s->b & ~VIA1B_vRTCData) |
                   ((v1s->data_in >> 7) & VIA1B_vRTCData);
            v1s->data_in <<= 1;
            v1s->data_in_cnt--;
        }
        return;
    }

    if (v1s->data_out_cnt != 8) {
        return;
    }

    v1s->data_out_cnt = 0;

    /* first byte: it's a command */
    if (v1s->cmd == REG_EMPTY) {
        cmd = via1_rtc_compact_cmd(v1s->data_out);

        if (cmd == REG_INVALID) {
            return;
        }

        if (cmd & 0x80) { /* this is a read command */
            switch (cmd & 0x7f) {
            case REG_0...REG_3: /* seconds registers */
                time = v1s->tick_offset +
                       (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                        / NANOSECONDS_PER_SECOND);
                v1s->data_in = (time >> ((cmd & 0x03) << 3)) & 0xff;
                v1s->data_in_cnt = 8;
                break;
            case REG_PRAM_ADDR...REG_PRAM_ADDR_LAST:
                /* PRAM address 0x00 -> 0x13 */
                v1s->data_in = v1s->PRAM[(cmd & 0x7f) - REG_PRAM_ADDR];
                v1s->data_in_cnt = 8;
                lc475_cuda_log("lc475 rtc: read pram 0x%02x -> 0x%02x\n",
                               (cmd & 0x7f) - REG_PRAM_ADDR, v1s->data_in);
                break;
            case REG_PRAM_SECT...REG_PRAM_SECT_LAST:
                /* the only two-byte read command */
                v1s->cmd = cmd;
                break;
            default:
                g_assert_not_reached();
            }
            return;
        }

        /* this is a write command, needs a parameter */
        if (cmd == REG_WPROTECT || !v1s->wprotect) {
            v1s->cmd = cmd;
        }
        return;
    }

    /* second byte: it's a parameter */
    if (v1s->alt == REG_EMPTY) {
        switch (v1s->cmd & 0x7f) {
        case REG_0...REG_3: /* seconds register */
        case REG_TEST:
            v1s->cmd = REG_EMPTY;
            break;
        case REG_WPROTECT:
            v1s->wprotect = !!(v1s->data_out & 0x80);
            v1s->cmd = REG_EMPTY;
            break;
        case REG_PRAM_ADDR...REG_PRAM_ADDR_LAST:
            v1s->PRAM[v1s->cmd - REG_PRAM_ADDR] = v1s->data_out;
            v1s->cmd = REG_EMPTY;
            break;
        case REG_PRAM_SECT...REG_PRAM_SECT_LAST:
            addr = (v1s->data_out >> 2) & 0x1f;
            sector = (v1s->cmd & 0x7f) - REG_PRAM_SECT;
            if (v1s->cmd & 0x80) {
                /* it's a read */
                v1s->data_in = v1s->PRAM[sector * 32 + addr];
                v1s->data_in_cnt = 8;
                lc475_cuda_log("lc475 rtc: read xpram 0x%02x -> 0x%02x\n",
                               sector * 32 + addr, v1s->data_in);
                v1s->cmd = REG_EMPTY;
            } else {
                /* it's a write, we need one more parameter */
                v1s->alt = addr;
            }
            break;
        default:
            g_assert_not_reached();
        }
        return;
    }

    /* third byte: it's the data of a REG_PRAM_SECT write */
    g_assert(REG_PRAM_SECT <= v1s->cmd && v1s->cmd <= REG_PRAM_SECT_LAST);
    sector = v1s->cmd - REG_PRAM_SECT;
    v1s->PRAM[sector * 32 + v1s->alt] = v1s->data_out;
    v1s->alt = REG_EMPTY;
    v1s->cmd = REG_EMPTY;
}

static void lc475_cuda_session_update(MOS6522Lc475State *v1s);
static void lc475_cuda_sr_written(MOS6522Lc475State *v1s);
static void lc475_cuda_acr_changed(MOS6522Lc475State *v1s);
static void lc475_cuda_idle_treq(MOS6522Lc475State *v1s);

static void lc475_via1_portA_write(MOS6522State *s)
{
}

static void lc475_via1_portB_write(MOS6522State *s)
{
    MOS6522Lc475State *v1s = MOS6522_LC475(s);

    if (v1s->machine) {
        lc475_cuda_session_update(v1s);
        lc475_cuda_idle_treq(v1s);
    }
}

static void mos6522_lc475_init(Object *obj)
{
    MOS6522Lc475State *v1s = MOS6522_LC475(obj);

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;

    /* the ADB bus hangs off the Cuda; the VIA1 device stands in for it */
    qbus_init(&v1s->adb_bus, sizeof(v1s->adb_bus), TYPE_ADB_BUS,
              DEVICE(obj), "adb.0");
}

static void mos6522_lc475_reset_hold(Object *obj, ResetType type)
{
    MOS6522Lc475State *v1s = MOS6522_LC475(obj);
    MOS6522State *ms = MOS6522(v1s);
    MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(ms);

    if (mdc->parent_phases.hold) {
        mdc->parent_phases.hold(obj, type);
    }

    /* board-specific idle state of the input pins (TREQ deasserted) */
    ms->a = LC475_VIA1_PINS_A;
    ms->b = 0xff;

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;
    v1s->data_out_cnt = 0;
    v1s->data_in_cnt = 0;
}

static void mos6522_lc475_class_init(ObjectClass *klass, const void *data)
{
    MOS6522DeviceClass *mdc = MOS6522_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    mdc->portA_write = lc475_via1_portA_write;
    mdc->portB_write = lc475_via1_portB_write;
    resettable_class_set_parent_phases(rc, NULL, mos6522_lc475_reset_hold,
                                       NULL, &mdc->parent_phases);
}

/*
 * Onboard video framebuffer display device (Valkyrie/CSC-class).  Starts
 * life as a fixed 640x480 mono view of the VRAM; grown as the ROM's
 * video driver behaviour is discovered.
 */

#define TYPE_LC475_FB "lc475-fb"
OBJECT_DECLARE_SIMPLE_TYPE(LC475FbState, LC475_FB)

struct LC475FbState {
    SysBusDevice parent_obj;

    MemoryRegion vram;
    MemoryRegionSection fbsection;
    QemuConsole *con;
    int invalidate;

    int depth;                  /* 1, 2, 4, 8 or 16 bpp */
    int width;
    int height;
    int stride;
    uint32_t palette[256];
    uint32_t fb_offset;
};

static int lc475_fb_stride(LC475FbState *s)
{
    return s->stride;
}

static void lc475_fb_draw_line(void *opaque, uint8_t *d, const uint8_t *src,
                               int width, int pitch)
{
    LC475FbState *s = opaque;
    uint32_t *buf = (uint32_t *)d;
    int i, b;

    switch (s->depth) {
    case 1:
        for (i = 0; i < s->width / 8; i++) {
            uint8_t v = src[i];
            for (b = 0; b < 8; b++) {
                buf[i * 8 + b] = (v & 0x80) ? 0xFF000000 : 0xFFFFFFFF;
                v <<= 1;
            }
        }
        break;
    case 2:
        for (i = 0; i < s->width / 4; i++) {
            uint8_t v = src[i];
            for (b = 0; b < 4; b++) {
                buf[i * 4 + b] = s->palette[(v >> 6) & 3];
                v <<= 2;
            }
        }
        break;
    case 4:
        for (i = 0; i < s->width / 2; i++) {
            uint8_t v = src[i];
            buf[i * 2] = s->palette[(v >> 4) & 0xf];
            buf[i * 2 + 1] = s->palette[v & 0xf];
        }
        break;
    case 8:
        for (i = 0; i < s->width; i++) {
            buf[i] = s->palette[src[i]];
        }
        break;
    case 16:
        /* big-endian xRGB1555 */
        for (i = 0; i < s->width; i++) {
            uint16_t v = (src[i * 2] << 8) | src[i * 2 + 1];
            uint8_t r = (v >> 10) & 0x1f;
            uint8_t g = (v >> 5) & 0x1f;
            uint8_t bl = v & 0x1f;

            buf[i] = 0xFF000000 | (r << 19) | (r >> 2 << 16)
                     | (g << 11) | (g >> 2 << 8) | (bl << 3) | (bl >> 2);
        }
        break;
    }
}

/*
 * Track the guest's idea of the screen: QuickDraw publishes the main
 * device's PixMap (base address, rowbytes, bounds, depth) through the
 * MainDevice GDevice handle at lowmem 0x8A4.  Reading it beats reverse
 * engineering the (undocumented) Valkyrie/CSC mode registers, and
 * automatically follows Display Manager depth switches.
 */
static void lc475_fb_track_mode(LC475FbState *s)
{
    hwaddr gdh, gd, pmh, pm, base;
    uint32_t rb, top, left, bottom, right, pixsz;
    int width, height;

    /*
     * Master pointers carry tag bits in the high byte when MacOS runs
     * in 24-bit addressing (the default out of a rebuilt PRAM), so
     * strip everything above the 24-bit RAM range before sanity checks.
     */
    gdh = ldl_be_phys(&address_space_memory, 0x8a4) & 0xffffff;
    if (gdh < 0x1000 || (gdh & 1)) {
        return;
    }
    gd = ldl_be_phys(&address_space_memory, gdh) & 0xffffff;
    if (gd < 0x1000 || (gd & 1)) {
        return;
    }
    pmh = ldl_be_phys(&address_space_memory, gd + 0x16) & 0xffffff;
    if (pmh < 0x1000 || (pmh & 1)) {
        return;
    }
    pm = ldl_be_phys(&address_space_memory, pmh) & 0xffffff;
    if (pm < 0x1000 || (pm & 1)) {
        return;
    }

    base = ldl_be_phys(&address_space_memory, pm);
    rb = lduw_be_phys(&address_space_memory, pm + 4) & 0x3fff;
    top = lduw_be_phys(&address_space_memory, pm + 6);
    left = lduw_be_phys(&address_space_memory, pm + 8);
    bottom = lduw_be_phys(&address_space_memory, pm + 10);
    right = lduw_be_phys(&address_space_memory, pm + 12);
    pixsz = lduw_be_phys(&address_space_memory, pm + 32);

    if ((base >> 20) != (LC475_VRAM_BASE >> 20)) {
        return;                 /* not pointing at VRAM (yet) */
    }
    width = (int16_t)right - (int16_t)left;
    height = (int16_t)bottom - (int16_t)top;
    if (width <= 0 || width > 1152 || height <= 0 || height > 870) {
        return;
    }
    if (pixsz != 1 && pixsz != 2 && pixsz != 4 && pixsz != 8 && pixsz != 16) {
        return;
    }
    if (rb < (uint32_t)width * pixsz / 8) {
        return;
    }
    base &= LC475_VRAM_SIZE - 1;
    if (base + (uint64_t)rb * height > LC475_VRAM_SIZE) {
        return;
    }

    if (s->width != width || s->height != height || s->depth != pixsz ||
        s->stride != (int)rb || s->fb_offset != base) {
        s->width = width;
        s->height = height;
        s->depth = pixsz;
        s->stride = rb;
        s->fb_offset = base;
        s->invalidate = 1;
        qemu_console_resize(s->con, width, height);
    }
}

static bool lc475_fb_update(void *opaque)
{
    LC475FbState *s = LC475_FB(opaque);
    DisplaySurface *surface;
    int first = 0, last = 0;

    lc475_fb_track_mode(s);
    surface = qemu_console_surface(s->con);

    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection, &s->vram,
                                          s->fb_offset,
                                          s->height,
                                          lc475_fb_stride(s));
        s->invalidate = 0;
    }

    framebuffer_update_display(surface, &s->fbsection,
                               s->width, s->height,
                               lc475_fb_stride(s), s->width * 4,
                               0, 1, lc475_fb_draw_line, s,
                               &first, &last);
    qemu_console_update(s->con, 0, 0, s->width, s->height);
    return true;
}

static void lc475_fb_invalidate(void *opaque)
{
    LC475FbState *s = LC475_FB(opaque);

    s->invalidate = 1;
}

static const GraphicHwOps lc475_fb_ops = {
    .invalidate = lc475_fb_invalidate,
    .gfx_update = lc475_fb_update,
};

static void lc475_fb_realize(DeviceState *dev, Error **errp)
{
    LC475FbState *s = LC475_FB(dev);
    int i;

    memory_region_init_ram(&s->vram, OBJECT(dev), "lc475.vram",
                           LC475_VRAM_SIZE, &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->vram);

    /*
     * Defaults until QuickDraw publishes the real geometry (see
     * lc475_fb_track_mode): the ROM runs the 13" 640x480 mode at 8bpp
     * with the frame buffer at VRAM offset 0x1000 (ScrnBase logical
     * 0x51901000).
     */
    s->depth = 8;
    s->width = 640;
    s->height = 480;
    s->stride = 640;
    s->fb_offset = 0x1000;
    /* default grayscale ramp until the CLUT is programmed */
    for (i = 0; i < 256; i++) {
        uint8_t v = 255 - i;
        s->palette[i] = 0xFF000000 | (v << 16) | (v << 8) | v;
    }
    s->invalidate = 1;
    s->con = qemu_graphic_console_create(dev, 0, &lc475_fb_ops, s);
    qemu_console_resize(s->con, s->width, s->height);
}

static void lc475_fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->realize = lc475_fb_realize;
}

/* machine */

typedef struct {
    const char *name;
    uint8_t *regs;
    uint32_t size;
    int log_budget;
} LC475RegBank;

struct LC475MachineState {
    MachineState parent_obj;

    M68kCPU cpu;
    GLUEState glue;
    MOS6522Lc475State via1;
    MOS6522Q800VIA2State via2;
    ESCCState escc;
    OrIRQState escc_orgate;
    ASCState asc;
    SysBusESPState esp;
    Swim swim;
    LC475FbState fb;

    MemoryRegion rom;
    MemoryRegion rom_alias;
    MemoryRegion machine_id;
    MemoryRegion via1mem;
    MemoryRegion memcjr_mem;
    MemoryRegion primetime_mem;
    uint8_t memcjr_regs[0x1000];
    uint8_t primetime_regs[0x2000];
    MemoryRegion valkyrie_mem;
    uint8_t valkyrie_regs[0x4000];
    LC475RegBank *valkyrie_bank;
    uint8_t clut_addr;
    uint8_t clut_phase;
    uint8_t clut_rgb[3];
    MemoryRegion vram_aliases[7];
    MemoryRegion ramio;
    MemoryRegion macio;
    MemoryRegion macio_alias;
    MemoryRegion escc_alias;
    MemoryRegion iotrace;
    MemoryRegion bgtrace;

    /* VIA1 CA1 60Hz tick and CA2 one-second tick (into the Cuda's VIA) */
    QEMUTimer *sixty_hz_timer;
    QEMUTimer *one_second_timer;

    /* Cuda MCU engine on the VIA1 shift register */
    QEMUTimer *cuda_timer;
    uint8_t cuda_cmd[16 + ADB_MAX_OUT_LEN];
    int cuda_cmd_len;
    uint8_t cuda_resp[16 + ADB_MAX_OUT_LEN];
    int cuda_resp_len;
    int cuda_resp_idx;
    bool cuda_session;
    bool cuda_no_resp;

    /* SETUPTIMEK calibration hack (see mac_via.c) */
    int timer_hack_state;
};

#define TYPE_LC475_MACHINE MACHINE_TYPE_NAME("lc475")
OBJECT_DECLARE_SIMPLE_TYPE(LC475MachineState, LC475_MACHINE)

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static uint32_t lc475_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

static uint64_t ramio_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0x0;
}

static void ramio_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
}

static const MemoryRegionOps ramio_ops = {
    .read = ramio_read,
    .write = ramio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static MemTxResult macio_alias_read(void *opaque, hwaddr addr, uint64_t *data,
                                    unsigned size, MemTxAttrs attrs)
{
    MemTxResult r;
    uint32_t val;

    addr &= IO_SLICE_MASK;
    addr |= IO_BASE;

    switch (size) {
    case 4:
        val = address_space_ldl_be(&address_space_memory, addr, attrs, &r);
        break;
    case 2:
        val = address_space_lduw_be(&address_space_memory, addr, attrs, &r);
        break;
    case 1:
        val = address_space_ldub(&address_space_memory, addr, attrs, &r);
        break;
    default:
        g_assert_not_reached();
    }

    *data = val;
    return r;
}

static MemTxResult macio_alias_write(void *opaque, hwaddr addr, uint64_t value,
                                     unsigned size, MemTxAttrs attrs)
{
    MemTxResult r;

    addr &= IO_SLICE_MASK;
    addr |= IO_BASE;

    switch (size) {
    case 4:
        address_space_stl_be(&address_space_memory, addr, value, attrs, &r);
        break;
    case 2:
        address_space_stw_be(&address_space_memory, addr, value, attrs, &r);
        break;
    case 1:
        address_space_stb(&address_space_memory, addr, value, attrs, &r);
        break;
    default:
        g_assert_not_reached();
    }

    return r;
}

static const MemoryRegionOps macio_alias_ops = {
    .read_with_attrs = macio_alias_read,
    .write_with_attrs = macio_alias_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* unmapped I/O space bus-errors on the real machine; log the probes */

static MemTxResult lc475_iotrace_read(void *opaque, hwaddr addr,
                                      uint64_t *data, unsigned size,
                                      MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "lc475 io: read  +0x%05x (%d) -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, lc475_trace_pc());
    }
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult lc475_iotrace_write(void *opaque, hwaddr addr,
                                       uint64_t val, unsigned size,
                                       MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "lc475 io: write +0x%05x (%d) <- 0x%08" PRIx64
                      " -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, val, lc475_trace_pc());
    }
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps lc475_iotrace_ops = {
    .read_with_attrs = lc475_iotrace_read,
    .write_with_attrs = lc475_iotrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* background catch-all outside the I/O slice: log + BERR */

static MemTxResult lc475_bgtrace_read(void *opaque, hwaddr addr,
                                      uint64_t *data, unsigned size,
                                      MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "lc475 bus: read  0x%08x (%d) -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, lc475_trace_pc());
    }
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult lc475_bgtrace_write(void *opaque, hwaddr addr,
                                       uint64_t val, unsigned size,
                                       MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "lc475 bus: write 0x%08x (%d) <- 0x%08" PRIx64
                      " -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, val, lc475_trace_pc());
    }
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps lc475_bgtrace_ops = {
    .read_with_attrs = lc475_bgtrace_read,
    .write_with_attrs = lc475_bgtrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * Generic byte-array register bank with logging: MEMCjr (memory
 * controller, 0x50F0E000) and PrimeTime I/O combo control regs
 * (0x50F18000).  Grown empirically: values written read back.
 */

static uint64_t lc475_regbank_read(void *opaque, hwaddr addr, unsigned size)
{
    LC475RegBank *b = opaque;
    uint64_t val = 0;
    int i;

    for (i = 0; i < size; i++) {
        val = (val << 8) | b->regs[(addr + i) & (b->size - 1)];
    }
    if (b->log_budget > 0) {
        b->log_budget--;
        qemu_log_mask(LOG_UNIMP, "lc475 %s: read  +0x%04x (%d) -> 0x%08"
                      PRIx64 " pc=0x%08x\n", b->name, (unsigned)addr, size,
                      val, lc475_trace_pc());
    }
    return val;
}

static void lc475_regbank_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    LC475RegBank *b = opaque;
    int i;

    if (b->log_budget > 0) {
        b->log_budget--;
        qemu_log_mask(LOG_UNIMP, "lc475 %s: write +0x%04x (%d) <- 0x%08"
                      PRIx64 " pc=0x%08x\n", b->name, (unsigned)addr, size,
                      val, lc475_trace_pc());
    }
    for (i = size - 1; i >= 0; i--) {
        b->regs[(addr + i) & (b->size - 1)] = val & 0xff;
        val >>= 8;
    }
}

static const MemoryRegionOps lc475_regbank_ops = {
    .read = lc475_regbank_read,
    .write = lc475_regbank_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* machine ID register: read-only, writes ignored */

static uint64_t machine_id_read(void *opaque, hwaddr addr, unsigned size)
{
    /*
     * Any access width works on the real register; the ROM uses long
     * reads early on (expects 0xA55Axxxx) and later `cmpiw #$2bad`
     * word reads, so narrow reads return the LOW part of the ID.
     */
    return LC475_MACHINE_ID & ((size == 4) ? 0xffffffff :
                               (size == 2) ? 0xffff : 0xff);
}

static void machine_id_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
}

static const MemoryRegionOps machine_id_ops = {
    .read = machine_id_read,
    .write = machine_id_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};


/*
 * Cuda MCU engine (grown empirically against the Quadra 605 ROM; the
 * transport is the maciisi Egret protocol with INVERTED /TREQ polarity:
 *  - the Cuda ACKS a host session (TIP and/or TACK asserted low) by
 *    asserting /TREQ (PB3 low);
 *  - each byte moves through the VIA shift register and is acknowledged
 *    with a shift-register interrupt;
 *  - the host turns the shifter around (ACR SR input) while holding the
 *    session to collect the response; /TREQ stays LOW while response
 *    bytes are valid and is RAISED at the interrupt that ends the
 *    response;
 *  - session release = both TIP and TACK high (send-phase byte toggles
 *    flip one of them at a time, so exactly one stays low mid-exchange).
 */

static void lc475_cuda_set_treq(MOS6522Lc475State *v1s, bool assert)
{
    MOS6522State *s = MOS6522(v1s);

    if (assert) {
        s->b &= ~CUDA_TREQ;             /* active low */
    } else {
        s->b |= CUDA_TREQ;
    }
}

/*
 * Host probes sometimes drive all of port B as outputs (the ROM's VIA
 * fingerprint, RTC bit-bang with sloppy masks), clobbering the stored
 * /TREQ input state.  Restore the idle level whenever no exchange is in
 * progress.
 */
static void lc475_cuda_idle_treq(MOS6522Lc475State *v1s)
{
    LC475MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    /*
     * Only when the host has really released the bus: the end-of-response
     * toggle still has one of TIP/TACK low and /TREQ must stay asserted
     * through the final interrupt (the end marker).
     */
    if (!m->cuda_session && m->cuda_resp_len == 0
        && (s->b & (CUDA_TIP | CUDA_TACK)) == (CUDA_TIP | CUDA_TACK)) {
        lc475_cuda_set_treq(v1s, false);
    }
}

static void lc475_cuda_schedule_int(LC475MachineState *m)
{
    timer_mod(m->cuda_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * 1000);
}

static void lc475_cuda_timer_cb(void *opaque)
{
    LC475MachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), SR_INT_BIT);

    qemu_set_irq(irq, 1);
}

static void lc475_cuda_no_response(LC475MachineState *m)
{
    /*
     * No response: run the interrupt cadence with two junk bytes but
     * with /TREQ asserted at the first response interrupt, which makes
     * the driver set its discard flag and zero the byte count.
     */
    m->cuda_no_resp = true;
    m->cuda_resp[m->cuda_resp_len++] = 0x00;
    m->cuda_resp[m->cuda_resp_len++] = 0x00;
}

static void lc475_cuda_process(LC475MachineState *m)
{
    ADBBusState *adb_bus = &m->via1.adb_bus;
    uint8_t *c = m->cuda_cmd;
    int n = m->cuda_cmd_len;

    m->cuda_resp_len = 0;
    m->cuda_resp_idx = 0;

    lc475_cuda_log(
                  "lc475 cuda: cmd len=%d [%02x %02x %02x %02x]\n",
                  n, n > 0 ? c[0] : 0, n > 1 ? c[1] : 0, n > 2 ? c[2] : 0,
                  n > 3 ? c[3] : 0);

    if (n == 0) {
        return;
    }

    m->cuda_no_resp = false;

    /*
     * Packet framing as the ROM driver speaks it: the raw ADB command
     * byte followed by optional listen data — no Cuda type byte (the
     * enumeration sends 1-byte Talk commands (addr<<4)|reg directly).
     * PRAM/RTC traffic goes over the emulated 343-0042 bit-bang
     * protocol instead.
     */
    if (n == 1 && c[0] == 0x00) {
        /* ADB SendReset, the ROM's startup sync */
        uint8_t scratch[ADB_MAX_OUT_LEN];

        adb_autopoll_block(adb_bus);
        adb_request(adb_bus, scratch, c, 1);
        adb_autopoll_unblock(adb_bus);
        lc475_cuda_no_response(m);
        return;
    }

    {
        uint8_t obuf[ADB_MAX_OUT_LEN];
        int olen;

        adb_autopoll_block(adb_bus);
        olen = adb_request(adb_bus, obuf, c, n);
        adb_autopoll_unblock(adb_bus);

        if (olen > 0) {
            /* reply: the raw register data */
            memcpy(m->cuda_resp, obuf, olen);
            m->cuda_resp_len = olen;
        } else {
            /* Listen/no-data/absent device: no response */
            lc475_cuda_no_response(m);
        }
    }
}

/*
 * Cuda-initiated transfer: autopolled ADB data raises /TREQ while the
 * bus is idle; the host then opens a session and clocks the packet out
 * like a command response.
 */
static void lc475_cuda_adb_poll(void *opaque)
{
    LC475MachineState *m = opaque;
    MOS6522Lc475State *v1s = &m->via1;
    MOS6522State *s = MOS6522(v1s);
    uint8_t obuf[ADB_MAX_OUT_LEN + 3];
    int olen;

    /* only when no exchange is in progress and the shifter is inbound */
    if (m->cuda_session || m->cuda_resp_len > 0 || (s->acr & SR_OUT)) {
        return;
    }

    olen = adb_poll(&v1s->adb_bus, obuf, v1s->adb_bus.autopoll_mask);
    if (olen <= 0) {
        return;
    }
    /* adb_poll tags the data with the Talk R0 command byte at obuf[0] */
    memcpy(m->cuda_resp, obuf, olen);
    m->cuda_resp_len = olen;
    m->cuda_resp_idx = 1;
    m->cuda_no_resp = false;
    m->cuda_session = true;             /* Cuda-initiated session */
    s->sr = m->cuda_resp[0];
    lc475_cuda_set_treq(v1s, true);
    lc475_cuda_schedule_int(m);
    lc475_cuda_log(
                  "lc475 cuda: unsol len=%d [%02x %02x %02x %02x]\n",
                  m->cuda_resp_len, obuf[0], obuf[1], obuf[2],
                  m->cuda_resp_len > 3 ? obuf[3] : 0);
}

static void lc475_cuda_ack_toggle(MOS6522Lc475State *v1s)
{
    LC475MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (m->cuda_resp_idx < m->cuda_resp_len) {
        s->sr = m->cuda_resp[m->cuda_resp_idx++];
        /*
         * /TREQ stays HIGH while valid bytes are presented (low at the
         * first response interrupt makes the driver discard the bytes).
         */
        lc475_cuda_set_treq(v1s, m->cuda_no_resp);
        lc475_cuda_schedule_int(m);
        lc475_cuda_log(
                      "lc475 cuda: feed 0x%02x (#%d/%d) pc=%08x b=%02x\n",
                      s->sr, m->cuda_resp_idx, m->cuda_resp_len,
                      lc475_trace_pc(), s->b);
    } else {
        /* final ack: /TREQ LOW = end-of-response */
        m->cuda_resp_len = 0;
        m->cuda_resp_idx = 0;
        m->cuda_session = false;
        lc475_cuda_set_treq(v1s, true);
        lc475_cuda_schedule_int(m);
        lc475_cuda_log(
                      "lc475 cuda: response complete pc=%08x b=%02x\n",
                      lc475_trace_pc(), s->b);
    }
}

static void lc475_cuda_session_update(MOS6522Lc475State *v1s)
{
    LC475MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);
    bool sys = (s->b & (CUDA_TIP | CUDA_TACK)) != (CUDA_TIP | CUDA_TACK);
    uint8_t hs_change = (s->b ^ v1s->last_b) & (CUDA_TIP | CUDA_TACK);

    if (sys && !m->cuda_session) {
        m->cuda_session = true;
        m->cuda_cmd_len = 0;
        m->cuda_resp_len = 0;
        m->cuda_resp_idx = 0;
        /* the ROM preloads the first byte before asserting the session */
        if (s->acr & SR_OUT) {
            lc475_cuda_sr_written(v1s);
        }
        return;
    }

    /*
     * TIP/TACK toggles during the receive phase acknowledge the byte in
     * SR and clock the next one (the cadence keeps exactly one of them
     * asserted; both released means the host closed the session).
     */
    if (m->cuda_session && !(s->acr & SR_OUT) && hs_change
        && m->cuda_resp_len > 0 && sys) {
        lc475_cuda_ack_toggle(v1s);
        return;
    }

    if (!sys && m->cuda_session) {
        if (m->cuda_resp_len > 0 && m->cuda_resp_idx < m->cuda_resp_len) {
            /*
             * A response staged by the ACR turnaround: the driver's
             * turnaround cadence releases both lines once ("eor #0x30"
             * from the both-asserted state) before re-asserting TIP for
             * the receive phase.  Keep the session open and interrupt so
             * the driver's continuation issues the first receive toggle.
             */
            lc475_cuda_schedule_int(m);
            return;
        }
        /*
         * Host closed the session.  A packet sent without the receive
         * turnaround is processed now (ADB Listens must reach the
         * devices); any response it builds is discarded — nobody is
         * listening.
         */
        if (m->cuda_cmd_len > 0 && m->cuda_resp_len == 0
            && m->cuda_resp_idx == 0) {
            lc475_cuda_process(m);
        }
        m->cuda_session = false;
        m->cuda_cmd_len = 0;
        m->cuda_resp_len = 0;
        m->cuda_resp_idx = 0;
        m->cuda_no_resp = false;
        /* /TREQ returns to idle (deasserted) */
        lc475_cuda_set_treq(v1s, false);
    }
}


static void lc475_cuda_sr_written(MOS6522Lc475State *v1s)
{
    LC475MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (!m->cuda_session || !(s->acr & SR_OUT)) {
        return;
    }

    if (m->cuda_cmd_len < (int)sizeof(m->cuda_cmd)) {
        m->cuda_cmd[m->cuda_cmd_len++] = s->sr;
    }
    /*
     * /TREQ stays HIGH during host sends: asserting it at the byte
     * acknowledge is the collision signal ("beq 2cde" continuations)
     * and makes the driver treat the exchange as Cuda-initiated.
     */
    lc475_cuda_set_treq(v1s, false);
    lc475_cuda_log(
                  "lc475 cuda: <- 0x%02x (#%d) pc=%08x b=%02x\n", s->sr,
                  m->cuda_cmd_len, lc475_trace_pc(), s->b);
    lc475_cuda_schedule_int(m);
}

static void lc475_cuda_acr_changed(MOS6522Lc475State *v1s)
{
    LC475MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    /*
     * The host turns the shifter around to receive while still holding
     * the session: treat the bytes collected so far as the command and
     * build the response; the host's following handshake toggle clocks
     * the first byte out.  /TREQ stays LOW for a real response and is
     * RAISED for the no-response turnaround.
     */
    if (m->cuda_session && !(s->acr & SR_OUT) && m->cuda_resp_len == 0
        && m->cuda_cmd_len > 0) {
        lc475_cuda_process(m);
        m->cuda_cmd_len = 0;
        /* /TREQ high across the response; low only for no-response */
        lc475_cuda_set_treq(v1s, m->cuda_no_resp && m->cuda_resp_len > 0);
    } else if (m->cuda_session && !(s->acr & SR_OUT)
               && m->cuda_resp_len > 0 && m->cuda_cmd_len > 0) {
        /*
         * Turnaround with a Cuda-initiated packet staged: the host
         * collided with us mid-send and is receiving our packet first;
         * it re-sends its command afterwards, so drop the partial one.
         */
        m->cuda_cmd_len = 0;
    }
}

/*
 * VIA1 window: VIA-spaced registers with the board straps merged into
 * port A input reads and the Cuda/RTC engine hooks.
 */

static uint64_t lc475_via1_read(void *opaque, hwaddr addr, unsigned size)
{
    LC475MachineState *m = opaque;
    MOS6522State *ms = MOS6522(&m->via1);
    hwaddr reg = (addr >> 9) & 0xf;
    uint64_t val;

    val = mos6522_read(ms, reg, size);

    /* input pins on port A read the board straps, not the last output */
    if (reg == VIA_REG_A || reg == VIA_REG_ANH) {
        val = (val & ms->dira) | (LC475_VIA1_PINS_A & ~ms->dira);
    }

    return val;
}

static void lc475_via1_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    LC475MachineState *m = opaque;
    MOS6522Lc475State *v1s = &m->via1;
    MOS6522State *ms = MOS6522(v1s);
    hwaddr reg = (addr >> 9) & 0xf;

    /*
     * SETUPTIMEK calibration hack, as on q800 (mac_via.c): under TCG the
     * ROM's dbra-loop timer calibration produces garbage (TimeDBRA at
     * 0xd00 ends up zero, later causing a divide-by-zero SysError in the
     * Time Manager conversion at ROM 0x40843a80).  Detect the T2=0x30c
     * calibration run and stuff known-good values at its end.
     */
    switch (m->timer_hack_state) {
    case 0:
        if (reg == VIA_REG_T2CL && val == 0xc) {
            m->timer_hack_state = 1;
        }
        break;
    case 1:
        if (reg == VIA_REG_T2CH && val == 0x3) {
            m->timer_hack_state = 2;
        } else if (reg == VIA_REG_T2CL || reg == VIA_REG_T2CH) {
            m->timer_hack_state = 0;
        }
        break;
    case 2:
        if (reg == VIA_REG_IER && val == 0x20) {
            /* end of SETUPTIMEK: force sane calibration results */
            stw_be_phys(&address_space_memory, 0xd00, 0x2a00 * 3);
            stw_be_phys(&address_space_memory, 0xd02, 0x079d * 3);
            m->timer_hack_state = 3;
        }
        break;
    default:
        break;
    }

    if (reg == VIA_REG_B || reg == VIA_REG_SR || reg == VIA_REG_ACR) {
        static int count;

        if (count < 4000) {
            count++;
            qemu_log_mask(LOG_UNIMP,
                          "lc475 via1: reg%d <- 0x%02" PRIx64 " pc=0x%08x\n",
                          (int)reg, val, lc475_trace_pc());
        }
    }
    mos6522_write(ms, reg, val, size);

    if (reg == VIA_REG_SR && v1s->machine) {
        lc475_cuda_sr_written(v1s);
    }
    if (reg == VIA_REG_ACR && v1s->machine) {
        lc475_cuda_acr_changed(v1s);
    }
    if (reg == VIA_REG_B) {
        via1_rtc_update(v1s);
        v1s->last_b = ms->b;
    }
}

static const MemoryRegionOps lc475_via1_ops = {
    .read = lc475_via1_read,
    .write = lc475_via1_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * Valkyrie interrupt/status registers (empirical):
 *   +0x104 (long) written 4    -> interrupt enable mask
 *   +0x108 (long) polled       -> status; bit 2 = vertical blank
 *   +0x10c (long) written 0    -> VBL status clear
 * The 60Hz tick sets the VBL bit; writes to +0x10c clear it.
 */

static void lc475_valkyrie_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    LC475MachineState *m = LC475_MACHINE(opaque);
    LC475RegBank *b = m->valkyrie_bank;

    if (addr == 0x10c || (addr <= 0x10c && addr + size > 0x10c)) {
        /* VBL status clear */
        m->valkyrie_regs[0x10b] &= ~0x04;
        return;
    }
    if (addr == 0x200) {
        /* CLUT address register */
        m->clut_addr = val & 0xff;
        m->clut_phase = 0;
    }
    if (addr == 0x213 && size == 1) {
        /* CLUT data: three writes (R, G, B) per entry */
        m->clut_rgb[m->clut_phase++] = val & 0xff;
        if (m->clut_phase == 3) {
            m->fb.palette[m->clut_addr] = 0xFF000000 |
                (m->clut_rgb[0] << 16) | (m->clut_rgb[1] << 8) |
                m->clut_rgb[2];
            m->fb.invalidate = 1;
            m->clut_phase = 0;
            m->clut_addr++;
        }
        return;
    }
    lc475_regbank_write(b, addr, val, size);
}

static uint64_t lc475_valkyrie_read(void *opaque, hwaddr addr, unsigned size)
{
    LC475MachineState *m = LC475_MACHINE(opaque);

    /*
     * Monitor sense: word reads at +0x122 return the live sense lines.
     * Raw code 6 = 13" 640x480 RGB (raw != 7 so the driver never tries
     * the extended sense dance).
     */
    if (addr == 0x122 && size == 2) {
        return 6;
    }
    return lc475_regbank_read(m->valkyrie_bank, addr, size);
}

static const MemoryRegionOps lc475_valkyrie_ops = {
    .read = lc475_valkyrie_read,
    .write = lc475_valkyrie_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* 60.15Hz tick on VIA1 CA1 and one-second tick on CA2 */

static void lc475_sixty_hz(void *opaque)
{
    LC475MachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA1_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    /* Valkyrie VBL status */
    m->valkyrie_regs[0x10b] |= 0x04;

    timer_mod(m->sixty_hz_timer,
              (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
               VIA_60HZ_TIMER_PERIOD_NS) /
              VIA_60HZ_TIMER_PERIOD_NS * VIA_60HZ_TIMER_PERIOD_NS);
}

static void lc475_one_second(void *opaque)
{
    LC475MachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA2_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->one_second_timer,
              (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000) / 1000 * 1000);
}

static void lc475_machine_init(MachineState *machine)
{
    LC475MachineState *m = LC475_MACHINE(machine);
    ram_addr_t ram_size = machine->ram_size;
    const char *bios_name = machine->firmware ?: LC475_ROM_FILENAME;
    char *filename;
    int bios_size;
    uint8_t *ptr;
    CPUState *cs;
    DeviceState *dev;
    SysBusDevice *sysbus;
    ESPState *esp;
    BusState *adb_bus;

    if (ram_size > 0x10000000) {
        error_report("Too much memory for this machine: %" PRId64 " MiB, "
                     "maximum 256 MiB", ram_size / MiB);
        exit(1);
    }

    /* CPU */
    object_initialize_child(OBJECT(machine), "cpu", &m->cpu, machine->cpu_type);
    qdev_realize(DEVICE(&m->cpu), NULL, &error_fatal);
    qemu_register_reset(main_cpu_reset, &m->cpu);

    /* RAM inside a container whose unbacked reads return 0 (RAM sizing) */
    memory_region_init_io(&m->ramio, OBJECT(machine), &ramio_ops, &m->ramio,
                          "ram", RAM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x0, &m->ramio);
    memory_region_add_subregion(&m->ramio, 0, machine->ram);

    /* background catch-all: everything unclaimed bus-errors, logged */
    memory_region_init_io(&m->bgtrace, OBJECT(machine), &lc475_bgtrace_ops,
                          m, "lc475.bus-trace", 0xffffffffull + 1);
    memory_region_add_subregion_overlap(get_system_memory(), 0,
                                        &m->bgtrace, -3);

    /* machine ID register */
    memory_region_init_io(&m->machine_id, NULL, &machine_id_ops, NULL,
                          "Machine ID", 4);
    memory_region_add_subregion(get_system_memory(), 0x5ffffffc,
                                &m->machine_id);

    /* IRQ glue (Quadra-class GLUE from q800) */
    object_initialize_child(OBJECT(machine), "glue", &m->glue, TYPE_GLUE);
    object_property_set_link(OBJECT(&m->glue), "cpu", OBJECT(&m->cpu),
                             &error_abort);
    /*
     * Pin the classic interrupt mapping (VIA1=1, VIA2=2, SCC=4): our
     * VIA1 is the Cuda's, which has no auxmode output to drive the GLUE.
     */
    qdev_prop_set_uint8(DEVICE(&m->glue), "auxmode-default", 1);
    sysbus_realize(SYS_BUS_DEVICE(&m->glue), &error_fatal);

    /* I/O container + repeating alias */
    memory_region_init(&m->macio, OBJECT(machine), "mac-io", IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE, &m->macio);

    memory_region_init_io(&m->macio_alias, OBJECT(machine), &macio_alias_ops,
                          &m->macio, "mac-io.alias", IO_SIZE - IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE + IO_SLICE,
                                &m->macio_alias);

    /* MEMCjr + PrimeTime register banks */
    {
        static LC475RegBank memcjr_bank, primetime_bank;

        memcjr_bank = (LC475RegBank){ "memcjr", m->memcjr_regs,
                                      sizeof(m->memcjr_regs), 4000 };
        memory_region_init_io(&m->memcjr_mem, OBJECT(machine),
                              &lc475_regbank_ops, &memcjr_bank,
                              "memcjr", sizeof(m->memcjr_regs));
        memory_region_add_subregion(&m->macio, MEMCJR_OFS, &m->memcjr_mem);

        primetime_bank = (LC475RegBank){ "primetime", m->primetime_regs,
                                         sizeof(m->primetime_regs), 4000 };
        memory_region_init_io(&m->primetime_mem, OBJECT(machine),
                              &lc475_regbank_ops, &primetime_bank,
                              "primetime", sizeof(m->primetime_regs));
        memory_region_add_subregion(&m->macio, PRIMETIME_OFS,
                                    &m->primetime_mem);
    }

    /* catch-all trace region behind the devices */
    memory_region_init_io(&m->iotrace, OBJECT(machine), &lc475_iotrace_ops,
                          m, "mac-io.trace", IO_SLICE);
    memory_region_add_subregion_overlap(&m->macio, 0, &m->iotrace, -1);

    /* VIA1 fronting the Cuda MCU */
    object_initialize_child(OBJECT(machine), "via1", &m->via1,
                            TYPE_MOS6522_LC475);
    qdev_prop_set_uint64(DEVICE(&m->via1), "frequency", VIA_TIMER_FREQ);
    sysbus = SYS_BUS_DEVICE(&m->via1);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_init_io(&m->via1mem, OBJECT(machine), &lc475_via1_ops,
                          m, "via1", 0x2000);
    memory_region_add_subregion(&m->macio, VIA1_OFS, &m->via1mem);
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), GLUE_IRQ_IN_VIA1));
    {
        struct tm tm;

        qemu_get_timedate(&tm, 0);
        m->via1.tick_offset = (uint32_t)mktimegm(&tm) + RTC_OFFSET;
    }
    /*
     * Seed XPRAM with the validity signature ('NuMc' at 0x0C) and the
     * 32-bit-addressing flag so the OS does not rebuild an "invalid"
     * PRAM from scratch on every boot.
     */
    /*
     * Leave PRAM invalid (no 'NuMc' signature): the ROM then rebuilds it
     * with proper defaults, including the OSDefault byte (XPRAM 0xF9)
     * that the boot scan matches against the disk's DDM ddType — seeding
     * a "valid" but zeroed PRAM makes the ROM search for ddType 0 and
     * reject every bootable disk.
     */
    m->via1.machine = m;
    m->cuda_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lc475_cuda_timer_cb, m);

    /* ADB devices behind the Cuda */
    adb_bus = qdev_get_child_bus(DEVICE(&m->via1), "adb.0");
    dev = qdev_new(TYPE_ADB_KEYBOARD);
    qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    dev = qdev_new(TYPE_ADB_MOUSE);
    qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    adb_register_autopoll_callback(&m->via1.adb_bus, lc475_cuda_adb_poll, m);

    m->sixty_hz_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lc475_sixty_hz, m);
    timer_mod(m->sixty_hz_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VIA_60HZ_TIMER_PERIOD_NS);
    m->one_second_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, lc475_one_second,
                                       m);
    timer_mod(m->one_second_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);

    /* VIA 2 */
    object_initialize_child(OBJECT(machine), "via2", &m->via2,
                            TYPE_MOS6522_Q800_VIA2);
    sysbus = SYS_BUS_DEVICE(&m->via2);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, VIA2_OFS,
                                sysbus_mmio_get_region(sysbus, 1));
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), GLUE_IRQ_IN_VIA2));

    /* SCC */
    object_initialize_child(OBJECT(machine), "escc", &m->escc, TYPE_ESCC);
    dev = DEVICE(&m->escc);
    qdev_prop_set_uint32(dev, "disabled", 0);
    qdev_prop_set_uint32(dev, "frequency", MAC_CLOCK);
    qdev_prop_set_uint32(dev, "it_shift", 1);
    qdev_prop_set_bit(dev, "bit_swap", true);
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));
    qdev_prop_set_uint32(dev, "chnBtype", 0);
    qdev_prop_set_uint32(dev, "chnAtype", 0);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize(sysbus, &error_fatal);

    object_initialize_child(OBJECT(machine), "escc_orgate", &m->escc_orgate,
                            TYPE_OR_IRQ);
    object_property_set_int(OBJECT(&m->escc_orgate), "num-lines", 2,
                            &error_fatal);
    dev = DEVICE(&m->escc_orgate);
    qdev_realize(dev, NULL, &error_fatal);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(dev, 0));
    sysbus_connect_irq(sysbus, 1, qdev_get_gpio_in(dev, 1));
    qdev_connect_gpio_out(dev, 0,
                          qdev_get_gpio_in(DEVICE(&m->glue),
                                           GLUE_IRQ_IN_ESCC));
    memory_region_add_subregion(&m->macio, SCC_OFS,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->escc),
                                                       0));

    /* ESCC alias 0x20 below, as on q800 (used by some drivers) */
    memory_region_init_alias(&m->escc_alias, OBJECT(machine), "escc-alias",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(&m->escc),
                                                    0), 0, 0x8);
    memory_region_add_subregion(&m->macio, SCC_OFS - 0x20, &m->escc_alias);

    /* SCSI: 53C96 ESP with VIA2-DRQ pseudo-DMA */
    object_initialize_child(OBJECT(machine), "esp", &m->esp,
                            TYPE_SYSBUS_ESP);
    esp = &m->esp.esp;
    esp->dma_memory_read = NULL;
    esp->dma_memory_write = NULL;
    esp->dma_opaque = NULL;
    m->esp.it_shift = 4;
    esp->dma_enabled = 1;

    sysbus = SYS_BUS_DEVICE(&m->esp);
    sysbus_realize(sysbus, &error_fatal);
    /* SCSI and SCSI data IRQs are negative edge triggered */
    sysbus_connect_irq(sysbus, 0,
                       qemu_irq_invert(
                           qdev_get_gpio_in(DEVICE(&m->via2),
                                            VIA2_IRQ_SCSI_BIT)));
    sysbus_connect_irq(sysbus, 1,
                       qemu_irq_invert(
                           qdev_get_gpio_in(DEVICE(&m->via2),
                                            VIA2_IRQ_SCSI_DATA_BIT)));
    memory_region_add_subregion(&m->macio, ESP_OFS,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_add_subregion(&m->macio, ESP_PDMA_OFS,
                                sysbus_mmio_get_region(sysbus, 1));

    scsi_bus_legacy_handle_cmdline(&esp->bus);

    /* EASC sound */
    object_initialize_child(OBJECT(machine), "asc", &m->asc, TYPE_ASC);
    qdev_prop_set_uint8(DEVICE(&m->asc), "asctype", ASC_TYPE_EASC);
    if (machine->audiodev) {
        qdev_prop_set_string(DEVICE(&m->asc), "audiodev", machine->audiodev);
    }
    sysbus = SYS_BUS_DEVICE(&m->asc);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, ASC_OFS,
                                sysbus_mmio_get_region(sysbus, 0));
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(DEVICE(&m->glue),
                                                   GLUE_IRQ_IN_ASC));
    /* ASC IRQ routed via GLUE to VIA2 in classic mode */
    qdev_connect_gpio_out(DEVICE(&m->glue), GLUE_IRQ_ASC,
                          qdev_get_gpio_in(DEVICE(&m->via2),
                                           VIA2_IRQ_ASC_BIT));
    /* keep the other GLUE output (SONIC->nubus9) connected but unused */
    qdev_connect_gpio_out(DEVICE(&m->glue), GLUE_IRQ_NUBUS_9,
                          qdev_get_gpio_in_named(DEVICE(&m->via2), "nubus-irq",
                                                 VIA2_NUBUS_IRQ_9));

    /* SWIM II floppy */
    object_initialize_child(OBJECT(machine), "swim", &m->swim, TYPE_SWIM);
    sysbus = SYS_BUS_DEVICE(&m->swim);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, SWIM_OFS,
                                sysbus_mmio_get_region(sysbus, 0));

    /* Valkyrie/CSC-class video control registers */
    {
        static LC475RegBank valkyrie_bank;

        valkyrie_bank = (LC475RegBank){ "valkyrie", m->valkyrie_regs,
                                        sizeof(m->valkyrie_regs), 8000 };
        m->valkyrie_bank = &valkyrie_bank;
        /*
         * Monitor sense at +0x122 (word): read once by the video driver.
         * 0 selects the 21" 1152x870 mode; Apple sense code 6 = 13"
         * 640x480 RGB.
         */
        m->valkyrie_regs[0x123] = 0x06;
        memory_region_init_io(&m->valkyrie_mem, OBJECT(machine),
                              &lc475_valkyrie_ops, m,
                              "valkyrie", sizeof(m->valkyrie_regs));
        memory_region_add_subregion(get_system_memory(), 0xf9800000,
                                    &m->valkyrie_mem);
    }

    /* Onboard video framebuffer at its physical decode */
    object_initialize_child(OBJECT(machine), "fb", &m->fb, TYPE_LC475_FB);
    sysbus = SYS_BUS_DEVICE(&m->fb);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(get_system_memory(), LC475_VRAM_BASE,
                                sysbus_mmio_get_region(sysbus, 0));
    /*
     * The VRAM aperture doesn't decode the upper address bits: the ROM's
     * video driver sizes VRAM by looking for aliasing (a BERR there is
     * fatal), so wrap the VRAM through the whole 8 MiB window below the
     * control registers.
     */
    {
        int i;

        for (i = 0; i < ARRAY_SIZE(m->vram_aliases); i++) {
            memory_region_init_alias(&m->vram_aliases[i], NULL,
                                     "lc475.vram-alias",
                                     sysbus_mmio_get_region(sysbus, 0),
                                     0, LC475_VRAM_SIZE);
            memory_region_add_subregion(get_system_memory(),
                                        LC475_VRAM_BASE +
                                        (i + 1) * LC475_VRAM_SIZE,
                                        &m->vram_aliases[i]);
        }
    }

    /* ROM */
    memory_region_init_rom(&m->rom, NULL, "lc475.rom", LC475_ROM_SIZE,
                           &error_abort);
    memory_region_add_subregion(get_system_memory(), LC475_ROM_ADDR, &m->rom);

    memory_region_init_alias(&m->rom_alias, NULL, "lc475.rom-alias",
                             &m->rom, 0, LC475_ROM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x40000000,
                                &m->rom_alias);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, LC475_ROM_ADDR,
                                        LC475_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }

    if (!qtest_enabled()) {
        if (bios_size <= 0 || bios_size > LC475_ROM_SIZE) {
            error_report("could not load MacROM '%s'", bios_name);
            exit(1);
        }

        cs = CPU(&m->cpu);
        ptr = rom_ptr(LC475_ROM_ADDR, bios_size);
        assert(ptr != NULL);
        stl_phys(cs->as, 0, ldl_be_p(ptr));    /* reset initial SP */
        stl_phys(cs->as, 4,
                 LC475_ROM_ADDR + ldl_be_p(ptr + 4)); /* reset initial PC */
    }
}

static GlobalProperty hw_compat_lc475[] = {
    { "scsi-hd", "quirk_mode_page_vendor_specific_apple", "on" },
    { "scsi-hd", "vendor", " SEAGATE" },
    { "scsi-hd", "product", "          ST225N" },
    { "scsi-hd", "ver", "1.0 " },
    { "scsi-cd", "quirk_mode_page_apple_vendor", "on" },
    { "scsi-cd", "quirk_mode_sense_rom_use_dbd", "on" },
    { "scsi-cd", "quirk_mode_page_vendor_specific_apple", "on" },
    { "scsi-cd", "quirk_mode_page_truncated", "on" },
    { "scsi-cd", "vendor", "MATSHITA" },
    { "scsi-cd", "product", "CD-ROM CR-8005" },
    { "scsi-cd", "ver", "1.0k" },
};
static const size_t hw_compat_lc475_len = G_N_ELEMENTS(hw_compat_lc475);

static void lc475_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68040"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Macintosh LC 475 / Quadra 605";
    mc->init = lc475_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->block_default_type = IF_SCSI;
    mc->default_ram_size = 32 * MiB;
    mc->default_ram_id = "lc475.ram";
    machine_add_audiodev_property(mc);
    compat_props_add(mc->compat_props, hw_compat_lc475, hw_compat_lc475_len);
}

static const TypeInfo lc475_machine_typeinfo[] = {
    {
        .name       = TYPE_MOS6522_LC475,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522Lc475State),
        .instance_init = mos6522_lc475_init,
        .class_init = mos6522_lc475_class_init,
    },
    {
        .name       = TYPE_LC475_FB,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(LC475FbState),
        .class_init = lc475_fb_class_init,
    },
    {
        .name       = TYPE_LC475_MACHINE,
        .parent     = TYPE_MACHINE,
        .instance_size = sizeof(LC475MachineState),
        .class_init = lc475_machine_class_init,
    },
};

DEFINE_TYPES(lc475_machine_typeinfo)
