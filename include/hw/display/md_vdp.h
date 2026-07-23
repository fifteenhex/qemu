/* SPDX-License-Identifier: MIT */
/* QEMU Sega MegaDrive machine / Everdrive / VDP */

#ifndef MD_VDP_H
#define MD_VDP_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MD_VDP "md-vdp"
OBJECT_DECLARE_SIMPLE_TYPE(MDVDPState, MD_VDP)

#define MD_VDP_NUM_REGS 24

#define MD_VDP_VRAM_SIZE  (64 * 1024)
#define MD_VDP_CRAM_SIZE  (64)
#define MD_VDP_VSRAM_SIZE (40)

struct MDVDPState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion iomem_hi;

    uint8_t  regs[MD_VDP_NUM_REGS];

    uint8_t  vram[MD_VDP_VRAM_SIZE];
    uint16_t cram[MD_VDP_CRAM_SIZE];
    uint16_t vsram[MD_VDP_VSRAM_SIZE];

    bool     pending_cmd;
    uint16_t cmd_first;
    uint32_t addr;
    uint8_t  code;

    qemu_irq irq_vint;
    qemu_irq irq_hint;

    QEMUTimer *vint_timer;
    bool       vint_pending;

    uint32_t   scale;

    QemuConsole *con;
};

#define VDP_PORT_DATA_0     0x00
#define VDP_PORT_DATA_1     0x02
#define VDP_PORT_CTRL_0     0x04
#define VDP_PORT_CTRL_1     0x06
#define VDP_PORT_HV_0       0x08

#endif
