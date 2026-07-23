/*
 * IWM (Integrated Woz Machine) floppy controller with one Sony 400K
 * GCR drive, as found in the original Macintosh 128K.
 *
 * The IWM is a bag of one-bit latches toggled by *touching* addresses:
 * register N of the 16 in the decode window is at base + N*512, and an
 * access (read or write, the data doesn't matter for even registers)
 * sets latch N>>1 to N&1.  The latches are CA0/CA1/CA2/LSTRB (the four
 * "phase" lines running to the drive), /ENBL (drive enable), the
 * internal/external drive select, and Q6/Q7 which pick the visible
 * register (data/status/handshake/mode).
 *
 * The Sony drive itself is addressed through the phase lines plus the
 * SEL line that arrives from VIA port A bit 5:
 *   - reading the IWM status register returns, in bit 7, the drive
 *     status line selected by {CA2,CA1,CA0,SEL};
 *   - pulsing LSTRB writes CA2 into the drive control register selected
 *     by {CA1,CA0,SEL} (step direction, step, motor, eject).
 *
 * Reads deal in raw GCR: the ROM's Sony driver polls the data register
 * for bytes with the MSB set and decodes address/data fields itself, so
 * the drive model GCR-encodes the 512-byte sectors of a raw 400K image
 * on the fly, one track at a time (mirroring what amiga_fdc.c does for
 * ADF-to-MFM).  The variable-speed zones only affect how many sectors a
 * track holds (12,11,10,9,8 per 16-track zone); the tachometer reports
 * the nominally correct speed for the zone so the ROM's speed servo is
 * always satisfied, regardless of the PWM value the Mac generates.
 *
 * Deliberate shortcut: the disk is always reported write-protected, so
 * the ROM/OS never tries to write and the GCR decode path (and flux-
 * level write timing) doesn't need to exist.  Good enough to boot and
 * run a System from a locked floppy, not enough to save files.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "hw/block/iwm.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"

/* IWM latch numbers (== address-line register number >> 1) */
#define IWM_L_CA0       0
#define IWM_L_CA1       1
#define IWM_L_CA2       2
#define IWM_L_LSTRB     3
#define IWM_L_ENABLE    4
#define IWM_L_EXTDRIVE  5
#define IWM_L_Q6        6
#define IWM_L_Q7        7

#define IWM_CA0         (1 << IWM_L_CA0)
#define IWM_CA1         (1 << IWM_L_CA1)
#define IWM_CA2         (1 << IWM_L_CA2)
#define IWM_LSTRB       (1 << IWM_L_LSTRB)
#define IWM_ENABLE      (1 << IWM_L_ENABLE)
#define IWM_EXTDRIVE    (1 << IWM_L_EXTDRIVE)
#define IWM_Q6          (1 << IWM_L_Q6)
#define IWM_Q7          (1 << IWM_L_Q7)

/*
 * Sony drive status register addresses, {CA2,CA1,CA0,SEL}.  All the
 * "active" states are low unless noted.
 */
#define SONY_DIRTN      0x0     /* step direction, 0 = toward track 79 */
#define SONY_CSTIN      0x1     /* 0 = disk inserted */
#define SONY_STEP       0x2     /* 1 = head step complete */
#define SONY_WRTPRT     0x3     /* 0 = write protected */
#define SONY_MOTORON    0x4     /* 0 = spindle motor running */
#define SONY_TK0        0x5     /* 0 = head at track 0 */
#define SONY_SWITCHED   0x6     /* 1 = disk was changed */
#define SONY_TACH       0x7     /* tachometer, 60 pulses/revolution */
#define SONY_RDDATA0    0x8     /* instantaneous read head, side 0 */
#define SONY_RDDATA1    0x9     /* instantaneous read head, side 1 */
#define SONY_SIDES      0xc     /* 0 = single sided drive */
#define SONY_DRVIN      0xf     /* 0 = drive present */

/* Sony drive control registers, {CA1,CA0,SEL} with CA2 as the data */
#define SONY_CMD_DIRTN      0x0 /* CA2=0: step up-track, CA2=1: down */
#define SONY_CMD_SWITCHED   0x1 /* SEL=1, CA2=1: clear disk-switched */
#define SONY_CMD_STEP       0x2 /* CA2=0: step one track */
#define SONY_CMD_MOTORON    0x4 /* CA2=0: motor on, CA2=1: off */
#define SONY_CMD_EJECT      0x6 /* CA2=1: eject the disk */

/*
 * Nominal spindle speed per 16-track zone, in RPM.  The tach emits 60
 * pulses per revolution, so pulses/second == RPM.
 */
static const unsigned sony_zone_rpm[5] = { 394, 429, 472, 525, 590 };

static int sony_zone_sectors(int track)
{
    return 12 - (track >> 4);
}

/* byte offset of a track's first sector in the raw 409600-byte image */
static int sony_track_offset(int track)
{
    int zone = track >> 4;
    /* 16 tracks of 12, then of 11, ... sectors before this zone */
    int sectors = 16 * (12 * zone - zone * (zone - 1) / 2);

    sectors += (track & 15) * sony_zone_sectors(track);
    return sectors * IWM_SONY_SECTOR_SIZE;
}

/* the classic Apple 6-and-2 GCR code: 6 data bits -> 8 bits on disk */
static const uint8_t gcr6[64] = {
    0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6,
    0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc,
    0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
    0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde,
    0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
    0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6,
    0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
};

/*
 * Nibblize 524 bytes (12 tag + 512 data) the Sony way: bytes are taken
 * three at a time and XOR-whitened with three running checksums that
 * chain their carries into one another (the ROM implements this as a
 * ROL/ADDX chain).  The 6-bit nibbles go to disk as groups of
 * (top-bits, b1, b2, b3); the last group has no third byte.
 */
static void sony_nibblize(const uint8_t *in, uint8_t *b1, uint8_t *b2,
                          uint8_t *b3, uint8_t *csum)
{
    uint32_t c1 = 0, c2 = 0, c3 = 0;
    int i = 0, j = 0;
    uint8_t val;

    while (1) {
        c1 = (c1 & 0xff) << 1;
        if (c1 & 0x100) {
            c1++;
        }

        val = in[i++];
        c3 += val;
        if (c1 & 0x100) {
            c3++;
            c1 &= 0xff;
        }
        b1[j] = (val ^ c1) & 0xff;

        val = in[i++];
        c2 += val;
        if (c3 > 0xff) {
            c2++;
            c3 &= 0xff;
        }
        b2[j] = (val ^ c3) & 0xff;

        if (i == 524) {
            break;
        }

        val = in[i++];
        c1 += val;
        if (c2 > 0xff) {
            c1++;
            c2 &= 0xff;
        }
        b3[j] = (val ^ c2) & 0xff;
        j++;
    }
    b3[174] = 0;

    csum[0] = c1 & 0xff;
    csum[1] = c2 & 0xff;
    csum[2] = c3 & 0xff;
}

static void sony_put_sync(uint8_t **p, int n)
{
    memset(*p, 0xff, n);
    *p += n;
}

/* GCR-encode one 512-byte sector (with zero tags) onto the track */
static void sony_encode_sector(uint8_t **pp, int track, int sector,
                               const uint8_t *data)
{
    uint8_t buf[524];
    uint8_t b1[175], b2[175], b3[175], csum[3];
    uint8_t *p = *pp;
    uint8_t side = (track >> 6) & 1;    /* bit 0 carries track bit 6 */
    uint8_t format = 0x02;              /* single sided, 2:1 interleave */
    int i;

    /* address field */
    sony_put_sync(&p, 8);
    *p++ = 0xd5;
    *p++ = 0xaa;
    *p++ = 0x96;
    *p++ = gcr6[track & 0x3f];
    *p++ = gcr6[sector & 0x3f];
    *p++ = gcr6[side];
    *p++ = gcr6[format];
    *p++ = gcr6[(track ^ sector ^ side ^ format) & 0x3f];
    *p++ = 0xde;
    *p++ = 0xaa;

    /* data field */
    sony_put_sync(&p, 6);
    *p++ = 0xd5;
    *p++ = 0xaa;
    *p++ = 0xad;
    *p++ = gcr6[sector & 0x3f];

    memset(buf, 0, 12);                 /* tag bytes: zero */
    memcpy(buf + 12, data, IWM_SONY_SECTOR_SIZE);
    sony_nibblize(buf, b1, b2, b3, csum);

    for (i = 0; i < 175; i++) {
        *p++ = gcr6[((b1[i] & 0xc0) >> 2 | (b2[i] & 0xc0) >> 4 |
                     (b3[i] & 0xc0) >> 6) & 0x3f];
        *p++ = gcr6[b1[i] & 0x3f];
        *p++ = gcr6[b2[i] & 0x3f];
        if (i < 174) {
            *p++ = gcr6[b3[i] & 0x3f];
        }
    }

    /* checksum group: top-bits nibble first, then c3, c2, c1 */
    *p++ = gcr6[((csum[0] & 0xc0) >> 6 | (csum[1] & 0xc0) >> 4 |
                 (csum[2] & 0xc0) >> 2) & 0x3f];
    *p++ = gcr6[csum[2] & 0x3f];
    *p++ = gcr6[csum[1] & 0x3f];
    *p++ = gcr6[csum[0] & 0x3f];

    *p++ = 0xde;
    *p++ = 0xaa;
    *p++ = 0xff;

    *pp = p;
}

static bool sony_disk_in(IWMState *s)
{
    return s->blk && blk_is_inserted(s->blk) && !s->ejected;
}

static bool sony_load_track(IWMState *s)
{
    uint8_t sectors[12 * IWM_SONY_SECTOR_SIZE];
    int nsect = sony_zone_sectors(s->track);
    uint8_t *p = s->track_buf;
    int i;

    if (!sony_disk_in(s)) {
        return false;
    }
    if (s->cached_track == s->track) {
        return true;
    }
    if (blk_pread(s->blk, sony_track_offset(s->track),
                  nsect * IWM_SONY_SECTOR_SIZE, sectors, 0) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "iwm: read error on track %d\n",
                      s->track);
        return false;
    }
    for (i = 0; i < nsect; i++) {
        sony_encode_sector(&p, s->track, i,
                           sectors + i * IWM_SONY_SECTOR_SIZE);
    }
    assert(p - s->track_buf <= IWM_TRACK_BUF_SIZE);
    s->track_len = p - s->track_buf;
    s->track_pos = 0;
    s->cached_track = s->track;
    return true;
}

/* status line of the selected drive; the external connector is empty */
static int sony_sense(IWMState *s)
{
    /* {CA2,CA1,CA0,SEL} */
    int reg = (!!(s->latches & IWM_CA2) << 3) |
              (!!(s->latches & IWM_CA1) << 2) |
              (!!(s->latches & IWM_CA0) << 1) | !!s->sel;

    if (s->latches & IWM_EXTDRIVE) {
        return 1;               /* nothing out there, lines pulled up */
    }

    switch (reg) {
    case SONY_DIRTN:
        return s->dirtn;
    case SONY_CSTIN:
        return !sony_disk_in(s);
    case SONY_STEP:
        return 1;               /* steps complete instantly */
    case SONY_WRTPRT:
        return 0;               /* always write protected (see header) */
    case SONY_MOTORON:
        return !s->motor_on;
    case SONY_TK0:
        return s->track != 0;
    case SONY_SWITCHED:
        return s->switched;
    case SONY_TACH:
        if (s->motor_on) {
            unsigned rpm = sony_zone_rpm[s->track >> 4];
            int64_t half = NANOSECONDS_PER_SECOND / (2 * rpm);

            return (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / half) & 1;
        }
        return 0;
    case SONY_RDDATA0:
    case SONY_RDDATA1:
        return 0;
    case SONY_SIDES:
        return 0;               /* single sided */
    case SONY_DRVIN:
        return 0;               /* internal drive present */
    default:
        qemu_log_mask(LOG_UNIMP, "iwm: sense of unknown reg 0x%x\n", reg);
        return 1;
    }
}

static void sony_command(IWMState *s)
{
    int reg = (!!(s->latches & IWM_CA1) << 2) |
              (!!(s->latches & IWM_CA0) << 1) | !!s->sel;
    int val = !!(s->latches & IWM_CA2);

    if (s->latches & IWM_EXTDRIVE) {
        return;
    }

    switch ((reg << 1) | val) {
    case SONY_CMD_DIRTN << 1:
        s->dirtn = 0;
        break;
    case (SONY_CMD_DIRTN << 1) | 1:
        s->dirtn = 1;
        break;
    case (SONY_CMD_SWITCHED << 1) | 1:
        s->switched = false;
        break;
    case SONY_CMD_STEP << 1:
        if (s->dirtn) {
            if (s->track > 0) {
                s->track--;
            }
        } else {
            if (s->track < IWM_SONY_TRACKS - 1) {
                s->track++;
            }
        }
        break;
    case SONY_CMD_MOTORON << 1:
        s->motor_on = true;
        break;
    case (SONY_CMD_MOTORON << 1) | 1:
        s->motor_on = false;
        break;
    case (SONY_CMD_EJECT << 1) | 1:
        if (sony_disk_in(s)) {
            s->ejected = true;
            s->switched = true;
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "iwm: unknown command reg %d val %d\n",
                      reg, val);
        break;
    }
}

static uint8_t iwm_data_read(IWMState *s)
{
    uint8_t val;

    if (!(s->latches & IWM_ENABLE)) {
        return 0xff;            /* "read all ones" */
    }
    if ((s->latches & IWM_EXTDRIVE) || !s->motor_on || !sony_load_track(s)) {
        return 0xff;
    }
    val = s->track_buf[s->track_pos];
    s->track_pos = (s->track_pos + 1) % s->track_len;
    return val;
}

/* every access, read or write, moves the addressed latch */
static void iwm_touch(IWMState *s, hwaddr addr)
{
    int reg = (addr >> 9) & 0xf;
    int latch = reg >> 1;
    bool on = reg & 1;
    uint8_t old = s->latches;

    if (on) {
        s->latches |= 1 << latch;
    } else {
        s->latches &= ~(1 << latch);
    }

    if (latch == IWM_L_LSTRB && !(old & IWM_LSTRB) && on) {
        sony_command(s);
    }
}

static uint64_t iwm_read(void *opaque, hwaddr addr, unsigned size)
{
    IWMState *s = opaque;

    iwm_touch(s, addr);

    switch (s->latches & (IWM_Q6 | IWM_Q7)) {
    case 0:
        return iwm_data_read(s);
    case IWM_Q6:
        /* status: sense in bit 7, enable in bit 5, mode low bits */
        return (sony_sense(s) << 7) |
               ((s->latches & IWM_ENABLE) ? 0x20 : 0) |
               (s->mode & 0x1f);
    case IWM_Q7:
        /* write handshake: buffer ready, no underrun */
        return 0xc0;
    case IWM_Q6 | IWM_Q7:
    default:
        return 0;
    }
}

static void iwm_write(void *opaque, hwaddr addr, uint64_t value,
                      unsigned size)
{
    IWMState *s = opaque;

    iwm_touch(s, addr);

    if ((s->latches & (IWM_Q6 | IWM_Q7)) == (IWM_Q6 | IWM_Q7)) {
        if (s->latches & IWM_ENABLE) {
            /* data write: unsupported, the disk reads as protected */
            qemu_log_mask(LOG_UNIMP, "iwm: dropped data write 0x%02x\n",
                          (unsigned)value);
        } else {
            s->mode = value;
        }
    }
}

static const MemoryRegionOps iwm_ops = {
    .read = iwm_read,
    .write = iwm_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

static void iwm_set_sel(void *opaque, int n, int level)
{
    IWMState *s = opaque;

    s->sel = level;
}

static void iwm_change_media(void *opaque, bool load, Error **errp)
{
    IWMState *s = opaque;

    if (load && blk_getlength(s->blk) != IWM_SONY_IMAGE_SIZE) {
        error_setg(errp, "floppy image must be a raw %d byte 400K image",
                   IWM_SONY_IMAGE_SIZE);
        return;
    }
    s->ejected = false;
    s->switched = true;
    s->cached_track = -1;
}

static const BlockDevOps iwm_block_ops = {
    .change_media_cb = iwm_change_media,
};

static void iwm_reset(DeviceState *dev)
{
    IWMState *s = IWM(dev);

    s->latches = 0;
    s->sel = false;
    s->mode = 0;
    s->track = 0;
    s->dirtn = false;
    s->motor_on = false;
    s->ejected = false;
    s->switched = false;
    s->cached_track = -1;
    s->track_len = 0;
    s->track_pos = 0;
}

static void iwm_realize(DeviceState *dev, Error **errp)
{
    IWMState *s = IWM(dev);

    if (s->blk) {
        if (blk_is_inserted(s->blk) &&
            blk_getlength(s->blk) != IWM_SONY_IMAGE_SIZE) {
            error_setg(errp, "floppy image must be a raw %d byte 400K image",
                       IWM_SONY_IMAGE_SIZE);
            return;
        }
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ, BLK_PERM_ALL,
                         errp) < 0) {
            return;
        }
        blk_set_dev_ops(s->blk, &iwm_block_ops, s);
    }
}

static void iwm_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    IWMState *s = IWM(obj);

    memory_region_init_io(&s->mem, obj, &iwm_ops, s, "iwm", 0x2000);
    sysbus_init_mmio(sbd, &s->mem);
    qdev_init_gpio_in_named(DEVICE(obj), iwm_set_sel, "sel", 1);
}

static const VMStateDescription vmstate_iwm = {
    .name = "iwm",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(latches, IWMState),
        VMSTATE_BOOL(sel, IWMState),
        VMSTATE_UINT8(mode, IWMState),
        VMSTATE_UINT8(track, IWMState),
        VMSTATE_BOOL(dirtn, IWMState),
        VMSTATE_BOOL(motor_on, IWMState),
        VMSTATE_BOOL(ejected, IWMState),
        VMSTATE_BOOL(switched, IWMState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property iwm_properties[] = {
    DEFINE_PROP_DRIVE("drive", IWMState, blk),
};

static void iwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = iwm_realize;
    device_class_set_legacy_reset(dc, iwm_reset);
    dc->vmsd = &vmstate_iwm;
    device_class_set_props(dc, iwm_properties);
}

static const TypeInfo iwm_info = {
    .name          = TYPE_IWM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IWMState),
    .instance_init = iwm_init,
    .class_init    = iwm_class_init,
};

static void iwm_register_types(void)
{
    type_register_static(&iwm_info);
}

type_init(iwm_register_types)
