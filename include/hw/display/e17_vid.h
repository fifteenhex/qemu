/*
 * ELTEC Eurocom E17 onboard video
 *
 * Chipset unidentified; modelled from how RMON 3.1.3 drives it (see
 * E17-NOTES.md).  Three pieces: a RAMDAC with VGA-style palette port
 * pair and an indexed PLL register file at 0xfec40000, a CRTC with a
 * 16-bit timing register file at 0xfec48000, and 4MB of VRAM at
 * 0x0fc00000 scanned out as 8bpp indexed pixels.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_E17_VID_H
#define HW_DISPLAY_E17_VID_H

#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_E17_VID "e17-vid"
OBJECT_DECLARE_SIMPLE_TYPE(E17VidState, E17_VID)

#define E17_VID_NREGS       32
#define E17_VID_PAL_SIZE    (256 * 3)
#define E17_VID_VRAM_SIZE   (4 * MiB)

struct E17VidState {
    SysBusDevice parent_obj;

    MemoryRegion dac_mem;
    MemoryRegion crtc_mem;
    MemoryRegion vram;
    QemuConsole *con;

    uint8_t pal[E17_VID_PAL_SIZE];
    uint16_t pal_addr;      /* palette address including RGB phase */
    uint8_t dac_idx;        /* last value written to DAC +0 */
    uint8_t dac_id;         /* register +2, must read 0x3a */
    uint8_t dac5[E17_VID_NREGS];
    uint8_t dac6[E17_VID_NREGS];

    uint8_t crtc_direct[8];
    uint8_t crtc_idx;       /* timing register index, autoincrements */
    bool crtc_high;         /* next data byte is the high byte */
    uint16_t crtc[E17_VID_NREGS];

    bool invalidate;
};

#endif
