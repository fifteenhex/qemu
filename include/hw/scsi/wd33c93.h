/*
 *
 */

#ifndef HW_WD33C93_H
#define HW_WD33C93_H

#include "hw/scsi/scsi.h"
#include "hw/sysbus.h"
#include "qemu/fifo8.h"
#include "qom/object.h"

#define TYPE_WD33C93 "wd33c93"

typedef struct WD33C93State WD33C93State;
OBJECT_DECLARE_SIMPLE_TYPE(WD33C93State, WD33C93)

enum WD33C93_STATE {
	WD33C93_IDLE,
	WD33C93_POLLED_TI_WAITINGFORDATA_OUT,
	WD33C93_POLLED_TI_WAITINGFORDATA_IN,
	WD33C93_POLLED_TI_EXECUTING,
	WD33C93_TI_COMPLETE_DATA_IN,
	WD33C93_TI_COMPLETE_DATA_OUT,
};

enum WD33C93_PHASE {
	WD33C93_PHASE_DATA_OUT,
	WD33C93_PHASE_DATA_IN,
};

struct WD33C93State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    /* QEMU SCSI bits */
    SCSIBus bus;
    SCSIDevice *current_dev;
    SCSIDevice *current_lun;
    SCSIRequest *current_req;

    enum WD33C93_STATE state;
    enum WD33C93_PHASE phase;
    Fifo8 fifo;
    int polled_transfer_lun;
    uint8_t* polled_transfer_buffer;
    off_t polled_transfer_pos;
    size_t polled_transfer_size;

    /* Registers */
    uint8_t reg_addr;

    uint8_t ownid;
    uint8_t control;
    uint8_t timeoutperiod;
    uint8_t totalsectors;
    uint8_t totalheads;
    uint16_t totalcylinders;
    uint32_t logicaladdress;
    uint8_t sectornumber;
    uint8_t headnumber;
    uint16_t cylindernumber;
    uint8_t targetlun;
    uint32_t transfercount;
    uint8_t destinationid;
    uint8_t sourceid;
    uint8_t scsistatus;
    uint8_t auxstat;
};

#define WD33C93_REG_FIFO_SZ 12

#define WD33C93_REG_BUS_ADDR 0x0
#define WD33C93_REG_BUS_DATA 0x1

#define WD33C93_REG_OWNID              0x00
#define WD33C93_REG_CONTROL            0x01
#define WD33C93_REG_CONTROL_DM_SHIFT   5
#define WD33C93_REG_CONTROL_DM_MASK    0x7
#define WD33C93_REG_CONTROL_DM_POLLED  0x0

#define WD33C93_REG_TIMEOUTPERIOD      0x02
#define WD33C93_REG_TOTALSECTORS       0x03
#define WD33C93_REG_TOTALHEADS         0x04
#define WD33C93_REG_TOTALCYLINDERS_MSB 0x05
#define WD33C93_REG_TOTALCYLINDERS_LSB 0x06
#define WD33C93_REG_LOGICALADDRESS_MSB 0x07
#define WD33C93_REG_LOGICALADDRESS_2ND 0x08
#define WD33C93_REG_LOGICALADDRESS_3RD 0x09
#define WD33C93_REG_LOGICALADDRESS_LSB 0x0a
#define WD33C93_REG_SECTORNUMBER       0x0b
#define WD33C93_REG_HEADNUMBER         0x0c
#define WD33C93_REG_CYLINDERNUMBER_MSB 0x0d
#define WD33C93_REG_CYLINDERNUMBER_LSB 0x0e
#define WD33C93_REG_TARGETLUN          0x0f
#define WD33C93_REG_COMMANDPHASE       0x10
#define WD33C93_REG_SYCHRONOUSTRANSFER 0x11
#define WD33C93_REG_TRANSFERCOUNT_MSB  0x12
#define WD33C93_REG_TRANSFERCOUNT_2ND  0x13
#define WD33C93_REG_TRANSFERCOUNT_LSB  0x14
#define WD33C93_REG_DESTINATIONID      0x15
#define WD33C93_REG_SOURCEID           0x16
#define WD33C93_REG_SCSISTATUS         0x17
#define WD33C93_REG_COMMAND            0x18
#define WD33C93_REG_DATA               0x19
#define WD33C93_REG_AUXILIARYSTAT      0x1f
#define WD33C93_REG_AUXILIARYSTAT_DBR  (1 << 0)
#define WD33C93_REG_AUXILIARYSTAT_INT  (1 << 7)

#define WD33C93_CMD_RESET              0x00
#define WD33C93_CMD_ABORT              0x01
#define WD33C93_CMD_ASSERT_ATN         0x02
#define WD33C93_CMD_NEGATE_ACK         0x03
#define WD33C93_CMD_DISCONNECT         0x04
#define WD33C93_CMD_RESELECT           0x05
#define WD33C93_CMD_SELECTWITHATN      0x06
#define WD33C93_CMD_SELECTWITHOUTATN   0x07
#define WD33C93_CMD_SELECTWITHATNTFR   0x08
#define WD33C93_CMD_SELECTWOATNTFR     0x09
#define WD33C93_CMD_RESELECTRX         0x0a
#define WD33C93_CMD_RESELECTTX         0x0b
#define WD33C93_CMD_WAITFORSELECTRX    0x0c
#define WD33C93_CMD_SENDSTATUSCMDCMPLT 0x0d
#define WD33C93_CMD_SENDDISCONMSG      0x0e
#define WD33C93_CMD_SENDIDI            0x0f
#define WD33C93_CMD_RECEIVECMD         0x10
#define WD33C93_CMD_RECEIVEDATA        0x11
#define WD33C93_CMD_RECEIVEMSGOUT      0x12
#define WD33C93_CMD_RECEIVEUNSPECOUT   0x13
#define WD33C93_CMD_SENDSTATUS         0x14
#define WD33C93_CMD_SENDIDATA          0x15
#define WD33C93_CMD_SENDMESSAGEIN      0x16
#define WD33C93_CMD_SENDUNSPECIN       0x17
#define WD33C93_CMD_TRANSLATEADDRESS   0x18
#define WD33C93_CMD_TRANSFERINFO       0x20

#define WD33C93_SCSISTATUS_COMPLETION          0x10
#define WD33C93_SCSISTATUS_COMPLETION_SELECTED 0x01
#define WD33C93_SCSISTATUS_COMPLETION_MCI      0x08
#define WD33C93_SCSISTATUS_MCI_DATA_OUT        0b000
#define WD33C93_SCSISTATUS_MCI_DATA_IN         0b001

#endif
