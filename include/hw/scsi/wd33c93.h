/*
 *
 */

#ifndef HW_WD33C93_H
#define HW_WD33C93_H

#include "hw/core/sysbus.h"
#include "hw/scsi/scsi.h"
#include "qemu/fifo8.h"
#include "qom/object.h"

#define TYPE_WD33C93 "wd33c93"

typedef struct WD33C93State WD33C93State;
OBJECT_DECLARE_SIMPLE_TYPE(WD33C93State, WD33C93)

enum WD33C93_STATE {
	WD33C93_IDLE,
	/* transfer info in progress, host moves bytes through the data reg */
	WD33C93_TI_IN,
	WD33C93_TI_OUT,
};

/* SCSI bus information transfer phases (MSG, C/D, I/O) */
enum WD33C93_BUS_PHASE {
	WD33C93_PHASE_DATA_OUT = 0,
	WD33C93_PHASE_DATA_IN  = 1,
	WD33C93_PHASE_COMMAND  = 2,
	WD33C93_PHASE_STATUS   = 3,
	WD33C93_PHASE_MSG_OUT  = 6,
	WD33C93_PHASE_MSG_IN   = 7,
	WD33C93_PHASE_BUS_FREE = 8,
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
    /* current data chunk from the scsi layer */
    uint8_t *scsi_buf;
    size_t scsi_len;
    size_t scsi_pos;
    bool req_done;
    uint8_t req_status;

    enum WD33C93_STATE state;
    enum WD33C93_BUS_PHASE bus_phase;
    bool atn;
    /* post a service-required interrupt once the last one is consumed */
    bool srv_req_pending;
    bool disconnect_pending;
    bool data_out_pending;
    Fifo8 fifo;

    /* raised while a dma-mode transfer wants servicing */
    qemu_irq drq;
    /* follows the INT bit in the auxiliary status register */
    qemu_irq irq;

    /*
     * A written command is processed after a short delay, during which
     * the chip reports Command-In-Progress / Busy, modelling the real
     * WD33C93 so a driver that polls status faster than the chip
     * updates it (i.e. without inter-access delays) misbehaves.
     */
    QEMUTimer *cmd_timer;
    uint8_t pending_cmd;
    bool cmd_in_progress;
    int64_t cmd_write_ns;

    /* select-and-transfer runs the whole transaction on its own */
    bool sat_active;
    /* registers 0x03..0x0e double as the cdb for select-and-transfer */
    uint8_t cdb_regs[12];

    /* transfer info bookkeeping */
    size_t ti_remaining;
    uint8_t identify;
    uint8_t cdb[16];
    int cdb_pos;
    uint8_t msg_in;
    bool msg_in_read;

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
    uint8_t cmdphase;
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
#define WD33C93_REG_OWNID_EAF          (1 << 3)
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
#define WD33C93_REG_AUXILIARYSTAT_CIP  (1 << 4)
#define WD33C93_REG_AUXILIARYSTAT_BUSY (1 << 5)
#define WD33C93_REG_AUXILIARYSTAT_INT  (1 << 7)

/* single byte transfer flag on transfer info */
#define WD33C93_CMD_SBT                0x80

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

/*
 * SCSI status register codes: high nibble is the class (reset,
 * completion, paused, terminated, service required), the phase of the
 * bus is in the low bits where noted.
 */
#define WD33C93_SCSISTATUS_RESET               0x00
#define WD33C93_SCSISTATUS_RESET_EAF           0x01
#define WD33C93_SCSISTATUS_SELECT_COMPLETE     0x11
#define WD33C93_SCSISTATUS_SEL_XFER_DONE       0x16
#define WD33C93_SCSISTATUS_XFER_DONE           0x18 /* | phase */
#define WD33C93_SCSISTATUS_MSGIN_PAUSED        0x20
#define WD33C93_SCSISTATUS_DISCONNECT          0x85
#define WD33C93_SCSISTATUS_SRV_REQ             0x88 /* | phase */

/*
 * Host bus interface (WD33C93_REG_BUS_ADDR / WD33C93_REG_BUS_DATA),
 * for boards whose glue chip decodes the SBIC itself.
 */
uint8_t wd33c93_io_read(WD33C93State *s, unsigned bus_addr);
void wd33c93_io_write(WD33C93State *s, unsigned bus_addr, uint8_t value);

/*
 * DMA port for the board dma engine: direction of the active
 * transfer (1 = device to memory, -1 = memory to device, 0 = idle)
 * and byte pull/push.
 */
int wd33c93_dma_dir(WD33C93State *s);
size_t wd33c93_dma_pull(WD33C93State *s, uint8_t *buf, size_t len);
size_t wd33c93_dma_push(WD33C93State *s, const uint8_t *buf, size_t len);

#endif
