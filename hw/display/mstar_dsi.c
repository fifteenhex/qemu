/*
 * MStar/SigmaStar MIPI DSI controller
 *
 * The SoC's MIPI DSI transmitter. It is a MediaTek DSI clone: the
 * register interface matches mainline's mtk_dsi (drivers/gpu/drm/
 * mediatek/mtk_dsi.c). Software builds a packet in the command queue,
 * kicks START, and the controller streams it over the D-PHY to the
 * panel and raises CMD_DONE in the interrupt status.
 *
 * The model decodes the queued packet into a MIPI packet and delivers
 * it to the linked panel (MIPI DSI is point-to-point, so the panel is
 * a single link, not a bus), then raises CMD_DONE. Without this the
 * vendor bring-up code spins on the status register and reports
 * "CMD Done Time Out".
 *
 * Register map (32-bit registers), from mtk_dsi (``linux``) and the
 * previous branch (``prev``):
 *   0x00 START      bit0 kicks the queued command-mode packet
 *   0x08 INTEN      interrupt enable
 *   0x0c INTSTA     bit1 CMD_DONE, bit2 TE_RDY, bit31 BUSY
 *   0x10 CON_CTRL   bit0 DSI_RESET, bit1 DSI_EN, bit2 DPHY_RESET
 *   0x14 MODE_CTRL  0 command mode, 1 sync-pulse, 2 event, 3 burst
 *   0x1c PSCTRL     [13:0] word count = width*bpp, [17:16] pixel sel
 *   0x60 CMDQ_SIZE  [5:0] number of 32-bit CMDQ words queued
 *   0x200 CMDQ0..   command queue; word0 = packet header (byte0
 *                   config, byte1 data type, byte2/3 data0/data1),
 *                   long-packet payload in the following words
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/mstar_dsi.h"
#include "trace.h"

#define DSI_START           0x00
#define DSI_START_KICK      (1 << 0)
#define DSI_INTEN           0x08
#define DSI_INTSTA          0x0c
#define DSI_INTSTA_CMD_DONE (1 << 1)
#define DSI_INTSTA_TE_RDY   (1 << 2)
#define DSI_INTSTA_BUSY     (1u << 31)
#define DSI_CON_CTRL        0x10
#define DSI_MODE_CTRL       0x14
#define DSI_CMDQ_SIZE       0x60
#define DSI_CMDQ_SIZE_MASK  0x3f
#define DSI_CMDQ            0x200

/* A long-packet data type has DT[3:0] == 0x9 (0x29 generic, 0x39 DCS) */
#define DSI_DT_IS_LONG(dt)  (((dt) & 0x0f) == 0x09)

#define DSI_MAX_PAYLOAD     256

static void mstar_dsi_update_irq(MStarDsiState *s)
{
    bool active = s->regs[DSI_INTSTA / 4] & s->regs[DSI_INTEN / 4];

    qemu_set_irq(s->irq, active);
}

/* Decode the queued packet and deliver it to the panel */
static void mstar_dsi_kick(MStarDsiState *s)
{
    uint32_t hdr = s->regs[DSI_CMDQ / 4];
    uint8_t data_type = (hdr >> 8) & 0xff;
    uint8_t data0 = (hdr >> 16) & 0xff;
    uint8_t data1 = (hdr >> 24) & 0xff;

    if (s->panel) {
        if (DSI_DT_IS_LONG(data_type)) {
            uint32_t wc = data0 | (data1 << 8);
            uint8_t payload[DSI_MAX_PAYLOAD];
            uint32_t i;

            wc = MIN(wc, DSI_MAX_PAYLOAD);
            for (i = 0; i < wc; i++) {
                uint32_t word = s->regs[DSI_CMDQ / 4 + 1 + i / 4];

                payload[i] = (word >> (8 * (i % 4))) & 0xff;
            }
            trace_mstar_dsi_packet("long", data_type, wc ? payload[0] : 0, wc);
            dsi_panel_receive(s->panel, data_type, payload, wc);
        } else {
            uint8_t payload[2] = { data0, data1 };

            trace_mstar_dsi_packet("short", data_type, data0, data1);
            dsi_panel_receive(s->panel, data_type, payload, 2);
        }
    }

    /* The transfer "completes" immediately: raise CMD_DONE, clear BUSY */
    s->regs[DSI_INTSTA / 4] =
        (s->regs[DSI_INTSTA / 4] | DSI_INTSTA_CMD_DONE) & ~DSI_INTSTA_BUSY;
    mstar_dsi_update_irq(s);
}

static uint64_t mstar_dsi_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarDsiState *s = MSTAR_DSI(opaque);

    return s->regs[addr / 4];
}

static void mstar_dsi_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MStarDsiState *s = MSTAR_DSI(opaque);

    switch (addr) {
    case DSI_START:
        s->regs[DSI_START / 4] = val;
        if (val & DSI_START_KICK) {
            mstar_dsi_kick(s);
        }
        break;
    case DSI_INTSTA:
        /* Status bits are write-0-to-clear (write back what was read) */
        s->regs[DSI_INTSTA / 4] &= val;
        mstar_dsi_update_irq(s);
        break;
    case DSI_MODE_CTRL:
        s->regs[DSI_MODE_CTRL / 4] = val;
        /*
         * In a video mode the controller free-runs, so a tear-effect
         * poll must make progress: latch TE_RDY when a mode is set.
         */
        s->regs[DSI_INTSTA / 4] |= DSI_INTSTA_TE_RDY;
        mstar_dsi_update_irq(s);
        break;
    default:
        s->regs[addr / 4] = val;
        break;
    }
}

static const MemoryRegionOps mstar_dsi_ops = {
    .read = mstar_dsi_read,
    .write = mstar_dsi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void mstar_dsi_reset(DeviceState *dev)
{
    MStarDsiState *s = MSTAR_DSI(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mstar_dsi_init(Object *obj)
{
    MStarDsiState *s = MSTAR_DSI(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_dsi_ops, s,
                          TYPE_MSTAR_DSI, MSTAR_DSI_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property mstar_dsi_properties[] = {
    DEFINE_PROP_LINK("panel", MStarDsiState, panel, TYPE_DSI_PANEL, DsiPanel *),
};

static void mstar_dsi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_dsi_reset);
    device_class_set_props(dc, mstar_dsi_properties);
}

static const TypeInfo mstar_dsi_types[] = {
    {
        .name           = TYPE_MSTAR_DSI,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarDsiState),
        .instance_init  = mstar_dsi_init,
        .class_init     = mstar_dsi_class_init,
    },
};

DEFINE_TYPES(mstar_dsi_types)
