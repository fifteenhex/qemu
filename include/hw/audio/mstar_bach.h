/*
 * MStar/SigmaStar "bach" audio controller
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_AUDIO_MSTAR_BACH_H
#define HW_AUDIO_MSTAR_BACH_H

#include "hw/core/sysbus.h"
#include "qemu/audio.h"
#include "qom/object.h"

#define TYPE_MSTAR_BACH "mstar-bach"
OBJECT_DECLARE_SIMPLE_TYPE(MStarBachState, MSTAR_BACH)

#define MSTAR_BACH_SIZE          0x600
#define MSTAR_BACH_NUM_REGS      (MSTAR_BACH_SIZE / 4)
#define MSTAR_AUDIOTOP_SIZE      0x200
#define MSTAR_AUDIOTOP_NUM_REGS  (MSTAR_AUDIOTOP_SIZE / 4)

struct MStarBachState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;     /* bach controller  @0x1f2a0400 */
    MemoryRegion atop;      /* audiotop syscon  @0x1f206800 */
    qemu_irq irq;
    /* Physical base of DRAM; the reader's ring address is a MIU offset */
    uint32_t dram_base;

    uint16_t regs[MSTAR_BACH_NUM_REGS];
    uint16_t atopregs[MSTAR_AUDIOTOP_NUM_REGS];

    /* QEMU audio output for the DMA-reader (playback) sub-channel */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    bool play_active;       /* guest has the reader sub-channel enabled */
    bool voice_on;          /* backend voice is currently active */
    uint32_t play_wptr;     /* guest write pointer within the DRAM ring */
    GByteArray *pcm;        /* PCM snapshotted from the ring, pending playout */
    unsigned pcm_rdpos;     /* consume offset into pcm */
    bool irq_pending;       /* reader underrun/empty IRQ asserted, awaiting ack */
    bool irq_armed;         /* level rose above threshold; a new IRQ may fire */
    bool mercury5_reader;   /* use the mercury5 reader control-bit layout */
};

#endif /* HW_AUDIO_MSTAR_BACH_H */
