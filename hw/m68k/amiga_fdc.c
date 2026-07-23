/*
 * Amiga floppy drive.
 *
 * This models the drive mechanics as seen through the CIA port pins
 * (select, motor, stepping, side, and the status lines), backed by a
 * plain ADF image: 3.5" double density, 11 sectors of 512 bytes per
 * track, 80 cylinders, 2 heads.
 *
 * Paula's disk DMA deals in raw MFM, so the drive encodes the track
 * under the head into AmigaDOS MFM sector format on the fly for reads,
 * and decodes MFM back into sectors for writes.  Only the standard
 * AmigaDOS format is produced, which is what trackdisk.device and the
 * vast majority of custom trackloaders use.
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
#include "hw/m68k/amiga_fdc.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"

/* 300 rpm: one index pulse every 200ms */
#define DRIVE_ROTATION_NS       (NANOSECONDS_PER_SECOND / 5)

/*
 * MFM long-word encoding: the data bits of a longword sit in the even
 * bit positions, the odd positions carry the clock.  A clock bit is
 * set between two zero data bits.  The clock bit above the top data
 * bit depends on the last data bit of the previously written long.
 */
#define MFM_DATA_MASK           0x55555555u
#define MFM_CLOCK_MASK          0xaaaaaaaau

/* the info longword identifying each sector */
#define MFM_INFO_FORMAT_AMIGA   0xff

/* byte offsets of the odd/even encoded fields behind the sync words */
#define MFM_SEC_INFO            0
#define MFM_SEC_LABEL           8
#define MFM_SEC_HEADER_CK       40
#define MFM_SEC_DATA_CK         48
#define MFM_SEC_DATA            56
#define MFM_SEC_PAYLOAD_BYTES   (MFM_SEC_DATA + 2 * ADF_SECTOR_SIZE)

static uint32_t mfm_clock_bits(uint32_t data, uint32_t prev)
{
    return ~((data << 1) | (data >> 1) | (prev << 31)) & MFM_CLOCK_MASK;
}

static void mfm_put_long(uint8_t **p, uint32_t *prev, uint32_t data)
{
    uint32_t enc = data | mfm_clock_bits(data, *prev);

    stl_be_p(*p, enc);
    *p += 4;
    *prev = enc;
}

/* gap and sync words carry fixed clock patterns; emit them verbatim */
static void mfm_put_word_raw(uint8_t **p, uint32_t *prev, uint16_t word)
{
    stw_be_p(*p, word);
    *p += 2;
    *prev = word;
}

static uint32_t mfm_odd(uint32_t val)
{
    return (val >> 1) & MFM_DATA_MASK;
}

static uint32_t mfm_even(uint32_t val)
{
    return val & MFM_DATA_MASK;
}

/*
 * Encode one 512-byte sector in AmigaDOS format: gap, sync, then the
 * info longword, the label area, both checksums and the data, each
 * split into a block of odd data bits followed by the even ones.
 */
static void mfm_encode_sector(uint8_t **p, uint32_t *prev, unsigned track,
                              unsigned sector, const uint8_t *data)
{
    uint32_t info = (MFM_INFO_FORMAT_AMIGA << 24) | (track << 16) |
                    (sector << 8) | (ADF_TRACK_SECTORS - sector);
    uint32_t header_ck, data_ck;
    unsigned i;

    mfm_put_word_raw(p, prev, MFM_GAP_WORD);
    mfm_put_word_raw(p, prev, MFM_GAP_WORD);
    mfm_put_word_raw(p, prev, MFM_SYNC);
    mfm_put_word_raw(p, prev, MFM_SYNC);

    mfm_put_long(p, prev, mfm_odd(info));
    mfm_put_long(p, prev, mfm_even(info));
    header_ck = mfm_odd(info) ^ mfm_even(info);

    /* the label area is unused (zero) under AmigaDOS */
    for (i = 0; i < 8; i++) {
        mfm_put_long(p, prev, 0);
    }

    data_ck = 0;
    for (i = 0; i < ADF_SECTOR_SIZE / 4; i++) {
        uint32_t val = ldl_be_p(data + i * 4);

        data_ck ^= mfm_odd(val) ^ mfm_even(val);
    }

    mfm_put_long(p, prev, mfm_odd(header_ck));
    mfm_put_long(p, prev, mfm_even(header_ck));
    mfm_put_long(p, prev, mfm_odd(data_ck));
    mfm_put_long(p, prev, mfm_even(data_ck));

    for (i = 0; i < ADF_SECTOR_SIZE / 4; i++) {
        mfm_put_long(p, prev, mfm_odd(ldl_be_p(data + i * 4)));
    }
    for (i = 0; i < ADF_SECTOR_SIZE / 4; i++) {
        mfm_put_long(p, prev, mfm_even(ldl_be_p(data + i * 4)));
    }
}

bool amiga_fdc_read_track(AmigaFDCState *s, const uint8_t **buf, int *len)
{
    uint8_t sectors[ADF_TRACK_BYTES];
    unsigned track, sector;
    uint32_t prev = 0;
    uint8_t *p = s->track_mfm;

    if (!s->selected || !s->motor || !s->blk || !blk_is_inserted(s->blk)) {
        return false;
    }
    if (s->cyl >= ADF_CYLINDERS) {
        /* past the data area: nothing but unformatted noise out there */
        return false;
    }

    track = s->cyl * ADF_HEADS + (s->side_level ? 0 : 1);
    if (blk_pread(s->blk, (int64_t)track * ADF_TRACK_BYTES, ADF_TRACK_BYTES,
                  sectors, 0) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "amiga-fdc: read error on track %u\n",
                      track);
        return false;
    }

    for (sector = 0; sector < ADF_TRACK_SECTORS; sector++) {
        mfm_encode_sector(&p, &prev, track, sector,
                          sectors + sector * ADF_SECTOR_SIZE);
    }
    memset(p, 0xaa, s->track_mfm + MFM_TRACK_BYTES - p);

    *buf = s->track_mfm;
    *len = MFM_TRACK_BYTES;
    return true;
}

static uint32_t mfm_decode_long(const uint8_t *odd, const uint8_t *even)
{
    return ((ldl_be_p(odd) & MFM_DATA_MASK) << 1) |
           (ldl_be_p(even) & MFM_DATA_MASK);
}

static uint32_t mfm_block_ck(const uint8_t *p, unsigned longs)
{
    uint32_t ck = 0;
    unsigned i;

    for (i = 0; i < longs; i++) {
        ck ^= ldl_be_p(p + i * 4) & MFM_DATA_MASK;
    }
    return ck;
}

/*
 * Decode a track's worth of MFM written by disk DMA.  Trackdisk always
 * writes whole tracks in the standard format, so scan for the sync
 * words and decode every well-formed sector behind them.
 */
void amiga_fdc_write_track(AmigaFDCState *s, const uint8_t *buf, int len)
{
    unsigned head_track = s->cyl * ADF_HEADS + (s->side_level ? 0 : 1);
    uint8_t data[ADF_SECTOR_SIZE];
    int pos = 0;

    if (!s->selected || !s->motor || !s->blk || !blk_is_inserted(s->blk)) {
        return;
    }
    if (!blk_is_writable(s->blk)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "amiga-fdc: write to write-protected disk\n");
        return;
    }

    while (pos + 2 <= len) {
        const uint8_t *p;
        uint32_t info;
        unsigned track, sector, i;

        if (lduw_be_p(buf + pos) != MFM_SYNC) {
            pos += 2;
            continue;
        }
        /* skip the sync mark (usually a pair) */
        while (pos + 2 <= len && lduw_be_p(buf + pos) == MFM_SYNC) {
            pos += 2;
        }
        if (pos + MFM_SEC_PAYLOAD_BYTES > len) {
            break;
        }
        p = buf + pos;
        info = mfm_decode_long(p + MFM_SEC_INFO, p + MFM_SEC_INFO + 4);
        track = (info >> 16) & 0xff;
        sector = (info >> 8) & 0xff;
        if ((info >> 24) != MFM_INFO_FORMAT_AMIGA ||
            track != head_track || sector >= ADF_TRACK_SECTORS) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "amiga-fdc: bad sector header 0x%08x on track %u\n",
                          info, head_track);
            continue;
        }
        if (mfm_decode_long(p + MFM_SEC_DATA_CK, p + MFM_SEC_DATA_CK + 4) !=
            (mfm_block_ck(p + MFM_SEC_DATA, ADF_SECTOR_SIZE / 4) ^
             mfm_block_ck(p + MFM_SEC_DATA + ADF_SECTOR_SIZE,
                          ADF_SECTOR_SIZE / 4))) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "amiga-fdc: bad data checksum, track %u sector %u\n",
                          track, sector);
            continue;
        }
        for (i = 0; i < ADF_SECTOR_SIZE / 4; i++) {
            stl_be_p(data + i * 4,
                     mfm_decode_long(p + MFM_SEC_DATA + i * 4,
                                     p + MFM_SEC_DATA + ADF_SECTOR_SIZE +
                                     i * 4));
        }
        if (blk_pwrite(s->blk,
                       (int64_t)track * ADF_TRACK_BYTES +
                       sector * ADF_SECTOR_SIZE,
                       ADF_SECTOR_SIZE, data, 0) < 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "amiga-fdc: write error, track %u sector %u\n",
                          track, sector);
        }
        pos = p + MFM_SEC_PAYLOAD_BYTES - buf;
    }
}

/* --- drive mechanics --- */

static void amiga_fdc_update_status(AmigaFDCState *s)
{
    bool disk = s->blk && blk_is_inserted(s->blk);
    bool wp = disk && !blk_is_writable(s->blk);

    /*
     * The status lines are open collector: a deselected drive floats
     * them all high.  With the motor off, /RDY carries the drive ID
     * shift register instead of the ready state.
     */
    qemu_set_irq(s->chng, !(s->selected && (!disk || s->disk_changed)));
    qemu_set_irq(s->wpro, !(s->selected && wp));
    qemu_set_irq(s->tk0, !(s->selected && s->cyl == 0));
    qemu_set_irq(s->rdy, !s->selected || (s->motor ? !disk : s->id_bit));
}

static void amiga_fdc_index_pulse(void *opaque)
{
    AmigaFDCState *s = opaque;

    if (!s->motor) {
        return;
    }
    qemu_set_irq(s->index, 0);
    qemu_set_irq(s->index, 1);
    timer_mod(&s->index_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              DRIVE_ROTATION_NS);
}

static void amiga_fdc_sel(void *opaque, int n, int level)
{
    AmigaFDCState *s = opaque;

    /* the select edge latches the motor line into the drive */
    if (s->sel_prev && !level) {
        bool motor = !s->mtr_level;

        if (motor != s->motor) {
            s->motor = motor;
            /* a motor toggle resets the drive ID shifter */
            s->id_pos = 0;
            if (motor) {
                amiga_fdc_index_pulse(s);
            }
        } else if (!s->motor) {
            /* each select pulse with the motor off shifts out one bit */
            if (s->id_pos < AMIGA_DRIVE_ID_BITS) {
                s->id_bit = (s->drive_id >>
                             (AMIGA_DRIVE_ID_BITS - 1 - s->id_pos)) & 1;
                s->id_pos++;
            } else {
                /* the shifter is empty, the pull-up wins */
                s->id_bit = true;
            }
        }
    }
    s->sel_prev = level;
    s->selected = !level;
    amiga_fdc_update_status(s);
}

static void amiga_fdc_step(void *opaque, int n, int level)
{
    AmigaFDCState *s = opaque;
    bool edge = s->step_prev && !level;

    s->step_prev = level;
    if (!edge || !s->selected) {
        return;
    }
    if (s->dir_level) {
        if (s->cyl > 0) {
            s->cyl--;
        }
    } else {
        if (s->cyl < DRIVE_MAX_CYLINDER) {
            s->cyl++;
        }
    }
    /* stepping with a disk in the drive clears the change latch */
    if (s->blk && blk_is_inserted(s->blk)) {
        s->disk_changed = false;
    }
    amiga_fdc_update_status(s);
}

static void amiga_fdc_mtr(void *opaque, int n, int level)
{
    AmigaFDCState *s = opaque;

    s->mtr_level = level;
}

static void amiga_fdc_dir(void *opaque, int n, int level)
{
    AmigaFDCState *s = opaque;

    s->dir_level = level;
}

static void amiga_fdc_side(void *opaque, int n, int level)
{
    AmigaFDCState *s = opaque;

    s->side_level = level;
}

/* claim the medium read-only or read-write, matching its backing file */
static bool amiga_fdc_claim(AmigaFDCState *s, Error **errp)
{
    uint64_t perm = BLK_PERM_CONSISTENT_READ |
                    (blk_supports_write_perm(s->blk) ? BLK_PERM_WRITE : 0);

    return blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp) == 0;
}

/*
 * A disk was inserted or removed from the monitor.  Re-claim the new
 * medium (or release the old one) and latch /CHNG: it stays asserted
 * until the drive next steps, which is how trackdisk.device learns the
 * disk has been swapped and drops its cached track.
 */
static void amiga_fdc_change_media(void *opaque, bool load, Error **errp)
{
    AmigaFDCState *s = opaque;

    if (!load) {
        blk_set_perm(s->blk, 0, BLK_PERM_ALL, &error_abort);
    } else {
        if (blk_getlength(s->blk) != ADF_IMAGE_SIZE) {
            error_setg(errp, "floppy image must be a %d byte ADF",
                       ADF_IMAGE_SIZE);
            return;
        }
        if (!amiga_fdc_claim(s, errp)) {
            return;
        }
    }
    s->disk_changed = true;
    amiga_fdc_update_status(s);
}

static const BlockDevOps amiga_fdc_block_ops = {
    .change_media_cb = amiga_fdc_change_media,
};

static void amiga_fdc_reset(DeviceState *dev)
{
    AmigaFDCState *s = AMIGA_FDC(dev);

    s->sel_prev = true;
    s->step_prev = true;
    s->mtr_level = true;
    s->dir_level = true;
    s->side_level = true;
    s->selected = false;
    s->motor = false;
    s->disk_changed = false;
    s->cyl = 0;
    s->id_pos = AMIGA_DRIVE_ID_BITS;
    s->id_bit = true;
    timer_del(&s->index_timer);
    amiga_fdc_update_status(s);
}

static void amiga_fdc_realize(DeviceState *dev, Error **errp)
{
    AmigaFDCState *s = AMIGA_FDC(dev);

    if (s->blk) {
        if (!amiga_fdc_claim(s, errp)) {
            return;
        }
        if (blk_is_inserted(s->blk) &&
            blk_getlength(s->blk) != ADF_IMAGE_SIZE) {
            error_setg(errp, "floppy image must be a %d byte ADF",
                       ADF_IMAGE_SIZE);
            return;
        }
        blk_set_dev_ops(s->blk, &amiga_fdc_block_ops, s);
    }
    timer_init_ns(&s->index_timer, QEMU_CLOCK_VIRTUAL,
                  amiga_fdc_index_pulse, s);
}

static void amiga_fdc_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    AmigaFDCState *s = AMIGA_FDC(obj);

    qdev_init_gpio_in_named(dev, amiga_fdc_sel, "sel", 1);
    qdev_init_gpio_in_named(dev, amiga_fdc_mtr, "mtr", 1);
    qdev_init_gpio_in_named(dev, amiga_fdc_step, "step", 1);
    qdev_init_gpio_in_named(dev, amiga_fdc_dir, "dir", 1);
    qdev_init_gpio_in_named(dev, amiga_fdc_side, "side", 1);
    qdev_init_gpio_out_named(dev, &s->chng, "chng", 1);
    qdev_init_gpio_out_named(dev, &s->wpro, "wpro", 1);
    qdev_init_gpio_out_named(dev, &s->tk0, "tk0", 1);
    qdev_init_gpio_out_named(dev, &s->rdy, "rdy", 1);
    qdev_init_gpio_out_named(dev, &s->index, "index", 1);
}

static const VMStateDescription vmstate_amiga_fdc = {
    .name = "amiga-fdc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(sel_prev, AmigaFDCState),
        VMSTATE_BOOL(step_prev, AmigaFDCState),
        VMSTATE_BOOL(mtr_level, AmigaFDCState),
        VMSTATE_BOOL(dir_level, AmigaFDCState),
        VMSTATE_BOOL(side_level, AmigaFDCState),
        VMSTATE_BOOL(selected, AmigaFDCState),
        VMSTATE_BOOL(motor, AmigaFDCState),
        VMSTATE_BOOL(disk_changed, AmigaFDCState),
        VMSTATE_UINT8(cyl, AmigaFDCState),
        VMSTATE_UINT8(id_pos, AmigaFDCState),
        VMSTATE_BOOL(id_bit, AmigaFDCState),
        VMSTATE_TIMER(index_timer, AmigaFDCState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property amiga_fdc_properties[] = {
    DEFINE_PROP_DRIVE("drive", AmigaFDCState, blk),
    DEFINE_PROP_UINT32("drive-id", AmigaFDCState, drive_id,
                       AMIGA_DRIVE_ID_DD),
};

static void amiga_fdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = amiga_fdc_realize;
    device_class_set_legacy_reset(dc, amiga_fdc_reset);
    dc->vmsd = &vmstate_amiga_fdc;
    device_class_set_props(dc, amiga_fdc_properties);
}

static const TypeInfo amiga_fdc_info = {
    .name          = TYPE_AMIGA_FDC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AmigaFDCState),
    .instance_init = amiga_fdc_init,
    .class_init    = amiga_fdc_class_init,
};

static void amiga_fdc_register_types(void)
{
    type_register_static(&amiga_fdc_info);
}

type_init(amiga_fdc_register_types)
