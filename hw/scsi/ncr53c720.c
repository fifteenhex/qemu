/*
 * NCR 53C720 SCSI I/O processor, low level (register bit-bang) mode
 *
 * Modelled for the ELTEC Eurocom E17, whose RMON firmware and OS-9
 * bootstrap drive the chip exclusively in the "low level mode" of the
 * 53C7xx/8xx family: the CPU drives the SCSI bus signals by hand
 * through SODL/SOCL and watches it through SBCL/SBDL, one REQ/ACK
 * handshake per byte, with SCRIPTS never used.  Consequently only
 * that mode is modelled here; there is no SCRIPTS processor, no DMA
 * and no interrupt generation (the E17 firmware polls).
 *
 * The register file is the second generation ("SCSI SCRIPTS") layout
 * shared by the 53C720 and the PCI 53C8xx parts: ISTAT at 0x14,
 * STEST0-3 at 0x4c, SIDL/SODL/SBDL at 0x50/0x54/0x58 — NOT the older
 * 53C710 layout.  This was established by reverse engineering the
 * E17 RMON driver (see E17-NOTES.md): it enables low level mode via
 * STEST2.LOW, selects with an ID bitmask in SODL(0x54) and reads
 * incoming bytes from SBDL(0x58).
 *
 * Everything the firmware exercises is implemented; the remaining
 * registers are plain storage so that future drivers can be traced.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/scsi/scsi.h"
#include "hw/scsi/ncr53c720.h"
#include "scsi/constants.h"
#include "migration/vmstate.h"
#include "trace.h"

/* register file offsets (little endian numbering, as in the manual) */
#define NCR_SCNTL0      0x00
#define NCR_SCNTL1      0x01
#define NCR_SCNTL2      0x02
#define NCR_SCNTL3      0x03
#define NCR_SCID        0x04
#define NCR_SXFER       0x05
#define NCR_SDID        0x06
#define NCR_GPREG       0x07
#define NCR_SFBR        0x08
#define NCR_SOCL        0x09
#define NCR_SSID        0x0a
#define NCR_SBCL        0x0b
#define NCR_DSTAT       0x0c
#define NCR_SSTAT0      0x0d
#define NCR_SSTAT1      0x0e
#define NCR_SSTAT2      0x0f
#define NCR_ISTAT       0x14
#define NCR_STEST0      0x4c
#define NCR_STEST1     0x4d
#define NCR_STEST2      0x4e
#define NCR_STEST3      0x4f
#define NCR_SIDL        0x50
#define NCR_SODL        0x54
#define NCR_SBDL        0x58

#define NCR_SCNTL0_ARB  0xc0    /* full arbitration, the reset value */

#define NCR_SCNTL1_RST  0x08    /* assert SCSI RST */
#define NCR_SCNTL1_CON  0x10    /* connected */
#define NCR_SCNTL1_ADB  0x40    /* assert SCSI data bus (SODL) */

/* SOCL/SBCL: the SCSI control signals */
#define NCR_BUS_IO      0x01
#define NCR_BUS_CD      0x02
#define NCR_BUS_MSG     0x04
#define NCR_BUS_ATN     0x08
#define NCR_BUS_SEL     0x10
#define NCR_BUS_BSY     0x20
#define NCR_BUS_ACK     0x40
#define NCR_BUS_REQ     0x80

#define NCR_ISTAT_SRST  0x40    /* software reset */

#define NCR_DSTAT_DFE   0x80    /* DMA FIFO empty */

#define NCR_STEST2_LOW  0x01    /* SCSI low level mode enable */

/* bus phases = target driven MSG/CD/IO, i.e. SBCL bits 2:0 */
#define PHASE_DO        0                               /* data out */
#define PHASE_DI        NCR_BUS_IO                      /* data in */
#define PHASE_CMD       NCR_BUS_CD                      /* command */
#define PHASE_ST        (NCR_BUS_CD | NCR_BUS_IO)       /* status */
#define PHASE_MI        (NCR_BUS_MSG | NCR_BUS_CD | NCR_BUS_IO)

#define MSG_COMMAND_COMPLETE    0x00

/* a target drives the data lines in all phases with I/O asserted */
#define PHASE_TARGET_DRIVES(p)  ((p) & NCR_BUS_IO)

static void ncr53c720_bus_free(NCR53C720State *s)
{
    if (s->current_req) {
        scsi_req_unref(s->current_req);
        s->current_req = NULL;
    }
    s->connected = false;
    s->req = false;
    s->phase = 0;
    s->bus_data = 0;
    s->async_len = 0;
    s->waiting_scsi = false;
}

static void ncr53c720_soft_reset(NCR53C720State *s)
{
    if (s->current_req) {
        scsi_req_cancel(s->current_req);
    }
    ncr53c720_bus_free(s);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[NCR_SCNTL0] = NCR_SCNTL0_ARB;
    s->regs[NCR_DSTAT] = NCR_DSTAT_DFE;
}

/* target asserts REQ for the next byte of an incoming phase */
static void ncr53c720_present(NCR53C720State *s, uint8_t phase, uint8_t data)
{
    s->phase = phase;
    s->bus_data = data;
    s->req = true;
    trace_ncr53c720_present(phase, data);
}

/* target asserts REQ to request a byte of an outgoing phase */
static void ncr53c720_request(NCR53C720State *s, uint8_t phase)
{
    s->phase = phase;
    s->req = true;
    trace_ncr53c720_request(phase);
}

static void ncr53c720_command_dispatch(NCR53C720State *s)
{
    /* pre-SCSI-2 initiators pass the LUN in the CDB */
    int lun = s->cdb[1] >> 5;
    SCSIDevice *dev = scsi_device_find(&s->bus, 0, s->target, lun);
    int32_t datalen;

    trace_ncr53c720_command(s->target, lun, s->cdb[0], s->cdb_len);

    if (!dev) {
        /* no such LUN behind a present target */
        ncr53c720_present(s, PHASE_ST, CHECK_CONDITION);
        return;
    }

    s->current_req = scsi_req_new(dev, 0, lun, s->cdb, s->cdb_len, s);
    s->waiting_scsi = true;
    datalen = scsi_req_enqueue(s->current_req);
    if (datalen) {
        s->data_in = datalen > 0;
        /*
         * scsi_req_continue() calls back into transfer_data or
         * command_complete, which take the transaction to the data
         * or status phase before this returns.
         */
        scsi_req_continue(s->current_req);
    }
}

/* an ACK edge from the initiator: latch on assert ... */
static void ncr53c720_ack_asserted(NCR53C720State *s)
{
    if (!s->req) {
        return;
    }
    s->req = false;
    if (PHASE_TARGET_DRIVES(s->phase)) {
        s->regs[NCR_SIDL] = s->bus_data;
    } else {
        s->latched = s->regs[NCR_SODL];
    }
}

/* ... act and raise REQ again on release */
static void ncr53c720_ack_released(NCR53C720State *s)
{
    if (s->req || !s->connected) {
        return;
    }
    switch (s->phase) {
    case PHASE_CMD:
        s->cdb[s->cdb_len++] = s->latched;
        if (s->cdb_len == 1) {
            s->cdb_expected = scsi_cdb_length(s->cdb);
            if (s->cdb_expected < 0) {
                qemu_log_mask(LOG_UNIMP, "ncr53c720: unknown CDB group for "
                              "opcode 0x%02x\n", s->cdb[0]);
                ncr53c720_present(s, PHASE_ST, CHECK_CONDITION);
                break;
            }
        }
        if (s->cdb_len < s->cdb_expected) {
            ncr53c720_request(s, PHASE_CMD);
        } else {
            ncr53c720_command_dispatch(s);
        }
        break;
    case PHASE_DI:
        s->async_pos++;
        if (s->async_pos < s->async_len) {
            ncr53c720_present(s, PHASE_DI, s->async_buf[s->async_pos]);
        } else {
            s->async_len = 0;
            scsi_req_continue(s->current_req);
        }
        break;
    case PHASE_DO:
        s->async_buf[s->async_pos++] = s->latched;
        if (s->async_pos < s->async_len) {
            ncr53c720_request(s, PHASE_DO);
        } else {
            s->async_len = 0;
            scsi_req_continue(s->current_req);
        }
        break;
    case PHASE_ST:
        ncr53c720_present(s, PHASE_MI, MSG_COMMAND_COMPLETE);
        break;
    case PHASE_MI:
        /* command complete delivered: disconnect */
        trace_ncr53c720_bus_free();
        ncr53c720_bus_free(s);
        break;
    }
}

/*
 * A write to SOCL drives SEL/BSY/ACK/ATN (and, as a target would see
 * them, the phase lines).  Selection: the initiator arbitrates (not
 * modelled: the bus is always won), puts its own and the target's ID
 * on the data lines via SODL, raises SEL+BSY, then releases BSY; a
 * present target responds with BSY.
 */
static void ncr53c720_socl_write(NCR53C720State *s, uint8_t val)
{
    uint8_t old = s->regs[NCR_SOCL];

    s->regs[NCR_SOCL] = val;

    if (!s->connected && (val & NCR_BUS_SEL) && !(val & NCR_BUS_BSY) &&
        (s->regs[NCR_SCNTL1] & NCR_SCNTL1_ADB)) {
        uint8_t ids = s->regs[NCR_SODL] & ~(1 << (s->regs[NCR_SCID] & 7));

        if (ids && !(ids & (ids - 1))) {        /* exactly one target bit */
            uint8_t target = ctz32(ids);

            if (!(s->regs[NCR_STEST2] & NCR_STEST2_LOW)) {
                qemu_log_mask(LOG_GUEST_ERROR, "ncr53c720: selection "
                              "without low level mode\n");
            }
            if (scsi_device_find(&s->bus, 0, target, 0)) {
                trace_ncr53c720_selected(target);
                s->connected = true;
                s->target = target;
                s->cdb_len = 0;
                s->cdb_expected = -1;
                /* no ATN: the target proceeds to the command phase */
                ncr53c720_request(s, PHASE_CMD);
            } else {
                trace_ncr53c720_selection_timeout(target);
            }
        }
    }

    if ((val & NCR_BUS_ACK) && !(old & NCR_BUS_ACK)) {
        ncr53c720_ack_asserted(s);
    } else if (!(val & NCR_BUS_ACK) && (old & NCR_BUS_ACK)) {
        ncr53c720_ack_released(s);
    }
}

/* the SCSI layer hands over (a chunk of) the data for the transfer */
static void ncr53c720_transfer_data(SCSIRequest *req, uint32_t len)
{
    NCR53C720State *s = req->hba_private;

    s->async_buf = scsi_req_get_buf(req);
    s->async_len = len;
    s->async_pos = 0;
    s->waiting_scsi = false;
    if (s->data_in) {
        ncr53c720_present(s, PHASE_DI, s->async_buf[0]);
    } else {
        ncr53c720_request(s, PHASE_DO);
    }
}

static void ncr53c720_command_complete(SCSIRequest *req, size_t resid)
{
    NCR53C720State *s = req->hba_private;

    trace_ncr53c720_command_complete(req->status);
    s->waiting_scsi = false;
    ncr53c720_present(s, PHASE_ST, req->status);
    scsi_req_unref(s->current_req);
    s->current_req = NULL;
}

static void ncr53c720_request_cancelled(SCSIRequest *req)
{
    NCR53C720State *s = req->hba_private;

    if (s->current_req == req) {
        scsi_req_unref(s->current_req);
        s->current_req = NULL;
        ncr53c720_bus_free(s);
    }
}

static uint64_t ncr53c720_read(void *opaque, hwaddr addr, unsigned size)
{
    NCR53C720State *s = opaque;
    unsigned reg = s->lane_swap ? addr ^ 3 : addr;
    uint8_t val;

    switch (reg) {
    case NCR_SBCL:
        /*
         * The live bus: the initiator's own control signals plus, when
         * a target is connected, BSY, the phase lines and REQ.
         */
        val = s->regs[NCR_SOCL] &
            (NCR_BUS_SEL | NCR_BUS_BSY | NCR_BUS_ACK | NCR_BUS_ATN);
        if (s->connected) {
            val |= NCR_BUS_BSY | s->phase;
            if (s->req) {
                val |= NCR_BUS_REQ;
            }
        }
        break;
    case NCR_SBDL:
        /* the live data lines */
        if (s->connected && PHASE_TARGET_DRIVES(s->phase)) {
            val = s->bus_data;
        } else if (s->regs[NCR_SCNTL1] & NCR_SCNTL1_ADB) {
            val = s->regs[NCR_SODL];
        } else {
            val = 0;
        }
        break;
    default:
        val = reg < NCR53C720_NREGS ? s->regs[reg] : 0;
        break;
    }
    trace_ncr53c720_reg_read(reg, val);
    return val;
}

static void ncr53c720_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    NCR53C720State *s = opaque;
    unsigned reg = s->lane_swap ? addr ^ 3 : addr;

    trace_ncr53c720_reg_write(reg, val);

    switch (reg) {
    case NCR_SCNTL1:
        s->regs[NCR_SCNTL1] = val;
        if (val & NCR_SCNTL1_RST) {
            /* SCSI bus reset: any connected target lets go */
            ncr53c720_bus_free(s);
        }
        break;
    case NCR_SOCL:
        ncr53c720_socl_write(s, val);
        break;
    case NCR_ISTAT:
        if (val & NCR_ISTAT_SRST) {
            ncr53c720_soft_reset(s);
            val = 0;
        }
        s->regs[NCR_ISTAT] = val;
        break;
    case NCR_SBCL:
    case NCR_SBDL:
    case NCR_SIDL:
    case NCR_DSTAT:
    case NCR_SSTAT0:
    case NCR_SSTAT1:
    case NCR_SSTAT2:
        /* read only */
        break;
    default:
        if (reg < NCR53C720_NREGS) {
            s->regs[reg] = val;
        } else {
            qemu_log_mask(LOG_UNIMP, "ncr53c720: write beyond register "
                          "file @0x%02x = 0x%02" PRIx64 "\n", reg, val);
        }
        break;
    }
}

static const MemoryRegionOps ncr53c720_ops = {
    .read = ncr53c720_read,
    .write = ncr53c720_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const struct SCSIBusInfo ncr53c720_scsi_info = {
    .tcq = false,
    .max_target = 8,
    .max_lun = 8,

    .transfer_data = ncr53c720_transfer_data,
    .complete = ncr53c720_command_complete,
    .cancel = ncr53c720_request_cancelled,
};

static void ncr53c720_reset(DeviceState *dev)
{
    NCR53C720State *s = NCR53C720(dev);

    ncr53c720_soft_reset(s);
}

static void ncr53c720_realize(DeviceState *dev, Error **errp)
{
    NCR53C720State *s = NCR53C720(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &ncr53c720_ops, s,
                          "ncr53c720", NCR53C720_NREGS);
    sysbus_init_mmio(sbd, &s->iomem);
    scsi_bus_init(&s->bus, sizeof(s->bus), dev, &ncr53c720_scsi_info);
}

static const VMStateDescription vmstate_ncr53c720 = {
    .name = "ncr53c720",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, NCR53C720State, NCR53C720_NREGS),
        VMSTATE_BOOL(connected, NCR53C720State),
        VMSTATE_BOOL(req, NCR53C720State),
        VMSTATE_UINT8(phase, NCR53C720State),
        VMSTATE_UINT8(bus_data, NCR53C720State),
        VMSTATE_UINT8(latched, NCR53C720State),
        VMSTATE_UINT8(target, NCR53C720State),
        VMSTATE_UINT8_ARRAY(cdb, NCR53C720State, 16),
        VMSTATE_INT32(cdb_len, NCR53C720State),
        VMSTATE_INT32(cdb_expected, NCR53C720State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ncr53c720_properties[] = {
    /* byte lanes reversed within 32-bit words, as wired on the E17 */
    DEFINE_PROP_BOOL("lane-swap", NCR53C720State, lane_swap, false),
};

static void ncr53c720_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ncr53c720_realize;
    device_class_set_legacy_reset(dc, ncr53c720_reset);
    dc->vmsd = &vmstate_ncr53c720;
    device_class_set_props(dc, ncr53c720_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    dc->desc = "NCR 53C720 SCSI I/O processor (low level mode)";
}

static const TypeInfo ncr53c720_info = {
    .name = TYPE_NCR53C720,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NCR53C720State),
    .class_init = ncr53c720_class_init,
};

static void ncr53c720_register_types(void)
{
    type_register_static(&ncr53c720_info);
}

type_init(ncr53c720_register_types)
