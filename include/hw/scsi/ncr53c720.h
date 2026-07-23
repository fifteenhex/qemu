/*
 * NCR 53C720 SCSI I/O processor, low level (register bit-bang) mode
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SCSI_NCR53C720_H
#define HW_SCSI_NCR53C720_H

#include "hw/core/sysbus.h"
#include "hw/scsi/scsi.h"
#include "qom/object.h"

#define TYPE_NCR53C720 "ncr53c720"
OBJECT_DECLARE_SIMPLE_TYPE(NCR53C720State, NCR53C720)

#define NCR53C720_NREGS 0x60

struct NCR53C720State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SCSIBus bus;
    bool lane_swap;

    uint8_t regs[NCR53C720_NREGS];

    /*
     * The state of the emulated target the initiator talks to:
     * connection, current phase (the MSG/CD/IO lines), REQ, and the
     * byte the target drives on the data lines.
     */
    bool connected;
    bool req;
    uint8_t phase;
    uint8_t bus_data;
    uint8_t latched;
    uint8_t target;

    uint8_t cdb[16];
    int32_t cdb_len;
    int32_t cdb_expected;

    SCSIRequest *current_req;
    uint8_t *async_buf;
    int32_t async_len;
    int32_t async_pos;
    bool data_in;
    bool waiting_scsi;
};

#endif
