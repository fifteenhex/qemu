/*
 *
 */

#ifndef HW_DRAGONBALL_LCDC_H
#define HW_DRAGONBALL_LCDC_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_LCDC "dragonball.lcdc"

typedef struct DragonBallLCDCState DragonBallLCDCState;
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallLCDCState, DRAGONBALL_LCDC)

struct DragonBallLCDCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    MemoryRegionSection fbsection;
    MemoryRegion *fbmem;


    QemuConsole *con;

    /* registers */
    uint32_t lssa;
    uint8_t  lvpw;
    uint16_t lxmax;
    uint16_t lymax;
    uint16_t lcxp;
    uint16_t lcyp;
    uint16_t lcwch;
    uint8_t  lblkc;
    uint8_t  lpicf;
    uint8_t  lpolcf;
    uint8_t  lacdrc;
    uint8_t  lpxcd;
    uint8_t  lckcon;
    uint8_t  lrra;
    uint8_t  lposr;
    uint8_t  lfrcm;
    uint8_t  lgpmr;
    uint16_t pwmr;
};

#define DRAGONBALL_LCDC_LSSA   0x0
#define DRAGONBALL_LCDC_LVPW   0x5
#define DRAGONBALL_LCDC_LXMAX  0x8
#define DRAGONBALL_LCDC_LYMAX  0xa
#define DRAGONBALL_LCDC_LCXP   0x18
#define DRAGONBALL_LCDC_LCYP   0x1a
#define DRAGONBALL_LCDC_LCWCH  0x1c
#define DRAGONBALL_LCDC_LBLKC  0x1f
#define DRAGONBALL_LCDC_LPICF  0x20
#define DRAGONBALL_LCDC_LPICF_PBSIZ_MASK  0x3
#define DRAGONBALL_LCDC_LPICF_PBSIZ_SHIFT 2
#define DRAGONBALL_LCDC_LPICF_GS_MASK     0x3

#define DRAGONBALL_LCDC_LPOLCF 0x21
#define DRAGONBALL_LCDC_LACDRC 0x23
#define DRAGONBALL_LCDC_LPXCD  0x25
#define DRAGONBALL_LCDC_LCKCON 0x27
#define DRAGONBALL_LCDC_LRRA   0x29
#define DRAGONBALL_LCDC_LPOSR  0x2d
#define DRAGONBALL_LCDC_LFRCM  0x31
#define DRAGONBALL_LCDC_LGPMR  0x33
#define DRAGONBALL_LCDC_PWMR   0x36

#endif
