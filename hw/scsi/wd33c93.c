/*
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/scsi/wd33c93.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"

/*
 * Command latency model: after a command is written the chip needs a
 * short time before it reflects Command-In-Progress in its status
 * (assertion latency), and a little longer before the command runs.
 */
#define WD33C93_CMD_ASSERT_NS 2000
#define WD33C93_CMD_DELAY_NS  4000

#define DPRINTF(...) do { \
    if (0) { \
        printf(__VA_ARGS__); \
    } \
} while (0)

static void wd33c93_sat_finish(WD33C93State *s);

static void wd33c93_do_int(WD33C93State *s, uint8_t scsi_stat)
{
	DPRINTF("%s: 0x%02x\n", __func__, scsi_stat);
	s->scsistatus = scsi_stat;
	s->auxstat |= WD33C93_REG_AUXILIARYSTAT_INT;
	qemu_set_irq(s->irq, 1);
}

static uint8_t wd33c93_read_aux_stat(WD33C93State *s)
{
	uint8_t dbr = 0;

	if (s->cmd_in_progress) {
		int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
		                  s->cmd_write_ns;

		/*
		 * A real WD33C93 does not update its status the instant a
		 * command is written: for a short assertion latency it still
		 * looks idle, then it reports Command-In-Progress / Busy
		 * until the command actually runs.  A driver that samples the
		 * status inside the assertion window (i.e. without the
		 * inter-access delays the chip needs) sees a stale "idle"
		 * status and races ahead.
		 */
		if (elapsed < WD33C93_CMD_ASSERT_NS) {
			return s->auxstat;
		}
		return s->auxstat | WD33C93_REG_AUXILIARYSTAT_CIP |
		       WD33C93_REG_AUXILIARYSTAT_BUSY;
	}

	switch (s->state) {
	case WD33C93_TI_IN:
		/* a byte is ready when the data source has one */
		switch (s->bus_phase) {
		case WD33C93_PHASE_DATA_IN:
			dbr = s->scsi_pos < s->scsi_len && s->ti_remaining;
			break;
		default:
			dbr = s->ti_remaining ? 1 : 0;
			break;
		}
		break;
	case WD33C93_TI_OUT:
		/* always room for the next byte */
		dbr = s->ti_remaining ? 1 : 0;
		break;
	default:
		dbr = fifo8_is_full(&s->fifo) ? 0 : 1;
		break;
	}

	return s->auxstat | (dbr ? WD33C93_REG_AUXILIARYSTAT_DBR : 0);
}

static uint8_t wd33c93_read_scsi_status(WD33C93State *s)
{
	uint8_t val = s->scsistatus;

	s->auxstat &= ~WD33C93_REG_AUXILIARYSTAT_INT;
	qemu_set_irq(s->irq, 0);

	/*
	 * Once the completion interrupt has been consumed the target
	 * requests the next information transfer phase, or, at the end
	 * of a select-and-transfer sequence, drops off the bus: the
	 * disconnect is its own interrupt, and the A3000 driver only
	 * completes a command when it sees it.
	 */
	if (s->srv_req_pending) {
		s->srv_req_pending = false;
		wd33c93_do_int(s, WD33C93_SCSISTATUS_SRV_REQ | s->bus_phase);
	} else if (s->disconnect_pending) {
		s->disconnect_pending = false;
		wd33c93_do_int(s, WD33C93_SCSISTATUS_DISCONNECT);
	}

	return val;
}

static void wd33c93_scsi_req_done(WD33C93State *s)
{
	if (s->current_req) {
		scsi_req_unref(s->current_req);
		s->current_req = NULL;
	}
}

/*
 * The current transfer info command has moved all of its bytes (or the
 * data ran out): move the bus to the next phase and complete.
 */
static void wd33c93_ti_complete(WD33C93State *s)
{
	DPRINTF("%s: phase %d\n", __func__, s->bus_phase);

	s->state = WD33C93_IDLE;
	qemu_set_irq(s->drq, 0);

	switch (s->bus_phase) {
	case WD33C93_PHASE_MSG_OUT:
		/* identify received, command follows */
		s->bus_phase = WD33C93_PHASE_COMMAND;
		s->cmdphase = 0x20;
		break;
	case WD33C93_PHASE_COMMAND: {
		/* have the cdb, run the command */
		int lun = s->identify & 0x80 ? s->identify & 0x7 : s->targetlun & 0x7;
		int datalen;

		s->current_lun = scsi_device_find(&s->bus, 0,
		                                  s->current_dev->id, lun);
		s->req_done = false;
		s->scsi_len = 0;
		s->scsi_pos = 0;
		s->current_req = scsi_req_new(s->current_lun, 0, lun, s->cdb,
		                              s->cdb_pos, s);
		datalen = scsi_req_enqueue(s->current_req);
		s->cmdphase = 0x41;
		if (datalen > 0) {
			s->bus_phase = WD33C93_PHASE_DATA_IN;
			scsi_req_continue(s->current_req);
		} else if (datalen < 0) {
			s->bus_phase = WD33C93_PHASE_DATA_OUT;
			scsi_req_continue(s->current_req);
		} else {
			s->bus_phase = WD33C93_PHASE_STATUS;
		}
		break;
	}
	case WD33C93_PHASE_DATA_IN:
	case WD33C93_PHASE_DATA_OUT:
		if (s->sat_active) {
			if (s->req_done) {
				wd33c93_sat_finish(s);
			}
			return;
		}
		if (s->req_done) {
			s->bus_phase = WD33C93_PHASE_STATUS;
			s->cmdphase = 0x46;
		}
		break;
	case WD33C93_PHASE_STATUS:
		s->bus_phase = WD33C93_PHASE_MSG_IN;
		s->msg_in = 0; /* command complete */
		s->msg_in_read = false;
		s->cmdphase = 0x47;
		break;
	case WD33C93_PHASE_MSG_IN:
		/*
		 * The message byte has been passed to the host but ACK is
		 * still asserted until it approves it.
		 */
		s->msg_in_read = true;
		s->cmdphase = 0x50;
		wd33c93_do_int(s, WD33C93_SCSISTATUS_MSGIN_PAUSED);
		return;
	default:
		break;
	}

	wd33c93_do_int(s, WD33C93_SCSISTATUS_XFER_DONE | (s->bus_phase & 7));
}

static void wd33c93_data_out_byte(WD33C93State *s, uint8_t value)
{
	if (s->scsi_pos < s->scsi_len) {
		s->scsi_buf[s->scsi_pos++] = value;
		if (s->scsi_pos == s->scsi_len && s->current_req) {
			/* chunk full, push it to the device */
			s->scsi_len = 0;
			s->scsi_pos = 0;
			scsi_req_continue(s->current_req);
		}
	}
}

static uint8_t wd33c93_data_in_byte(WD33C93State *s)
{
	uint8_t value = 0;

	if (s->scsi_pos < s->scsi_len) {
		value = s->scsi_buf[s->scsi_pos++];
		if (s->scsi_pos == s->scsi_len && s->current_req) {
			/* chunk consumed, ask for the next one */
			s->scsi_len = 0;
			s->scsi_pos = 0;
			scsi_req_continue(s->current_req);
		}
	}

	return value;
}

static uint8_t wd33c93_handle_data_read(WD33C93State *s)
{
	uint8_t value;

	if (s->state != WD33C93_TI_IN || !s->ti_remaining) {
		return fifo8_is_empty(&s->fifo) ? 0 : fifo8_pop(&s->fifo);
	}

	switch (s->bus_phase) {
	case WD33C93_PHASE_DATA_IN:
		value = wd33c93_data_in_byte(s);
		break;
	case WD33C93_PHASE_STATUS:
		value = s->req_status;
		break;
	case WD33C93_PHASE_MSG_IN:
		value = s->msg_in;
		break;
	default:
		value = 0;
		break;
	}

	s->ti_remaining--;
	if (s->transfercount) {
		s->transfercount--;
	}
	if (!s->ti_remaining ||
	    (s->bus_phase == WD33C93_PHASE_DATA_IN && s->req_done &&
	     s->scsi_pos >= s->scsi_len)) {
		wd33c93_ti_complete(s);
	}

	return value;
}

static void wd33c93_handle_data_write(WD33C93State *s, uint8_t value)
{
	if (s->state != WD33C93_TI_OUT || !s->ti_remaining) {
		if (!fifo8_is_full(&s->fifo)) {
			fifo8_push(&s->fifo, value);
		}
		return;
	}

	switch (s->bus_phase) {
	case WD33C93_PHASE_MSG_OUT:
		if (value & 0x80) {
			s->identify = value;
		}
		break;
	case WD33C93_PHASE_COMMAND:
		if (s->cdb_pos < (int) sizeof(s->cdb)) {
			s->cdb[s->cdb_pos++] = value;
		}
		break;
	case WD33C93_PHASE_DATA_OUT:
		wd33c93_data_out_byte(s, value);
		break;
	default:
		break;
	}

	s->ti_remaining--;
	if (s->transfercount) {
		s->transfercount--;
	}
	if (!s->ti_remaining) {
		wd33c93_ti_complete(s);
	}
}

uint8_t wd33c93_io_read(WD33C93State *s, unsigned bus_addr)
{
    switch(bus_addr){
    case WD33C93_REG_BUS_ADDR:
    	return wd33c93_read_aux_stat(s);
    case WD33C93_REG_BUS_DATA: {
    	uint8_t reg = s->reg_addr;

    	DPRINTF("%s - 0x%02x\n", __func__, (unsigned) reg);
    	/* the register address auto-increments, except at the data reg */
    	if (s->reg_addr < WD33C93_REG_DATA) {
    		s->reg_addr++;
    	}
    	switch(reg) {
    	case WD33C93_REG_OWNID:
    		return s->ownid;
    	case WD33C93_REG_CONTROL:
    		return s->control;
    	case WD33C93_REG_TIMEOUTPERIOD:
    		return s->timeoutperiod;
    	case WD33C93_REG_TARGETLUN:
    		return s->targetlun;
    	case WD33C93_REG_COMMANDPHASE:
    		return s->cmdphase;
    	case WD33C93_REG_TRANSFERCOUNT_MSB:
    		return (s->transfercount >> 16) & 0xff;
    	case WD33C93_REG_TRANSFERCOUNT_2ND:
    		return (s->transfercount >> 8) & 0xff;
    	case WD33C93_REG_TRANSFERCOUNT_LSB:
    		return s->transfercount & 0xff;
    	case WD33C93_REG_DESTINATIONID:
    		return s->destinationid;
    	case WD33C93_REG_SOURCEID:
    		return s->sourceid;
    	case WD33C93_REG_SCSISTATUS:
    		return wd33c93_read_scsi_status(s);
    	case WD33C93_REG_DATA:
    		return wd33c93_handle_data_read(s);
    	case WD33C93_REG_AUXILIARYSTAT:
    		return wd33c93_read_aux_stat(s);
    	default:
    		break;
    	}
    	break;
    }
    }

    return 0;
}

static uint64_t wd33c93_read(void *opaque, hwaddr addr, unsigned size)
{
	return wd33c93_io_read(WD33C93(opaque), addr);
}

static void wd33c93_cmd_reset(WD33C93State *s)
{
	DPRINTF("%s\n", __func__);

	bus_cold_reset(BUS(&s->bus));

	wd33c93_scsi_req_done(s);
	s->state = WD33C93_IDLE;
	s->bus_phase = WD33C93_PHASE_BUS_FREE;
	s->srv_req_pending = false;
	s->ti_remaining = 0;
	s->cmdphase = 0;
	fifo8_reset(&s->fifo);

	/*
	 * The reset command completes with an interrupt; the status
	 * reports whether advanced features were enabled via the own id
	 * register at reset time.
	 */
	wd33c93_do_int(s, (s->ownid & WD33C93_REG_OWNID_EAF) ?
	               WD33C93_SCSISTATUS_RESET_EAF : WD33C93_SCSISTATUS_RESET);
}

static void wd33c93_cmd_select(WD33C93State *s, bool atn)
{
	int target = s->destinationid & 0x7;

	DPRINTF("%s\n", __func__);

	s->current_dev = scsi_device_find(&s->bus, 0, target, 0);
	if (!s->current_dev) {
		DPRINTF("failed to select %d\n", target);
		/* selection timeout */
		s->bus_phase = WD33C93_PHASE_BUS_FREE;
		wd33c93_do_int(s, 0x42);
		return;
	}

	DPRINTF("selected %d\n", target);
	s->atn = atn;
	s->identify = 0;
	s->cdb_pos = 0;
	s->bus_phase = atn ? WD33C93_PHASE_MSG_OUT : WD33C93_PHASE_COMMAND;
	s->cmdphase = 0x10;
	s->srv_req_pending = true;
	wd33c93_do_int(s, WD33C93_SCSISTATUS_SELECT_COMPLETE);
}

static void wd33c93_cmd_negate_ack(WD33C93State *s)
{
	DPRINTF("%s\n", __func__);

	/*
	 * Releasing ACK after the command complete message lets the
	 * target disconnect.
	 */
	if (s->bus_phase == WD33C93_PHASE_MSG_IN && s->msg_in_read) {
		wd33c93_scsi_req_done(s);
		s->bus_phase = WD33C93_PHASE_BUS_FREE;
		s->cmdphase = 0;
		wd33c93_do_int(s, WD33C93_SCSISTATUS_DISCONNECT);
	}
}

/* cdb length by scsi command group code */
static int wd33c93_cdb_len(uint8_t opcode)
{
	switch (opcode >> 5) {
	case 0:
		return 6;
	case 1:
	case 2:
		return 10;
	case 5:
		return 12;
	default:
		return 6;
	}
}

static void wd33c93_sat_finish(WD33C93State *s)
{
	wd33c93_scsi_req_done(s);
	s->sat_active = false;
	s->state = WD33C93_IDLE;
	s->bus_phase = WD33C93_PHASE_BUS_FREE;
	/* the target's status byte lands in the lun register */
	s->targetlun = s->req_status;
	s->cmdphase = 0x60;
	s->disconnect_pending = true;
	wd33c93_do_int(s, WD33C93_SCSISTATUS_SEL_XFER_DONE);
}

static int wd33c93_get_dma_mode(WD33C93State *s)
{
	return (s->control >> WD33C93_REG_CONTROL_DM_SHIFT) & WD33C93_REG_CONTROL_DM_MASK;
}

static void wd33c93_cmd_transfer_info(WD33C93State *s, bool single)
{
	unsigned int transfercount = single ? 1 : s->transfercount;

	DPRINTF("%s - txcnt:%d phase:%d\n", __func__, transfercount, s->bus_phase);

	if (!transfercount) {
		transfercount = 1;
	}

	s->ti_remaining = transfercount;
	if (single) {
		s->transfercount = 0;
	}

	switch (s->bus_phase) {
	case WD33C93_PHASE_MSG_OUT:
	case WD33C93_PHASE_COMMAND:
	case WD33C93_PHASE_DATA_OUT:
		s->state = WD33C93_TI_OUT;
		break;
	case WD33C93_PHASE_DATA_IN:
	case WD33C93_PHASE_STATUS:
	case WD33C93_PHASE_MSG_IN:
		s->state = WD33C93_TI_IN;
		break;
	default:
		qemu_log_mask(LOG_GUEST_ERROR,
		              "wd33c93: transfer info with bus free\n");
		s->ti_remaining = 0;
		break;
	}

	if (s->ti_remaining &&
	    wd33c93_get_dma_mode(s) != WD33C93_REG_CONTROL_DM_POLLED) {
		/* ask the board dma engine to move the bytes */
		qemu_set_irq(s->drq, 1);
	}
}

int wd33c93_dma_dir(WD33C93State *s)
{
	if (s->state == WD33C93_TI_IN && s->ti_remaining) {
		return 1;
	}
	if (s->state == WD33C93_TI_OUT && s->ti_remaining) {
		return -1;
	}
	return 0;
}

size_t wd33c93_dma_pull(WD33C93State *s, uint8_t *buf, size_t len)
{
	size_t count = 0;

	if (s->state != WD33C93_TI_IN) {
		return 0;
	}

	while (count < len && s->ti_remaining) {
		if (s->bus_phase == WD33C93_PHASE_DATA_IN &&
		    s->scsi_pos >= s->scsi_len) {
			/* waiting for the next chunk from the device */
			break;
		}
		buf[count++] = wd33c93_handle_data_read(s);
		if (s->state != WD33C93_TI_IN) {
			/* transfer completed */
			break;
		}
	}

	return count;
}

size_t wd33c93_dma_push(WD33C93State *s, const uint8_t *buf, size_t len)
{
	size_t count = 0;

	if (s->state != WD33C93_TI_OUT) {
		return 0;
	}

	while (count < len && s->ti_remaining) {
		if (s->bus_phase == WD33C93_PHASE_DATA_OUT &&
		    s->scsi_pos >= s->scsi_len) {
			/* no room, waiting for the device to take the chunk */
			break;
		}
		wd33c93_handle_data_write(s, buf[count++]);
		if (s->state != WD33C93_TI_OUT) {
			break;
		}
	}

	return count;
}

/*
 * Select-and-transfer: select the target and run the entire command
 * from the cdb registers, including the status and command complete
 * message at the end.  Completes with a single interrupt.
 */
static void wd33c93_cmd_sel_atn_tfr(WD33C93State *s)
{
	int target = s->destinationid & 0x7;
	int lun = s->targetlun & 0x7;
	int datalen;

	DPRINTF("%s\n", __func__);

	s->current_dev = scsi_device_find(&s->bus, 0, target, 0);
	if (!s->current_dev) {
		s->bus_phase = WD33C93_PHASE_BUS_FREE;
		wd33c93_do_int(s, 0x42);
		return;
	}

	s->sat_active = true;
	s->identify = 0x80 | lun;
	s->cdb_pos = wd33c93_cdb_len(s->cdb_regs[0]);
	memcpy(s->cdb, s->cdb_regs, s->cdb_pos);
	DPRINTF("%s: cdb %02x %02x %02x %02x %02x %02x tc=%u\n", __func__,
	        s->cdb[0], s->cdb[1], s->cdb[2], s->cdb[3], s->cdb[4],
	        s->cdb[5], s->transfercount);

	s->current_lun = scsi_device_find(&s->bus, 0, target, lun);
	s->req_done = false;
	s->scsi_len = 0;
	s->scsi_pos = 0;
	s->current_req = scsi_req_new(s->current_lun, 0, lun, s->cdb,
	                              s->cdb_pos, s);
	datalen = scsi_req_enqueue(s->current_req);
	s->cmdphase = 0x41;
	if (datalen != 0) {
		s->bus_phase = datalen > 0 ? WD33C93_PHASE_DATA_IN :
		                             WD33C93_PHASE_DATA_OUT;
		s->state = datalen > 0 ? WD33C93_TI_IN : WD33C93_TI_OUT;
		s->ti_remaining = s->transfercount ? s->transfercount :
		                  (datalen > 0 ? datalen : -datalen);
		scsi_req_continue(s->current_req);
		if (wd33c93_get_dma_mode(s) != WD33C93_REG_CONTROL_DM_POLLED) {
			qemu_set_irq(s->drq, 1);
		}
	} else {
		/*
		 * No data phase.  The completion callback may already have
		 * fired during enqueue (and finished the transaction), so
		 * only complete here if it is still pending.
		 */
		if (s->sat_active && s->req_done) {
			wd33c93_sat_finish(s);
		}
	}
}

/*
 * Nanoseconds the chip takes to accept and start a written command.
 * A driver that polls the auxiliary status faster than this (i.e.
 * without the inter-access delays real hardware needs) will observe
 * Command-In-Progress and must wait; one that doesn't will race.
 */

static void wd33c93_do_cmd(WD33C93State *s, uint8_t cmd);

static void wd33c93_cmd_timer(void *opaque)
{
	WD33C93State *s = opaque;

	s->cmd_in_progress = false;
	wd33c93_do_cmd(s, s->pending_cmd);
}

static void wd33c93_do_cmd(WD33C93State *s, uint8_t cmd)
{
	bool single = cmd & WD33C93_CMD_SBT;

	DPRINTF("%s - 0x%02x\n", __func__, (unsigned) cmd);

	switch(cmd & ~WD33C93_CMD_SBT) {
	case WD33C93_CMD_RESET:
		wd33c93_cmd_reset(s);
		break;
	case WD33C93_CMD_ABORT:
		wd33c93_scsi_req_done(s);
		s->state = WD33C93_IDLE;
		s->bus_phase = WD33C93_PHASE_BUS_FREE;
		wd33c93_do_int(s, WD33C93_SCSISTATUS_DISCONNECT);
		break;
	case WD33C93_CMD_ASSERT_ATN:
		s->atn = true;
		break;
	case WD33C93_CMD_NEGATE_ACK:
		wd33c93_cmd_negate_ack(s);
		break;
	case WD33C93_CMD_DISCONNECT:
		wd33c93_scsi_req_done(s);
		s->state = WD33C93_IDLE;
		s->bus_phase = WD33C93_PHASE_BUS_FREE;
		break;
	case WD33C93_CMD_SELECTWITHATN:
		wd33c93_cmd_select(s, true);
		break;
	case WD33C93_CMD_SELECTWITHOUTATN:
		wd33c93_cmd_select(s, false);
		break;
	case WD33C93_CMD_SELECTWITHATNTFR:
	case WD33C93_CMD_SELECTWOATNTFR:
		wd33c93_cmd_sel_atn_tfr(s);
		break;
	case WD33C93_CMD_TRANSFERINFO:
		wd33c93_cmd_transfer_info(s, single);
		break;
	default:
		qemu_log_mask(LOG_UNIMP, "wd33c93: unimplemented command 0x%02x\n",
		              cmd);
		break;
	}
}

void wd33c93_io_write(WD33C93State *s, unsigned bus_addr, uint8_t value)
{
    switch(bus_addr){
    case WD33C93_REG_BUS_ADDR:
    	s->reg_addr = value;
        break;
    case WD33C93_REG_BUS_DATA: {
    	uint8_t reg = s->reg_addr;

    	DPRINTF("%s - 0x%02x\n", __func__, (unsigned) reg);
    	/* the register address auto-increments, except at the data reg */
    	if (s->reg_addr < WD33C93_REG_DATA) {
    		s->reg_addr++;
    	}
    	if (reg >= WD33C93_REG_TOTALSECTORS &&
    	    reg <= WD33C93_REG_CYLINDERNUMBER_LSB) {
    		/* the translation registers double as the cdb */
    		s->cdb_regs[reg - WD33C93_REG_TOTALSECTORS] = value;
    	}
    	switch(reg) {
    	case WD33C93_REG_OWNID:
    		s->ownid = value;
    		break;
    	case WD33C93_REG_CONTROL:
    		s->control = value;
    		break;
    	case WD33C93_REG_TIMEOUTPERIOD:
    		s->timeoutperiod = value;
    		break;
    	case WD33C93_REG_TOTALSECTORS:
    		s->totalsectors = value;
    		break;
    	case WD33C93_REG_CYLINDERNUMBER_MSB:
    	case WD33C93_REG_CYLINDERNUMBER_LSB:
    		break;
    	case WD33C93_REG_TOTALHEADS:
    		s->totalheads = value;
    		break;
    	case WD33C93_REG_TOTALCYLINDERS_MSB:
    		s->totalcylinders &= 0x00ff;
    		s->totalcylinders |= ((value & 0xff) << 8);
    		break;
    	case WD33C93_REG_TOTALCYLINDERS_LSB:
    		s->totalcylinders &= 0xff00;
    		s->totalcylinders |= (value & 0xff);
    		break;
    	case WD33C93_REG_LOGICALADDRESS_MSB:
    		s->logicaladdress &= 0x00ffffff;
    		s->logicaladdress |= ((value & 0xff) << 24);
    		break;
    	case WD33C93_REG_LOGICALADDRESS_2ND:
    		s->logicaladdress &= 0xff00ffff;
    		s->logicaladdress |= ((value & 0xff) << 16);
    		break;
    	case WD33C93_REG_LOGICALADDRESS_3RD:
    		s->logicaladdress &= 0xffff00ff;
    		s->logicaladdress |= ((value & 0xff) << 8);
    		break;
    	case WD33C93_REG_LOGICALADDRESS_LSB:
    		s->logicaladdress &= 0xffffff00;
    		s->logicaladdress |= (value & 0xff);
    		break;
    	case WD33C93_REG_SECTORNUMBER:
    		s->sectornumber = value;
    		break;
    	case WD33C93_REG_HEADNUMBER:
    		s->headnumber = value;
    		break;
    	case WD33C93_REG_TARGETLUN:
    		s->targetlun = value;
    		break;
    	case WD33C93_REG_COMMANDPHASE:
    		s->cmdphase = value;
    		break;
    	case WD33C93_REG_SYCHRONOUSTRANSFER:
    		break;
    	case WD33C93_REG_TRANSFERCOUNT_MSB:
    		s->transfercount &= 0x00ffff;
    		s->transfercount |= ((value & 0xff) << 16);
    		break;
    	case WD33C93_REG_TRANSFERCOUNT_2ND:
    		s->transfercount &= 0xff00ff;
    		s->transfercount |= ((value & 0xff) << 8);
    		break;
    	case WD33C93_REG_TRANSFERCOUNT_LSB:
    		s->transfercount &= 0xffff00;
    		s->transfercount |= (value & 0xff);
    		break;
    	case WD33C93_REG_DESTINATIONID:
    		s->destinationid = value;
    		break;
    	case WD33C93_REG_SOURCEID:
    		s->sourceid = value;
    		break;
    	case WD33C93_REG_SCSISTATUS:
    		break;
    	case WD33C93_REG_COMMAND:
    		/*
    		 * Model the real chip's command latency: report
    		 * Command-In-Progress until the timer fires and the
    		 * command actually runs.
    		 */
    		s->pending_cmd = value;
    		s->cmd_in_progress = true;
    		s->cmd_write_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    		timer_mod(s->cmd_timer,
    		          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
    		          WD33C93_CMD_DELAY_NS);
    		break;
    	case WD33C93_REG_DATA:
    		wd33c93_handle_data_write(s, value);
    		break;
    	case WD33C93_REG_AUXILIARYSTAT:
    		break;
    	}
        break;
    }
    }
}

static void wd33c93_write(void *opaque, hwaddr addr, uint64_t value,
        unsigned size)
{
	wd33c93_io_write(WD33C93(opaque), addr, value);
}

static const MemoryRegionOps wd33c93_ops = {
    .read = wd33c93_read,
    .write = wd33c93_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void wd33c93_reset(DeviceState *dev)
{
	WD33C93State *s = WD33C93(dev);

	s->state = WD33C93_IDLE;
	s->bus_phase = WD33C93_PHASE_BUS_FREE;
	s->srv_req_pending = false;
	s->ti_remaining = 0;
	s->auxstat = 0;
	s->scsistatus = 0;
	s->cmdphase = 0;
	s->cmd_in_progress = false;
	qemu_set_irq(s->irq, 0);
	if (s->cmd_timer) {
		timer_del(s->cmd_timer);
	}
}

static void wd33c93_transfer_data(SCSIRequest *req, uint32_t len)
{
	WD33C93State *s = req->hba_private;

	DPRINTF("%s - %d\n", __func__, (unsigned) len);

	s->scsi_buf = scsi_req_get_buf(req);
	s->scsi_len = len;
	s->scsi_pos = 0;

	if (s->ti_remaining &&
	    wd33c93_get_dma_mode(s) != WD33C93_REG_CONTROL_DM_POLLED) {
		qemu_set_irq(s->drq, 1);
	}
}

static void wd33c93_command_complete(SCSIRequest *req, size_t resid)
{
	WD33C93State *s = req->hba_private;

	DPRINTF("%s - status %d\n", __func__, req->status);

	s->req_status = req->status;
	s->req_done = true;
	s->scsi_len = 0;
	s->scsi_pos = 0;

	/*
	 * If the host is still waiting inside a data-phase transfer
	 * (e.g. it asked for more than the device returned) the bus
	 * moves to the status phase now.
	 */
	if (s->sat_active && s->state == WD33C93_IDLE) {
		wd33c93_sat_finish(s);
	} else if (s->state != WD33C93_IDLE &&
	    (s->bus_phase == WD33C93_PHASE_DATA_IN ||
	     s->bus_phase == WD33C93_PHASE_DATA_OUT)) {
		if (!s->ti_remaining) {
			wd33c93_ti_complete(s);
		}
	} else if (s->state == WD33C93_IDLE &&
	           (s->bus_phase == WD33C93_PHASE_DATA_IN ||
	            s->bus_phase == WD33C93_PHASE_DATA_OUT)) {
		s->bus_phase = WD33C93_PHASE_STATUS;
	}
}

static void wd33c93_request_cancelled(SCSIRequest *req)
{
	WD33C93State *s = req->hba_private;

	if (s->current_req == req) {
		scsi_req_unref(s->current_req);
		s->current_req = NULL;
	}
}

static const struct SCSIBusInfo wd33c93_scsi_info = {
    .tcq = false,
    .max_target = 7,
    .max_lun = 7,

    .transfer_data = wd33c93_transfer_data,
    .complete = wd33c93_command_complete,
    .cancel = wd33c93_request_cancelled,
};

static void wd33c93_realize(DeviceState *dev, Error **errp)
{
    WD33C93State *s = WD33C93(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &wd33c93_ops, s, TYPE_WD33C93, 0x20);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    qdev_init_gpio_out_named(dev, &s->drq, "drq", 1);
    qdev_init_gpio_out_named(dev, &s->irq, "irq", 1);

    scsi_bus_init(&s->bus, sizeof(s->bus), dev, &wd33c93_scsi_info);
}

static const VMStateDescription vmstate_wd33c93 = {
    .name = "wd33c93",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
            VMSTATE_END_OF_LIST()
        }
};

static void wd33c93_init(Object *obj)
{
	WD33C93State *s = WD33C93(obj);

	fifo8_create(&s->fifo, WD33C93_REG_FIFO_SZ);
	s->cmd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wd33c93_cmd_timer, s);
}

static void wd33c93_finalize(Object *obj)
{
	WD33C93State *s = WD33C93(obj);

	fifo8_destroy(&s->fifo);
}

static void wd33c93_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, wd33c93_reset);
    dc->realize = wd33c93_realize;
    dc->vmsd = &vmstate_wd33c93;

    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo wd33c93_info = {
    .name              = TYPE_WD33C93,
    .parent            = TYPE_SYS_BUS_DEVICE,
    .instance_size     = sizeof(WD33C93State),
    .instance_init     = wd33c93_init,
    .instance_finalize = wd33c93_finalize,
    .class_init        = wd33c93_class_init,
};

static void wd33c93_register_types(void)
{
    type_register_static(&wd33c93_info);
}

type_init(wd33c93_register_types)
