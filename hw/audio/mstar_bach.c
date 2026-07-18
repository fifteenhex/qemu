/*
 * MStar/SigmaStar "bach" audio controller
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The bach block (mstar,msc313-bach) is the SoC audio controller: a set of DMA
 * sub-channels that move PCM between DRAM and an analog codec, plus a sine
 * generator and mixer. Its analog codec registers live in a separate
 * "audiotop" syscon, which the driver reaches through the mstar,audiotop
 * phandle (regmap offset REG_ATOP_OFFSET, 0x1000).
 *
 * This model plays back the DMA "reader" sub-channel through QEMU's audio
 * subsystem so a "-audiodev" backend hears what the firmware renders. It still
 * stores/returns the raw 16-bit registers (RIU registers at the usual 4-byte
 * stride) for both the bach block and the audiotop syscon and logs every access
 * via mstar_iolog(), so the rest of the driver's programming can be captured.
 *
 * Reader (playback) sub-channel, byte offsets from the bach base:
 *   0x100 CTRL0        channel control (reset/enable/int flags)
 *   0x104 EN           addr_lo[11:0], count[12], trigger[13], init[14], en[15]
 *   0x108 ADDR         addr_hi[14:0]
 *   0x10c SIZE         ring size, in MIU units (bytes = size << ADDR_SHIFT)
 *   0x110 TRIGGER      trigger level, in MIU units (bytes queued per trigger)
 *   0x118 UNDERRUN     underrun threshold
 *   0x11c LEVEL        bytes queued but not yet consumed, in MIU units
 * The DMA buffer address is ((addr_hi << 12) | addr_lo) << ADDR_SHIFT, a DRAM
 * bus address; each trigger (EN bit13 rising edge) queues TRIGGER<<ADDR_SHIFT
 * more bytes. The Miyoo firmware runs this at 44100 Hz, stereo, S16_LE.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/loader.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/audio.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

/* MIU address/size granularity for the msc313-bach variant (addr_sz_shift=3). */
#define BACH_ADDR_SHIFT     3

/* Reader (playback) sub-channel register byte offsets. */
#define BACH_RD_CTRL0       0x100
#define BACH_RD_EN          0x104
#define BACH_RD_ADDR        0x108
#define BACH_RD_SIZE        0x10c
#define BACH_RD_TRIGGER     0x110
#define BACH_RD_UNDERRUN    0x118
#define BACH_RD_LEVEL       0x11c
#define BACH_RD_CTRL8       0x120      /* interrupt/status flags */

#define BACH_EN_ADDRLO_MASK 0x0fff
#define BACH_EN_COUNT       (1 << 12)
#define BACH_EN_TRIGGER     (1 << 13)
#define BACH_EN_INIT        (1 << 14)
#define BACH_EN_EN          (1 << 15)

/* CTRL0 (0x100) interrupt control bits (see the msc313-bach ALSA driver). */
#define BACH_CTRL0_INT_CLEAR    (1 << 8)    /* ack: write 1 then 0 */
#define BACH_CTRL0_EMPTY_IE     (1 << 10)
#define BACH_CTRL0_UNDERRUN_IE  (1 << 13)

/* CTRL8 (0x120) reader flag bits. */
#define BACH_CTRL8_UNDERRUN_FLAG (1 << 2)
#define BACH_CTRL8_EMPTY_FLAG    (1 << 4)

/* Playback format the Miyoo firmware programs (Ao Param: 44100, channel 2). */
#define BACH_FREQ           44100
#define BACH_CHANNELS       2
#define BACH_BYTES_PER_FRAME (BACH_CHANNELS * 2)   /* S16_LE stereo */

static uint32_t bach_ring_addr(Msc313BachState *s)
{
    uint32_t lo = s->regs[BACH_RD_EN / 4] & BACH_EN_ADDRLO_MASK;
    uint32_t hi = s->regs[BACH_RD_ADDR / 4] & 0x7fff;

    return (((hi << 12) | lo) << BACH_ADDR_SHIFT);
}

static uint32_t bach_ring_size(Msc313BachState *s)
{
    return (s->regs[BACH_RD_SIZE / 4] & 0xffff) << BACH_ADDR_SHIFT;
}

/* Bytes snapshotted from the ring but not yet handed to the backend. */
static unsigned bach_backlog(Msc313BachState *s)
{
    return s->pcm->len - s->pcm_rdpos;
}

/* Queued-but-unplayed level, in MIU units, as the LEVEL register reports it. */
static unsigned bach_level_miu(Msc313BachState *s)
{
    return bach_backlog(s) >> BACH_ADDR_SHIFT;
}

/*
 * Reader interrupt. The driver arms an underrun (and empty) interrupt after
 * queuing a period; the hardware raises the shared bach IRQ - and latches the
 * matching CTRL8 flag - once the DMA has drained the queue down to (or below)
 * the underrun threshold. Its ISR reads the flags, acks via CTRL0.INT_CLEAR
 * (write 1 then 0) and queues the next period (snd_pcm_period_elapsed). We fire
 * on the falling edge across the threshold and re-arm once the guest has queued
 * back above it, so a stream advances one period per interrupt without storming.
 */
static void mstar_bach_update_irq(Msc313BachState *s)
{
    uint16_t ctrl0 = s->regs[BACH_RD_CTRL0 / 4];
    unsigned thresh = s->regs[BACH_RD_UNDERRUN / 4] & 0xffff;
    uint16_t flags = 0;

    if (bach_level_miu(s) > thresh) {
        s->irq_armed = true;            /* guest queued fresh data above thresh */
    }

    if (s->play_active && s->irq_armed && !s->irq_pending) {
        if ((ctrl0 & BACH_CTRL0_UNDERRUN_IE) && bach_level_miu(s) <= thresh) {
            flags |= BACH_CTRL8_UNDERRUN_FLAG;
        }
        if ((ctrl0 & BACH_CTRL0_EMPTY_IE) && bach_backlog(s) == 0) {
            flags |= BACH_CTRL8_EMPTY_FLAG;
        }
        if (flags) {
            s->regs[BACH_RD_CTRL8 / 4] |= flags;
            s->irq_pending = true;
            s->irq_armed = false;
            qemu_set_irq(s->irq, 1);
        }
    }
}

/*
 * The backend voice must run whenever the guest wants playback *or* there is
 * still snapshotted PCM to drain. The emulated CPU and the audio clock are
 * decoupled: the guest typically renders a whole sound and moves on (even
 * reusing the ring) long before it has been heard, so keep the voice active
 * until the snapshotted data is actually consumed.
 */
static void mstar_bach_update_voice(Msc313BachState *s)
{
    bool want = s->play_active || bach_backlog(s) > 0;

    if (s->voice && want != s->voice_on) {
        s->voice_on = want;
        audio_be_set_active_out(s->audio_be, s->voice, want);
    }
}

/*
 * Audio backend pull callback. "avail" is how many bytes the backend can take;
 * feed it from the snapshotted PCM FIFO. Once fully drained (and the guest has
 * disabled the channel) the voice is deactivated.
 */
static void mstar_bach_out_cb(void *opaque, int avail)
{
    Msc313BachState *s = opaque;
    unsigned backlog = bach_backlog(s);
    size_t written;

    if (backlog == 0) {
        mstar_bach_update_voice(s);
        return;
    }
    if ((unsigned)avail > backlog) {
        avail = backlog;
    }
    written = audio_be_write(s->audio_be, s->voice,
                             s->pcm->data + s->pcm_rdpos, avail);
    s->pcm_rdpos += written;

    /* Reclaim space once the FIFO has been fully consumed. */
    if (s->pcm_rdpos >= s->pcm->len) {
        g_byte_array_set_size(s->pcm, 0);
        s->pcm_rdpos = 0;
    } else if (s->pcm_rdpos >= 0x10000) {
        g_byte_array_remove_range(s->pcm, 0, s->pcm_rdpos);
        s->pcm_rdpos = 0;
    }

    mstar_bach_update_irq(s);        /* level dropped: maybe underrun/empty */
    mstar_bach_update_voice(s);
}

/*
 * Copy "len" bytes of freshly written PCM out of the DRAM ring (starting at the
 * guest write pointer, wrapping at the ring end) into the playout FIFO, so the
 * audio survives the guest reusing or resetting the ring.
 */
static void mstar_bach_snapshot(Msc313BachState *s, uint32_t len)
{
    uint32_t base = MSTAR_DRAM_BASE + bach_ring_addr(s);
    uint32_t size = bach_ring_size(s);
    uint8_t buf[4096];

    if (size == 0) {
        return;
    }
    while (len > 0) {
        uint32_t chunk = MIN(len, sizeof(buf));

        if (s->play_wptr + chunk > size) {
            chunk = size - s->play_wptr;
        }
        if (address_space_read(&address_space_memory, base + s->play_wptr,
                               MEMTXATTRS_UNSPECIFIED, buf, chunk) != MEMTX_OK) {
            break;
        }
        g_byte_array_append(s->pcm, buf, chunk);
        s->play_wptr = (s->play_wptr + chunk) % size;
        len -= chunk;
    }
}

/* React to a write of the reader sub-channel EN register (playback control). */
static void mstar_bach_rd_en(Msc313BachState *s, uint16_t oldval, uint16_t val)
{
    if (val & BACH_EN_INIT) {
        s->play_wptr = 0;
    }

    /*
     * The trigger bit is a self-clearing "queue one trigger level of data"
     * command: each write with it set means the guest has filled another
     * TRIGGER << ADDR_SHIFT bytes at the ring write pointer (the driver holds
     * it high across the en/count writes of a period, so this is level- not
     * edge-triggered). Snapshot that PCM immediately.
     */
    if (val & BACH_EN_TRIGGER) {
        uint32_t add = (s->regs[BACH_RD_TRIGGER / 4] & 0xffff) << BACH_ADDR_SHIFT;

        mstar_bach_snapshot(s, add);
        s->regs[BACH_RD_EN / 4] &= ~BACH_EN_TRIGGER;   /* auto-clear */
    }

    s->play_active = (val & BACH_EN_EN) != 0;
    mstar_bach_update_irq(s);        /* fresh data queued: re-arm / maybe fire */
    mstar_bach_update_voice(s);
}

static uint64_t mstar_bach_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313BachState *s = opaque;
    uint16_t val = s->regs[addr / 4];

    /* Report the live FIFO fill level (in MIU units) as the DMA drains it. */
    if (addr == BACH_RD_LEVEL) {
        val = (uint16_t)(bach_backlog(s) >> BACH_ADDR_SHIFT);
    }

    mstar_iolog(MSTAR_BACH_BASE + addr, false, val, size);
    return val;
}

static void mstar_bach_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Msc313BachState *s = opaque;
    uint16_t oldval = s->regs[addr / 4];

    mstar_iolog(MSTAR_BACH_BASE + addr, true, val, size);
    s->regs[addr / 4] = val;

    if (addr == BACH_RD_EN) {
        mstar_bach_rd_en(s, oldval, val);
    } else if (addr == BACH_RD_CTRL0) {
        /*
         * The driver acks by pulsing INT_CLEAR high: clear the latched flags
         * and drop the line. Then re-evaluate (e.g. an interrupt was just
         * unmasked, or it is still under threshold and armed).
         */
        if (val & BACH_CTRL0_INT_CLEAR) {
            s->regs[BACH_RD_CTRL8 / 4] &=
                ~(BACH_CTRL8_UNDERRUN_FLAG | BACH_CTRL8_EMPTY_FLAG);
            s->irq_pending = false;
            qemu_set_irq(s->irq, 0);
        }
        mstar_bach_update_irq(s);
    } else if (addr == BACH_RD_UNDERRUN) {
        mstar_bach_update_irq(s);
    }
}

static const MemoryRegionOps mstar_bach_ops = {
    .read = mstar_bach_read,
    .write = mstar_bach_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static uint64_t mstar_bach_atop_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313BachState *s = opaque;

    mstar_iolog(MSTAR_AUDIOTOP_BASE + addr, false, s->atopregs[addr / 4], size);
    return s->atopregs[addr / 4];
}

static void mstar_bach_atop_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    Msc313BachState *s = opaque;

    mstar_iolog(MSTAR_AUDIOTOP_BASE + addr, true, val, size);
    s->atopregs[addr / 4] = val;
}

static const MemoryRegionOps mstar_bach_atop_ops = {
    .read = mstar_bach_atop_read,
    .write = mstar_bach_atop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void mstar_bach_reset_hold(Object *obj, ResetType type)
{
    Msc313BachState *s = MSC313_BACH(obj);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->atopregs, 0, sizeof(s->atopregs));
    s->play_wptr = 0;
    s->pcm_rdpos = 0;
    g_byte_array_set_size(s->pcm, 0);
    s->play_active = false;
    s->irq_pending = false;
    s->irq_armed = false;
    mstar_bach_update_voice(s);
    qemu_set_irq(s->irq, 0);
}

static void mstar_bach_init(Object *obj)
{
    Msc313BachState *s = MSC313_BACH(obj);

    s->pcm = g_byte_array_new();
}

static void mstar_bach_finalize(Object *obj)
{
    Msc313BachState *s = MSC313_BACH(obj);

    g_byte_array_free(s->pcm, TRUE);
}

static void mstar_bach_realize(DeviceState *dev, Error **errp)
{
    Msc313BachState *s = MSC313_BACH(dev);
    struct audsettings as = {
        .freq = BACH_FREQ,
        .nchannels = BACH_CHANNELS,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = 0,
    };

    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_bach_ops, s,
                          "mstar.bach", MSTAR_BACH_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    memory_region_init_io(&s->atop, OBJECT(dev), &mstar_bach_atop_ops, s,
                          "mstar.audiotop", MSTAR_AUDIOTOP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->atop);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }
    s->voice = audio_be_open_out(s->audio_be, s->voice, "mstar-bach",
                                 s, mstar_bach_out_cb, &as);
    if (!s->voice) {
        error_setg(errp, "mstar-bach: could not open audio output");
        return;
    }
    s->voice_on = false;
    audio_be_set_active_out(s->audio_be, s->voice, false);
}

static const Property mstar_bach_properties[] = {
    DEFINE_AUDIO_PROPERTIES(Msc313BachState, audio_be),
};

/*
 * The audio voice and the snapshotted-PCM FIFO are transient: on restore the
 * pending samples are dropped (an at-most one-sound glitch) and the voice
 * activity is re-derived from the migrated channel state.
 */
static int mstar_bach_post_load(void *opaque, int version_id)
{
    Msc313BachState *s = opaque;

    g_byte_array_set_size(s->pcm, 0);
    s->pcm_rdpos = 0;
    s->voice_on = false;
    mstar_bach_update_voice(s);
    return 0;
}

static const VMStateDescription vmstate_mstar_bach = {
    .name = "mstar-msc313-bach",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mstar_bach_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, Msc313BachState, MSTAR_BACH_NUM_REGS),
        VMSTATE_UINT16_ARRAY(atopregs, Msc313BachState, MSTAR_AUDIOTOP_NUM_REGS),
        VMSTATE_BOOL(play_active, Msc313BachState),
        VMSTATE_UINT32(play_wptr, Msc313BachState),
        VMSTATE_BOOL(irq_pending, Msc313BachState),
        VMSTATE_BOOL(irq_armed, Msc313BachState),
        VMSTATE_END_OF_LIST()
    },
};

static void mstar_bach_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_bach_realize;
    rc->phases.hold = mstar_bach_reset_hold;
    dc->vmsd = &vmstate_mstar_bach;
    device_class_set_props(dc, mstar_bach_properties);
}

static const TypeInfo mstar_bach_types[] = {
    {
        .name           = TYPE_MSC313_BACH,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313BachState),
        .instance_init  = mstar_bach_init,
        .instance_finalize = mstar_bach_finalize,
        .class_init     = mstar_bach_class_init,
    },
};

DEFINE_TYPES(mstar_bach_types)
