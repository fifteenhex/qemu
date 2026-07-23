/*
 * Amiga floppy drive (mechanics and MFM track encoding).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_M68K_AMIGA_FDC_H
#define HW_M68K_AMIGA_FDC_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_AMIGA_FDC "amiga-fdc"
OBJECT_DECLARE_SIMPLE_TYPE(AmigaFDCState, AMIGA_FDC)

/*
 * The floppy interface pins on the two CIAs.  Control lines are
 * outputs on CIA-B port B, status lines are inputs on CIA-A port A;
 * all of them are active low.
 */
#define AMIGA_CIAB_PB_STEP  0   /* pulse: step the head */
#define AMIGA_CIAB_PB_DIR   1   /* step direction, low = towards centre */
#define AMIGA_CIAB_PB_SIDE  2   /* head select, low = upper head (side 1) */
#define AMIGA_CIAB_PB_SEL0  3   /* drive select, DF0-DF3 */
#define AMIGA_CIAB_PB_SEL1  4
#define AMIGA_CIAB_PB_SEL2  5
#define AMIGA_CIAB_PB_SEL3  6
#define AMIGA_CIAB_PB_MTR   7   /* motor, latched by the SEL edge */

#define AMIGA_CIAA_PA_CHNG  2   /* low: no disk, or disk was removed */
#define AMIGA_CIAA_PA_WPRO  3   /* low: disk is write protected */
#define AMIGA_CIAA_PA_TK0   4   /* low: head is over cylinder 0 */
#define AMIGA_CIAA_PA_RDY   5   /* low: motor is up to speed */

/* 3.5" double density AmigaDOS geometry, the format of a plain ADF */
#define ADF_SECTOR_SIZE         512
#define ADF_TRACK_SECTORS       11
#define ADF_CYLINDERS           80
#define ADF_HEADS               2
#define ADF_TRACK_BYTES         (ADF_TRACK_SECTORS * ADF_SECTOR_SIZE)
#define ADF_IMAGE_SIZE          (ADF_CYLINDERS * ADF_HEADS * ADF_TRACK_BYTES)

/* the head can step a little past the last data cylinder */
#define DRIVE_MAX_CYLINDER      82

/*
 * An MFM-encoded sector: 2 gap words, 2 sync words, then the odd/even
 * encoded info longword, 16 label bytes, header and data checksum
 * longwords, and 512 data bytes.
 */
#define MFM_SYNC                0x4489
#define MFM_GAP_WORD            0xaaaa
#define MFM_SECTOR_BYTES        (4 + 4 + 8 + 32 + 8 + 8 + 2 * ADF_SECTOR_SIZE)
#define MFM_TRACK_GAP_BYTES     700
#define MFM_TRACK_BYTES         (ADF_TRACK_SECTORS * MFM_SECTOR_BYTES + \
                                 MFM_TRACK_GAP_BYTES)

/* double density MFM bit cell: 2us, 500kbit/s */
#define MFM_BITCELL_NS          2000

struct AmigaFDCState {
    SysBusDevice parent_obj;

    BlockBackend *blk;

    qemu_irq chng, wpro, tk0, rdy;  /* status lines, CIA-A port A */
    qemu_irq index;                 /* index pulse, CIA-B FLAG */

    /* control line levels as last driven by CIA-B port B */
    bool sel_prev, step_prev, mtr_level, dir_level, side_level;

    bool selected;
    bool motor;
    bool disk_changed;              /* the /CHNG latch */
    uint8_t cyl;

    QEMUTimer index_timer;

    uint8_t track_mfm[MFM_TRACK_BYTES];
};

/*
 * Encode the track under the head into buf (MFM_TRACK_BYTES bytes).
 * Fails if no readable disk is spinning in the drive.
 */
bool amiga_fdc_read_track(AmigaFDCState *s, const uint8_t **buf, int *len);

/* Decode an MFM track buffer written by disk DMA back into sectors. */
void amiga_fdc_write_track(AmigaFDCState *s, const uint8_t *buf, int len);

#endif
