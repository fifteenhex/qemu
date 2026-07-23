/*
 * Amiga 3000 SuperDMAC (SCSI DMA controller).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_M68K_A3000_SDMAC_H
#define HW_M68K_A3000_SDMAC_H

#include "hw/core/sysbus.h"
#include "hw/scsi/wd33c93.h"
#include "qom/object.h"

#define TYPE_A3000_SDMAC "a3000-sdmac"
OBJECT_DECLARE_SIMPLE_TYPE(A3000SDMACState, A3000_SDMAC)

struct A3000SDMACState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;               /* to INT2 */
    WD33C93State *sbic;

    uint8_t dawr;
    uint16_t cntr;
    uint32_t wtc;               /* word transfer count */
    uint32_t acr;               /* dma address */
    bool dma_active;
    bool e_int;                 /* end-of-process latched */
    bool sbic_int;              /* SBIC INT line level */
    bool drq;                   /* SBIC DRQ line level */
};

#endif
