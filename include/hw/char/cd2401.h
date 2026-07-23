/*
 * Cirrus Logic CD2401 four channel multi-protocol serial controller
 *
 * Modelled far enough for the polled/interrupt-driven async use the
 * ELTEC E17 RMON firmware makes of it; the same chip also sits on the
 * Motorola MVME167.  Register knowledge comes from the CL-CD2400/2401
 * datasheet and from reverse engineering RMON (see E17-NOTES.md).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_CHAR_CD2401_H
#define HW_CHAR_CD2401_H

#include "chardev/char-fe.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_CD2401 "cd2401"
OBJECT_DECLARE_SIMPLE_TYPE(CD2401State, CD2401)

#define CD2401_NR_CHAN      4
#define CD2401_RX_FIFO_LEN  16

/* interrupt types as encoded in the acknowledge byte (LIVR bits 1:0) */
#define CD2401_INT_NONE     0
#define CD2401_INT_MODEM    1
#define CD2401_INT_TX       2
#define CD2401_INT_RX       3

typedef struct {
    uint8_t regs[0x80];     /* per channel bank of the register file */
    uint8_t rx_fifo[CD2401_RX_FIFO_LEN];
    int rx_count;
    CharFrontend chr;
} CD2401Channel;

struct CD2401State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion iack;

    CD2401Channel chan[CD2401_NR_CHAN];
    uint8_t gregs[0x80];    /* global registers, offsets 0x80-0xff */

    /* channel/type currently under interrupt service */
    int svc_chan;
    int svc_type;
};

#endif
