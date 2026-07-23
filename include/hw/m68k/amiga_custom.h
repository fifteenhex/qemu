/*
 * Amiga custom chip register block (Agnus/Denise/Paula).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_M68K_AMIGA_CUSTOM_H
#define HW_M68K_AMIGA_CUSTOM_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "hw/m68k/amiga_fdc.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qom/object.h"

#define TYPE_AMIGA_CUSTOM "amiga-custom"
OBJECT_DECLARE_SIMPLE_TYPE(AmigaCustomState, AMIGA_CUSTOM)

/* plenty for the few hundred entries a busy per-line copper list makes */
#define AMIGA_COPPER_JOURNAL_MAX    4096

/* gameport 0 fire button / left mouse button input on CIA-A port A */
#define AMIGA_CIAA_PA_FIR0          6

/* INTENA/INTREQ bits */
#define INT_TBE     (1 << 0)
#define INT_DSKBLK  (1 << 1)
#define INT_SOFT    (1 << 2)
#define INT_PORTS   (1 << 3)
#define INT_COPER   (1 << 4)
#define INT_VERTB   (1 << 5)
#define INT_BLIT    (1 << 6)
#define INT_AUD0    (1 << 7)
#define INT_AUD1    (1 << 8)
#define INT_AUD2    (1 << 9)
#define INT_AUD3    (1 << 10)
#define INT_RBF     (1 << 11)
#define INT_DSKSYN  (1 << 12)
#define INT_EXTER   (1 << 13)
#define INT_INTEN   (1 << 14)

struct AmigaCustomState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq ipl;               /* current interrupt level, 0-6 */
    CharFrontend chr;           /* Paula serial port */

    uint32_t agnus_id;          /* VPOSR bits 14-8 */
    uint32_t denise_id;

    uint16_t intena, intreq;
    uint16_t dmacon, adkcon;
    uint16_t serper, potgo;
    uint8_t serial_rx;

    AmigaFDCState *fdc;         /* DF0, the source of disk DMA data */
    uint16_t dsklen;
    bool dsklen_armed;          /* one DMAEN write seen, one to go */
    QEMUTimer disk_timer;

    /* mouse in gameport 0: counters in JOY0DAT, buttons on the CIA/POT */
    qemu_irq mouse_btn;         /* left button, CIA-A PA6, active low */
    QemuInputHandlerState *mouse_hs;
    uint8_t mouse_x, mouse_y;
    bool mouse_rmb;
    /*
     * INT2/INT6 are open-drain lines shared by the CIAs (bit 0) and
     * board hardware (bit 1); INTREQ re-latches while any source holds
     * its line.
     */
    uint8_t ports_levels, exter_levels;

    /* backing store for the (mostly write-only) chipset registers */
    uint16_t regs[0x100];

    /*
     * Display state journal: the copper's writes to display registers,
     * tagged with the beam line they take effect on.  The renderer
     * replays them over the frame-start register snapshot, which is
     * how mid-frame splits and per-line palette changes get drawn.
     */
    struct {
        uint16_t line, reg, val;
    } journal[AMIGA_COPPER_JOURNAL_MAX];
    unsigned journal_len;
    uint16_t frame_regs[0x100];

    bool blit_zero;             /* last blit produced only zeroes */

    int64_t frame_origin_ns;
    QEMUTimer vblank_timer;

    QemuConsole *con;
};

#endif
