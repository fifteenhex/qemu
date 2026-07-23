/*
 * Akiko, the CD32 system chip.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_M68K_CD32_AKIKO_H
#define HW_M68K_CD32_AKIKO_H

#include "hw/core/sysbus.h"
#include "hw/i2c/bitbang_i2c.h"
#include "qom/object.h"

#define TYPE_CD32_AKIKO "cd32-akiko"
OBJECT_DECLARE_SIMPLE_TYPE(CD32AkikoState, CD32_AKIKO)

/* Akiko decodes a 0x40-byte register block at 0xb80000 */
#define CD32_AKIKO_BASE 0xb80000
#define CD32_AKIKO_SIZE 0x40

struct CD32AkikoState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t intreq;
    uint32_t intena;

    /* CD controller registers (stored, no drive modelled yet) */
    uint32_t cd_dma_data;
    uint32_t cd_dma_misc;
    uint8_t cd_subcode_off;
    uint8_t cd_tx_inx;
    uint8_t cd_rx_inx;
    uint8_t cd_rx_cmp;
    uint8_t cd_tx_cmp;
    uint16_t cd_pbx;
    uint32_t cd_flags;

    /* NVRAM I2C bit-bang lines */
    I2CBus *i2c_bus;
    bitbang_i2c_interface bitbang;
    uint8_t nvram_io;
    uint8_t nvram_dir;
    bool nvram_scl_in;
    bool nvram_sda_in;

    /* chunky-to-planar port */
    uint32_t c2p_buf[8];
    uint32_t c2p_result[8];
    uint8_t c2p_wpos;
    uint8_t c2p_rpos;
    bool c2p_valid;
};

#endif
