/*
 * NCR 5380 SCSI controller
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef HW_SCSI_NCR5380_H
#define HW_SCSI_NCR5380_H

#include "hw/core/sysbus.h"
#include "hw/scsi/scsi.h"
#include "qom/object.h"

#define TYPE_NCR5380 "ncr5380"
OBJECT_DECLARE_SIMPLE_TYPE(NCR5380State, NCR5380)

struct NCR5380State {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    SCSIBus bus;
    SCSIDevice *dev;
    SCSIRequest *req;

    uint8_t reg_shift;

    /* register file */
    uint8_t odr;        /* output data */
    uint8_t icr;        /* initiator command */
    uint8_t mr;         /* mode */
    uint8_t tcr;        /* target command */
    uint8_t ser;        /* select enable */
    uint8_t csb;        /* current SCSI bus status (read reg 4) */
    uint8_t bsr;        /* bus and status (read reg 5) */
    uint8_t last_data;  /* last byte latched on the data lines */

    /* bus/transaction state */
    uint8_t phase;
    int target;
    int lun;
    uint8_t cmd[16];
    int cmd_len;
    uint8_t status;
    uint8_t msg_in;
    uint8_t dma_mode;
    bool xfer_pending;
    bool status_done;
    bool msg_done;

    /* current data-phase buffer, owned by the SCSI layer */
    uint8_t *buf;
    uint32_t buf_len;
    uint32_t buf_pos;
    uint8_t *async_buf;
    uint32_t async_len;
};

void ncr5380_ack(NCR5380State *s);
void ncr5380_ack_release(NCR5380State *s);
uint8_t ncr5380_pdma_read(NCR5380State *s);
void ncr5380_pdma_write(NCR5380State *s, uint8_t val);

#endif
