/*
 * QEMU Macintosh Quadra 630 / LC 630 / Performa 630 hardware system emulator
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
#include "hw/ide/mmio.h"
#include "system/blockdev.h"
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

#define Q630_ROM_ADDR        0x40800000
#define Q630_ROM_SIZE        0x00100000
#define Q630_ROM_FILENAME    "quadra630.rom"

#define IO_BASE               0x50000000
/*
 * The F108 I/O decode repeats every 128 KiB (the ROM's decoder-kind-13
 * probe checks that VIA1 aliases at +0x20000: 0x408030ea probes
 * VIA1+0x1c00+0x20000 and fails identification on a bus error there).
 */
#define IO_SLICE              0x00020000
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
#define IDE_OFS               0x1a000
#define SINGER_OFS            0x04000

#define MAC_CLOCK             3686418

/* Size of whole RAM area (unbacked reads return 0 for RAM sizing) */
#define RAM_SIZE              0x40000000

/* Onboard video: physical VRAM base the ROM's page tables resolve to */
#define Q630_VRAM_BASE       0xf9000000
#define Q630_VRAM_SIZE       0x00100000    /* 1 MiB */

#define VIA_TIMER_FREQ        783360
#define VIA_60HZ_TIMER_PERIOD_NS   16625800

/*
 * Machine ID register at 0x5FFFFFFC: read-only, upper word must be 0xA55A.
 * The Q630 ROM (06684214) identifies the box purely by the LOW word: its
 * universal-table entry list (ROM+0xA79BC) matches the id word against
 * entry+0x44 (kind-13 family: 0x2224=box 0x57 Quadra 630, 0x2225=0x58,
 * 0x2226=0x59, 0x2231=0x5A, 0x2232=0x5B, 0x225x=boxes 0x5C/0x5D = the
 * LC/Performa 580 group), and bit 11 of the id word is CLEAR for these so
 * the VIA1 port A strap compare is skipped entirely (all masks are 0).
 */
#define Q630_MACHINE_ID      0xa55a2224

/*
 * VIA1 port A board straps, read by the ROM's machine identification with
 * DDRA forced to input: the box is selected by (PA & 0x56).  In the
 * Quadra 605 ROM's decoder table:
 *   0x42 -> box 0x34 (LC 475),   0x56/0x16 -> box 0x35 (P475 w/FPU),
 *   0x50 -> box 0x39 (Quadra 605), ...
 * Bits 1,2 low and 4,6 high give 0x50 = Quadra 605.
 */
#define Q630_VIA1_PINS_A     0xf9


/*
 * VIA1 subclass: on the Q630 the VIA1 fronts the Cuda MCU.  The Cuda
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

/* Cuda pseudo commands used by the Q630 ROM */
#define CUDA_CMD_AUTOPOLL       0x01
#define CUDA_CMD_GET_TIME       0x03
#define CUDA_CMD_GET_PRAM       0x07
#define CUDA_CMD_SET_TIME       0x09
#define CUDA_CMD_SET_PRAM       0x0c
#define CUDA_CMD_GET_SET_IIC    0x22

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

#define TYPE_MOS6522_Q630 "mos6522-q630"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522Q630State, MOS6522_Q630)

struct Q630MachineState;

struct MOS6522Q630State {
    MOS6522State parent_obj;

    struct Q630MachineState *machine;
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

static int q630_cuda_log_budget = 6000;

#define q630_cuda_log(...) do {                    \
        if (q630_cuda_log_budget > 0) {            \
            q630_cuda_log_budget--;                \
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

static void via1_rtc_update(MOS6522Q630State *v1s)
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
                q630_cuda_log("q630 rtc: read pram 0x%02x -> 0x%02x\n",
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
                q630_cuda_log("q630 rtc: read xpram 0x%02x -> 0x%02x\n",
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

static void q630_cuda_session_update(MOS6522Q630State *v1s);
static void q630_cuda_sr_written(MOS6522Q630State *v1s);
static void q630_cuda_acr_changed(MOS6522Q630State *v1s);
static void q630_cuda_idle_treq(MOS6522Q630State *v1s);

static void q630_via1_portA_write(MOS6522State *s)
{
}

static void q630_via1_portB_write(MOS6522State *s)
{
    MOS6522Q630State *v1s = MOS6522_Q630(s);

    if (v1s->machine) {
        q630_cuda_session_update(v1s);
        q630_cuda_idle_treq(v1s);
    }
}

static void mos6522_q630_init(Object *obj)
{
    MOS6522Q630State *v1s = MOS6522_Q630(obj);

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;

    /* the ADB bus hangs off the Cuda; the VIA1 device stands in for it */
    qbus_init(&v1s->adb_bus, sizeof(v1s->adb_bus), TYPE_ADB_BUS,
              DEVICE(obj), "adb.0");
}

static void mos6522_q630_reset_hold(Object *obj, ResetType type)
{
    MOS6522Q630State *v1s = MOS6522_Q630(obj);
    MOS6522State *ms = MOS6522(v1s);
    MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(ms);

    if (mdc->parent_phases.hold) {
        mdc->parent_phases.hold(obj, type);
    }

    /* board-specific idle state of the input pins (TREQ deasserted) */
    ms->a = Q630_VIA1_PINS_A;
    ms->b = 0xff;

    v1s->cmd = REG_EMPTY;
    v1s->alt = REG_EMPTY;
    v1s->data_out_cnt = 0;
    v1s->data_in_cnt = 0;
}

static void mos6522_q630_class_init(ObjectClass *klass, const void *data)
{
    MOS6522DeviceClass *mdc = MOS6522_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    mdc->portA_write = q630_via1_portA_write;
    mdc->portB_write = q630_via1_portB_write;
    resettable_class_set_parent_phases(rc, NULL, mos6522_q630_reset_hold,
                                       NULL, &mdc->parent_phases);
}

/*
 * Onboard video framebuffer display device (Valkyrie/CSC-class).  Starts
 * life as a fixed 640x480 mono view of the VRAM; grown as the ROM's
 * video driver behaviour is discovered.
 */

#define TYPE_Q630_FB "q630-fb"
OBJECT_DECLARE_SIMPLE_TYPE(Q630FbState, Q630_FB)

struct Q630FbState {
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

static int q630_fb_stride(Q630FbState *s)
{
    return s->stride;
}

static void q630_fb_draw_line(void *opaque, uint8_t *d, const uint8_t *src,
                               int width, int pitch)
{
    Q630FbState *s = opaque;
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
static void q630_fb_track_mode(Q630FbState *s)
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

    /*
     * The PixMap baseAddr is either the physical VRAM window
     * (0xF90xxxxx, used by the ROM startup UI) or the Slot-Manager
     * logical window 0x519xxxxx that MacOS's page tables map onto the
     * VRAM (used once the ROM video driver publishes the real mode,
     * e.g. the 1152x870 8bpp desktop of the 32-bit boot).  Both reduce
     * to the same VRAM offset below.
     */
    if ((base >> 20) != (Q630_VRAM_BASE >> 20) && (base >> 20) != 0x519) {
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
    base &= Q630_VRAM_SIZE - 1;
    if (base + (uint64_t)rb * height > Q630_VRAM_SIZE) {
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

static bool q630_fb_update(void *opaque)
{
    Q630FbState *s = Q630_FB(opaque);
    DisplaySurface *surface;
    int first = 0, last = 0;

    q630_fb_track_mode(s);
    surface = qemu_console_surface(s->con);

    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection, &s->vram,
                                          s->fb_offset,
                                          s->height,
                                          q630_fb_stride(s));
        s->invalidate = 0;
    }

    framebuffer_update_display(surface, &s->fbsection,
                               s->width, s->height,
                               q630_fb_stride(s), s->width * 4,
                               0, 1, q630_fb_draw_line, s,
                               &first, &last);
    qemu_console_update(s->con, 0, 0, s->width, s->height);
    return true;
}

static void q630_fb_invalidate(void *opaque)
{
    Q630FbState *s = Q630_FB(opaque);

    s->invalidate = 1;
}

static const GraphicHwOps q630_fb_ops = {
    .invalidate = q630_fb_invalidate,
    .gfx_update = q630_fb_update,
};

static void q630_fb_realize(DeviceState *dev, Error **errp)
{
    Q630FbState *s = Q630_FB(dev);
    int i;

    memory_region_init_ram(&s->vram, OBJECT(dev), "q630.vram",
                           Q630_VRAM_SIZE, &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->vram);

    /*
     * Defaults until QuickDraw publishes the real geometry (see
     * q630_fb_track_mode): the Q630 ROM draws its startup UI as a
     * 1152x870 1bpp screen with rowbytes 576 at VRAM offset 0x80
     * (observed from the gray desktop pattern in VRAM).
     */
    s->depth = 1;
    s->width = 1152;
    s->height = 870;
    s->stride = 576;
    s->fb_offset = 0x80;
    /* default grayscale ramp until the CLUT is programmed */
    for (i = 0; i < 256; i++) {
        uint8_t v = 255 - i;
        s->palette[i] = 0xFF000000 | (v << 16) | (v << 8) | v;
    }
    s->invalidate = 1;
    s->con = qemu_graphic_console_create(dev, 0, &q630_fb_ops, s);
    qemu_console_resize(s->con, s->width, s->height);
}

static void q630_fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->realize = q630_fb_realize;
}

/* machine */

typedef struct {
    const char *name;
    uint8_t *regs;
    uint32_t size;
    int log_budget;
} Q630RegBank;

struct Q630MachineState {
    MachineState parent_obj;

    M68kCPU cpu;
    GLUEState glue;
    MOS6522Q630State via1;
    MOS6522Q800VIA2State via2;
    ESCCState escc;
    OrIRQState escc_orgate;
    ASCState asc;
    SysBusESPState esp;
    Swim swim;
    Q630FbState fb;

    /* F108 MMIO IDE (MAC_IDE_QUADRA) */
    DeviceState *ide;
    MemoryRegion ide_tf_alias;
    MemoryRegion ide_cs_alias;
    MemoryRegion ide_ifr_mem;
    uint8_t ide_ifr;
    bool ide_irq_level;

    MemoryRegion rom;
    MemoryRegion rom_alias;
    MemoryRegion rom_alias24;
    MemoryRegion machine_id;
    MemoryRegion via1mem;
    MemoryRegion memcjr_mem;
    MemoryRegion primetime_mem;
    uint8_t memcjr_regs[0x1000];
    uint8_t primetime_regs[0x2000];
    MemoryRegion singer_mem;
    uint8_t singer_regs[0x2000];
    MemoryRegion asc_fifoirq_mem;
    MemoryRegion valkyrie_mem;
    uint8_t valkyrie_regs[0x4000];
    Q630RegBank *valkyrie_bank;
    uint8_t clut_addr;
    uint8_t clut_phase;
    uint8_t clut_rgb[3];
    MemoryRegion vram_aliases[7];
    MemoryRegion ramio;
    MemoryRegion ram_alias24;
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
    /* current response is a Cuda-initiated (autopoll) packet */
    bool cuda_unsol;
    /*
     * Early-ROM polled sync exchange: session opened by asserting TACK
     * alone (TIP left high); the Cuda acks by asserting /TREQ for the
     * duration of the session (0x40885718..0x4088578a in ROM 06684214).
     */
    bool cuda_sync;

    /*
     * Cuda GET_SET_IIC (pseudo 0x22) backing store: MacOS's Cuda driver
     * write-then-read-verifies I2C device registers (device 0x6F on the
     * Q630); a data-less generic ACK makes it retry forever, and the
     * permanently-pending request starves unsolicited ADB input.
     */
    uint8_t iic_regs[128][256];
    uint8_t iic_last_reg[128];

    /* SETUPTIMEK calibration hack (see mac_via.c) */
    int timer_hack_state;
};

#define TYPE_Q630_MACHINE MACHINE_TYPE_NAME("quadra630")
OBJECT_DECLARE_SIMPLE_TYPE(Q630MachineState, Q630_MACHINE)

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static uint32_t q630_trace_pc(void)
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
    hwaddr orig = addr;

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

    if (r != MEMTX_OK) {
        qemu_log_mask(LOG_UNIMP,
                      "q630 io-alias: read 0x%08x (%d) failed pc=0x%08x\n",
                      (unsigned)(IO_BASE + IO_SLICE + orig), size,
                      q630_trace_pc());
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

static MemTxResult q630_iotrace_read(void *opaque, hwaddr addr,
                                      uint64_t *data, unsigned size,
                                      MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q630 io: read  +0x%05x (%d) -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, q630_trace_pc());
    }
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult q630_iotrace_write(void *opaque, hwaddr addr,
                                       uint64_t val, unsigned size,
                                       MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q630 io: write +0x%05x (%d) <- 0x%08" PRIx64
                      " -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, val, q630_trace_pc());
    }
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps q630_iotrace_ops = {
    .read_with_attrs = q630_iotrace_read,
    .write_with_attrs = q630_iotrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* background catch-all outside the I/O slice: log + BERR */

static MemTxResult q630_bgtrace_read(void *opaque, hwaddr addr,
                                      uint64_t *data, unsigned size,
                                      MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q630 bus: read  0x%08x (%d) -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, q630_trace_pc());
    }
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult q630_bgtrace_write(void *opaque, hwaddr addr,
                                       uint64_t val, unsigned size,
                                       MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q630 bus: write 0x%08x (%d) <- 0x%08" PRIx64
                      " -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, val, q630_trace_pc());
    }
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps q630_bgtrace_ops = {
    .read_with_attrs = q630_bgtrace_read,
    .write_with_attrs = q630_bgtrace_write,
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

static uint64_t q630_regbank_read(void *opaque, hwaddr addr, unsigned size)
{
    Q630RegBank *b = opaque;
    uint64_t val = 0;
    int i;

    for (i = 0; i < size; i++) {
        val = (val << 8) | b->regs[(addr + i) & (b->size - 1)];
    }
    if (b->log_budget > 0) {
        b->log_budget--;
        qemu_log_mask(LOG_UNIMP, "q630 %s: read  +0x%04x (%d) -> 0x%08"
                      PRIx64 " pc=0x%08x\n", b->name, (unsigned)addr, size,
                      val, q630_trace_pc());
    }
    return val;
}

static void q630_regbank_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    Q630RegBank *b = opaque;
    int i;

    if (b->log_budget > 0) {
        b->log_budget--;
        qemu_log_mask(LOG_UNIMP, "q630 %s: write +0x%04x (%d) <- 0x%08"
                      PRIx64 " pc=0x%08x\n", b->name, (unsigned)addr, size,
                      val, q630_trace_pc());
    }
    for (i = size - 1; i >= 0; i--) {
        b->regs[(addr + i) & (b->size - 1)] = val & 0xff;
        val >>= 8;
    }
}

static const MemoryRegionOps q630_regbank_ops = {
    .read = q630_regbank_read,
    .write = q630_regbank_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* single machine instance, for small register hooks */
static Q630MachineState *q630_machine;

/*
 * PrimeTime II address-map config at 0x50F18200: after the RAM test the
 * ROM writes 0x1B here (right after storing the addressing-mode PRAM
 * byte 0x8A), which opens the 24-bit-compatibility ROM window at
 * physical 0x00800000 — the ROM's page tables then identity-map logical
 * 0x00800000-0x008FFFFF onto it (lowmem ROMBase becomes 0x00800000 and
 * all Device Manager ROM-driver pointers are based on it).  The window
 * must be CLOSED during the RAM test (that range is tested as RAM).
 */

static void q630_primetime_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    if (q630_machine && addr <= 0x200 && addr + size > 0x200) {
        uint8_t v = val >> ((addr + size - 1 - 0x200) * 8);

        memory_region_set_enabled(&q630_machine->rom_alias24, v != 0);
    }
    q630_regbank_write(opaque, addr, val, size);
}

static const MemoryRegionOps q630_primetime_ops = {
    .read = q630_regbank_read,
    .write = q630_primetime_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * Singer/DFAC-class sound codec mailbox at 0x50F04000: generic regbank
 * plus a "done/data ready" latch on bit 0 of the command/status byte at
 * +2.  The ROM's sound driver has two flows sharing the bit:
 *  - command: cmd byte -> +2, data -> +6, strobe (write 1) -> +2, then
 *    busy-wait for +2 bit0 SET (0x408b9fe4..0x408b9ff2);
 *  - idle service: if +2 bit0 is set, ack (write 1 -> +2), check error
 *    bits &0x70, consume the data byte at +6 (0x408ba0f4..0x408ba114).
 * So: the strobe completes instantly (latch set), and READING +6
 * clears the latch — otherwise the idle service loops forever eating
 * phantom data.
 */

static bool q630_singer_done;

static uint64_t q630_singer_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t val = q630_regbank_read(opaque, addr, size);

    if (addr <= 2 && addr + size > 2) {
        uint64_t bit = (uint64_t)1 << ((addr + size - 1 - 2) * 8);

        val = q630_singer_done ? (val | bit) : (val & ~bit);
    }
    if (addr <= 6 && addr + size > 6) {
        q630_singer_done = false;
    }
    return val;
}

static void q630_singer_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    if (addr <= 2 && addr + size > 2
        && ((val >> ((addr + size - 1 - 2) * 8)) & 0xff) == 1) {
        /* strobe: the command "completes" immediately */
        q630_singer_done = true;
    }
    q630_regbank_write(opaque, addr, val, size);
}

static const MemoryRegionOps q630_singer_ops = {
    .read = q630_singer_read,
    .write = q630_singer_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * ASC FIFO IRQ status shim (reg +0x804): the ROM's boot chime feeds the
 * FIFOs and busy-waits on the half-empty status bits with a tight
 * timeout (0x40845de0..0x40845e08); the real drain is paced by the
 * audio backend in host real time and always loses that race under TCG,
 * upon which the ROM declares the sound hardware broken and drops into
 * the death monitor.  Reading the status here instantly "plays" the
 * buffered samples: drain to just under half (setting the half-empty
 * bit), then to empty on the next read.
 */

static uint64_t q630_asc_fifoirq_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    Q630MachineState *m = opaque;
    uint8_t val = 0;
    int i;

    for (i = 0; i < 2; i++) {
        ASCFIFOState *fs = &m->asc.fifos[i];

        if (fs->cnt > 0x1ff) {
            fs->rptr = (fs->rptr + (fs->cnt - 0x1ff)) & 0x3ff;
            fs->cnt = 0x1ff;
            fs->int_status |= 0x1;              /* half empty */
        } else if (fs->cnt > 0) {
            fs->rptr = (fs->rptr + fs->cnt) & 0x3ff;
            fs->cnt = 0;
            fs->int_status |= 0x3;              /* half empty + empty */
        } else {
            /* engine running with no data: both bits assert each cycle */
            fs->int_status |= 0x3;
        }
        val |= (fs->int_status & 0x3) << (i * 2);
        fs->int_status = 0;
    }
    return val;
}

static void q630_asc_fifoirq_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
}

static const MemoryRegionOps q630_asc_fifoirq_ops = {
    .read = q630_asc_fifoirq_read,
    .write = q630_asc_fifoirq_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
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
    return Q630_MACHINE_ID & ((size == 4) ? 0xffffffff :
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

static void q630_cuda_set_treq(MOS6522Q630State *v1s, bool assert)
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
static void q630_cuda_idle_treq(MOS6522Q630State *v1s)
{
    Q630MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    /*
     * Only when the host has really released the bus: the end-of-response
     * toggle still has one of TIP/TACK low and /TREQ must stay asserted
     * through the final interrupt (the end marker).
     */
    if (!m->cuda_session && m->cuda_resp_len == 0
        && (s->b & (CUDA_TIP | CUDA_TACK)) == (CUDA_TIP | CUDA_TACK)) {
        q630_cuda_set_treq(v1s, false);
    }
}

static void q630_cuda_schedule_int(Q630MachineState *m)
{
    timer_mod(m->cuda_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100 * 1000);
}

static void q630_cuda_timer_cb(void *opaque)
{
    Q630MachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), SR_INT_BIT);

    qemu_set_irq(irq, 1);
}

/*
 * Keep the ROM's in-RAM XPRAM mirror coherent for the addressing-mode
 * byte 0x8A.  The ROM latches that byte into lowmem 0x1EFC via
 * _ReadXPRam (0x4080cc60), which is served from a 256-byte cache at
 * [lowmem 0xDE0]+102 — but the cache is initialised from a ROM default
 * template (0x8A = 0, "24-bit"), the ROM's own direct Cuda reads of
 * 0x8A never update it, and the cache's Cuda sync only reaches 0x8A
 * *after* the [0x644] checkpoint at 0x408001e0 has latched the stale
 * template value, leaving the fatal 24-bit Translate24To32 vector
 * installed (see CUDA_CMD_GET_PRAM below).  Refresh the cached copy on
 * every PRAM exchange once the driver struct exists, so the refresh
 * immediately before the checkpoint sees the true value — as it would
 * on a machine whose previous boot had written it back.
 */
static void q630_cuda_sync_xpram_cache(MOS6522Q630State *v1s)
{
    hwaddr xpram_cache = ldl_be_phys(&address_space_memory, 0xde0);

    if (xpram_cache >= 0x1000 && xpram_cache < 0x10000000 - 0x200 &&
        !(xpram_cache & 1)) {
        stb_phys(&address_space_memory, xpram_cache + 102 + 0x8a,
                 v1s->PRAM[0x8a] | 0x05);
        /*
         * Same template-wipe problem hits the boot-scan OSDefault: this
         * ROM's boot-disk scan (0x40801350) latches its expected DDM
         * ddType ONCE, before the flashing-"?" loop, from OS trap A084
         * (handler 0x408013b0) = _ReadXPRam 2 bytes at XPRAM 0x76 — the
         * expected ddType is the LOW byte, XPRAM 0x77 (this ROM's
         * offset; the Q605/lc475 ROM used 0xF8-0xFB for the same idea,
         * see LC475-NOTES #13).  The wiped-to-0 cache makes the scan
         * demand ddType 0 and reject every bootable disk (infinite
         * lba-0 re-reads, flashing "?").  Refresh from the Cuda PRAM,
         * forcing the ddType byte to at least 1 (= MacOS) so the scan
         * accepts real disks (ddType 1) even if PRAM was wiped.
         */
        stb_phys(&address_space_memory, xpram_cache + 102 + 0x76,
                 v1s->PRAM[0x76]);
        stb_phys(&address_space_memory, xpram_cache + 102 + 0x77,
                 v1s->PRAM[0x77] ? v1s->PRAM[0x77] : 1);
    }
}

static void q630_cuda_process(Q630MachineState *m)
{
    ADBBusState *adb_bus = &m->via1.adb_bus;
    MOS6522Q630State *v1s = &m->via1;
    uint8_t *c = m->cuda_cmd;
    uint8_t *r = m->cuda_resp;
    int n = m->cuda_cmd_len;
    uint32_t now, val;

    m->cuda_resp_len = 0;
    m->cuda_resp_idx = 0;
    m->cuda_unsol = false;
    m->cuda_no_resp = false;

    q630_cuda_log(
                  "q630 cuda: cmd len=%d [%02x %02x %02x %02x]\n",
                  n, n > 0 ? c[0] : 0, n > 1 ? c[1] : 0, n > 2 ? c[2] : 0,
                  n > 3 ? c[3] : 0);

    if (n == 0) {
        return;
    }

    /*
     * REAL Cuda packet framing (unlike the Q605's Egret-style raw ADB
     * bytes): byte 0 is the packet type, responses carry a 3 byte
     * header [type, flags, cmd] followed by data.
     */
    switch (c[0]) {
    case CUDA_PKT_ADB:
        if (n < 2) {
            /* bare [00]: ADB SendReset sync from the early ROM probe */
            uint8_t scratch[ADB_MAX_OUT_LEN];
            uint8_t rst = 0x00;

            adb_autopoll_block(adb_bus);
            adb_request(adb_bus, scratch, &rst, 1);
            adb_autopoll_unblock(adb_bus);
            r[0] = CUDA_PKT_ADB;
            r[1] = 0x00;
            r[2] = 0x00;
            m->cuda_resp_len = 3;
            break;
        }
        {
            uint8_t obuf[ADB_MAX_OUT_LEN];
            int olen;

            adb_autopoll_block(adb_bus);
            olen = adb_request(adb_bus, obuf, c + 1, n - 1);
            adb_autopoll_unblock(adb_bus);

            r[0] = CUDA_PKT_ADB;
            r[2] = c[1];
            if (olen > 0) {
                r[1] = 0x00;
                memcpy(r + 3, obuf, olen);
                m->cuda_resp_len = 3 + olen;
            } else {
                /*
                 * No data.  Only a TALK ((cmd & 0xC) == 0xC) with no
                 * response is an ADB timeout (flag 0x02) — that is how
                 * enumeration detects absent devices.  Listen / Flush /
                 * SendReset have no reply by design and must ACK with
                 * flags 0: MacOS's Cuda ADB Manager treats a "timeout"
                 * on its Flush/Listen-R3 setup commands as a dead
                 * device and then discards all autopolled input from
                 * it (keyboard/mouse dead at the Finder despite
                 * delivered packets).
                 */
                r[1] = (n >= 2 && (c[1] & 0x0c) == 0x0c) ? 0x02 : 0x00;
                m->cuda_resp_len = 3;
            }
        }
        break;

    case CUDA_PKT_PSEUDO:
        /* generic ACK header; commands append data / have side effects */
        r[0] = CUDA_PKT_PSEUDO;
        r[1] = 0x00;
        r[2] = n > 1 ? c[1] : 0;
        m->cuda_resp_len = 3;
        if (n < 2) {
            break;
        }
        switch (c[1]) {
        case CUDA_CMD_AUTOPOLL:
            if (n >= 3) {
                adb_set_autopoll_enabled(adb_bus, c[2] != 0);
            }
            break;
        case CUDA_CMD_GET_TIME:
            now = v1s->tick_offset +
                  (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                   / NANOSECONDS_PER_SECOND);
            r[3] = now >> 24;
            r[4] = now >> 16;
            r[5] = now >> 8;
            r[6] = now;
            m->cuda_resp_len = 7;
            break;
        case CUDA_CMD_SET_TIME:
            if (n >= 6) {
                val = (c[2] << 24) | (c[3] << 16) | (c[4] << 8) | c[5];
                v1s->tick_offset = val -
                    (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                     / NANOSECONDS_PER_SECOND);
            }
            break;
        case CUDA_CMD_GET_PRAM:
            if (n >= 4) {
                uint8_t pram_addr = ((c[2] << 8) | c[3]) & 0xff;

                r[3] = v1s->PRAM[pram_addr];
                /*
                 * XPRAM 0x8A is the MacOS addressing-mode byte (bit 0 =
                 * "boot 32-bit", bit 2 = "32-bit desired"; 0x05 = 32-bit).
                 * The Q630 ROM is 32-bit-ONLY (it hardcodes MMU32Bit
                 * lowmem 0xCB2 = 1 at 0x40803e14 with no PRAM input), but
                 * it still keys its pointer-translation vector [0x644] off
                 * this byte (read into lowmem 0x1EFC at 0x4080cc64, tested
                 * at 0x408001e0): bit 0 clear leaves [0x644] = the 24-bit
                 * Translate24To32 routine 0x4082F090, which QuickDraw's
                 * StdBits (0x40836B60) applies unconditionally to its two
                 * mask-region pointer params.  Any RAM pointer with bit 23
                 * set (8-16 MiB, e.g. the boot stack at BufPtr/2 on a
                 * 32 MiB machine) is then "translated" 0x00FFxxxx ->
                 * 0x500Fxxxx (the F108 I/O map) and the dereference is a
                 * guest MMU fault -> Sad Mac 0000000F/00000001 (dsBusError).
                 * A real Q630's battery-backed Cuda carries 0x8A = 0x05
                 * from the System's first boot; model that by forcing the
                 * 32-bit bits on reads regardless of wipes/rebuild order,
                 * so [0x644] becomes the 32-bit no-op (rts 0x4080047E) and
                 * pointers pass through unmodified, matching a real 32-bit
                 * boot.  (lc475 is different: its ROM derives the WHOLE
                 * mode from this byte, so 0 there means a consistent
                 * 24-bit boot with MemTop = 8 MiB and the translator is
                 * correct; forcing 0x05 is only right for 32-bit-only
                 * boxes like the 630.)
                 */
                if (pram_addr == 0x8a) {
                    r[3] |= 0x05;
                }
                q630_cuda_sync_xpram_cache(v1s);
                m->cuda_resp_len = 4;
                q630_cuda_log("q630 cuda: get pram 0x%02x -> 0x%02x\n",
                              c[3], r[3]);
            }
            break;
        case CUDA_CMD_SET_PRAM:
            if (n >= 5) {
                uint8_t pram_addr = ((c[2] << 8) | c[3]) & 0xff;
                int i;

                /*
                 * Real Cuda SET_PRAM writes a block of consecutive bytes;
                 * the OS's XPRAM sync uses 4-byte blocks (cmd len 8).
                 * Storing only the first byte silently dropped the rest.
                 */
                for (i = 0; i < n - 4; i++) {
                    v1s->PRAM[(pram_addr + i) & 0xff] = c[4 + i];
                }
                q630_cuda_sync_xpram_cache(v1s);
                q630_cuda_log("q630 cuda: set pram 0x%02x <- 0x%02x (x%d)\n",
                              c[3], c[4], n - 4);
            }
            break;
        case CUDA_CMD_GET_SET_IIC:
            /*
             * [01 22 aa rr vv...]: aa = I2C bus address (bit 0 = read),
             * rr = register subaddress.  Writes latch into the backing
             * store; reads return the latched value so the driver's
             * write-then-verify cycles succeed (observed: device
             * 0xDE/0xDF regs 0x02/0x0D, and a bare probe of 0x41).
             */
            if (n >= 3) {
                uint8_t dev = (c[2] >> 1) & 0x7f;
                bool rd = c[2] & 1;
                uint8_t reg = n >= 4 ? c[3] : m->iic_last_reg[dev];

                if (rd) {
                    r[3] = m->iic_regs[dev][reg];
                    m->cuda_resp_len = 4;
                    q630_cuda_log("q630 cuda: iic rd %02x[%02x] -> %02x\n",
                                  c[2], reg, r[3]);
                } else if (n >= 5) {
                    int i;

                    for (i = 0; i < n - 4; i++) {
                        m->iic_regs[dev][(reg + i) & 0xff] = c[4 + i];
                    }
                    m->iic_last_reg[dev] = reg;
                    q630_cuda_log("q630 cuda: iic wr %02x[%02x] <- %02x (x%d)\n",
                                  c[2], reg, c[4], n - 4);
                }
            }
            break;
        default:
            /* unknown pseudo commands are generically acknowledged */
            break;
        }
        break;

    default:
        /* unknown packet type */
        r[0] = CUDA_PKT_ERROR;
        r[1] = 0x02;
        r[2] = c[0];
        m->cuda_resp_len = 3;
        break;
    }
}

/*
 * Cuda-initiated transfer: autopolled ADB data raises /TREQ while the
 * bus is idle; the host then opens a session and clocks the packet out
 * like a command response.
 */
static void q630_cuda_adb_poll(void *opaque)
{
    Q630MachineState *m = opaque;
    MOS6522Q630State *v1s = &m->via1;
    MOS6522State *s = MOS6522(v1s);
    uint8_t obuf[ADB_MAX_OUT_LEN + 3];
    int olen;

    /* only when no exchange is in progress and the shifter is inbound */
    if (m->cuda_session || m->cuda_resp_len > 0 || (s->acr & SR_OUT)) {
        return;
    }

    olen = adb_poll(&v1s->adb_bus, obuf + 2, v1s->adb_bus.autopoll_mask);
    if (olen <= 0) {
        return;
    }
    /* adb_poll tags the data with the Talk R0 command byte at obuf[2] */
    obuf[0] = CUDA_PKT_ADB;
    obuf[1] = 0x40;                     /* autopolled data flag */
    /*
     * Prepend a PAD byte: the ROM/OS int-driven Cuda driver
     * (0x408a9c3a store loop, dispatch at 0x408a9d34) stores the very
     * first SR byte of any Cuda-initiated session at buffer[0] and
     * parses the packet from buffer[1] on (type at [1], flags at [2],
     * cmd at [3]) — on real hardware that first read is the stale
     * shift-register content, not packet data.  Without the pad the
     * flags byte 0x40 lands where the driver expects the type and
     * every autopolled ADB packet is dropped at 0x408a9e4a (keyboard/
     * mouse dead at the Finder despite fully-read packets).  Host-
     * initiated responses get their pad naturally (the SR still holds
     * the last command echo when the driver opens the read session).
     */
    memcpy(m->cuda_resp + 1, obuf, olen + 2);
    m->cuda_resp[0] = 0x00;             /* pad, discarded by the driver */
    m->cuda_resp_len = olen + 3;
    m->cuda_resp_idx = 1;
    m->cuda_unsol = true;
    m->cuda_no_resp = false;
    m->cuda_session = true;             /* Cuda-initiated session */
    s->sr = m->cuda_resp[0];
    q630_cuda_set_treq(v1s, true);
    q630_cuda_schedule_int(m);
    q630_cuda_log(
                  "q630 cuda: unsol len=%d [%02x %02x %02x %02x]\n",
                  m->cuda_resp_len, obuf[0], obuf[1], obuf[2],
                  m->cuda_resp_len > 3 ? obuf[3] : 0);
}

static void q630_cuda_ack_toggle(MOS6522Q630State *v1s)
{
    Q630MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    if (m->cuda_resp_idx < m->cuda_resp_len) {
        s->sr = m->cuda_resp[m->cuda_resp_idx++];
        /*
         * Real Cuda semantics: /TREQ is LOW while the Cuda still has
         * response bytes to transfer (the ROM checks it right after
         * every byte-acknowledge toggle to decide whether to keep
         * reading, 0x408b3bee).
         *
         * For Cuda-INITIATED (unsolicited autopoll) packets, deassert
         * /TREQ already when the LAST byte is fed: the OS's int-driven
         * driver stores each SR byte and then checks PB (TIP|TACK|
         * /TREQ all high = 0x38 at 0x408a9cae) to detect end-of-packet
         * WITH the byte just read.  Deasserting only on the following
         * toggle makes the driver take one extra interrupt and STORE a
         * stale trailing byte, which promotes 2-byte classic ADB mouse
         * data to the 3-byte extended-mouse format and garbles the
         * deltas (dx inflated / sign lost).  Host-initiated responses
         * keep the late deassert, which the ROM's polled drivers
         * depend on.
         */
        q630_cuda_set_treq(v1s,
                           !(m->cuda_unsol &&
                             m->cuda_resp_idx == m->cuda_resp_len));
        q630_cuda_schedule_int(m);
        q630_cuda_log(
                      "q630 cuda: feed 0x%02x (#%d/%d) pc=%08x b=%02x\n",
                      s->sr, m->cuda_resp_idx, m->cuda_resp_len,
                      q630_trace_pc(), s->b);
    } else {
        /*
         * Toggle after the last byte was read: deassert /TREQ so the
         * post-read check sees end-of-response.  No interrupt here —
         * the host's session release generates the final one.
         */
        m->cuda_resp_len = 0;
        m->cuda_resp_idx = 0;
        m->cuda_unsol = false;
        /*
         * Keep the session marked open: the host still holds the
         * handshake lines and the subsequent release must be seen as
         * the session close (which schedules the final interrupt).
         */
        q630_cuda_set_treq(v1s, false);
        /*
         * The interrupt-driven driver (0x408a9bb4 cadence) advances its
         * state machine on an SR interrupt after this final toggle and
         * only then observes /TREQ deasserted; without it the request
         * completion flag never gets set (idle loop at 0x408a9b50).
         */
        q630_cuda_schedule_int(m);
        q630_cuda_log(
                      "q630 cuda: response complete pc=%08x b=%02x\n",
                      q630_trace_pc(), s->b);
    }
}

static void q630_cuda_session_update(MOS6522Q630State *v1s)
{
    Q630MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);
    bool sys = (s->b & (CUDA_TIP | CUDA_TACK)) != (CUDA_TIP | CUDA_TACK);
    uint8_t hs_change = (s->b ^ v1s->last_b) & (CUDA_TIP | CUDA_TACK);

    if (sys && !m->cuda_session) {
        m->cuda_session = true;
        m->cuda_sync = (s->b & CUDA_TIP) && !(s->b & CUDA_TACK);
        m->cuda_cmd_len = 0;
        m->cuda_resp_len = 0;
        m->cuda_resp_idx = 0;
        /* the ROM preloads the first byte before asserting the session */
        if (s->acr & SR_OUT) {
            q630_cuda_sr_written(v1s);
        }
        if (m->cuda_sync) {
            /* polled sync: /TREQ low is the session/byte acknowledge */
            q630_cuda_set_treq(v1s, true);
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
        q630_cuda_ack_toggle(v1s);
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
            q630_cuda_schedule_int(m);
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
            q630_cuda_process(m);
        }
        /*
         * The Q630 ROM's Cuda driver is POLLED (0x408aa040..0x408aa1a2):
         * after releasing the handshake lines at the end of any exchange
         * (the sync at 0x4088578a, and every send/receive session at
         * 0x408aa094/0x408aa098) it busy-waits on IFR bit 2 for one
         * final SR interrupt before proceeding.  Always schedule it.
         */
        q630_cuda_schedule_int(m);
        m->cuda_session = false;
        m->cuda_sync = false;
        m->cuda_cmd_len = 0;
        m->cuda_resp_len = 0;
        m->cuda_resp_idx = 0;
        m->cuda_no_resp = false;
        /* /TREQ returns to idle (deasserted) */
        q630_cuda_set_treq(v1s, false);
    }
}


static void q630_cuda_sr_written(MOS6522Q630State *v1s)
{
    Q630MachineState *m = v1s->machine;
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
     * EXCEPT in the early polled-sync exchange, where /TREQ low is
     * precisely the acknowledge the ROM busy-waits for.
     */
    q630_cuda_set_treq(v1s, m->cuda_sync);
    q630_cuda_log(
                  "q630 cuda: <- 0x%02x (#%d) pc=%08x b=%02x\n", s->sr,
                  m->cuda_cmd_len, q630_trace_pc(), s->b);
    q630_cuda_schedule_int(m);
}

static void q630_cuda_acr_changed(MOS6522Q630State *v1s)
{
    Q630MachineState *m = v1s->machine;
    MOS6522State *s = MOS6522(v1s);

    /*
     * Host turns the shifter to OUTPUT: it is starting a new command.
     * The ROM's polled driver routinely abandons a response after the
     * first header byte (e.g. the SET_PRAM acks during the PRAM
     * rebuild at 0x408b3a24): drop the stale staged bytes or the next
     * turnaround would mistake them for a Cuda-initiated packet and
     * discard the new command instead.
     */
    if ((s->acr & SR_OUT) && m->cuda_resp_len > 0) {
        m->cuda_resp_len = 0;
        m->cuda_resp_idx = 0;
        q630_cuda_set_treq(v1s, false);
    }

    /*
     * The host turns the shifter around to receive while still holding
     * the session: treat the bytes collected so far as the command and
     * build the response; the host's following handshake toggle clocks
     * the first byte out.  /TREQ stays LOW for a real response and is
     * RAISED for the no-response turnaround.
     */
    if (m->cuda_session && !(s->acr & SR_OUT) && m->cuda_resp_len == 0
        && m->cuda_cmd_len > 0) {
        q630_cuda_process(m);
        m->cuda_cmd_len = 0;
        /* real Cuda: /TREQ LOW = response data pending */
        q630_cuda_set_treq(v1s, m->cuda_resp_len > 0);
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

static uint64_t q630_via1_read(void *opaque, hwaddr addr, unsigned size)
{
    Q630MachineState *m = opaque;
    MOS6522State *ms = MOS6522(&m->via1);
    hwaddr reg = (addr >> 9) & 0xf;
    uint64_t val;

    val = mos6522_read(ms, reg, size);

    /* input pins on port A read the board straps, not the last output */
    if (reg == VIA_REG_A || reg == VIA_REG_ANH) {
        val = (val & ms->dira) | (Q630_VIA1_PINS_A & ~ms->dira);
    }

    return val;
}

static void q630_via1_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Q630MachineState *m = opaque;
    MOS6522Q630State *v1s = &m->via1;
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
                          "q630 via1: reg%d <- 0x%02" PRIx64 " pc=0x%08x\n",
                          (int)reg, val, q630_trace_pc());
        }
    }
    mos6522_write(ms, reg, val, size);

    if (reg == VIA_REG_SR && v1s->machine) {
        q630_cuda_sr_written(v1s);
    }
    if (reg == VIA_REG_ACR && v1s->machine) {
        q630_cuda_acr_changed(v1s);
    }
    if (reg == VIA_REG_B) {
        via1_rtc_update(v1s);
        v1s->last_b = ms->b;
    }
}

static const MemoryRegionOps q630_via1_ops = {
    .read = q630_via1_read,
    .write = q630_via1_write,
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

static void q630_ide_update_irq(Q630MachineState *m);

static void q630_valkyrie_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    Q630MachineState *m = Q630_MACHINE(opaque);
    Q630RegBank *b = m->valkyrie_bank;

    if (addr == 0x10c || (addr <= 0x10c && addr + size > 0x10c)) {
        /* VBL status clear (drops the slot interrupt line too) */
        m->valkyrie_regs[0x10b] &= ~0x04;
        q630_ide_update_irq(m);
        return;
    }
    if (addr <= 0x107 && addr + size > 0x104) {
        /* interrupt enable mask update: recompute the slot line */
        q630_regbank_write(b, addr, val, size);
        q630_ide_update_irq(m);
        return;
    }
    if (addr == 0x20) {
        /*
         * Depth register: the ROM writes 0/1/2/3/4 for 1/2/4/8/16 bpp
         * (plus transient values like 7 during the probe).  The Q630
         * ROM runs its startup UI at 1bpp, so follow the register until
         * QuickDraw publishes the real PixMap (q630_fb_track_mode).
         */
        unsigned code = val & 0xff;

        if (code <= 4) {
            int depth = 1 << code;

            if (m->fb.depth != depth) {
                m->fb.depth = depth;
                m->fb.invalidate = 1;
            }
        }
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
    q630_regbank_write(b, addr, val, size);
}

static uint64_t q630_valkyrie_read(void *opaque, hwaddr addr, unsigned size)
{
    Q630MachineState *m = Q630_MACHINE(opaque);

    /*
     * Monitor sense: word reads at +0x122 return the live sense lines.
     * Raw code 6 = 13" 640x480 RGB (raw != 7 so the driver never tries
     * the extended sense dance).
     */
    if (addr == 0x122 && size == 2) {
        return 6;
    }
    /*
     * Q630 Valkyrie monitor sense: the ROM's video driver does a
     * drive/read dance on +0x200/+0x220 and reads the sense lines
     * back from +0x220 (RAM-code pc 0x6e32..0x6e58).  Return Apple
     * sense code 6 = 13" 640x480 RGB (a plain code, so the extended
     * sense protocol is never attempted).
     */
    if (addr == 0x220) {
        return 6;
    }
    return q630_regbank_read(m->valkyrie_bank, addr, size);
}

static const MemoryRegionOps q630_valkyrie_ops = {
    .read = q630_valkyrie_read,
    .write = q630_valkyrie_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * F108 IDE interrupt flag register at 0x50F1A101 (byte, not word
 * aligned).  Semantics from Linux's old drivers/ide/macide.c (itself
 * reverse engineered from the MacOS IDE driver):
 *   bit 5: IDE interrupt flag (write 0 to clear)
 *   bit 6: IDE interrupt enable
 *   bit 7: any interrupt condition
 * The IDE IRQ is delivered as the VIA2 "nubus" slot $F interrupt
 * (Linux IRQ_NUBUS_F; QEMU input 6 of the via2 nubus-irq gpio array).
 */

/*
 * Valkyrie VBL pending: the ROM's Q630 slot ISR (0x4088BC2C, chained
 * from VIA2 CA1 via lowmem 0xD74) reads THIS SAME F108 register
 * 0x50F1A101 and treats bit 6 as the internal-video (pseudo-slot 0)
 * VBL interrupt flag: pending byte bit 6 -> slot table 0x40806F14
 * entry 0x06 -> slot 0, whose SInt handler (RAM, installed by the
 * video driver) clears the Valkyrie VBL status (+0x10C) and runs the
 * slot-0 VBL queue = jCrsrTask -> cursor/mouse coupling.  The VBL
 * interrupt is enabled by the driver's long write of 4 to Valkyrie
 * +0x104 and acknowledged via the Valkyrie +0x10C clear, so bit 6
 * here simply mirrors "status AND enable".
 */
static bool q630_vbl_pending(Q630MachineState *m)
{
    return (m->valkyrie_regs[0x10b] & 0x04) &&
           (m->valkyrie_regs[0x107] & 0x04);
}

static void q630_ide_update_irq(Q630MachineState *m)
{
    bool flag = (m->ide_ifr & 0x20) || m->ide_irq_level;
    qemu_irq slot = qdev_get_gpio_in_named(DEVICE(&m->via2), "nubus-irq",
                                           VIA2_NUBUS_IRQ_INTVIDEO);

    qemu_set_irq(slot, (flag && (m->ide_ifr & 0x40)) || q630_vbl_pending(m));
}

static void q630_ide_set_irq(void *opaque, int n, int level)
{
    Q630MachineState *m = opaque;

    m->ide_irq_level = level;
    if (level) {
        m->ide_ifr |= 0x20;
    }
    q630_ide_update_irq(m);
}

static uint64_t q630_ide_ifr_read(void *opaque, hwaddr addr, unsigned size)
{
    Q630MachineState *m = opaque;
    uint8_t val;

    if (addr != 1) {
        return 0;
    }
    /*
     * Visible bit 6 is the VBL pending flag (see q630_vbl_pending);
     * the internally-stored bit 6 (macide.c-style IDE irq enable)
     * must not leak into it or the ROM slot ISR would dispatch
     * spurious slot-0 interrupts (SysError while the slot-0 queue is
     * still empty).
     */
    val = m->ide_ifr & 0x3f;
    if (m->ide_irq_level) {
        val |= 0x20;
    }
    if (q630_vbl_pending(m)) {
        val |= 0x40;
    }
    if (val & 0x20) {
        val |= 0x80;
    }
    return val;
}

static void q630_ide_ifr_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    Q630MachineState *m = opaque;

    if (addr != 1) {
        return;
    }
    /* enables are stored; the flag latch can only be cleared (bit5=0) */
    m->ide_ifr = (val & 0x5f) | (val & m->ide_ifr & 0x20);
    q630_ide_update_irq(m);
}

static const MemoryRegionOps q630_ide_ifr_ops = {
    .read = q630_ide_ifr_read,
    .write = q630_ide_ifr_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* 60.15Hz tick on VIA1 CA1 and one-second tick on CA2 */

static void q630_sixty_hz(void *opaque)
{
    Q630MachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA1_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    /* Valkyrie VBL status; deliver the slot interrupt if enabled */
    m->valkyrie_regs[0x10b] |= 0x04;
    q630_ide_update_irq(m);

    timer_mod(m->sixty_hz_timer,
              (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
               VIA_60HZ_TIMER_PERIOD_NS) /
              VIA_60HZ_TIMER_PERIOD_NS * VIA_60HZ_TIMER_PERIOD_NS);
}

static void q630_one_second(void *opaque)
{
    Q630MachineState *m = opaque;
    qemu_irq irq = qdev_get_gpio_in(DEVICE(&m->via1), CA2_INT_BIT);

    qemu_irq_lower(irq);
    qemu_irq_raise(irq);

    timer_mod(m->one_second_timer,
              (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000) / 1000 * 1000);
}

static void q630_machine_init(MachineState *machine)
{
    Q630MachineState *m = Q630_MACHINE(machine);
    ram_addr_t ram_size = machine->ram_size;
    const char *bios_name = machine->firmware ?: Q630_ROM_FILENAME;
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

    q630_machine = m;

    /* CPU */
    object_initialize_child(OBJECT(machine), "cpu", &m->cpu, machine->cpu_type);
    qdev_realize(DEVICE(&m->cpu), NULL, &error_fatal);
    qemu_register_reset(main_cpu_reset, &m->cpu);

    /* RAM inside a container whose unbacked reads return 0 (RAM sizing) */
    memory_region_init_io(&m->ramio, OBJECT(machine), &ramio_ops, &m->ramio,
                          "ram", RAM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x0, &m->ramio);
    memory_region_add_subregion(&m->ramio, 0, machine->ram);

    /*
     * 24-bit-mode tagged-pointer window: MacOS boots this ROM in 24-bit
     * addressing (rebuilt PRAM), and the ROM sets 68040 DTTR1/ITTR1 to
     * 0x807FC040 — transparently translating logical 0x80000000-
     * 0xFFFFFFFF to identical physical.  The Memory Manager forms
     * handle/DCE pointers as 0x80000000 | 24bit_addr (e.g. the .Sony
     * DCE 0x800061E0), so RAM must also be decoded at 0x80000000 for
     * those to resolve (on real F108 hardware the high address bits are
     * ignored by the RAM decoder).  Mirror the low 16 MiB of RAM there.
     */
    memory_region_init_alias(&m->ram_alias24, OBJECT(machine),
                             "q630.ram-24bit", machine->ram, 0,
                             MIN(ram_size, 0x01000000));
    memory_region_add_subregion(get_system_memory(), 0x80000000,
                                &m->ram_alias24);

    /* background catch-all: everything unclaimed bus-errors, logged */
    memory_region_init_io(&m->bgtrace, OBJECT(machine), &q630_bgtrace_ops,
                          m, "q630.bus-trace", 0xffffffffull + 1);
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
        static Q630RegBank memcjr_bank, primetime_bank;

        memcjr_bank = (Q630RegBank){ "memcjr", m->memcjr_regs,
                                      sizeof(m->memcjr_regs), 4000 };
        memory_region_init_io(&m->memcjr_mem, OBJECT(machine),
                              &q630_regbank_ops, &memcjr_bank,
                              "memcjr", sizeof(m->memcjr_regs));
        memory_region_add_subregion(&m->macio, MEMCJR_OFS, &m->memcjr_mem);

        primetime_bank = (Q630RegBank){ "primetime", m->primetime_regs,
                                         sizeof(m->primetime_regs), 4000 };
        memory_region_init_io(&m->primetime_mem, OBJECT(machine),
                              &q630_primetime_ops, &primetime_bank,
                              "primetime", sizeof(m->primetime_regs));
        memory_region_add_subregion(&m->macio, PRIMETIME_OFS,
                                    &m->primetime_mem);

        /*
         * Sound codec ("Singer"/DFAC-class) at 0x50F04000: the ROM's
         * sound driver programs register/value pairs through a mailbox
         * (+2 command/ready with bit0 polled, +6 data) and retries
         * forever on a bus error; the RAM-backed regbank satisfies the
         * ready handshake because writes read back.
         */
        static Q630RegBank singer_bank;
        singer_bank = (Q630RegBank){ "singer", m->singer_regs,
                                     sizeof(m->singer_regs), 2000 };
        memory_region_init_io(&m->singer_mem, OBJECT(machine),
                              &q630_singer_ops, &singer_bank,
                              "singer", sizeof(m->singer_regs));
        memory_region_add_subregion(&m->macio, SINGER_OFS, &m->singer_mem);
    }

    /* catch-all trace region behind the devices */
    memory_region_init_io(&m->iotrace, OBJECT(machine), &q630_iotrace_ops,
                          m, "mac-io.trace", IO_SLICE);
    memory_region_add_subregion_overlap(&m->macio, 0, &m->iotrace, -1);

    /* VIA1 fronting the Cuda MCU */
    object_initialize_child(OBJECT(machine), "via1", &m->via1,
                            TYPE_MOS6522_Q630);
    qdev_prop_set_uint64(DEVICE(&m->via1), "frequency", VIA_TIMER_FREQ);
    sysbus = SYS_BUS_DEVICE(&m->via1);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_init_io(&m->via1mem, OBJECT(machine), &q630_via1_ops,
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
     * Seed the Cuda PRAM like a battery-backed part on a machine that has
     * booted MacOS before (UNLIKE lc475, which must leave PRAM invalid so
     * its ROM rebuilds a 24-bit-consistent image — see LC475-NOTES #13):
     *
     * - 'NuMc' at 0x0C: XPRAM validity signature.  With the signature
     *   missing, the ROM zero-wipes its in-RAM XPRAM cache (loaded from
     *   Cuda via 256 GET_PRAMs), and _ReadXPRam serves the CACHE, so the
     *   GET_PRAM-level 0x8A read hook alone cannot make the 32-bit flag
     *   stick.
     * - 0x8A = 0x05: 32-bit addressing (bit 0 boot-32bit, bit 2 32-bit
     *   desired).  REQUIRED on Q630: the ROM hardcodes MMU32Bit=1 but
     *   still keys the [0x644] pointer-translation vector off this byte;
     *   0 leaves the 24-bit Translate24To32 live and QuickDraw corrupts
     *   any bit-23 RAM pointer (0x00FFxxxx -> 0x500Fxxxx F108 I/O) ->
     *   dsBusError Sad Mac.  See the CUDA_CMD_GET_PRAM comment.
     * - OSDefault at XPRAM 0x77 = 1 (MacOS): the boot scan (trap A084 =
     *   _ReadXPRam word at 0x76) matches the DDM driver ddType against
     *   this; a "valid" PRAM with OSDefault 0 rejects every bootable
     *   disk (the lc475 pitfall; that ROM kept the value at 0xF8-0xFB
     *   instead, seeded too for good measure).
     */
    m->via1.PRAM[0x0c] = 'N';
    m->via1.PRAM[0x0d] = 'u';
    m->via1.PRAM[0x0e] = 'M';
    m->via1.PRAM[0x0f] = 'c';
    m->via1.PRAM[0x77] = 0x01;
    m->via1.PRAM[0x8a] = 0x05;
    m->via1.PRAM[0xfb] = 0x01;
    m->via1.machine = m;
    m->cuda_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q630_cuda_timer_cb, m);

    /* ADB devices behind the Cuda */
    adb_bus = qdev_get_child_bus(DEVICE(&m->via1), "adb.0");
    dev = qdev_new(TYPE_ADB_KEYBOARD);
    qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    dev = qdev_new(TYPE_ADB_MOUSE);
    qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    adb_register_autopoll_callback(&m->via1.adb_bus, q630_cuda_adb_poll, m);

    m->sixty_hz_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q630_sixty_hz, m);
    timer_mod(m->sixty_hz_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              VIA_60HZ_TIMER_PERIOD_NS);
    m->one_second_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, q630_one_second,
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
    memory_region_init_io(&m->asc_fifoirq_mem, OBJECT(machine),
                          &q630_asc_fifoirq_ops, m, "asc-fifoirq-shim", 1);
    memory_region_add_subregion_overlap(&m->macio, ASC_OFS + 0x804,
                                        &m->asc_fifoirq_mem, 1);
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

    /*
     * F108 IDE: memory-mapped ATA taskfile at 0x50F1A000 with register
     * stride 4 (Linux mac_pata_quadra_rsrc/ioport_shift=2), alt status/
     * device control at +0x38, interrupt flag register at +0x101.
     */
    dev = qdev_new(TYPE_MMIO_IDE);
    qdev_prop_set_uint32(dev, "shift", 2);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    m->ide = dev;
    sysbus_connect_irq(sysbus, 0, qemu_allocate_irq(q630_ide_set_irq, m, 0));
    memory_region_init_alias(&m->ide_tf_alias, OBJECT(machine), "ide-tf",
                             sysbus_mmio_get_region(sysbus, 0), 0, 0x38);
    memory_region_add_subregion(&m->macio, IDE_OFS, &m->ide_tf_alias);
    memory_region_init_alias(&m->ide_cs_alias, OBJECT(machine), "ide-cs",
                             sysbus_mmio_get_region(sysbus, 1), 0, 0x4);
    memory_region_add_subregion(&m->macio, IDE_OFS + 0x38, &m->ide_cs_alias);
    memory_region_init_io(&m->ide_ifr_mem, OBJECT(machine), &q630_ide_ifr_ops,
                          m, "ide-ifr", 0x10);
    memory_region_add_subregion(&m->macio, IDE_OFS + 0x100, &m->ide_ifr_mem);
    m->ide_ifr = 0x40;
    mmio_ide_init_drives(dev, drive_get(IF_IDE, 0, 0),
                         drive_get(IF_IDE, 0, 1));

    /* Valkyrie/CSC-class video control registers */
    {
        static Q630RegBank valkyrie_bank;

        valkyrie_bank = (Q630RegBank){ "valkyrie", m->valkyrie_regs,
                                        sizeof(m->valkyrie_regs), 8000 };
        m->valkyrie_bank = &valkyrie_bank;
        /*
         * Monitor sense at +0x122 (word): read once by the video driver.
         * 0 selects the 21" 1152x870 mode; Apple sense code 6 = 13"
         * 640x480 RGB.
         */
        m->valkyrie_regs[0x123] = 0x06;
        memory_region_init_io(&m->valkyrie_mem, OBJECT(machine),
                              &q630_valkyrie_ops, m,
                              "valkyrie", sizeof(m->valkyrie_regs));
        memory_region_add_subregion(get_system_memory(), 0xf9800000,
                                    &m->valkyrie_mem);
    }

    /* Onboard video framebuffer at its physical decode */
    object_initialize_child(OBJECT(machine), "fb", &m->fb, TYPE_Q630_FB);
    sysbus = SYS_BUS_DEVICE(&m->fb);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(get_system_memory(), Q630_VRAM_BASE,
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
                                     "q630.vram-alias",
                                     sysbus_mmio_get_region(sysbus, 0),
                                     0, Q630_VRAM_SIZE);
            memory_region_add_subregion(get_system_memory(),
                                        Q630_VRAM_BASE +
                                        (i + 1) * Q630_VRAM_SIZE,
                                        &m->vram_aliases[i]);
        }
    }

    /* ROM */
    memory_region_init_rom(&m->rom, NULL, "q630.rom", Q630_ROM_SIZE,
                           &error_abort);
    memory_region_add_subregion(get_system_memory(), Q630_ROM_ADDR, &m->rom);

    memory_region_init_alias(&m->rom_alias, NULL, "q630.rom-alias",
                             &m->rom, 0, Q630_ROM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x40000000,
                                &m->rom_alias);

    /*
     * The F108 also decodes the ROM at physical 0x00800000 (the 24-bit
     * compatibility window): the ROM sets lowmem ROMBase = 0x00800000
     * and installs DCE driver pointers relative to it (e.g. the .Sony
     * DCE built at 0x4080b990 with `movel 0x2ae,%d0`), with the MMU
     * tables identity-mapping that range — without this mirror the
     * .Sony open jumps into RAM-test patterns and dies to the monitor.
     * Mapped ABOVE RAM (priority 1): with more than 8 MiB of RAM this
     * shadows RAM at 0x00800000-0x008FFFFF, exactly like the real F108
     * where the ROM window punches a hole in the RAM banks; the ROM
     * sizes around it and its 24-bit-mode page tables identity-map the
     * window onto the mirror.
     */
    memory_region_init_alias(&m->rom_alias24, NULL, "q630.rom-alias24",
                             &m->rom, 0, Q630_ROM_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(), 0x00800000,
                                        &m->rom_alias24, 1);
    /* disabled at reset: the RAM test must see RAM there; the ROM
     * enables the window through an F108 register (hook TBD) */
    memory_region_set_enabled(&m->rom_alias24, false);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, Q630_ROM_ADDR,
                                        Q630_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }

    if (!qtest_enabled()) {
        if (bios_size <= 0 || bios_size > Q630_ROM_SIZE) {
            error_report("could not load MacROM '%s'", bios_name);
            exit(1);
        }

        cs = CPU(&m->cpu);
        ptr = rom_ptr(Q630_ROM_ADDR, bios_size);
        assert(ptr != NULL);
        stl_phys(cs->as, 0, ldl_be_p(ptr));    /* reset initial SP */
        stl_phys(cs->as, 4,
                 Q630_ROM_ADDR + ldl_be_p(ptr + 4)); /* reset initial PC */
    }
}

static GlobalProperty hw_compat_q630[] = {
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
static const size_t hw_compat_q630_len = G_N_ELEMENTS(hw_compat_q630);

static void q630_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68040"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Macintosh Quadra 630 / LC 630 / Performa 630";
    mc->init = q630_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->block_default_type = IF_SCSI;
    mc->default_ram_size = 32 * MiB;
    mc->default_ram_id = "q630.ram";
    machine_add_audiodev_property(mc);
    compat_props_add(mc->compat_props, hw_compat_q630, hw_compat_q630_len);
}

static const TypeInfo q630_machine_typeinfo[] = {
    {
        .name       = TYPE_MOS6522_Q630,
        .parent     = TYPE_MOS6522,
        .instance_size = sizeof(MOS6522Q630State),
        .instance_init = mos6522_q630_init,
        .class_init = mos6522_q630_class_init,
    },
    {
        .name       = TYPE_Q630_FB,
        .parent     = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Q630FbState),
        .class_init = q630_fb_class_init,
    },
    {
        .name       = TYPE_Q630_MACHINE,
        .parent     = TYPE_MACHINE,
        .instance_size = sizeof(Q630MachineState),
        .class_init = q630_machine_class_init,
    },
};

DEFINE_TYPES(q630_machine_typeinfo)
