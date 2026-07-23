/*
 * IWM (Integrated Woz Machine) floppy controller with a single Sony
 * 400K GCR drive attached, as found in the original Macintosh 128K.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_BLOCK_IWM_H
#define HW_BLOCK_IWM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IWM "iwm"
OBJECT_DECLARE_SIMPLE_TYPE(IWMState, IWM)

/* 400K single-sided GCR: 80 tracks in 5 speed zones of 16 tracks */
#define IWM_SONY_TRACKS         80
#define IWM_SONY_SECTOR_SIZE    512
#define IWM_SONY_IMAGE_SIZE     409600

/*
 * Worst case GCR track: 12 sectors of (sync + address field + sync +
 * data field) at ~750 bytes each; round up generously.
 */
#define IWM_TRACK_BUF_SIZE      12288

struct IWMState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    BlockBackend *blk;

    /* IWM internal state: latches set by address-line accesses */
    uint8_t latches;
    bool sel;                   /* SEL line, driven by VIA port A bit 5 */
    uint8_t mode;

    /* Sony 400K drive mechanism */
    uint8_t track;
    bool dirtn;                 /* 0 = step toward higher tracks */
    bool motor_on;
    bool ejected;
    bool switched;              /* disk-switched latch */

    int64_t lstrb_time;
    QEMUTimer *reinsert_timer;

    /*
     * The Macintosh 128K sets the drive speed with a PWM signal taken
     * from the low bytes of the words in the sound page; the machine
     * points this at that buffer so the tachometer can answer with the
     * speed the ROM asked for.
     */
    const uint8_t *pwm_buf;

    /* GCR-encoded track under the head */
    int cached_track;
    int track_len;
    int track_pos;
    uint8_t track_buf[IWM_TRACK_BUF_SIZE];
};

#endif
