/*
 * MStar/SigmaStar RTC power/wake controller (rtcpwc)
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_RTC_MSTAR_RTCPWC_H
#define HW_RTC_MSTAR_RTCPWC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_RTCPWC "mstar-rtcpwc"
OBJECT_DECLARE_SIMPLE_TYPE(MStarRtcpwcState, MSTAR_RTCPWC)

#define MSTAR_RTCPWC_SIZE     0x200
#define MSTAR_RTCPWC_NUM_REGS (MSTAR_RTCPWC_SIZE / 4)

struct MStarRtcpwcState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    uint16_t regs[MSTAR_RTCPWC_NUM_REGS];
    bool iso_ack;           /* current DIG2RTC/RTC2DIG ISO handshake ack */
    uint32_t base_seconds;  /* wall-clock seconds at reset */
    int64_t reset_ns;       /* virtual-clock ns at reset (counter epoch) */
};

#endif /* HW_RTC_MSTAR_RTCPWC_H */
