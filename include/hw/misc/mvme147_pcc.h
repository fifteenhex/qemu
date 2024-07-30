/*
 *
 */

#ifndef HW_MVME147_PCC_H
#define HW_MVME147_PCC_H

#include "hw/sysbus.h"
#include "hw/ptimer.h"
#include "qom/object.h"

#define TYPE_MVME147_PCC "mvme147.pcc"

typedef struct MVME147PCCState MVME147PCCState;
OBJECT_DECLARE_SIMPLE_TYPE(MVME147PCCState, MVME147_PCC)

struct MVME147PCCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    ptimer_state *timers[2];

    uint32_t table_address;
    uint32_t data_address;
    uint32_t link;

    uint16_t timerN_preload[2];
    uint8_t timerN_int_ctrl[2];
    uint8_t timerN_ctrl[2];
    uint8_t gen_purpose_control;
    uint8_t gen_purpose_stat;
};

/* 32 bit registers */
#define MVME147_PCC_TABLE_ADDRESS            0x0
#define MVME147_PCC_DATA_ADDRESS             0x4
#define MVME147_PCC_LINK                     0x8
/* 16 bit registers */
#define MVME147_PCC_TIMER1_PRELOAD           0x10
#define MVME147_PCC_TIMER1_COUNT             0x12
#define MVME147_PCC_TIMER2_PRELOAD           0x14
#define MVME147_PCC_TIMER2_COUNT             0x16
/* 8 bit registers */
#define MVME147_PCC_TIMER1_INT_CTRL          0x18
#define MVME147_PCC_TIMERN_INT_CTRL_INTSTAT	 (1 << 7)
#define MVME147_PCC_TIMER1_CTRL              0x19
#define MVME147_PCC_TIMERN_CTRL_ENABLE       (1 << 0)
#define MVME147_PCC_TIMERN_CTRL_ENACNT       (1 << 1)
#define MVME147_PCC_TIMERN_CTRL_CLROVF       (1 << 2)
#define MVME147_PCC_TIMERN_CTRL_OVF_SHIFT    4
#define MVME147_PCC_TIMERN_CTRL_OVF_MASK     0xf
#define MVME147_PCC_TIMER2_INT_CTRL          0x1a
#define MVME147_PCC_TIMER2_CTRL              0x1b
#define MVME147_PCC_AC_FAIL_INT_CTRL         0x1c
#define MVME147_PCC_WDOG_TIMER_CTRL          0x1d
#define MVME147_PCC_PRINTER_INT_CTRL         0x1e
#define MVME147_PCC_PRINTER_CTRL             0x1f
#define MVME147_PCC_DMA_INT_CTRL             0x20
#define MVME147_PCC_DMA_CTRL_STAT            0x21
#define MVME147_PCC_DMA_CTRL_STAT_ENABLE	 (1 << 0)
#define MVME147_PCC_BUS_ERROR_INT_CTRL       0x22
#define MVME147_PCC_DMA_STATUS               0x23
#define MVME147_PCC_ABORT_INT_CTRL           0x24
#define MVME147_PCC_ABORT_INT_CTRL_INT_STAT  (1 << 7)
#define MVME147_PCC_TBL_AD_FUNC_CTRL         0x25
#define MVME147_PCC_SERIAL_PRT_INT_CTRL      0x26
#define MVME147_PCC_GEN_PURPOSE_CTRL         0x27
#define MVME147_PCC_GEN_PURPOSE_CTRL_MINTEN  (1 << 4)
#define MVME147_PCC_LAN_INT_CTRL             0x28
#define MVME147_PCC_GEN_PURPOSE_STAT         0x29
#define MVME147_PCC_GEN_PURPOSE_STAT_PARERR  (1 << 0)
#define MVME147_PCC_GEN_PURPOSE_STAT_PURESET (1 << 1)
#define MVME147_PCC_SCSI_PRT_INT_CTRL        0x2a
#define MVME147_PCC_SLAVE_BASE_ADDR          0x2b
#define MVME147_PCC_SW_INT1_CTRL             0x2c
#define MVME147_PCC_INT_VECTOR_BASE          0x2d
#define MVME147_PCC_SW_INT2_CTRL             0x2e
#endif
