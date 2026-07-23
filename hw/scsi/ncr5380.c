/*
 * NCR 5380 SCSI controller, as used by the Macintosh IIsi and other
 * compact/LC-era Macs.  The CPU drives the SCSI bus phases directly;
 * data phases use the machine's pseudo-DMA path.  Backed by QEMU's
 * generic SCSI bus so a scsi-hd/scsi-cd can be attached.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/scsi/scsi.h"
#include "scsi/constants.h"
#include "scsi/utils.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/scsi/ncr5380.h"
#include "trace.h"

/* Write registers */
#define R_ODR    0   /* output data */
#define R_ICR    1   /* initiator command */
#define R_MR     2   /* mode */
#define R_TCR    3   /* target command */
#define R_SER    4   /* select enable */
#define R_DMASND 5   /* start DMA send */
#define R_DMATR  6   /* start DMA target receive */
#define R_DMAIR  7   /* start DMA initiator receive */

/* Read registers */
#define R_CSD    0   /* current SCSI data */
#define R_ICR_R  1
#define R_MR_R   2
#define R_TCR_R  3
#define R_CSB    4   /* current SCSI bus status */
#define R_BSR    5   /* bus and status */
#define R_IDR    6   /* input data */
#define R_RPI    7   /* reset parity/interrupt */

/* ICR bits */
#define ICR_ASSERT_DATA  0x01
#define ICR_ASSERT_ATN   0x02
#define ICR_ASSERT_SEL   0x04
#define ICR_ASSERT_BSY   0x08
#define ICR_ASSERT_ACK   0x10
#define ICR_ARB_LOST     0x20   /* r/o */
#define ICR_ARB_IN_PROG  0x40   /* r/o */
#define ICR_ASSERT_RST   0x80

/* MR bits */
#define MR_ARBITRATE     0x01
#define MR_DMA_MODE      0x02
#define MR_MONITOR_BSY   0x04
#define MR_ENABLE_EOP    0x08
#define MR_ENABLE_PAR    0x10
#define MR_PAR_INT       0x20
#define MR_BSY_INT       0x40   /* interrupt on loss of BSY */
#define MR_TARGET        0x80

/* CSB (read reg 4) bits — active high in the register */
#define CSB_DBP   0x01
#define CSB_SEL   0x02
#define CSB_IO    0x04
#define CSB_CD    0x08
#define CSB_MSG   0x10
#define CSB_REQ   0x20
#define CSB_BSY   0x40
#define CSB_RST   0x80

/* BSR (read reg 5) bits */
#define BSR_ACK        0x01
#define BSR_ATN        0x02
#define BSR_BUSY_ERR   0x04
#define BSR_PHASE_MATCH 0x08
#define BSR_IRQ        0x10
#define BSR_PAR_ERR    0x20
#define BSR_DRQ        0x40
#define BSR_END_DMA    0x80

/* SCSI bus phases (C/D, I/O, MSG bits) */
#define PHASE_DO   0            /* data out */
#define PHASE_DI   CSB_IO       /* data in */
#define PHASE_CMD  CSB_CD       /* command */
#define PHASE_ST   (CSB_CD | CSB_IO)         /* status */
#define PHASE_MI   (CSB_CD | CSB_IO | CSB_MSG) /* message in */
#define PHASE_MO   (CSB_CD | CSB_MSG)          /* message out */

static void ncr5380_do_command(NCR5380State *s);

static void ncr5380_update_irq(NCR5380State *s)
{
    int level = (s->bsr & BSR_IRQ) ? 1 : 0;

    qemu_set_irq(s->irq, level);
}

static void ncr5380_set_phase(NCR5380State *s, uint8_t phase)
{
    s->phase = phase;
    s->csb = (s->csb & ~(CSB_CD | CSB_IO | CSB_MSG)) | phase;
}

static void ncr5380_reset_bus(NCR5380State *s)
{
    if (s->req) {
        scsi_req_unref(s->req);
        s->req = NULL;
    }
    s->dev = NULL;
    s->csb = 0;
    s->bsr = 0;
    s->phase = PHASE_DO;
    s->cmd_len = 0;
    s->buf_len = 0;
    s->buf_pos = 0;
    s->status = 0;
    s->msg_in = 0;
}

/* pull the next chunk of a data-in transfer from the SCSI layer */
static void ncr5380_fill_buffer(NCR5380State *s)
{
    if (!s->req || s->buf_pos < s->buf_len) {
        return;
    }
    if (s->async_len == 0) {
        return;
    }
    s->buf = s->async_buf;
    s->buf_len = s->async_len;
    s->buf_pos = 0;
}

static void ncr5380_command_complete(SCSIRequest *req, size_t resid);

static void ncr5380_do_command(NCR5380State *s)
{
    uint32_t buflen;

    if (!s->dev) {
        return;
    }
    trace_ncr5380_command(s->cmd[0], s->cmd_len);
    /*
     * The Mac ROM's boot reader is "blind": it does not issue TEST UNIT
     * READY / REQUEST SENSE to absorb the power-on UNIT ATTENTION that a
     * freshly reset SCSI device reports.  A real, already-running disk
     * has long since cleared it, so drop any pending unit-attention on
     * the target to match that warm-drive behaviour.
     */
    s->dev->unit_attention.key = 0;
    s->bus.unit_attention.key = 0;
    s->req = scsi_req_new(s->dev, 0, s->lun, s->cmd, s->cmd_len, s);
    buflen = scsi_req_enqueue(s->req);
    s->async_len = 0;
    s->buf_len = 0;
    s->buf_pos = 0;

    if (buflen != 0) {
        /*
         * Data phase.  The first chunk is fetched by ncr5380_ack_release
         * (which runs right after this, on the final command byte's ACK
         * release); kicking scsi_req_continue here as well would start a
         * second async transfer and trip the scsi-disk in-flight assert.
         */
        if (s->req->cmd.mode == SCSI_XFER_FROM_DEV) {
            ncr5380_set_phase(s, PHASE_DI);
        } else {
            ncr5380_set_phase(s, PHASE_DO);
        }
    } else {
        /* no data; wait for completion callback to move to STATUS */
    }
    /*
     * REQ for the new phase is asserted only once the initiator releases
     * ACK from the final command byte (see ncr5380_ack_release); asserting
     * it here, while ACK is still high, would make the driver's "wait for
     * REQ to clear" handshake on the last command byte deadlock.
     */
}

/* select a target: ODR holds the initiator+target ID bitmask */
static void ncr5380_select(NCR5380State *s)
{
    int id;

    /* target id = the non-initiator (bit 7 = host) bit set in ODR */
    s->dev = NULL;
    for (id = 0; id < 7; id++) {
        if ((s->odr & 0x7f) & (1 << id)) {
            SCSIDevice *d = scsi_device_find(&s->bus, 0, id, 0);
            if (d) {
                s->dev = d;
                s->target = id;
                break;
            }
        }
    }

    if (s->dev) {
        s->csb |= CSB_BSY;              /* target asserted BSY */
        ncr5380_set_phase(s, PHASE_CMD);
        s->csb |= CSB_REQ;
        s->bsr |= BSR_PHASE_MATCH;
        s->cmd_len = 0;
        trace_ncr5380_select_ok(s->target);
    } else {
        /* selection timeout: no BSY appears */
        s->csb &= ~CSB_BSY;
        trace_ncr5380_select_timeout();
    }
}

static uint64_t ncr5380_read(void *opaque, hwaddr addr, unsigned size)
{
    NCR5380State *s = opaque;
    int reg = (addr >> s->reg_shift) & 7;
    uint64_t val = 0;

    switch (reg) {
    case R_CSD:
        /* current data byte during data-in / status / message-in */
        if (s->phase & CSB_IO) {
            val = s->last_data;
        } else {
            val = s->odr;
        }
        break;
    case R_ICR_R:
        /* bits 5 (lost-arb) and 6 (arb-in-progress) are read-only status */
        val = s->icr;
        break;
    case R_MR_R:
        val = s->mr;
        break;
    case R_TCR_R:
        val = s->tcr;
        break;
    case R_CSB:
        val = s->csb;
        break;
    case R_BSR:
        {
            /*
             * Phase match: the phase programmed into the TCR (bit0 I/O,
             * bit1 C/D, bit2 MSG) equals the actual bus phase.
             */
            uint8_t want = ((s->tcr & 0x01) ? CSB_IO : 0)
                         | ((s->tcr & 0x02) ? CSB_CD : 0)
                         | ((s->tcr & 0x04) ? CSB_MSG : 0);

            val = s->bsr & ~BSR_PHASE_MATCH;
            if (s->dev && want == s->phase) {
                val |= BSR_PHASE_MATCH;
            }
        }
        break;
    case R_IDR:
        val = s->last_data;
        break;
    case R_RPI:
        /* reading resets parity/interrupt */
        s->bsr &= ~(BSR_IRQ | BSR_PAR_ERR | BSR_BUSY_ERR);
        ncr5380_update_irq(s);
        val = 0;
        break;
    }

    trace_ncr5380_read(reg, (uint8_t)val);
    return val;
}

static void ncr5380_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    NCR5380State *s = opaque;
    int reg = (addr >> s->reg_shift) & 7;

    trace_ncr5380_write(reg, (uint8_t)val);

    switch (reg) {
    case R_ODR:
        s->odr = val;
        /* command / message / data-out byte handed over on ACK */
        break;
    case R_ICR:
        {
            uint8_t old = s->icr;

            /* keep read-only status bits 5 (LA) and 6 (AIP) */
            s->icr = (val & 0x9f) | (s->icr & 0x60);
            if (val & ICR_ASSERT_RST) {
                ncr5380_reset_bus(s);
                s->icr = 0;
                break;
            }
            /*
             * Selection completes on the transition where SEL and the
             * data drivers are asserted while BSY is released: the target
             * ID is then on the data bus (ODR, own-id bit 7 excluded).
             */
            if ((val & ICR_ASSERT_SEL) && (val & ICR_ASSERT_DATA)
                && !(val & ICR_ASSERT_BSY) && (old & ICR_ASSERT_BSY)
                && !s->dev) {
                ncr5380_select(s);
            }
            /*
             * REQ/ACK byte handshake for the non-DMA phases: the target
             * asserts REQ, the initiator transfers a byte and asserts
             * ACK (REQ drops); when ACK is released REQ re-asserts for
             * the next byte.  The driver paces on REQ (CSB bit 5).
             */
            if ((val & ICR_ASSERT_ACK) && !(old & ICR_ASSERT_ACK)) {
                ncr5380_ack(s);
            }
            if (!(val & ICR_ASSERT_ACK) && (old & ICR_ASSERT_ACK)) {
                ncr5380_ack_release(s);
            }
        }
        break;
    case R_MR:
        s->mr = val;
        if (val & MR_ARBITRATE) {
            /* arbitration always wins on this single-initiator bus */
            s->icr &= ~ICR_ARB_LOST;
            s->icr |= ICR_ARB_IN_PROG;
        } else {
            s->icr &= ~(ICR_ARB_IN_PROG | ICR_ARB_LOST);
        }
        break;
    case R_TCR:
        s->tcr = val;
        break;
    case R_SER:
        s->ser = val;
        break;
    case R_DMASND:
    case R_DMATR:
    case R_DMAIR:
        /* pseudo-DMA is driven through the PDMA aperture; arm DRQ */
        s->dma_mode = reg;
        s->bsr |= BSR_DRQ;
        break;
    }
}

/* initiator asserted ACK: accept/deliver the current byte and drop REQ */
void ncr5380_ack(NCR5380State *s)
{
    s->csb &= ~CSB_REQ;

    switch (s->phase) {
    case PHASE_CMD:
        if (s->cmd_len < (int)sizeof(s->cmd)) {
            s->cmd[s->cmd_len++] = s->odr;
        }
        break;
    case PHASE_DI:
        if (s->buf_pos < s->buf_len) {
            s->buf_pos++;
        }
        break;
    case PHASE_DO:
        if (s->buf_pos < s->buf_len) {
            s->buf[s->buf_pos++] = s->odr;
        }
        break;
    case PHASE_ST:
        s->status_done = true;
        break;
    case PHASE_MI:
        s->msg_done = true;
        break;
    default:
        break;
    }
}

/* initiator released ACK: advance the phase and re-assert REQ as needed */
void ncr5380_ack_release(NCR5380State *s)
{
    switch (s->phase) {
    case PHASE_CMD:
        if (s->cmd_len >= scsi_cdb_length(s->cmd) && s->cmd_len > 0) {
            ncr5380_do_command(s);      /* → DI/DO, or completes → ST */
            /* fall through to arm REQ for whatever phase we are now in */
        } else {
            s->csb |= CSB_REQ;          /* next command byte */
            break;
        }
        /* fallthrough */
    case PHASE_DI:
        if (s->phase == PHASE_DI) {
            if (s->buf_pos < s->buf_len) {
                s->last_data = s->buf[s->buf_pos];
                s->csb |= CSB_REQ;
            } else {
                /*
                 * Chunk drained.  Ask the SCSI layer for the next chunk
                 * (async): transfer_data or command_complete will assert
                 * REQ for the next data byte or the status phase.
                 */
                s->buf_len = 0;
                s->buf_pos = 0;
                if (s->req && !s->xfer_pending) {
                    s->xfer_pending = true;
                    scsi_req_continue(s->req);
                }
            }
            break;
        }
        if (s->phase == PHASE_ST) {
            s->csb |= CSB_REQ;          /* status byte available */
        }
        break;
    case PHASE_DO:
        if (s->buf_pos < s->buf_len) {
            s->csb |= CSB_REQ;
        } else {
            s->buf_len = 0;
            s->buf_pos = 0;
            if (s->req && !s->xfer_pending) {
                s->xfer_pending = true;
                scsi_req_continue(s->req);
            }
        }
        break;
    case PHASE_ST:
        if (s->status_done) {
            ncr5380_set_phase(s, PHASE_MI);
            s->last_data = 0x00;        /* COMMAND COMPLETE message */
            s->csb |= CSB_REQ;
        } else {
            s->csb |= CSB_REQ;
        }
        break;
    case PHASE_MI:
        if (s->msg_done) {
            /* bus free: end of transaction */
            s->csb &= ~(CSB_BSY | CSB_REQ);
            s->bsr |= BSR_IRQ;
            ncr5380_update_irq(s);
            if (s->req) {
                scsi_req_unref(s->req);
                s->req = NULL;
            }
            s->dev = NULL;
        } else {
            s->csb |= CSB_REQ;
        }
        break;
    default:
        break;
    }
}

/* pseudo-DMA data movement: one byte per aperture access */
uint8_t ncr5380_pdma_read(NCR5380State *s)
{
    uint8_t v = 0;

    ncr5380_fill_buffer(s);
    if (s->buf_pos < s->buf_len) {
        v = s->buf[s->buf_pos++];
        s->last_data = v;
    }
    if (s->buf_pos >= s->buf_len && s->req) {
        /* chunk drained: fetch the next one or finish → STATUS phase */
        s->buf_len = 0;
        s->buf_pos = 0;
        if (s->async_len == 0) {
            if (!s->xfer_pending) {
                s->xfer_pending = true;
                scsi_req_continue(s->req);
            }
        } else {
            ncr5380_fill_buffer(s);
        }
    }
    return v;
}

void ncr5380_pdma_write(NCR5380State *s, uint8_t val)
{
    if (s->phase == PHASE_CMD) {
        if (s->cmd_len < (int)sizeof(s->cmd)) {
            s->cmd[s->cmd_len++] = val;
        }
        if (s->cmd_len >= scsi_cdb_length(s->cmd) && s->cmd_len > 0) {
            ncr5380_do_command(s);
        }
        return;
    }
    if (s->buf_pos < s->buf_len) {
        s->buf[s->buf_pos++] = val;
    }
    if (s->buf_pos >= s->buf_len && s->req && !s->xfer_pending) {
        s->xfer_pending = true;
        scsi_req_continue(s->req);
    }
}

static const MemoryRegionOps ncr5380_ops = {
    .read = ncr5380_read,
    .write = ncr5380_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/* SCSI bus callbacks */

static void ncr5380_transfer_data(SCSIRequest *req, uint32_t len)
{
    NCR5380State *s = req->hba_private;

    trace_ncr5380_transfer(len);
    s->xfer_pending = false;
    s->async_len = len;
    s->async_buf = scsi_req_get_buf(req);
    ncr5380_fill_buffer(s);
    if (s->phase == PHASE_DI && s->buf_pos < s->buf_len) {
        s->last_data = s->buf[s->buf_pos];
        /*
         * Assert REQ for the first byte of this chunk, unless we are
         * still mid-handshake on the final command byte (ACK high), in
         * which case ncr5380_ack_release will assert it.
         */
        if (!(s->icr & ICR_ASSERT_ACK)) {
            s->csb |= CSB_REQ;
        }
    }
    s->bsr |= BSR_DRQ;
}

static void ncr5380_command_complete(SCSIRequest *req, size_t resid)
{
    NCR5380State *s = req->hba_private;

    trace_ncr5380_complete(req->status);
    s->xfer_pending = false;
    s->status = req->status;
    s->async_len = 0;
    s->buf_len = 0;
    s->buf_pos = 0;
    ncr5380_set_phase(s, PHASE_ST);
    s->last_data = s->status;
    s->status_done = false;
    s->msg_done = false;
    /* REQ asserted by ncr5380_ack_release on the next ACK release */
    if (!(s->icr & ICR_ASSERT_ACK)) {
        s->csb |= CSB_REQ;      /* no handshake in flight (no-data command) */
    }
}

static void ncr5380_request_cancelled(SCSIRequest *req)
{
    NCR5380State *s = req->hba_private;

    if (req == s->req) {
        scsi_req_unref(s->req);
        s->req = NULL;
        s->dev = NULL;
    }
}

static const struct SCSIBusInfo ncr5380_scsi_info = {
    .tcq = false,
    .max_target = 7,
    .max_lun = 7,
    .transfer_data = ncr5380_transfer_data,
    .complete = ncr5380_command_complete,
    .cancel = ncr5380_request_cancelled,
};

static void ncr5380_reset_hold(Object *obj, ResetType type)
{
    NCR5380State *s = NCR5380(obj);

    s->icr = 0;
    s->mr = 0;
    s->tcr = 0;
    s->ser = 0;
    s->odr = 0;
    s->last_data = 0;
    s->dma_mode = 0;
    ncr5380_reset_bus(s);
    ncr5380_update_irq(s);
}

static void ncr5380_realize(DeviceState *dev, Error **errp)
{
    NCR5380State *s = NCR5380(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &ncr5380_ops, s,
                          "ncr5380", 8 << s->reg_shift);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);

    scsi_bus_init(&s->bus, sizeof(s->bus), dev, &ncr5380_scsi_info);
}

static const Property ncr5380_properties[] = {
    DEFINE_PROP_UINT8("reg-shift", NCR5380State, reg_shift, 4),
};

static void ncr5380_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = ncr5380_realize;
    device_class_set_props(dc, ncr5380_properties);
    rc->phases.hold = ncr5380_reset_hold;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo ncr5380_info = {
    .name = TYPE_NCR5380,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NCR5380State),
    .class_init = ncr5380_class_init,
};

static void ncr5380_register_types(void)
{
    type_register_static(&ncr5380_info);
}

type_init(ncr5380_register_types)
