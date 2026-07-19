/*
 * MIPI DSI panel interface
 *
 * MIPI DSI is a point-to-point link: one host (the SoC's DSI
 * controller) drives one peripheral (the panel). This models that
 * link as a QOM link from the host to a panel device plus a single
 * "receive a packet" entry point, rather than a shared bus.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_DSI_H
#define HW_DISPLAY_DSI_H

#include "hw/core/qdev.h"
#include "qom/object.h"

/*
 * MIPI DSI data types (the low byte of a packet header). Short
 * packets carry the two header data bytes; long packets carry a
 * word-count and a payload.
 */
#define MIPI_DSI_DCS_SHORT_WRITE        0x05    /* command, no parameter */
#define MIPI_DSI_DCS_SHORT_WRITE_PARAM  0x15    /* command + 1 parameter */
#define MIPI_DSI_DCS_READ               0x06
#define MIPI_DSI_DCS_LONG_WRITE         0x39    /* command + payload */
#define MIPI_DSI_GENERIC_SHORT_WRITE_0  0x03
#define MIPI_DSI_GENERIC_SHORT_WRITE_1  0x13
#define MIPI_DSI_GENERIC_SHORT_WRITE_2  0x23
#define MIPI_DSI_GENERIC_LONG_WRITE     0x29

#define TYPE_DSI_PANEL "dsi-panel"
OBJECT_DECLARE_TYPE(DsiPanel, DsiPanelClass, DSI_PANEL)

/* A generic panel implementing the standard MIPI DCS command set */
#define TYPE_DSI_DCS_PANEL "dsi-dcs-panel"

struct DsiPanel {
    /*< private >*/
    DeviceState parent_obj;
};

struct DsiPanelClass {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/

    /*
     * Deliver one DSI packet to the panel. data_type is the MIPI data
     * type; for a short packet payload holds the two header data bytes
     * (len 1 or 2), for a long packet it holds the whole payload.
     */
    void (*receive)(DsiPanel *panel, uint8_t data_type,
                    const uint8_t *payload, uint32_t len);
};

static inline void dsi_panel_receive(DsiPanel *panel, uint8_t data_type,
                                     const uint8_t *payload, uint32_t len)
{
    DsiPanelClass *pc = DSI_PANEL_GET_CLASS(panel);

    if (pc->receive) {
        pc->receive(panel, data_type, payload, len);
    }
}

#endif /* HW_DISPLAY_DSI_H */
