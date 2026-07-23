/* SPDX-License-Identifier: MIT */
/* QEMU Sega MegaDrive I/O area: version register + controller ports */

#ifndef MD_IO_H
#define MD_IO_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MD_IO "md-io"
OBJECT_DECLARE_SIMPLE_TYPE(MDIOState, MD_IO)

#define MD_IO_NUM_PORTS 3   /* controller 1, controller 2, EXT */

struct MDIOState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint8_t version;                    /* 0xA10001, read-only (property) */

    /* Per-port controller registers */
    uint8_t ctrl[MD_IO_NUM_PORTS];      /* data direction (1 = output)    */
    uint8_t data_out[MD_IO_NUM_PORTS];  /* output latch                   */
    uint8_t txdata[MD_IO_NUM_PORTS];    /* serial TxData                  */
    uint8_t sctrl[MD_IO_NUM_PORTS];     /* serial control                 */
};

#endif /* MD_IO_H */
