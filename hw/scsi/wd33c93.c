/*
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/scsi/wd33c93.h"
#include "migration/vmstate.h"

static void wd33c93_do_int(WD33C93State *s, uint8_t scsi_stat)
{
	s->scsistatus = scsi_stat;
	s->auxstat |= WD33C93_REG_AUXILIARYSTAT_INT;
	// Raise hw irq here?
}

static void wd33c93_change_phase(WD33C93State *s, enum WD33C93_PHASE new_phase)
{
	s->phase = new_phase;
}

static void wd33c93_change_state(WD33C93State *s, enum WD33C93_STATE new_state)
{
	switch (new_state) {
	case WD33C93_TI_COMPLETE_DATA_IN:
		wd33c93_change_phase(s, WD33C93_PHASE_DATA_IN);
		wd33c93_do_int(s, (WD33C93_SCSISTATUS_COMPLETION |
				           WD33C93_SCSISTATUS_COMPLETION_MCI |
						   WD33C93_SCSISTATUS_MCI_DATA_IN));
		break;
	case WD33C93_TI_COMPLETE_DATA_OUT:
		wd33c93_change_phase(s, WD33C93_PHASE_DATA_OUT);
		wd33c93_do_int(s, (WD33C93_SCSISTATUS_COMPLETION |
				           WD33C93_SCSISTATUS_COMPLETION_MCI |
						   WD33C93_SCSISTATUS_MCI_DATA_OUT));
		break;
	default:
		break;
	}

	s->state = new_state;
}

static uint8_t wd33c93_read_aux_stat(WD33C93State *s)
{
    printf("%s\n", __func__);
    return s->auxstat | (fifo8_is_full(&s->fifo) ? 0 : WD33C93_REG_AUXILIARYSTAT_DBR);
}

static uint8_t wd33c93_read_scsi_status(WD33C93State *s)
{
	s->auxstat &= ~WD33C93_REG_AUXILIARYSTAT_INT;
	return s->scsistatus;
}

static uint8_t wd33c93_handle_data_read(WD33C93State *s)
{
	uint8_t value;

	switch(s->state) {
	/* We are waiting for the host to write all of the data it wants to send */
	case WD33C93_POLLED_TI_WAITINGFORDATA_IN:
		printf("%s %d/%d\n", __func__,
				(unsigned) s->polled_transfer_pos, (unsigned) s->polled_transfer_size);
		value = s->polled_transfer_buffer[s->polled_transfer_pos++];
		if (s->polled_transfer_pos == s->polled_transfer_size) {
			printf("%s\n", __func__);
			printf("all data in\n");
		}
		break;
	default:
		printf("%s\n", __func__);
		value = fifo8_pop(&s->fifo);
		break;
	}

	return value;
}

static uint64_t wd33c93_read(void *opaque, hwaddr addr, unsigned size)
{
	WD33C93State *s = WD33C93(opaque);

    switch(addr){
    case WD33C93_REG_BUS_ADDR:
    	return wd33c93_read_aux_stat(s);
    case WD33C93_REG_BUS_DATA:
	    printf("%s - 0x%02x\n", __func__, (unsigned) s->reg_addr);
    	switch(s->reg_addr) {
    	case WD33C93_REG_OWNID:
    		break;
    	case WD33C93_REG_CONTROL:
    		return s->control;
    	case WD33C93_REG_TIMEOUTPERIOD:
    		break;
    	case WD33C93_REG_TOTALSECTORS:
    		break;
    	case WD33C93_REG_TOTALHEADS:
    		break;
    	case WD33C93_REG_TOTALCYLINDERS_MSB:
    		break;
    	case WD33C93_REG_TOTALCYLINDERS_LSB:
    		break;
    	case WD33C93_REG_LOGICALADDRESS_MSB:
    		break;
    	case WD33C93_REG_LOGICALADDRESS_2ND:
    		break;
    	case WD33C93_REG_LOGICALADDRESS_3RD:
    		break;
    	case WD33C93_REG_LOGICALADDRESS_LSB:
    		break;
    	case WD33C93_REG_SECTORNUMBER:
    		break;
    	case WD33C93_REG_HEADNUMBER:
    		break;
    	case WD33C93_REG_CYLINDERNUMBER_MSB:
    		break;
    	case WD33C93_REG_CYLINDERNUMBER_LSB:
    		break;
    	case WD33C93_REG_TARGETLUN:
    		break;
    	case WD33C93_REG_COMMANDPHASE:
    		break;
    	case WD33C93_REG_SYCHRONOUSTRANSFER:
    		break;
    	case WD33C93_REG_TRANSFERCOUNT_MSB:
    		break;
    	case WD33C93_REG_TRANSFERCOUNT_2ND:
    		break;
    	case WD33C93_REG_TRANSFERCOUNT_LSB:
    		break;
    	case WD33C93_REG_DESTINATIONID:
    		break;
    	case WD33C93_REG_SOURCEID:
    		break;
    	case WD33C93_REG_SCSISTATUS:
    		return wd33c93_read_scsi_status(s);
    	case WD33C93_REG_COMMAND:
    		break;
    	case WD33C93_REG_DATA:
    		return wd33c93_handle_data_read(s);
    	case WD33C93_REG_AUXILIARYSTAT:
    		return wd33c93_read_aux_stat(s);
    	}
    	break;
    }

    return 0;
}

static void wd33c93_cmd_reset(WD33C93State *s)
{
	printf("%s\n", __func__);

	bus_cold_reset(BUS(&s->bus));
}

static void wd33c93_cmd_assert_atn(WD33C93State *s)
{
	printf("%s\n", __func__);
}

static void wd33c93_cmd_select(WD33C93State *s)
{
	int target = s->destinationid;
	uint8_t scsi_stat = 0;

	printf("%s\n", __func__);

	s->current_dev = scsi_device_find(&s->bus, 0, target, 0);
	if (!s->current_dev) {
		printf("failed to select %d\n", target);
		return;
	}
	else {
		printf("selected %d\n", target);
		scsi_stat = WD33C93_SCSISTATUS_COMPLETION | WD33C93_SCSISTATUS_COMPLETION_SELECTED;
	}

	wd33c93_do_int(s, scsi_stat);
}

static int wd33c93_get_dma_mode(WD33C93State *s)
{
	return (s->control >> WD33C93_REG_CONTROL_DM_SHIFT) & WD33C93_REG_CONTROL_DM_MASK;
}

static void wd33c93_start_req(WD33C93State *s, int lun, uint8_t *buf, size_t len)
{
	int datalen;

	printf("%s\n", __func__);

	s->current_req = scsi_req_new(s->current_lun, 0, lun, buf, len, s);
    datalen = scsi_req_enqueue(s->current_req);

    if (datalen != 0) {
        if (datalen > 0) {
        	printf("%s\n", __func__);
        	wd33c93_change_state(s, WD33C93_TI_COMPLETE_DATA_IN);
        } else {
        	printf("%s\n", __func__);
        	wd33c93_change_state(s, WD33C93_TI_COMPLETE_DATA_OUT);
        }
        scsi_req_continue(s->current_req);
        return;
    }
}

static void wd33c93_cmd_transfer_info(WD33C93State *s)
{
	int lun = s->targetlun;
	unsigned int transfercount = s->transfercount;

	printf("%s - txcnt:%d\n", __func__, transfercount);

    s->current_lun = scsi_device_find(&s->bus, 0, s->current_dev->id, lun);
    if (!s->current_lun) {
    	printf("%s\n", __func__);
        return;
    }

    switch(wd33c93_get_dma_mode(s)) {
    case WD33C93_REG_CONTROL_DM_POLLED:
    	s->polled_transfer_lun = lun;
    	s->polled_transfer_pos = 0;
    	s->polled_transfer_size = transfercount;
    	switch(s->phase) {
    	case WD33C93_PHASE_DATA_OUT:
        	printf("Starting polled transfer out, host should push into fifo\n");
        	s->polled_transfer_buffer = g_malloc(transfercount);
        	wd33c93_change_state(s, WD33C93_POLLED_TI_WAITINGFORDATA_OUT);
    		break;
    	case WD33C93_PHASE_DATA_IN:
        	printf("Starting polled transfer in, host should pop from fifo\n");
        	wd33c93_change_state(s, WD33C93_POLLED_TI_WAITINGFORDATA_IN);
    		break;
    	}
    	break;
    default:
    	printf("unimplemented/incorrect dma mode\n");
    }
}

static void wd33c93_do_cmd(WD33C93State *s, uint8_t cmd)
{
	printf("%s - 0x%02x\n", __func__, (unsigned) cmd);

	switch(cmd) {
	case WD33C93_CMD_RESET:
		wd33c93_cmd_reset(s);
		break;
	case WD33C93_CMD_ABORT:
		break;
	case WD33C93_CMD_ASSERT_ATN:
		wd33c93_cmd_assert_atn(s);
		break;
	case WD33C93_CMD_NEGATE_ACK:
		break;
	case WD33C93_CMD_DISCONNECT:
		break;
	case WD33C93_CMD_RESELECT:
		break;
	case WD33C93_CMD_SELECTWITHATN:
		wd33c93_cmd_select(s);
		break;
	case WD33C93_CMD_SELECTWITHOUTATN:
		wd33c93_cmd_select(s);
		break;
	case WD33C93_CMD_SELECTWITHATNTFR:
		wd33c93_cmd_select(s);
		break;
	case WD33C93_CMD_SELECTWOATNTFR:
		wd33c93_cmd_select(s);
		break;
	case WD33C93_CMD_RESELECTRX:
		break;
	case WD33C93_CMD_RESELECTTX:
		break;
	case WD33C93_CMD_WAITFORSELECTRX:
		break;
	case WD33C93_CMD_SENDSTATUSCMDCMPLT:
		break;
	case WD33C93_CMD_SENDDISCONMSG:
		break;
	case WD33C93_CMD_SENDIDI:
		break;
	case WD33C93_CMD_RECEIVECMD:
		break;
	case WD33C93_CMD_RECEIVEDATA:
		break;
	case WD33C93_CMD_RECEIVEMSGOUT:
		break;
	case WD33C93_CMD_RECEIVEUNSPECOUT:
		break;
	case WD33C93_CMD_SENDSTATUS:
		break;
	case WD33C93_CMD_SENDIDATA:
		break;
	case WD33C93_CMD_SENDMESSAGEIN:
		break;
	case WD33C93_CMD_SENDUNSPECIN:
		break;
	case WD33C93_CMD_TRANSLATEADDRESS:
		break;
	case WD33C93_CMD_TRANSFERINFO:
		wd33c93_cmd_transfer_info(s);
		break;
	}
}

static void wd33c93_handle_data_write(WD33C93State *s, uint8_t value)
{
	switch(s->state) {
	/* We are waiting for the host to write all of the data it wants to send */
	case WD33C93_POLLED_TI_WAITINGFORDATA_OUT:
		printf("%s %d/%d\n", __func__,
				(unsigned) s->polled_transfer_pos, (unsigned) s->polled_transfer_size);
		s->polled_transfer_buffer[s->polled_transfer_pos++] = value;
		if (s->polled_transfer_pos == s->polled_transfer_size) {
			printf("%s\n", __func__);
			wd33c93_start_req(s, s->polled_transfer_lun, s->polled_transfer_buffer, s->polled_transfer_size);
			wd33c93_change_state(s, WD33C93_POLLED_TI_EXECUTING);
		}
		break;
	default:
		printf("%s\n", __func__);
		fifo8_push(&s->fifo, value);
		break;
	}
}

static void wd33c93_write(void *opaque, hwaddr addr, uint64_t value,
        unsigned size)
{
	WD33C93State *s = WD33C93(opaque);


    switch(addr){
    case WD33C93_REG_BUS_ADDR:
    	s->reg_addr = value;
        break;
    case WD33C93_REG_BUS_DATA:
        printf("%s - 0x%02x\n", __func__, (unsigned) s->reg_addr);
    	switch(s->reg_addr) {
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
    	case WD33C93_REG_CYLINDERNUMBER_MSB:
    		break;
    	case WD33C93_REG_CYLINDERNUMBER_LSB:
    		break;
    	case WD33C93_REG_TARGETLUN:
    		s->targetlun = value;
    		break;
    	case WD33C93_REG_COMMANDPHASE:
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
    		wd33c93_do_cmd(s, value);
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

static const MemoryRegionOps wd33c93_ops = {
    .read = wd33c93_read,
    .write = wd33c93_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void wd33c93_reset(DeviceState *dev)
{
	WD33C93State *s = WD33C93(dev);

	s->state = WD33C93_IDLE;
	s->phase = WD33C93_PHASE_DATA_OUT;
}

static void wd33c93_transfer_data(SCSIRequest *req, uint32_t len)
{
	WD33C93State *s = req->hba_private;
	printf("%s - %d\n", __func__, (unsigned) len);

    s->polled_transfer_size = len;
    s->polled_transfer_buffer = scsi_req_get_buf(req);
}

static const struct SCSIBusInfo wd33c93_scsi_info = {
    .tcq = false,
    .max_target = 7,
    .max_lun = 7,

    //.load_request = esp_load_request,
    .transfer_data = wd33c93_transfer_data,
    //.complete = esp_command_complete,
   // .cancel = esp_request_cancelled
};

static void wd33c93_realize(DeviceState *dev, Error **errp)
{
    WD33C93State *s = WD33C93(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &wd33c93_ops, s, TYPE_WD33C93, 0x20);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

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

static Property wd33c93_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void wd33c93_init(Object *obj)
{
	WD33C93State *s = WD33C93(obj);

	fifo8_create(&s->fifo, WD33C93_REG_FIFO_SZ);
}

static void wd33c93_finalize(Object *obj)
{
	WD33C93State *s = WD33C93(obj);

	fifo8_destroy(&s->fifo);
}

static void wd33c93_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->legacy_reset = wd33c93_reset;
    device_class_set_props(dc, wd33c93_properties);
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
