/*
 *
 */

#ifndef HW_MVME147_VMECHIP_H
#define HW_MVME147_VMECHIP_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_MVME147_VMECHIP "mvme147.vmechip"

typedef struct MVME147VMEChipState MVME147VMEChipState;
OBJECT_DECLARE_SIMPLE_TYPE(MVME147VMEChipState, MVME147_VMECHIP)

struct MVME147VMEChipState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    uint8_t syscontroller_cnfg;
    uint8_t timer_cnfg;
    uint8_t bus_err_status;
};

#define MVME147_VMECHIP_SYSCONTROLLER_CNFG      0x1
#define MVME147_VMECHIP_SYSCONTROLLER_CNFG_SCON (1 << 0)
#define MVME147_VMECHIP_VMEBUS_REQ_CNFG         0x3
#define MVME147_VMECHIP_VMEBUS_REQ_CNFG_MASTER  (1 << 6)
#define MVME147_VMECHIP_TIMER_CNFG              0x9
#define MVME147_VMECHIP_MASTER_ADDR_MOD         0xd
#define MVME147_VMECHIP_BUS_ERR_STATUS          0x19

#endif
