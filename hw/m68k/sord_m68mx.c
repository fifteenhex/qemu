/*
 * SORD M68MX (1985, SORD Computer Corporation)
 *
 * A 68000 business machine running CP/M-68K.  Modelled from the boot
 * ROM ("M68MX BOOT ROM REVISION 01E") -- see SORD-M68MX-NOTES.md for
 * the reverse-engineered memory map:
 *
 *   0x000000  DRAM (sized by the ROM via bus errors)
 *   0xE00000  16KB boot ROM (reset SP/PC in its first 8 bytes)
 *   0xE10000  text VRAM, 80x25, one longword per cell (attr.w, char.w)
 *   0xE12001  display-type ID bytes (odd); nonzero => ASCII messages
 *   0xE20201  DMA controller (addr/count per channel, ch1=FDC, ch3=HD)
 *   0xE20281  NEC uPD765/i8272 FDC (MSR/data), interrupt vector 0x45
 *   0xE20301  addressable latch (bit addr in bits2-0, data in bit3):
 *             bit1 speed/density, bit2 motor, bit5 precomp, bit7 RTC hold
 *   0xE2030B  RTC address / 0xE2030D RTC data (nibble registers)
 *   0xE20381+ interrupt controller (vector base 0x40)
 *   0xE20781  strap register: boot device in ~bits7:6 (1=FD 2=HD 3=first)
 *   0xE207C1  DMA page registers (address bits 23-16)
 *   0xE30001  HD6845 CRTC (addr/data, cursor in R14/15)
 *   0xE30141  MC6850 ACIA #1 / 0xE30161 MC6850 ACIA #0 (Sunbug console)
 *   0xE40000  64KB graphics(?) RAM, probed with an unguarded readback
 *   0xE50000  palette(?), 16 word registers
 *   0xE80005  HD controller / 0xE80105 Z8530 SCC: left unmapped, the
 *             guarded probes take the bus-error path ("not connected")
 *
 * Floppies are the CP/M-68K 8" format: 77 cyl, 2 heads, 26 sec/trk;
 * track (0,0) FM 128 bytes/sec, everything else MFM 256 bytes/sec.
 * Raw image layout: SD track first, then tracks in (cyl*2+head) order.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/system.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "system/reset.h"
#include "system/qtest.h"
#include "system/address-spaces.h"
#include "chardev/char-fe.h"
#include "target/m68k/cpu.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "ui/console.h"
#include "qom/object.h"

#define SORD_ROM_BASE     0xE00000
#define SORD_ROM_SIZE     0x4000
#define SORD_VRAM_BASE    0xE10000
#define SORD_VRAM_SIZE    0x4000    /* 8K text cells + ID/aux area */
#define SORD_SYSIO_BASE   0xE20000
#define SORD_SYSIO_SIZE   0x1000
#define SORD_CTLIO_BASE   0xE30000
#define SORD_CTLIO_SIZE   0x200
#define SORD_GRAM_BASE    0xE40000
#define SORD_GRAM_SIZE    0x10000
#define SORD_PAL_BASE     0xE50000
#define SORD_PAL_SIZE     0x20

#define SORD_INTC_IPL     2
#define SORD_INTC_SRC_KBD 2         /* vector 0x42: ACIA0 rx */
#define SORD_INTC_SRC_FDC 5         /* vector 0x45 */
#define SORD_INTC_SRC_HD  6         /* vector 0x46 */

/* display */
#define SORD_COLS         80
#define SORD_ROWS         25
#define FONT_W            8
#define FONT_H            16

extern const uint8_t vgafont16[256 * 16];

/* floppy geometry */
#define SORD_FD_CYLS      77
#define SORD_FD_HEADS     2
#define SORD_FD_SPT       26
#define SORD_FD_SD_SSZ    128
#define SORD_FD_DD_SSZ    256
#define SORD_FD_SD_TRK    (SORD_FD_SPT * SORD_FD_SD_SSZ)
#define SORD_FD_DD_TRK    (SORD_FD_SPT * SORD_FD_DD_SSZ)

#define TYPE_SORD_M68MX_MACHINE MACHINE_TYPE_NAME("sord-m68mx")
OBJECT_DECLARE_SIMPLE_TYPE(SordMachineState, SORD_M68MX_MACHINE)

typedef struct SordAcia {
    CharFrontend chr;
    uint8_t ctrl;
    uint8_t rxbuf;
    bool rxfull;
} SordAcia;

typedef struct SordDma {
    uint16_t addr[4];
    uint16_t count[4];      /* as written: bits13-0 len-1, bits15-14 dir */
    uint32_t left[4];       /* bytes until terminal count */
    uint8_t page[4];
    uint8_t control;
    bool addr_flip[4];
    bool count_flip[4];
} SordDma;

enum {
    FDC_IDLE,
    FDC_COMMAND,
    FDC_RESULT,
};

typedef struct SordFdcDrive {
    BlockBackend *blk;
    uint8_t cyl;
    bool seek_int;
    uint8_t seek_st0;
} SordFdcDrive;

typedef struct SordFdc {
    int phase;
    uint8_t cmd[9];
    int cmd_len;
    int cmd_idx;
    uint8_t res[7];
    int res_len;
    int res_idx;
    bool irq;
    SordFdcDrive drive[4];
} SordFdc;

struct SordMachineState {
    MachineState parent_obj;

    M68kCPU *cpu;
    MemoryRegion rom;
    MemoryRegion vram;
    MemoryRegion gram;
    MemoryRegion sysio;
    MemoryRegion ctlio;
    MemoryRegion pal;

    uint32_t reset_sp;
    uint32_t reset_pc;

    uint8_t strap;

    /* interrupt controller: pending sources, vectors 0x40 + bit */
    uint8_t intc_pending;

    /* addressable latch at 0xE20301 */
    uint8_t latch;
    uint8_t rtc_addr;

    /* 6845 */
    uint8_t crtc_addr;
    uint8_t crtc[32];

    SordAcia acia[2];
    SordDma dma;
    SordFdc fdc;

    QemuConsole *con;
};

/* ---------------------------------------------------------------- DMA */

static void sord_dma_write(SordMachineState *s, hwaddr offset, uint8_t val)
{
    SordDma *d = &s->dma;
    int ch;

    switch (offset) {
    case 0x201: case 0x203: case 0x205: case 0x207:   /* address, lo/mid */
        ch = (offset - 0x201) >> 1;
        if (!d->addr_flip[ch]) {
            d->addr[ch] = (d->addr[ch] & 0xff00) | val;
        } else {
            d->addr[ch] = (d->addr[ch] & 0x00ff) | (val << 8);
        }
        d->addr_flip[ch] = !d->addr_flip[ch];
        break;
    case 0x211: case 0x213: case 0x215: case 0x217:   /* count, lo/hi */
        ch = (offset - 0x211) >> 1;
        if (!d->count_flip[ch]) {
            d->count[ch] = (d->count[ch] & 0xff00) | val;
        } else {
            d->count[ch] = (d->count[ch] & 0x00ff) | (val << 8);
            d->left[ch] = (d->count[ch] & 0x3fff) + 1;
        }
        d->count_flip[ch] = !d->count_flip[ch];
        break;
    case 0x209:                                       /* control */
        d->control = val;
        break;
    default:
        break;
    }
}

static void sord_dma_page_write(SordMachineState *s, hwaddr offset,
                                uint8_t val)
{
    /* page registers at 0xE207C1 + n (ch1 at +0, ch3 at +2) */
    static const int chmap[4] = { 1, -1, 3, -1 };
    int idx = offset - 0x7c1;

    if (idx >= 0 && idx < 4 && chmap[idx] >= 0) {
        s->dma.page[chmap[idx]] = val;
    }
}

static uint32_t sord_dma_cur_addr(SordMachineState *s, int ch)
{
    return (s->dma.page[ch] << 16) | s->dma.addr[ch];
}

/* device -> memory; returns bytes accepted (stops at terminal count) */
static int sord_dma_push(SordMachineState *s, int ch, const uint8_t *buf,
                         int len)
{
    SordDma *d = &s->dma;
    int n = MIN((uint32_t)len, d->left[ch]);

    if (n > 0) {
        address_space_write(&address_space_memory, sord_dma_cur_addr(s, ch),
                            MEMTXATTRS_UNSPECIFIED, buf, n);
        /* no carry out of the 64K page: the ROM splits transfers */
        d->addr[ch] += n;
        d->left[ch] -= n;
    }
    return n;
}

/* memory -> device */
static int sord_dma_pull(SordMachineState *s, int ch, uint8_t *buf, int len)
{
    SordDma *d = &s->dma;
    int n = MIN((uint32_t)len, d->left[ch]);

    if (n > 0) {
        address_space_read(&address_space_memory, sord_dma_cur_addr(s, ch),
                           MEMTXATTRS_UNSPECIFIED, buf, n);
        d->addr[ch] += n;
        d->left[ch] -= n;
    }
    return n;
}

/* ----------------------------------------------------- interrupts */

/*
 * Vectored interrupt controller (stub): sources 0-7 map to vectors
 * 0x40-0x47 (base programmed via 0xE203E1).  Known sources: 2 =
 * keyboard ACIA rx, 5 = FDC, 6 = HD.
 */
static void sord_intc_update(SordMachineState *s)
{
    if (s->intc_pending) {
        int src = ctz32(s->intc_pending);

        m68k_set_irq_level(s->cpu, SORD_INTC_IPL, 0x40 + src);
    } else {
        m68k_set_irq_level(s->cpu, 0, 0x40);
    }
}

static void sord_intc_set(SordMachineState *s, int src, bool level)
{
    if (level) {
        s->intc_pending |= 1 << src;
    } else {
        s->intc_pending &= ~(1 << src);
    }
    sord_intc_update(s);
}

/* ---------------------------------------------------------------- FDC */

static void sord_fdc_set_irq(SordMachineState *s, bool level)
{
    s->fdc.irq = level;
    sord_intc_set(s, SORD_INTC_SRC_FDC, level);
}

static uint8_t sord_fdc_msr(SordMachineState *s)
{
    switch (s->fdc.phase) {
    case FDC_COMMAND:
        return 0x90;            /* RQM | CB */
    case FDC_RESULT:
        return 0xd0;            /* RQM | DIO | CB */
    default:
        return 0x80;            /* RQM */
    }
}

/*
 * Track layout: cylinder 0 head 0 is FM with 128-byte sectors, all
 * other tracks MFM with 256-byte sectors, 26 sectors per track.
 */
static void sord_fd_track_geom(int c, int h, bool *fm, int *ssz, int *n)
{
    if (c == 0 && h == 0) {
        *fm = true;
        *ssz = SORD_FD_SD_SSZ;
        *n = 0;
    } else {
        *fm = false;
        *ssz = SORD_FD_DD_SSZ;
        *n = 1;
    }
}

static int64_t sord_fd_offset(int c, int h, int r)
{
    if (c >= SORD_FD_CYLS || h >= SORD_FD_HEADS || r < 1 || r > SORD_FD_SPT) {
        return -1;
    }
    if (c == 0 && h == 0) {
        return (r - 1) * SORD_FD_SD_SSZ;
    }
    return SORD_FD_SD_TRK + (int64_t)(c * 2 + h - 1) * SORD_FD_DD_TRK +
           (r - 1) * SORD_FD_DD_SSZ;
}

static void sord_fdc_result(SordMachineState *s, int len)
{
    s->fdc.res_len = len;
    s->fdc.res_idx = 0;
    s->fdc.phase = FDC_RESULT;
}

/* READ DATA (0x06) / WRITE DATA (0x05) */
static void sord_fdc_rw_data(SordMachineState *s, bool is_write)
{
    SordFdc *f = &s->fdc;
    int unit = f->cmd[1] & 3;
    int head = (f->cmd[1] >> 2) & 1;
    int c = f->cmd[2];
    int r = f->cmd[4];
    int eot = f->cmd[6];
    bool mfm = f->cmd[0] & 0x40;
    SordFdcDrive *dr = &f->drive[unit];
    uint8_t st0 = (head << 2) | unit;
    uint8_t st1 = 0, st2 = 0;
    uint8_t buf[SORD_FD_DD_SSZ];
    bool track_fm;
    int ssz, n;

    sord_fd_track_geom(c, head, &track_fm, &ssz, &n);

    if (!dr->blk || !blk_is_inserted(dr->blk) || track_fm == mfm) {
        /* no media, or wrong recording mode: no address mark found */
        st0 |= 0x40;
        st1 |= 0x01;
        goto out;
    }

    for (;;) {
        int64_t off = sord_fd_offset(c, head, r);
        int done;

        if (off < 0) {
            st0 |= 0x40;
            st1 |= 0x04;        /* no data */
            break;
        }
        if (is_write) {
            done = sord_dma_pull(s, 1, buf, ssz);
            if (done < ssz) {
                memset(buf + done, 0, ssz - done);
            }
            if (blk_pwrite(dr->blk, off, ssz, buf, 0) < 0) {
                st0 |= 0x40;
                st1 |= 0x02;    /* not writable */
                break;
            }
        } else {
            if (blk_pread(dr->blk, off, ssz, buf, 0) < 0) {
                st0 |= 0x40;
                st1 |= 0x04;
                break;
            }
            done = sord_dma_push(s, 1, buf, ssz);
        }
        if (done < ssz || s->dma.left[1] == 0) {
            /* terminal count: normal completion */
            r++;
            break;
        }
        if (r >= eot) {
            /* ran off the end of the track without TC */
            st0 |= 0x40;
            st1 |= 0x80;        /* end of cylinder */
            r++;
            break;
        }
        r++;
    }

out:
    f->res[0] = st0;
    f->res[1] = st1;
    f->res[2] = st2;
    f->res[3] = c;
    f->res[4] = head;
    f->res[5] = r;
    f->res[6] = f->cmd[5];
    sord_fdc_result(s, 7);
    sord_fdc_set_irq(s, true);
}

static void sord_fdc_read_id(SordMachineState *s)
{
    SordFdc *f = &s->fdc;
    int unit = f->cmd[1] & 3;
    int head = (f->cmd[1] >> 2) & 1;
    bool mfm = f->cmd[0] & 0x40;
    SordFdcDrive *dr = &f->drive[unit];
    uint8_t st0 = (head << 2) | unit;
    uint8_t st1 = 0;
    bool track_fm;
    int ssz, n;

    sord_fd_track_geom(dr->cyl, head, &track_fm, &ssz, &n);

    if (!dr->blk || !blk_is_inserted(dr->blk) || track_fm == mfm) {
        st0 |= 0x40;
        st1 |= 0x01;            /* missing address mark */
        n = 0;
    }

    f->res[0] = st0;
    f->res[1] = st1;
    f->res[2] = 0;
    f->res[3] = dr->cyl;
    f->res[4] = head;
    f->res[5] = 1;
    f->res[6] = n;
    sord_fdc_result(s, 7);
    sord_fdc_set_irq(s, true);
}

static void sord_fdc_execute(SordMachineState *s)
{
    SordFdc *f = &s->fdc;
    int unit = f->cmd[1] & 3;
    SordFdcDrive *dr = &f->drive[unit];
    int i;

    f->phase = FDC_IDLE;

    if (getenv("SORD_FDC_TRACE")) {
        fprintf(stderr, "fdc: cmd");
        for (i = 0; i < f->cmd_len; i++) {
            fprintf(stderr, " %02x", f->cmd[i]);
        }
        fprintf(stderr, "\n");
    }

    switch (f->cmd[0] & 0x1f) {
    case 0x03:                                  /* SPECIFY */
        break;
    case 0x07:                                  /* RECALIBRATE */
        dr->cyl = 0;
        dr->seek_int = true;
        dr->seek_st0 = 0x20 | unit;
        sord_fdc_set_irq(s, true);
        break;
    case 0x0f:                                  /* SEEK */
        dr->cyl = f->cmd[2];
        dr->seek_int = true;
        dr->seek_st0 = 0x20 | (f->cmd[1] & 7);
        sord_fdc_set_irq(s, true);
        break;
    case 0x08:                                  /* SENSE INTERRUPT STATUS */
        for (i = 0; i < 4; i++) {
            if (f->drive[i].seek_int) {
                break;
            }
        }
        if (i < 4) {
            f->drive[i].seek_int = false;
            f->res[0] = f->drive[i].seek_st0;
            f->res[1] = f->drive[i].cyl;
            sord_fdc_result(s, 2);
            for (i = 0; i < 4; i++) {
                if (f->drive[i].seek_int) {
                    break;
                }
            }
            if (i == 4) {
                sord_fdc_set_irq(s, false);
            }
        } else {
            f->res[0] = 0x80;                   /* invalid command */
            sord_fdc_result(s, 1);
            sord_fdc_set_irq(s, false);
        }
        break;
    case 0x04:                                  /* SENSE DRIVE STATUS */
        f->res[0] = (f->cmd[1] & 7) | 0x08 |
                    (dr->cyl == 0 ? 0x10 : 0) |
                    ((dr->blk && blk_is_inserted(dr->blk)) ? 0x20 : 0);
        sord_fdc_result(s, 1);
        break;
    case 0x0a:                                  /* READ ID */
        sord_fdc_read_id(s);
        break;
    case 0x06:                                  /* READ DATA */
        sord_fdc_rw_data(s, false);
        break;
    case 0x05:                                  /* WRITE DATA */
        sord_fdc_rw_data(s, true);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sord-fdc: unimplemented command 0x%02x\n",
                      f->cmd[0]);
        f->res[0] = 0x80;
        sord_fdc_result(s, 1);
        break;
    }
}

static const uint8_t sord_fdc_cmd_len[32] = {
    [0x03] = 3,     /* SPECIFY */
    [0x04] = 2,     /* SENSE DRIVE STATUS */
    [0x05] = 9,     /* WRITE DATA */
    [0x06] = 9,     /* READ DATA */
    [0x07] = 2,     /* RECALIBRATE */
    [0x08] = 1,     /* SENSE INTERRUPT STATUS */
    [0x0a] = 2,     /* READ ID */
    [0x0f] = 3,     /* SEEK */
};

static void sord_fdc_data_write(SordMachineState *s, uint8_t val)
{
    SordFdc *f = &s->fdc;

    if (f->phase == FDC_RESULT) {
        /* command written over an unread result: drop the result */
        f->phase = FDC_IDLE;
    }
    if (f->phase == FDC_IDLE) {
        f->cmd[0] = val;
        f->cmd_idx = 1;
        f->cmd_len = sord_fdc_cmd_len[val & 0x1f];
        if (f->cmd_len == 0) {
            f->cmd_len = 1;
        }
        f->phase = FDC_COMMAND;
    } else {
        if (f->cmd_idx < (int)sizeof(f->cmd)) {
            f->cmd[f->cmd_idx] = val;
        }
        f->cmd_idx++;
    }
    if (f->cmd_idx >= f->cmd_len) {
        sord_fdc_execute(s);
    }
}

static uint8_t sord_fdc_data_read(SordMachineState *s)
{
    SordFdc *f = &s->fdc;
    uint8_t val = 0;

    if (f->phase == FDC_RESULT && f->res_idx < f->res_len) {
        val = f->res[f->res_idx++];
        if (f->res_idx >= f->res_len) {
            f->phase = FDC_IDLE;
            sord_fdc_set_irq(s, false);
        }
    }
    return val;
}

/* -------------------------------------------------------- system I/O */

static uint64_t sord_sysio_read(void *opaque, hwaddr offset, unsigned size)
{
    SordMachineState *s = opaque;

    switch (offset) {
    case 0x281:                 /* FDC main status */
        return sord_fdc_msr(s);
    case 0x283:                 /* FDC data */
        return sord_fdc_data_read(s);
    case 0x301:                 /* addressable latch readback */
        return s->latch;
    case 0x30d:                 /* RTC data: not battery backed, 0 */
        return 0;
    case 0x781:                 /* configuration straps */
        return s->strap;
    default:
        return 0;
    }
}

static void sord_sysio_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    SordMachineState *s = opaque;
    uint8_t val = value;

    switch (offset) {
    case 0x201 ... 0x21f:
        sord_dma_write(s, offset, val);
        break;
    case 0x283:
        sord_fdc_data_write(s, val);
        break;
    case 0x301:
        /* LS259-style: bits2-0 select the bit, bit3 is the data */
        if (val & 0x08) {
            s->latch |= 1 << (val & 7);
        } else {
            s->latch &= ~(1 << (val & 7));
        }
        break;
    case 0x30b:
        s->rtc_addr = val;
        break;
    case 0x305: case 0x307: case 0x30d: case 0x30f:
    case 0x381: case 0x3a1: case 0x3c1: case 0x3e1:   /* intc */
    case 0x7a0: case 0x7a1:                           /* strap latch */
        break;
    case 0x7c1 ... 0x7c7:
        sord_dma_page_write(s, offset, val);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sord: sysio write 0x%02x -> 0x%06" HWADDR_PRIx "\n",
                      val, SORD_SYSIO_BASE + offset);
        break;
    }
}

static const MemoryRegionOps sord_sysio_ops = {
    .read = sord_sysio_read,
    .write = sord_sysio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* ------------------------------------------------- CRTC/ACIA I/O */

static void sord_acia_update_irq(SordMachineState *s, int n)
{
    SordAcia *a = &s->acia[n];
    bool irq = (a->ctrl & 0x80) && a->rxfull;   /* RIE and RDRF */

    if (n == 0) {
        sord_intc_set(s, SORD_INTC_SRC_KBD, irq);
    }
}

static uint64_t sord_acia_read(SordMachineState *s, int n, hwaddr reg)
{
    SordAcia *a = &s->acia[n];

    if (reg == 0) {
        /* status: TDRE always, RDRF when a byte is buffered, bit7 IRQ */
        return 0x02 | (a->rxfull ? 0x01 : 0) |
               (((a->ctrl & 0x80) && a->rxfull) ? 0x80 : 0);
    }
    a->rxfull = false;
    sord_acia_update_irq(s, n);
    qemu_chr_fe_accept_input(&a->chr);
    return a->rxbuf;
}

static void sord_acia_write(SordMachineState *s, int n, hwaddr reg,
                            uint8_t val)
{
    SordAcia *a = &s->acia[n];

    if (reg == 0) {
        if ((val & 0x03) == 0x03) {             /* master reset */
            a->rxfull = false;
        }
        a->ctrl = val;
        sord_acia_update_irq(s, n);
    } else {
        qemu_chr_fe_write_all(&a->chr, &val, 1);
    }
}

static uint64_t sord_ctlio_read(void *opaque, hwaddr offset, unsigned size)
{
    SordMachineState *s = opaque;

    switch (offset) {
    case 0x001:
        return s->crtc_addr;
    case 0x003:
        return s->crtc[s->crtc_addr & 0x1f];
    case 0x141:
    case 0x143:
        return sord_acia_read(s, 1, (offset - 0x141) >> 1);
    case 0x161:
    case 0x163:
        return sord_acia_read(s, 0, (offset - 0x161) >> 1);
    default:
        return 0;
    }
}

static void sord_ctlio_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    SordMachineState *s = opaque;
    uint8_t val = value;

    switch (offset) {
    case 0x001:
        s->crtc_addr = val;
        break;
    case 0x003:
        s->crtc[s->crtc_addr & 0x1f] = val;
        break;
    case 0x081 ... 0x08f:       /* movep-programmed timer(?), ignored */
        break;
    case 0x141:
    case 0x143:
        sord_acia_write(s, 1, (offset - 0x141) >> 1, val);
        break;
    case 0x161:
    case 0x163:
        sord_acia_write(s, 0, (offset - 0x161) >> 1, val);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sord: ctlio write 0x%02x -> 0x%06" HWADDR_PRIx "\n",
                      val, SORD_CTLIO_BASE + offset);
        break;
    }
}

static const MemoryRegionOps sord_ctlio_ops = {
    .read = sord_ctlio_read,
    .write = sord_ctlio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static uint64_t sord_pal_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void sord_pal_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
}

static const MemoryRegionOps sord_pal_ops = {
    .read = sord_pal_read,
    .write = sord_pal_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------ ACIA rx */

static int sord_acia0_can_receive(void *opaque)
{
    SordMachineState *s = opaque;

    return s->acia[0].rxfull ? 0 : 1;
}

static void sord_acia0_receive(void *opaque, const uint8_t *buf, int size)
{
    SordMachineState *s = opaque;

    if (size > 0) {
        s->acia[0].rxbuf = buf[0];
        s->acia[0].rxfull = true;
        sord_acia_update_irq(s, 0);
    }
}

static int sord_acia1_can_receive(void *opaque)
{
    SordMachineState *s = opaque;

    return s->acia[1].rxfull ? 0 : 1;
}

static void sord_acia1_receive(void *opaque, const uint8_t *buf, int size)
{
    SordMachineState *s = opaque;

    if (size > 0) {
        s->acia[1].rxbuf = buf[0];
        s->acia[1].rxfull = true;
        sord_acia_update_irq(s, 1);
    }
}

/* ------------------------------------------------------------ display */

static bool sord_gfx_update(void *opaque)
{
    SordMachineState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t *dst;
    uint8_t *vram;
    int cursor, stride;

    if (!surface || surface_width(surface) < SORD_COLS * FONT_W) {
        return true;
    }
    dst = surface_data(surface);
    stride = surface_stride(surface) / 4;
    vram = memory_region_get_ram_ptr(&s->vram);
    cursor = (s->crtc[14] << 8) | s->crtc[15];

    for (int row = 0; row < SORD_ROWS; row++) {
        for (int col = 0; col < SORD_COLS; col++) {
            int cell = row * SORD_COLS + col;
            uint8_t attr = vram[cell * 4 + 1];
            uint8_t ch = vram[cell * 4 + 3];
            const uint8_t *glyph = &vgafont16[ch * FONT_H];
            uint32_t fg = (attr & 0x0f) ? 0xffe8e8e8 : 0xff404040;
            uint32_t bg = 0xff000000;

            if (cell == cursor) {
                uint32_t t = fg;
                fg = bg;
                bg = t;
            }
            for (int y = 0; y < FONT_H; y++) {
                uint32_t *line = dst + (row * FONT_H + y) * stride +
                                 col * FONT_W;
                uint8_t bits = glyph[y];

                for (int x = 0; x < FONT_W; x++) {
                    line[x] = (bits & (0x80 >> x)) ? fg : bg;
                }
            }
        }
    }
    qemu_console_update(s->con, 0, 0, SORD_COLS * FONT_W,
                        SORD_ROWS * FONT_H);
    return true;
}

static void sord_gfx_invalidate(void *opaque)
{
}

static const GraphicHwOps sord_gfx_ops = {
    .invalidate = sord_gfx_invalidate,
    .gfx_update = sord_gfx_update,
};


/*
 * Minimal QOM wrapper that owns the floppy block backends (so the
 * -fda/-fdb drives are properly claimed); controller state lives in
 * the machine.
 */
#define TYPE_SORD_FDC "sord-fdc"
OBJECT_DECLARE_SIMPLE_TYPE(SordFdcDevice, SORD_FDC)

struct SordFdcDevice {
    SysBusDevice parent_obj;
    BlockBackend *blk[2];
};

static const Property sord_fdc_properties[] = {
    DEFINE_PROP_DRIVE("driveA", SordFdcDevice, blk[0]),
    DEFINE_PROP_DRIVE("driveB", SordFdcDevice, blk[1]),
};

static void sord_fdc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->user_creatable = false;
    device_class_set_props(dc, sord_fdc_properties);
}

static const TypeInfo sord_fdc_typeinfo = {
    .name = TYPE_SORD_FDC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SordFdcDevice),
    .class_init = sord_fdc_class_init,
};

/*
 * Minimal QOM wrapper so the graphic console has a DeviceState; all
 * actual state lives in the machine.
 */
#define TYPE_SORD_VIDEO "sord-video"
OBJECT_DECLARE_SIMPLE_TYPE(SordVideoState, SORD_VIDEO)

struct SordVideoState {
    SysBusDevice parent_obj;
};

static void sord_video_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->user_creatable = false;
}

static const TypeInfo sord_video_typeinfo = {
    .name = TYPE_SORD_VIDEO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SordVideoState),
    .class_init = sord_video_class_init,
};

/* -------------------------------------------------------------- reset */


static void sord_machine_reset(void *opaque)
{
    SordMachineState *s = opaque;
    CPUM68KState *env = &s->cpu->env;
    static const uint8_t disp_id = 0xff;

    cpu_reset(CPU(s->cpu));
    /* SP/PC are fetched from the first two ROM longwords at reset */
    env->aregs[7] = s->reset_sp;
    env->sp[env->current_sp] = s->reset_sp;
    env->pc = s->reset_pc;

    s->latch = 0;
    s->crtc_addr = 0;
    memset(s->crtc, 0, sizeof(s->crtc));
    memset(&s->dma, 0, sizeof(s->dma));

    s->fdc.phase = FDC_IDLE;
    s->fdc.irq = false;
    for (int i = 0; i < 4; i++) {
        s->fdc.drive[i].cyl = 0;
        s->fdc.drive[i].seek_int = false;
    }

    s->acia[0].rxfull = false;
    s->acia[0].ctrl = 0;
    s->acia[1].rxfull = false;
    s->acia[1].ctrl = 0;
    s->intc_pending = 0;

    /*
     * Display-type ID bytes at 0xE12001 (odd): the ROM switches to
     * Japanese/kanji message mode when they all read zero; a nonzero
     * byte selects the ASCII message set.
     */
    address_space_write(&address_space_memory, SORD_VRAM_BASE + 0x2001,
                        MEMTXATTRS_UNSPECIFIED, &disp_id, 1);
}

/* --------------------------------------------------------------- init */

static void sord_machine_init(MachineState *machine)
{
    SordMachineState *s = SORD_M68MX_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    const char *bios_name;
    char *filename;
    int bios_size;

    s->cpu = M68K_CPU(cpu_create(machine->cpu_type));

    /* RAM at 0; everything unmapped bus-errors (the ROM sizes RAM so) */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    /* boot ROM */
    memory_region_init_rom(&s->rom, NULL, "sord.rom",
                           SORD_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, SORD_ROM_BASE, &s->rom);

    bios_name = machine->firmware ?: "m68mx-bootrom.bin";
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, SORD_ROM_BASE,
                                        SORD_ROM_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }
    if (!qtest_enabled()) {
        uint8_t *ptr;

        if (bios_size < 8) {
            error_report("could not load M68MX boot ROM '%s'", bios_name);
            exit(1);
        }
        ptr = rom_ptr(SORD_ROM_BASE, 8);
        assert(ptr != NULL);
        s->reset_sp = ldl_be_p(ptr);
        s->reset_pc = ldl_be_p(ptr + 4);
    }

    /* text VRAM (+ display ID bytes) and the probed 64K RAM bank */
    memory_region_init_ram(&s->vram, NULL, "sord.vram",
                           SORD_VRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, SORD_VRAM_BASE, &s->vram);
    memory_region_init_ram(&s->gram, NULL, "sord.gram",
                           SORD_GRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, SORD_GRAM_BASE, &s->gram);

    /* I/O */
    memory_region_init_io(&s->sysio, OBJECT(machine), &sord_sysio_ops, s,
                          "sord.sysio", SORD_SYSIO_SIZE);
    memory_region_add_subregion(sysmem, SORD_SYSIO_BASE, &s->sysio);
    memory_region_init_io(&s->ctlio, OBJECT(machine), &sord_ctlio_ops, s,
                          "sord.ctlio", SORD_CTLIO_SIZE);
    memory_region_add_subregion(sysmem, SORD_CTLIO_BASE, &s->ctlio);
    memory_region_init_io(&s->pal, OBJECT(machine), &sord_pal_ops, s,
                          "sord.pal", SORD_PAL_SIZE);
    memory_region_add_subregion(sysmem, SORD_PAL_BASE, &s->pal);

    /* serial: ACIA0 is the Sunbug/loader console, ACIA1 auxiliary */
    if (serial_hd(0)) {
        qemu_chr_fe_init(&s->acia[0].chr, serial_hd(0), &error_fatal);
        qemu_chr_fe_set_handlers(&s->acia[0].chr, sord_acia0_can_receive,
                                 sord_acia0_receive, NULL, NULL, s, NULL,
                                 true);
    }
    if (serial_hd(1)) {
        qemu_chr_fe_init(&s->acia[1].chr, serial_hd(1), &error_fatal);
        qemu_chr_fe_set_handlers(&s->acia[1].chr, sord_acia1_can_receive,
                                 sord_acia1_receive, NULL, NULL, s, NULL,
                                 true);
    }

    /* floppies */
    {
        DeviceState *fdev = qdev_new(TYPE_SORD_FDC);

        for (int i = 0; i < 2; i++) {
            DriveInfo *dinfo = drive_get(IF_FLOPPY, 0, i);

            if (dinfo) {
                qdev_prop_set_drive_err(fdev, i ? "driveB" : "driveA",
                                        blk_by_legacy_dinfo(dinfo),
                                        &error_fatal);
            }
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(fdev), &error_fatal);
        for (int i = 0; i < 2; i++) {
            s->fdc.drive[i].blk = SORD_FDC(fdev)->blk[i];
        }
    }

    /* display */
    {
        DeviceState *vdev = qdev_new(TYPE_SORD_VIDEO);

        sysbus_realize_and_unref(SYS_BUS_DEVICE(vdev), &error_fatal);
        s->con = qemu_graphic_console_create(vdev, 0, &sord_gfx_ops, s);
    }
    qemu_console_resize(s->con, SORD_COLS * FONT_W, SORD_ROWS * FONT_H);

    qemu_register_reset(sord_machine_reset, s);
}

static void sord_instance_init(Object *obj)
{
    SordMachineState *s = SORD_M68MX_MACHINE(obj);

    s->strap = 0x82;            /* boot device: floppy */
    object_property_add_uint8_ptr(obj, "strap", &s->strap,
                                  OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "strap",
        "Configuration strap byte (default 0x82: boot from floppy; "
        "0xE2: Sunbug monitor on serial)");
}

static const char *const sord_valid_cpu_types[] = {
    M68K_CPU_TYPE_NAME("m68000"),
    NULL
};

static void sord_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SORD M68MX (68000)";
    mc->init = sord_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68000");
    mc->valid_cpu_types = sord_valid_cpu_types;
    mc->max_cpus = 1;
    mc->default_ram_id = "sord.ram";
    mc->default_ram_size = 1 * MiB;
    mc->block_default_type = IF_FLOPPY;
    mc->no_parallel = 1;
}

static const TypeInfo sord_machine_typeinfo = {
    .name = TYPE_SORD_M68MX_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_init = sord_instance_init,
    .instance_size = sizeof(SordMachineState),
    .class_init = sord_machine_class_init,
};

static void sord_machine_register_types(void)
{
    type_register_static(&sord_fdc_typeinfo);
    type_register_static(&sord_video_typeinfo);
    type_register_static(&sord_machine_typeinfo);
}

type_init(sord_machine_register_types)
