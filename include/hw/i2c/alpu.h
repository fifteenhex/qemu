/*
 * Neowine ALPU-FA i2c copy-protection / authentication chip
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_ALPU_H
#define HW_I2C_ALPU_H

#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_ALPU "alpu"
OBJECT_DECLARE_SIMPLE_TYPE(AlpuState, ALPU)

#define ALPU_BUFSZ 64

struct AlpuState {
    /*< private >*/
    I2CSlave parent_obj;
    /*< public >*/

    bool reading;               /* current transfer is a master read */
    unsigned cmd_len;           /* bytes written by the host so far */
    uint8_t cmd[ALPU_BUFSZ];
    unsigned resp_pos;          /* next response byte to hand back */

    /* Challenge-response crypto state (see alpu.c) */
    uint8_t ac1;                /* mirror of the host's buf_ac[1] counter */
    bool have_e9;               /* a challenge was written to register 0xe9 */
    bool have_87;               /* a challenge was written to register 0x87 */
    uint8_t tgt_e9[8];          /* captured arg0 from the 0xe9 write */
    uint8_t tgt_87[8];          /* captured arg0 from the 0x87 write */
    uint8_t resp[16];           /* prepared response for the next read */
    unsigned resp_len;
};

#endif /* HW_I2C_ALPU_H */
