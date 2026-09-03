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
#include "hw/core/cpu.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"
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

static uint64_t ncr5380_guest_pc(void)
{
    if (current_cpu) {
        CPUClass *cc = CPU_GET_CLASS(current_cpu);

        if (cc->get_pc) {
            return cc->get_pc(current_cpu);
        }
    }
    return 0;
}


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
    s->out_pos = 0;
    s->collecting = false;
    s->cmd_done = false;
    s->status = 0;
    s->msg_in = 0;
    s->sun_dma_done = false;
    s->sun_cmd_complete = false;
    s->sun_msg_taken = false;
}

static void ncr5380_grow_dbuf(NCR5380State *s, uint32_t size)
{
    if (s->dbuf_size < size) {
        s->dbuf = g_realloc(s->dbuf, size);
        s->dbuf_size = size;
    }
}

/*
 * Run the AIO machinery until the current request's completion callback
 * has fired.  Called with the whole-transfer buffer in place, so every
 * transfer_data callback immediately re-continues the request: the
 * command runs to completion synchronously from the guest's viewpoint.
 */
static void ncr5380_pump(NCR5380State *s)
{
    while (!s->cmd_done && s->req) {
        if (s->dev && s->dev->conf.blk) {
            blk_drain(s->dev->conf.blk);
        } else {
            aio_poll(qemu_get_aio_context(), true);
        }
    }
}

/* all data consumed/delivered: move the bus to the STATUS phase */
static void ncr5380_enter_status(NCR5380State *s)
{
    ncr5380_set_phase(s, PHASE_ST);
    s->last_data = s->status;
    s->status_done = false;
    s->msg_done = false;
    s->bsr &= ~BSR_DRQ;
    /*
     * REQ for the new phase may be asserted only when the initiator is
     * not holding ACK; otherwise ncr5380_ack_release asserts it.
     */
    if (!(s->icr & ICR_ASSERT_ACK)) {
        s->csb |= CSB_REQ;
    }
}

/* the guest delivered the last data-out byte: feed it all to the device */
static void ncr5380_flush_data_out(NCR5380State *s)
{
    if (!s->req) {
        return;
    }
    s->out_pos = 0;
    s->collecting = true;
    scsi_req_continue(s->req);
    ncr5380_pump(s);
    s->collecting = false;
    ncr5380_enter_status(s);
    /*
     * In DMA send mode the empty output register re-asserts DRQ once
     * the target has taken the final byte; the Mac Plus ROM's blind
     * writer waits for that trailing DRQ before it drops DMA mode.
     */
    if (s->mr & MR_DMA_MODE) {
        s->bsr |= BSR_DRQ;
    }
}

static void ncr5380_do_command(NCR5380State *s)
{
    int32_t buflen;

    if (!s->dev) {
        return;
    }
    trace_ncr5380_command(s->cmd[0], s->cmd_len);
    if (trace_event_get_state_backends(TRACE_NCR5380_CMD_PC) && current_cpu) {
        CPUClass *cc = CPU_GET_CLASS(current_cpu);

        if (cc->get_pc) {
            trace_ncr5380_cmd_pc(cc->get_pc(current_cpu));
        }
    }
    /*
     * The Mac ROM's boot reader is "blind": it does not issue TEST UNIT
     * READY / REQUEST SENSE to absorb the power-on UNIT ATTENTION that a
     * freshly reset SCSI device reports.  A real, already-running disk
     * has long since cleared it, so drop any pending unit-attention on
     * the target to match that warm-drive behaviour.
     */
    s->dev->unit_attention.key = 0;
    s->bus.unit_attention.key = 0;
    s->cmd_done = false;
    s->sun_dma_done = false;
    s->buf_len = 0;
    s->buf_pos = 0;
    s->out_pos = 0;
    s->req = scsi_req_new(s->dev, 0, s->lun, s->cmd, s->cmd_len, s);
    buflen = scsi_req_enqueue(s->req);

    if (buflen > 0 && !s->cmd_done) {
        /*
         * Data-in.  Pre-read the ENTIRE transfer synchronously before
         * showing the guest the data phase.  The Mac ROM/SCSI Manager
         * reads blind (pseudo-DMA, or CSD polling with a trailing
         * "handshake until the phase changes" loop): if the DI->ST
         * transition waited on an async aiocb, the trailing loop would
         * clock in phantom stale bytes past the real transfer and
         * corrupt the guest's buffer (observed: 515 phantom bytes after
         * a 512-byte READ -> double MMU fault).
         */
        ncr5380_grow_dbuf(s, buflen);
        s->collecting = true;
        scsi_req_continue(s->req);
        ncr5380_pump(s);
        s->collecting = false;
        if (trace_event_get_state_backends(TRACE_NCR5380_DATAIN)) {
            uint32_t sum = 0, lba = 0, i;

            for (i = 0; i < s->buf_len; i++) {
                sum = (sum * 31 + s->dbuf[i]) & 0xffffffff;
            }
            if (s->cmd[0] == 0x08) {
                lba = ((s->cmd[1] & 0x1f) << 16) | (s->cmd[2] << 8)
                    | s->cmd[3];
            } else if (s->cmd[0] == 0x28) {
                lba = (s->cmd[2] << 24) | (s->cmd[3] << 16)
                    | (s->cmd[4] << 8) | s->cmd[5];
            }
            trace_ncr5380_datain(s->cmd[0], lba, s->buf_len, sum);
        }
        if (s->buf_len > 0) {
            ncr5380_set_phase(s, PHASE_DI);
            s->buf_pos = 0;
            s->last_data = s->dbuf[0];
            s->bsr |= BSR_DRQ;
            if (!(s->icr & ICR_ASSERT_ACK)) {
                s->csb |= CSB_REQ;
            }
        } else {
            ncr5380_enter_status(s);
        }
    } else if (buflen < 0 && !s->cmd_done) {
        /*
         * Data-out: collect the whole transfer from the guest first;
         * ncr5380_flush_data_out feeds it to the device when the last
         * byte arrives.
         */
        ncr5380_grow_dbuf(s, -buflen);
        s->buf_len = -buflen;
        s->buf_pos = 0;
        ncr5380_set_phase(s, PHASE_DO);
        s->bsr |= BSR_DRQ;
        if (!(s->icr & ICR_ASSERT_ACK)) {
            s->csb |= CSB_REQ;
        }
    }
    /*
     * No-data commands complete during scsi_req_enqueue: the completion
     * callback has already moved the bus to the STATUS phase.
     */
}

/* complete selection of an explicit target id */
static void ncr5380_select_target(NCR5380State *s, int id)
{
    SCSIDevice *d = scsi_device_find(&s->bus, 0, id, 0);

    s->dev = d;
    if (s->dev) {
        s->target = id;
        s->csb |= CSB_BSY;              /* target asserted BSY */
        ncr5380_set_phase(s, PHASE_CMD);
        s->csb |= CSB_REQ;
        s->bsr |= BSR_PHASE_MATCH | BSR_IRQ;
        s->cmd_len = 0;
        ncr5380_update_irq(s);
        trace_ncr5380_select_ok(s->target);
        /*
         * Sun si: the CDB was pre-staged into sun_fifo via reg0 writes.  On
         * a real board the si clocks it out during the command phase; here we
         * hand the whole CDB to the target now, which moves the bus to the
         * data (or status) phase the driver then services via DMA.
         */
        if (s->sun_mode && s->sun_fifo_count) {
            int i, clen, n = s->sun_fifo_count;

            for (i = 0; i < n && i < (int)sizeof(s->cmd); i++) {
                s->cmd[i] = s->sun_fifo[i];
            }
            /*
             * The CDB length is defined by the opcode (cmd[0]), not by how many
             * bytes the driver happened to write to reg0.  The PROM/ufsboot
             * "si" path stages exactly the CDB (fifo count == CDB length), but
             * the kernel sd driver stages a padded 16-byte command buffer, so
             * using the raw fifo count would hand a 16-byte "TUR" (opcode 0x00)
             * to the SCSI layer and corrupt the transfer.  Use the SCSI-defined
             * length, capped by what was actually staged.
             */
            clen = scsi_cdb_length(s->cmd);
            s->cmd_len = (clen > 0 && clen <= n) ? clen : n;
            s->sun_fifo_count = 0;
            ncr5380_do_command(s);
        }
    } else {
        /* selection timeout: no BSY appears; the bus stays released */
        s->csb = 0;
        trace_ncr5380_select_timeout();
    }
}

/* select a target: ODR holds the initiator+target ID bitmask */
static void ncr5380_select(NCR5380State *s)
{
    int id;

    /* target id = the non-initiator (bit 7 = host) bit set in ODR */
    for (id = 0; id < 7; id++) {
        if ((s->odr & 0x7f) & (1 << id)) {
            if (scsi_device_find(&s->bus, 0, id, 0)) {
                break;
            }
        }
    }
    ncr5380_select_target(s, id);
}

/*
 * Sun 3/80 si read-map quirk: the PROM driver derives the live SCSI bus phase
 * from the low 3 bits of reg2 (0x66000010), and the bus-and-status (with
 * PHASE_MATCH) from reg3 (0x66000014), rather than from CSB/BSR the way a
 * stock 5380 driver does.  reg2 uses the *standard* SCSI phase encoding
 * (bit0 = I/O, bit1 = C/D, bit2 = MSG):
 *
 *   1 = DATA IN, 0 = DATA OUT  -> the driver's state machine ARMS the si DMA
 *                                 (0xfeff0e04: writes dma_addr + si_csr|=SBC_IP)
 *   3 = STATUS-encoding, reused by the si driver as "data transfer complete"
 *                              -> it finalises the DMA (0xfeff0e6e), reading the
 *                                 auto-incremented dma_addr as the byte count
 *   7 = MESSAGE IN, reused for STATUS -> it reads the status/message bytes
 *   2 = COMMAND -> dispatcher error (never presented: we run the CDB at select)
 *
 * So a data-in transfer must be presented first as DATA (reg2==1) so the arm
 * fires, then — once the si DMA engine has drained it (sun_dma_done) — as the
 * "complete" code (reg2==3) so the finalise reads a non-zero byte count.
 * Returning 3 for the data phase up-front (the old behaviour) made the driver
 * jump straight to finalise without ever arming DMA, so dma_addr never
 * advanced and the returned count was 0 — the PROM then skipped the disk READ.
 */
static uint8_t sun_phase_bits(NCR5380State *s)
{
    if (!s->dev) {
        /*
         * After a blind (ufsboot / sd-open) command has completed and torn
         * the bus down to bus-free, present MESSAGE IN (7) for the fixed
         * number of trailing state-machine calls the transfer wrapper makes,
         * so it re-takes the benign success branch rather than the phase-0
         * retry path (err5 -> -1 -> "bdevvp: bad open").  The PROM probe's
         * ACK-handshake teardown does not set sun_cmd_complete, so its
         * trailing calls still see phase 0 (unchanged, benign).
         */
        return s->sun_cmd_complete ? 7 : 0;
    }
    switch (s->phase) {
    case PHASE_DI:
        return s->sun_dma_done ? 3 : 1;
    case PHASE_DO:
        return s->sun_dma_done ? 3 : 0;
    case PHASE_ST:
    case PHASE_MI:
        return 7;
    case PHASE_CMD:
        return 2;
    default:
        return 0;
    }
}

/*
 * Sun si: the driver, having finalised the data phase (read back the
 * auto-incremented dma_addr as the byte count), clears the si DMA latches;
 * the board then advances the bus to STATUS.  Model that here.
 */
void ncr5380_sun_to_status(NCR5380State *s)
{
    if (s->dev && (s->phase == PHASE_DI || s->phase == PHASE_DO)) {
        s->sun_dma_done = false;
        ncr5380_enter_status(s);
    }
}

static uint64_t ncr5380_read(void *opaque, hwaddr addr, unsigned size)
{
    NCR5380State *s = opaque;
    int reg = (addr >> s->reg_shift) & 7;
    uint64_t val = 0;

    switch (reg) {
    case R_CSD:
        /*
         * Current data byte.  During a data-in phase the Mac driver can
         * "blind" read: it reads CSD repeatedly without a per-byte ACK,
         * relying on the hardware to advance.  Deliver the current byte
         * and step to the next so successive reads drain the buffer.
         */
        if (s->sun_mode && !s->dev && s->sun_cmd_complete) {
            /*
             * Trailing state-machine calls after a blind completion: the
             * benign phase-7 completion handler (0x213c24) reads reg0 as the
             * final message byte and dispatches on it.  Present COMMAND
             * COMPLETE (0x00) so it settles into the done state (32) with no
             * error, rather than a stale CDB/data byte that trips err4.
             * Record that the trailing message has been taken so reg3 flips
             * from 0x08 (phase dispatch) to 0x20 (BSR dispatch) and the next
             * call completes the state machine to its "done" state 0.
             */
            val = 0x00;
            s->sun_msg_taken = true;
        } else if (s->phase == PHASE_DI) {
            if (s->buf_pos < s->buf_len) {
                val = s->dbuf[s->buf_pos++];
                s->last_data = val;
                /*
                 * Buffer exhausted?  The DI->ST transition happens on
                 * the ACK release that follows this read (the ROM's
                 * per-byte handshake), see ncr5380_ack_release.
                 */
            } else {
                trace_ncr5380_stale_read(s->buf_pos, s->buf_len);
                val = s->last_data;
            }
        } else if (s->sun_mode && !(s->icr & ICR_ASSERT_ACK)
                   && (s->phase == PHASE_ST || s->phase == PHASE_MI)) {
            /*
             * Sun si blind STATUS/MESSAGE read.  ufsboot's polled driver reads
             * reg0 in the status/message phase *without* asserting ACK (unlike
             * the PROM probe, which does the reg1/ICR ACK handshake), and
             * spins `while (reg5 != 0) read reg0` waiting for the bus to go
             * idle (0x213aba).  With reg5-IRQ modelled as the live /REQ, that
             * spin can only end if reading the byte here also advances the
             * handshake -- take the byte, drop /REQ, and step the phase:
             * STATUS -> MESSAGE IN, MESSAGE IN -> bus free.  Gated on ACK being
             * de-asserted so the PROM's explicit-handshake path is untouched.
             */
            val = s->last_data;
            s->csb &= ~CSB_REQ;
            if (s->phase == PHASE_ST) {
                ncr5380_set_phase(s, PHASE_MI);
                s->last_data = 0x00;            /* COMMAND COMPLETE message */
                s->csb |= CSB_REQ;
            } else {
                /* MESSAGE IN consumed: bus free, end of transaction */
                s->csb = 0;
                s->phase = PHASE_DO;
                s->bsr |= BSR_IRQ;
                ncr5380_update_irq(s);
                if (s->req) {
                    scsi_req_unref(s->req);
                    s->req = NULL;
                }
                s->dev = NULL;
                /*
                 * Trailing state-machine calls after this blind completion
                 * must see MESSAGE IN (7), not phase 0, or the phase-0 retry
                 * path exhausts (err5) and the wrapper returns -1.
                 */
                s->sun_cmd_complete = true;
            }
        } else if (s->phase & CSB_IO) {
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
        val = s->sun_mode ? sun_phase_bits(s) : s->mr;
        break;
    case R_TCR_R:
        if (s->sun_mode) {
            /*
             * reg3 read drives the driver's dispatcher (values are decoded
             * against {0x80,0x50,0x40,0x20,0x18,0x10,0x08,...}): 0x20 routes
             * into the STATE dispatcher (advance/complete, incl. state 32 ->
             * done) while 0x08 routes into the phase handler that services an
             * active data/status phase.  Present 0x08 while the target has a
             * live REQ to service, else 0x20 so the state machine advances.
             */
            if (s->dev && (s->csb & CSB_REQ)) {
                val = 0x08;
            } else if (!s->dev && s->sun_cmd_complete && !s->sun_msg_taken) {
                /*
                 * Trailing call after a blind completion, before the final
                 * MESSAGE byte has been taken (state 27): present 0x08 so the
                 * phase-match dispatcher (0x213d36) routes to the *phase*
                 * handler (0x213d00), which with reg2==7 reads the message
                 * (reg0) and advances state 27 -> 32.  Once the message is
                 * taken, fall through to 0x20 so the *BSR* dispatcher
                 * (0x213b50) completes state 32 -> 0 (done).
                 */
                val = 0x08;
            } else {
                val = 0x20;
            }
        } else {
            val = s->tcr;
        }
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
            /*
             * Sun si: model reg5 (BSR) bit4 (IRQ) as a *live per-/REQ signal*,
             * not a latch.  The polled boot driver reads reg5 two opposite
             * ways with no reg7/RPI clear in between: at the start of a
             * STATUS/MESSAGE byte it waits for `(reg5 & 0x1f) != 0` = "a bus
             * event/byte is pending" (PROM 0xfeff12da; masking IRQ there hangs
             * the probe), and after servicing it waits for `reg5 == 0` = "bus
             * idle" before completing (ufsboot 0x213aba).  Both are satisfied
             * if IRQ tracks the live target /REQ: asserted while a phase byte
             * is waiting to be transferred, de-asserted once the initiator has
             * taken it (CSB_REQ drops).  So drive bit4 from CSB_REQ rather than
             * the stored latch.
             */
            if (s->sun_mode) {
                val &= ~BSR_IRQ;
                if (s->csb & CSB_REQ) {
                    val |= BSR_IRQ;
                }
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

    trace_ncr5380_read(reg, (uint8_t)val, ncr5380_guest_pc());
    return val;
}

static void ncr5380_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    NCR5380State *s = opaque;
    int reg = (addr >> s->reg_shift) & 7;

    trace_ncr5380_write(reg, (uint8_t)val, ncr5380_guest_pc());

    switch (reg) {
    case R_ODR:
        s->odr = val;
        /*
         * Sun si: the driver stages the CDB by writing all its bytes to the
         * data register (reg0) before it arms selection.  Buffer them; they
         * are delivered to the target as the command once selection lands.
         */
        if (s->sun_mode && !s->dev
            && s->sun_fifo_count < sizeof(s->sun_fifo)) {
            s->sun_fifo[s->sun_fifo_count++] = val;
        }
        /* command / message / data-out byte handed over on ACK */
        break;
    case R_ICR:
        {
            uint8_t old = s->icr;

            /* keep read-only status bits 5 (LA) and 6 (AIP) */
            s->icr = (val & 0x9f) | (s->icr & 0x60);
            /*
             * In sun-mode ICR bit7 is a DMA-arming strobe (the driver writes
             * 0x80/0x90 while setting up a transfer), not a SCSI bus reset.
             */
            if ((val & ICR_ASSERT_RST) && !s->sun_mode) {
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
        /*
         * Sun 3/80 "si" selection: the driver stages the CDB into reg0, then
         * writes reg2 (MR) = the *target id* (low 3 bits) followed by ICR to
         * launch selection.  The "arbitrate" bit (0x01) is NOT a mode flag
         * here — it is just bit 0 of the target id, so it is set only for ODD
         * targets.  The PROM/ufsboot boot target 3 (odd), so the old
         * "trigger on MR_ARBITRATE" heuristic happened to work; but the SunOS
         * kernel autoconfig probes ALL ids, and its first probe (target 6,
         * MR=0x06, bit0=0) never fired selection -> dev stayed NULL -> the
         * probe state machine looped on CSB=0 forever.  Trigger selection
         * whenever a CDB has been staged (sun_fifo_count > 0) and no device is
         * yet selected, using target = MR & 7 regardless of bit 0.  (Non-
         * selection MR writes during a transfer have dev set / the FIFO
         * drained, so they never re-trigger.)
         */
        if (s->sun_mode && !s->dev && s->sun_fifo_count > 0) {
            s->icr &= ~ICR_ARB_LOST;
            s->icr |= ICR_ARB_IN_PROG;
            /* new selection: a fresh command, no trailing-completion yet */
            s->sun_cmd_complete = false;
            s->sun_msg_taken = false;
            ncr5380_select_target(s, val & 0x07);
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
        /* the byte was already consumed and advanced by the CSD read */
        break;
    case PHASE_DO:
        if (s->buf_pos < s->buf_len) {
            s->dbuf[s->buf_pos++] = s->odr;
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
                s->last_data = s->dbuf[s->buf_pos];
                s->csb |= CSB_REQ;
            } else {
                /*
                 * Whole transfer consumed: move to STATUS right here,
                 * synchronously, so the initiator's very next phase
                 * poll sees the change (no phantom-data window).
                 */
                ncr5380_enter_status(s);
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
            /* last byte received: run the whole transfer synchronously */
            ncr5380_flush_data_out(s);
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
            /*
             * Bus free: end of transaction.  The target releases ALL
             * bus lines, phase lines included: CSB must read 0.  Stale
             * phase bits (0x1c) latched here make the Mac driver's
             * wait-for-bus-free poll spin into its timeout and fail an
             * otherwise perfect transfer.
             */
            s->csb = 0;
            s->phase = PHASE_DO;
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

/*
 * Is a pseudo-DMA byte ready to move?  The Mac's handshake aperture
 * bus-errors when DRQ does not arrive in time; the machine uses this
 * to fault accesses outside an active data phase.
 */
bool ncr5380_pdma_ready(NCR5380State *s, bool out)
{
    uint8_t want = out ? PHASE_DO : PHASE_DI;

    return s->phase == want && s->buf_pos < s->buf_len;
}

/* pseudo-DMA data movement: one byte per aperture access */
uint8_t ncr5380_pdma_read(NCR5380State *s)
{
    uint8_t v = 0;

    if (s->phase == PHASE_DI && s->buf_pos < s->buf_len) {
        v = s->dbuf[s->buf_pos++];
        s->last_data = v;
        if (s->buf_pos >= s->buf_len) {
            /*
             * Transfer complete.  Pseudo-DMA has no per-byte ACK, so
             * flip to STATUS right now: the blind loop's byte counter
             * runs out on this access and the ROM's trailing phase
             * poll must already see the change.
             *
             * Sun si: do NOT advance to STATUS here.  The si driver first
             * finalises the data phase (reads back the auto-incremented
             * dma_addr as the byte count) while the bus still reads DATA
             * (reg2==3, sun_dma_done); the board only moves to STATUS once
             * the driver clears the si DMA latches — ncr5380_sun_to_status,
             * driven from sun3x.c, does that.
             */
            if (!s->sun_mode) {
                ncr5380_enter_status(s);
            }
        } else {
            /*
             * Model the per-byte SCSI /REQ handshake: the target
             * deasserts /REQ once the initiator's pseudo-DMA read
             * (DACK) has taken this byte, and reasserts it for the
             * next one.  A blind reader that drains the whole transfer
             * in a single loop only ever samples /REQ *after* the last
             * byte (by which point enter_status above has moved the bus
             * to STATUS), so this is invisible to it -- e.g. the IIci/
             * IIsi ROMs are unaffected.  But a *chunked* reader hangs
             * without it: the Mac II / SE30 ROM SCSI Manager drains a
             * multi-sector transfer 512 bytes at a time and, between
             * chunks, spins on CSB bit 5 waiting for /REQ to drop
             * before fetching the next chunk (observed live: a
             * 192-sector READ(10) that stalled after exactly one 512-
             * byte chunk).  The chunk's byte-move loop gates on DRQ,
             * which stays asserted until the buffer fully drains, so
             * clearing /REQ here does not disturb the copy loop; it
             * only lets the inter-chunk /REQ poll make progress.
             */
            s->csb &= ~CSB_REQ;
        }
    } else {
        trace_ncr5380_stale_read(s->buf_pos, s->buf_len);
        v = s->last_data;
    }
    trace_ncr5380_pdma_rd(v);
    return v;
}

void ncr5380_pdma_write(NCR5380State *s, uint8_t val)
{
    trace_ncr5380_pdma_wr(val);
    if (s->phase == PHASE_CMD) {
        if (s->cmd_len < (int)sizeof(s->cmd)) {
            s->cmd[s->cmd_len++] = val;
        }
        if (s->cmd_len >= scsi_cdb_length(s->cmd) && s->cmd_len > 0) {
            ncr5380_do_command(s);
        }
        return;
    }
    if (s->phase != PHASE_DO) {
        return;
    }
    if (s->buf_pos < s->buf_len) {
        s->dbuf[s->buf_pos++] = val;
    }
    if (s->buf_pos >= s->buf_len) {
        /* whole transfer collected: run it synchronously */
        ncr5380_flush_data_out(s);
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

/*
 * SCSI-layer chunk handover.  Only ever called inside the synchronous
 * pump: for data-in, append the chunk to the whole-transfer buffer; for
 * data-out, feed the next chunk of the collected buffer to the device.
 * Then immediately re-continue so the request runs to completion.
 */
static void ncr5380_transfer_data(SCSIRequest *req, uint32_t len)
{
    NCR5380State *s = req->hba_private;
    uint8_t *chunk = scsi_req_get_buf(req);

    trace_ncr5380_transfer(len);
    if (!s->collecting) {
        /* shouldn't happen with the synchronous pump; drop the chunk */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ncr5380: unexpected transfer_data outside pump\n");
        return;
    }
    if (req->cmd.mode == SCSI_XFER_FROM_DEV) {
        ncr5380_grow_dbuf(s, s->buf_len + len);
        memcpy(s->dbuf + s->buf_len, chunk, len);
        s->buf_len += len;
    } else {
        if (s->out_pos + len > s->buf_len) {
            len = s->buf_len > s->out_pos ? s->buf_len - s->out_pos : 0;
        }
        memcpy(chunk, s->dbuf + s->out_pos, len);
        s->out_pos += len;
    }
    scsi_req_continue(req);
}

static void ncr5380_command_complete(SCSIRequest *req, size_t resid)
{
    NCR5380State *s = req->hba_private;

    trace_ncr5380_complete(req->status);
    s->status = req->status;
    s->cmd_done = true;
    if (s->collecting) {
        /* do_command / flush_data_out set up the next bus phase */
        return;
    }
    /* no-data command: completes during scsi_req_enqueue */
    s->buf_len = 0;
    s->buf_pos = 0;
    ncr5380_enter_status(s);
}

static void ncr5380_request_cancelled(SCSIRequest *req)
{
    NCR5380State *s = req->hba_private;

    if (req == s->req) {
        scsi_req_unref(s->req);
        s->req = NULL;
        s->dev = NULL;
        s->cmd_done = true;     /* unblock the pump */
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
    DEFINE_PROP_BOOL("sun-mode", NCR5380State, sun_mode, false),
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
