/*
 * QEMU GeForce 6200 (NV44A) skeleton
 *
 * A register-level model of just enough of an NV44A for the Linux
 * nouveau driver to probe headless (nouveau.config=NvForcePost=0,
 * modeset=2, noaccel).  This is not a GPU: BAR0 is a 16 MiB register
 * file that reads back what was written, with real behavior layered
 * on top only where nouveau's probe path depends on it (PMC boot0,
 * straps, PFB memory size, PTIMER, PRAMIN, PROM).
 *
 * Developed against the nouveau source as the reference for what the
 * hardware must answer.
 *
 * Copyright (c) 2026 Daniel's intern
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/i2c/i2c.h"
#include "hw/core/loader.h"
#include "qemu/datadir.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_GEFORCE6200 "geforce6200"
OBJECT_DECLARE_SIMPLE_TYPE(GeForce6200State, GEFORCE6200)

#define NV_FIFO_CHANNELS 32

/* one FIFO channel's functional state */
typedef struct NVChan {
    bool active;
    bool blocked;           /* stalled on semaphore acquire */
    uint32_t put, get, ref;
    uint32_t pb_inst;       /* pushbuf ctxdma instance (RAMIN offset) */
    uint32_t subr_ret;
    bool in_subr;
    /* method decode state */
    uint32_t mcnt, mthd;
    uint8_t msubc;
    bool ni;
    /* subchannel bindings */
    uint8_t subc_engine[8];
    uint16_t subc_class[8];
    uint32_t subc_inst[8];
    /* host semaphore state */
    uint32_t sema_inst, sema_off, acq_val;
    /* M2MF engine state */
    struct {
        uint32_t notify_inst, src_inst, dst_inst;
        uint32_t off_in, off_out, pitch_in, pitch_out;
        uint32_t line_len, line_count, format;
    } m2mf;
    /* curie 3D interpreter state, allocated on first use */
    struct NV3DState *n3d;
} NVChan;

#define NV_BAR0_SIZE          (16 * MiB)
#define NV_PRAMIN_BAR0_BASE   0x700000  /* 1 MiB window at end of VRAM */
#define NV_PRAMIN_BAR0_SIZE   (1 * MiB)
#define NV_PROM_BASE          0x300000  /* VBIOS shadow window */
#define NV_PROM_SIZE          0x10000
#define NV_RAMIN_BAR_SIZE     (16 * MiB)

/* PMC */
#define NV_PMC_BOOT_0         0x000000
#define NV_PMC_INTR_0         0x000100
#define NV_PMC_INTR_EN_0      0x000140
#define NV_PMC_INTR_DISP      0x01010000
/* PCRTC (per head, +0x2000) */
#define NV_PCRTC_INTR_0       0x600100
#define NV_PCRTC_INTR_EN_0    0x600140
/* PTIMER */
#define NV_PTIMER_INTR_0      0x009100
#define NV_PTIMER_INTR_EN_0   0x009140
#define NV_PTIMER_NUMERATOR   0x009200
#define NV_PTIMER_DENOMINATOR 0x009210
#define NV_PTIMER_TIME_0      0x009400
#define NV_PTIMER_TIME_1      0x009410

struct GeForce6200State {
    PCIDevice parent_obj;

    MemoryRegion bar0;          /* register file + windows */
    MemoryRegion vram;          /* BAR1 */
    MemoryRegion ramin;         /* BAR2/3: alias into VRAM (reversed) */
    MemoryRegion pramin_alias;  /* BAR0 window into end of VRAM */

    uint32_t *regs;             /* BAR0 backing store */

    uint32_t vram_size_mb;
    char *romfile;
    uint8_t vbios[NV_PROM_SIZE];

    /*
     * Card endian mode.  NV4x has an endian switch (PMC_BOOT_1, reg
     * 0x000004): 0 = little-endian (power-on default), 0x01000001 =
     * big-endian.  nouveau flips it to match the CPU on a big-endian
     * host (nvkm_device_endianness()); once set, the card byte-swaps
     * every 32-bit register and in-memory-structure access, so the
     * driver's native accessors see logical values transparently.  We
     * model that: `be` gates a byte-swap on the MMIO register boundary
     * (to cancel QEMU's own DEVICE_LITTLE_ENDIAN swap for a BE guest)
     * and selects the byte order of all structure accessors.
     */
    bool be;

    /* VGA CRTC shadow registers, one file per head */
    uint8_t cr_index[2];
    uint8_t cr[2][256];

    /* scanout */
    QemuConsole *con;
    int scan_head;
    uint32_t scan_base, scan_pitch, scan_w, scan_h, scan_bpp;
    bool scan_be;

    QEMUTimer *vblank_timer;

    /* DDC: two bit-banged i2c buses behind CRTC registers */
    struct {
        I2CBus *bus;
        bitbang_i2c_interface bitbang;
        uint8_t scl, sda_in;
    } ddc[2];

    /* FIFO */
    NVChan chan[NV_FIFO_CHANNELS];
    int trap_chid;
    bool trap_acked;
};

static void nv_update_irq(GeForce6200State *s);
static void nv_fifo_poll(GeForce6200State *s);

/*
 * PTIMER: a 31.25 MHz-ish nanosecond timebase.  nouveau programs
 * numerator/denominator so that TIME_0/1 counts nanoseconds; back it
 * directly with the virtual clock so all driver timeouts behave.
 */
static uint64_t nv_ptimer_ns(GeForce6200State *s)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/*
 * Load/store a 32-bit structure word from guest memory (VRAM/RAMIN) in
 * the card's current byte order.  nouveau writes these structures with
 * native CPU stores, and the card's endian mode matches the CPU, so
 * the byte order in memory is little-endian on an LE guest and
 * big-endian on a BE guest - which is exactly what `be` tracks.
 */
static inline uint32_t nv_ld32(GeForce6200State *s, const void *p)
{
    return s->be ? ldl_be_p(p) : ldl_le_p(p);
}

static inline void nv_st32(GeForce6200State *s, void *p, uint32_t v)
{
    if (s->be) {
        stl_be_p(p, v);
    } else {
        stl_le_p(p, v);
    }
}

/*
 * Cancel QEMU's DEVICE_LITTLE_ENDIAN swap for a logical register when
 * the card is in big-endian mode.  On a BE guest QEMU byte-swaps every
 * register access (guest BE access != LE device); returning/consuming
 * the swapped value here makes the driver's native accessor see the
 * logical value, matching a real card after its endian switch.  Byte
 * (size 1) accesses and the PROM byte-stream window are exempt.
 */
static inline uint64_t nv_reg_be(GeForce6200State *s, uint64_t v,
                                 unsigned size)
{
    if (!s->be) {
        return v;
    }
    return size == 4 ? bswap32(v) : size == 2 ? bswap16(v) : v;
}

/*
 * VGA CRTC index/data ports live at 0x601000 + head * 0x2000 + 0x3d4/5.
 * nouveau uses them for the "was the card POSTed?" test (CR06/07/25/41
 * horizontal total), ownership/lock handshakes (CR44/CR1F) and DDC
 * bit-banging for DCB i2c buses.  Model a real index/data pair per
 * head so per-register values survive, unlike the flat register file.
 */
static bool nv_vga_crtc_access(GeForce6200State *s, hwaddr addr, int *head)
{
    if ((addr & ~0x2001ULL) != 0x6013d4) {
        return false;
    }
    *head = (addr >> 13) & 1;
    return true;
}

/*
 * DDC buses: nouveau's nv04 i2c bit-bangs through CRTC registers on
 * head 0.  Drive register (CR 0x3f bus 0, CR 0x37 bus 1): bit5 SCL
 * out, bit4 SDA out.  Sense register (CR 0x3e / CR 0x36): bit2 SCL
 * in, bit3 SDA in.  (nouveau nvkm/subdev/i2c/busnv04.c)
 */
static int nv_ddc_bus_for_cr(uint8_t index, bool *sense)
{
    switch (index) {
    case 0x3f: *sense = false; return 0;
    case 0x3e: *sense = true;  return 0;
    case 0x37: *sense = false; return 1;
    case 0x36: *sense = true;  return 1;
    default:   return -1;
    }
}

static void nv_ddc_drive(GeForce6200State *s, int busnr, uint8_t val)
{
    bool scl = !!(val & 0x20), sda = !!(val & 0x10);

    s->ddc[busnr].scl = scl;
    bitbang_i2c_set(&s->ddc[busnr].bitbang, BITBANG_I2C_SCL, scl);
    s->ddc[busnr].sda_in =
        bitbang_i2c_set(&s->ddc[busnr].bitbang, BITBANG_I2C_SDA, sda);
}

static uint8_t nv_ddc_sense(GeForce6200State *s, int busnr)
{
    return (s->ddc[busnr].scl ? 0x04 : 0) |
           (s->ddc[busnr].sda_in ? 0x08 : 0);
}

static uint32_t nv_pmc_intr_pending(GeForce6200State *s)
{
    uint32_t pending = 0;
    int head;

    for (head = 0; head < 2; head++) {
        uint32_t hoff = head * 0x2000;

        if (s->regs[(NV_PCRTC_INTR_0 + hoff) / 4] &
            s->regs[(NV_PCRTC_INTR_EN_0 + hoff) / 4] & 1) {
            pending |= NV_PMC_INTR_DISP;
        }
    }
    if (s->regs[0x002100 / 4] & s->regs[0x002140 / 4]) {
        pending |= 0x00000100;      /* PFIFO */
    }
    return pending;
}

static void nv_update_irq(GeForce6200State *s)
{
    uint32_t pending = nv_pmc_intr_pending(s);
    bool level = (s->regs[NV_PMC_INTR_EN_0 / 4] & 1) && pending;

    pci_set_irq(PCI_DEVICE(s), level);
}

static void nv_vblank_timer_cb(void *opaque)
{
    GeForce6200State *s = opaque;
    int head;

    for (head = 0; head < 2; head++) {
        uint32_t hoff = head * 0x2000;

        if (s->regs[(NV_PCRTC_INTR_EN_0 + hoff) / 4] & 1) {
            s->regs[(NV_PCRTC_INTR_0 + hoff) / 4] |= 1;
        }
    }
    nv_update_irq(s);
    nv_fifo_poll(s);

    timer_mod(s->vblank_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 16);
}

#include "geforce6200-fifo.c.inc"

static uint64_t nv_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    GeForce6200State *s = opaque;
    uint32_t val;
    int head;

    if (size == 4 && (addr & 3) == 0) {
        uint32_t fval;

        if (nv_fifo_bar0_read(s, addr, &fval)) {
            return nv_reg_be(s, fval, size);
        }
    }

    if (size == 1 && nv_vga_crtc_access(s, addr, &head)) {
        if (addr & 1) {
            bool sense;
            int busnr = head ? -1 :
                nv_ddc_bus_for_cr(s->cr_index[head], &sense);

            if (busnr >= 0 && sense) {
                return nv_ddc_sense(s, busnr);
            }
            return s->cr[head][s->cr_index[head]];
        }
        return s->cr_index[head];
    }

    switch (addr & ~3) {
    case NV_PMC_INTR_0:
        val = nv_pmc_intr_pending(s);
        break;
    case 0x680608:      /* PRAMDAC_TEST_CONTROL, DAC A */
    case 0x682608:      /* DAC B */
        /*
         * Load detection: SENSEB_ALLHI (bit28) set means a CRT load
         * is present on this DAC; bits 31:29 clear means no TV load.
         * Reported live so the modeset-time register writes cannot
         * clear it.  (nouveau dispnv04/dac.c nv17_dac_sample_load)
         */
        val = (s->regs[addr / 4] & ~0xe0000000) | 0x10000000;
        break;
    case NV_PTIMER_TIME_0:
        val = nv_ptimer_ns(s) & 0xffffffe0;
        break;
    case NV_PTIMER_TIME_1:
        val = nv_ptimer_ns(s) >> 32;
        break;
    default:
        if (addr >= NV_PROM_BASE && addr < NV_PROM_BASE + NV_PROM_SIZE) {
            /*
             * Byte-wise reads of the shadowed VBIOS.  The VBIOS is a
             * little-endian byte stream that nouveau copies verbatim
             * (native rd32 into a byte buffer, parsed with
             * get_unaligned_le32); QEMU's own DEVICE_LITTLE_ENDIAN swap
             * already makes that copy byte-faithful on a BE guest, so
             * this window must NOT get the nv_reg_be() treatment.
             */
            val = s->vbios[addr - NV_PROM_BASE];
            if (size > 1) {
                val |= s->vbios[(addr - NV_PROM_BASE + 1) % NV_PROM_SIZE] << 8;
            }
            if (size > 2) {
                val |= s->vbios[(addr - NV_PROM_BASE + 2) % NV_PROM_SIZE] << 16;
                val |= s->vbios[(addr - NV_PROM_BASE + 3) % NV_PROM_SIZE] << 24;
            }
            return val;
        }
        if (addr + size <= 0x2000 + PCI_CONFIG_SPACE_SIZE &&
            addr >= 0x1800) {
            /* PBUS 0x1800: PCI config space mirror */
            val = pci_default_read_config(PCI_DEVICE(s), addr - 0x1800, size);
            break;
        }
        if (addr >= 0x88000 && addr + size <= 0x88000 + PCI_CONFIG_SPACE_SIZE) {
            /* NV40+ config space mirror */
            val = pci_default_read_config(PCI_DEVICE(s), addr - 0x88000, size);
            break;
        }
        val = s->regs[addr / 4];
        break;
    }

    if (!(size == 4 && (addr & 3) == 0)) {
        val = (val >> ((addr & 3) * 8)) & ((1ULL << (size * 8)) - 1);
    }
    return nv_reg_be(s, val, size);
}

static void nv_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    GeForce6200State *s = opaque;
    uint32_t cur;
    int head;

    if (size == 1 && nv_vga_crtc_access(s, addr, &head)) {
        if (addr & 1) {
            bool sense;
            int busnr = head ? -1 :
                nv_ddc_bus_for_cr(s->cr_index[head], &sense);

            s->cr[head][s->cr_index[head]] = val;
            if (busnr >= 0 && !sense) {
                nv_ddc_drive(s, busnr, val);
            }
        } else {
            s->cr_index[head] = val;
        }
        return;
    }

    /*
     * Undo QEMU's DEVICE_LITTLE_ENDIAN swap for a BE guest so the rest
     * of this function deals in logical register values.  Applied to
     * aligned 16/32-bit accesses only; byte writes need no swap.  The
     * endian-switch write itself (reg 0x000004) lands here while `be`
     * is still false, i.e. un-swapped only by QEMU - and its value
     * 0x01000001 is a byte-swap palindrome, so it reads correctly
     * either way.
     */
    if (s->be && (size == 4 || size == 2) && !(addr & (size - 1))) {
        val = size == 4 ? bswap32(val) : bswap16(val);
    }

    /* PMC_BOOT_1 endian switch: 0x01000001 selects big-endian mode */
    if (addr == 0x000004) {
        s->be = (val == 0x01000001);
    }

    if (size != 4 || (addr & 3)) {
        /* read-modify-write into the register file */
        unsigned shift = (addr & 3) * 8;
        uint64_t mask = ((1ULL << (size * 8)) - 1) << shift;

        cur = s->regs[addr / 4];
        cur = (cur & ~mask) | ((val << shift) & mask);
        s->regs[addr / 4] = cur;
        return;
    }

    if (nv_fifo_bar0_write(s, addr, val)) {
        return;
    }

    switch (addr) {
    case 0x600800:      /* PCRTC_START: follow the last-programmed head */
    case 0x602800:
        s->scan_head = (addr >> 13) & 1;
        break;
    case NV_PCRTC_INTR_0:
    case NV_PCRTC_INTR_0 + 0x2000:
        s->regs[addr / 4] &= ~val;      /* write-1-to-clear */
        nv_update_irq(s);
        return;
    case NV_PMC_INTR_EN_0:
    case NV_PCRTC_INTR_EN_0:
    case NV_PCRTC_INTR_EN_0 + 0x2000:
        s->regs[addr / 4] = val;
        nv_update_irq(s);
        return;
    default:
        break;
    }

    s->regs[addr / 4] = val;
}

static const MemoryRegionOps nv_bar0_ops = {
    .read = nv_bar0_read,
    .write = nv_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * Scanout state, decoded from what nouveau's nv04 modeset code wrote
 * into the register file and CRTC shadow (head 0 only for now).
 */
static bool geforce6200_scanout_params(GeForce6200State *s, uint32_t *base,
                                       uint32_t *pitch, uint32_t *w,
                                       uint32_t *h, uint32_t *bpp)
{
    const uint8_t *cr = s->cr[s->scan_head];
    uint32_t depth = cr[0x28] & 3;

    /* bit sources per nouveau dispnv04/crtc.c mode programming */
    *w = ((cr[0x01] | (cr[0x2d] & 0x02) << 7) + 1) * 8;
    *h = (cr[0x12] | (cr[0x07] & 0x02) << 7 | (cr[0x07] & 0x40) << 3 |
          (cr[0x25] & 0x02) << 9 | (cr[0x41] & 0x04) << 9) + 1;
    *pitch = (cr[0x13] | (cr[0x19] & 0xe0) << 3 |
              (cr[0x42] & 0x40) << 5) * 8;
    *base = s->regs[(0x600800 + s->scan_head * 0x2000) / 4];
    *bpp = depth == 3 ? 32 : depth == 2 ? 16 : depth == 1 ? 8 : 0;

    return *bpp && *w >= 320 && *w <= 4096 && *h >= 200 && *h <= 4096 &&
           *pitch >= *w * (*bpp / 8) &&
           (uint64_t)*base + (uint64_t)*pitch * *h <=
               (uint64_t)s->vram_size_mb * MiB;
}

static bool geforce6200_gfx_update(void *opaque)
{
    GeForce6200State *s = opaque;
    DisplaySurface *surface;
    uint32_t base, pitch, w, h, bpp;
    pixman_format_code_t format;

    if (!geforce6200_scanout_params(s, &base, &pitch, &w, &h, &bpp)) {
        return true;
    }

    if (base != s->scan_base || pitch != s->scan_pitch || w != s->scan_w ||
        h != s->scan_h || bpp != s->scan_bpp || s->be != s->scan_be) {
        s->scan_base = base;
        s->scan_pitch = pitch;
        s->scan_w = w;
        s->scan_h = h;
        s->scan_bpp = bpp;
        s->scan_be = s->be;

        if (bpp == 32) {
            /*
             * In big-endian mode the framebuffer is stored native-endian
             * (a 0x00RRGGBB pixel lands in memory as X,R,G,B), so pick
             * the big-endian x8r8g8b8 pixman format; otherwise the byte
             * order is little-endian (B,G,R,X).
             */
            format = s->be ? PIXMAN_BE_x8r8g8b8 : PIXMAN_LE_x8r8g8b8;
        } else if (bpp == 16) {
            /*
             * PRAMDAC_GENERAL_CONTROL bit12: RGB565, else XRGB1555.
             * 16bpp is not byte-swapped for a BE guest here (best
             * effort, matching bochs-display); nouveau's fbdev uses
             * 32bpp on this much VRAM so this path is rarely hit.
             */
            format = s->regs[0x680600 / 4] & 0x1000 ? PIXMAN_r5g6b5
                                                    : PIXMAN_x1r5g5b5;
        } else {
            format = PIXMAN_a8;
        }
        surface = qemu_create_displaysurface_from(w, h, format, pitch,
                    memory_region_get_ram_ptr(&s->vram) + base);
        qemu_console_set_surface(s->con, surface);
    }

    qemu_console_update_full(s->con);
    return true;
}

static void geforce6200_invalidate(void *opaque)
{
    GeForce6200State *s = opaque;

    /* force a surface rebuild on the next update */
    s->scan_bpp = 0;
}

static const GraphicHwOps geforce6200_gfx_ops = {
    .invalidate = geforce6200_invalidate,
    .gfx_update = geforce6200_gfx_update,
};

static void geforce6200_init_regs(GeForce6200State *s)
{
    memset(s->regs, 0, NV_BAR0_SIZE);

    /*
     * Register reset/constant values consumed by the nouveau probe
     * path live here.  Filled in as the model grows; see NOTES.md in
     * the repo root for the derivation of each value.
     */

    /* PMC_BOOT_0: chipset NV4A ("GeForce 6200 [NV44A]"), rev A2 */
    s->regs[NV_PMC_BOOT_0 / 4] = 0x04a200a2;

    /*
     * Nonzero CRTC horizontal total: nouveau reads this as "the VBIOS
     * already POSTed the card" and skips executing init scripts.
     */
    memset(s->cr, 0, sizeof(s->cr));
    memset(s->cr_index, 0, sizeof(s->cr_index));
    s->cr[0][0x06] = 0x4f;
    s->cr[1][0x06] = 0x4f;

    /* PFB: VRAM size (nv44_ram_new: 0x10020c & 0xff000000) ... */
    s->regs[0x10020c / 4] = s->vram_size_mb * MiB;
    /* ... and type: bit0 = DDR1 */
    s->regs[0x100474 / 4] = 0x00000001;
}

static void geforce6200_realize(PCIDevice *pdev, Error **errp)
{
    GeForce6200State *s = GEFORCE6200(pdev);
    uint64_t vram_size = (uint64_t)s->vram_size_mb * MiB;
    int i;

    if (s->vram_size_mb < 16 || s->vram_size_mb > 256 ||
        !is_power_of_2(vram_size)) {
        error_setg(errp, "vram_mb must be a power of two between 16 and 256");
        return;
    }

    if (s->romfile) {
        char *path = qemu_find_file(QEMU_FILE_TYPE_BIOS, s->romfile);
        int len;

        if (!path) {
            path = g_strdup(s->romfile);
        }
        len = load_image_size(path, s->vbios, sizeof(s->vbios));
        g_free(path);
        if (len < 0) {
            error_setg(errp, "geforce6200: cannot load vbios '%s'",
                       s->romfile);
            return;
        }
    }

    pci_config_set_interrupt_pin(pdev->config, 1);

    s->regs = g_malloc0(NV_BAR0_SIZE);

    memory_region_init_io(&s->bar0, OBJECT(s), &nv_bar0_ops, s,
                          "nv44-bar0", NV_BAR0_SIZE);

    memory_region_init_ram(&s->vram, OBJECT(s), "nv44-vram", vram_size,
                           &error_fatal);

    /* PRAMIN: last MiB of VRAM, visible at BAR0+0x700000 */
    memory_region_init_alias(&s->pramin_alias, OBJECT(s), "nv44-pramin",
                             &s->vram, vram_size - NV_PRAMIN_BAR0_SIZE,
                             NV_PRAMIN_BAR0_SIZE);
    memory_region_add_subregion(&s->bar0, NV_PRAMIN_BAR0_BASE,
                                &s->pramin_alias);

    /* BAR2: RAMIN aperture (start of the instmem region) */
    memory_region_init_alias(&s->ramin, OBJECT(s), "nv44-ramin",
                             &s->vram, vram_size - NV_RAMIN_BAR_SIZE >
                             vram_size ? 0 : vram_size - NV_RAMIN_BAR_SIZE,
                             MIN(vram_size, NV_RAMIN_BAR_SIZE));

    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vram);
    pci_register_bar(pdev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->ramin);

    geforce6200_init_regs(s);
    s->trap_chid = -1;

    for (i = 0; i < 2; i++) {
        g_autofree char *name = g_strdup_printf("nv44-ddc%d", i);
        DeviceState *ddc;

        s->ddc[i].bus = i2c_init_bus(DEVICE(pdev), name);
        bitbang_i2c_init(&s->ddc[i].bitbang, s->ddc[i].bus);
        s->ddc[i].scl = 1;
        s->ddc[i].sda_in = 1;
        ddc = qdev_new("i2c-ddc");
        qdev_prop_set_uint8(ddc, "address", 0x50);
        qdev_realize_and_unref(ddc, BUS(s->ddc[i].bus), &error_fatal);
    }

    s->con = qemu_graphic_console_create(DEVICE(pdev), 0, &geforce6200_gfx_ops, s);

    s->vblank_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, nv_vblank_timer_cb, s);
    timer_mod(s->vblank_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 16);
}

static void geforce6200_reset(DeviceState *dev)
{
    GeForce6200State *s = GEFORCE6200(dev);

    s->be = false;      /* little-endian until the guest flips the switch */
    if (s->regs) {
        geforce6200_init_regs(s);
    }
}

static const Property geforce6200_props[] = {
    DEFINE_PROP_UINT32("vram_mb", GeForce6200State, vram_size_mb, 64),
    DEFINE_PROP_STRING("vbios", GeForce6200State, romfile),
};

static void geforce6200_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = geforce6200_realize;
    k->vendor_id = PCI_VENDOR_ID_NVIDIA;
    k->device_id = 0x0221;          /* GeForce 6200 (NV44A) */
    k->revision = 0xa2;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    dc->desc = "GeForce 6200 (NV44A) register-level skeleton";
    device_class_set_props(dc, geforce6200_props);
    device_class_set_legacy_reset(dc, geforce6200_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->user_creatable = true;
}

static const TypeInfo geforce6200_info = {
    .name = TYPE_GEFORCE6200,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(GeForce6200State),
    .class_init = geforce6200_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void geforce6200_register_types(void)
{
    type_register_static(&geforce6200_info);
}

type_init(geforce6200_register_types)
