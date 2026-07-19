/*
 * MStar/SigmaStar FSP flash sequencer
 *
 * The FSP is the part of the ISP SPI NOR controller that runs small
 * command sequences against the flash: the guest fills a write
 * buffer, fires the trigger and polls the done flag; reply bytes land
 * in a read buffer. Register names follow the vendor SDK
 * (regSERFLASH.h) as relayed by the previous branch; the register the
 * boot ROM polls at +0x1b8 was confirmed to behave as the done flag.
 *
 * This model does not yet talk to a real SPI flash: firing a
 * transaction just logs the command bytes and sets the done flag, and
 * the read buffer reads back zeros. That is enough for the boot ROM
 * to march through its flash init; the data itself is fetched through
 * the XIP window, which is backed separately.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "system/block-backend.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/ssi/mstar_fsp.h"

/* Register numbers (16-bit registers on a 4 byte stride) */
#define FSP_WD_BASE         0x60    /* write buffer, 2 bytes per register */
#define FSP_RD_BASE         0x65    /* read buffer, 2 bytes per register */
#define FSP_WBF_SIZE        0x6a    /* three 4-bit write byte counts */
#define FSP_RBF_SIZE        0x6b    /* three 4-bit read byte counts */
#define FSP_CTRL            0x6c
#define FSP_TRIGGER         0x6d
#define FSP_TRIGGER_FIRE    (1 << 0)
#define FSP_DONE_FLAG       0x6e
#define FSP_DONE            (1 << 0)
#define FSP_DONE_CLR        0x6f
#define FSP_DONE_CLR_CLEAR  (1 << 0)

static unsigned int mstar_fsp_size_sum(uint16_t val)
{
    return (val & 0xf) + ((val >> 4) & 0xf) + ((val >> 8) & 0xf);
}

static void mstar_fsp_fire(MStarFspState *s)
{
    unsigned int nw = mstar_fsp_size_sum(s->regs[FSP_WBF_SIZE]);
    unsigned int i;
    g_autoptr(GString) cmd = g_string_new(NULL);

    for (i = 0; i < MIN(nw, 10); i++) {
        uint8_t byte = s->regs[FSP_WD_BASE + i / 2] >> ((i & 1) * 8);

        g_string_append_printf(cmd, " %02x", byte);
    }
    qemu_log_mask(LOG_UNIMP, "mstar-fsp: fired (transaction not modelled):"
                  " cmd%s\n", cmd->str);

    s->regs[FSP_DONE_FLAG] |= FSP_DONE;
}

static uint64_t mstar_fsp_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarFspState *s = MSTAR_FSP(opaque);

    return s->regs[addr / 4];
}

static void mstar_fsp_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MStarFspState *s = MSTAR_FSP(opaque);
    unsigned int reg = addr / 4;

    s->regs[reg] = val;

    switch (reg) {
    case FSP_TRIGGER:
        if (val & FSP_TRIGGER_FIRE) {
            mstar_fsp_fire(s);
        }
        break;
    case FSP_DONE_CLR:
        if (val & FSP_DONE_CLR_CLEAR) {
            s->regs[FSP_DONE_FLAG] &= ~FSP_DONE;
        }
        break;
    }
}

static const MemoryRegionOps mstar_fsp_ops = {
    .read = mstar_fsp_read,
    .write = mstar_fsp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_fsp_reset(DeviceState *dev)
{
    MStarFspState *s = MSTAR_FSP(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mstar_fsp_init(Object *obj)
{
    MStarFspState *s = MSTAR_FSP(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_fsp_ops, s, TYPE_MSTAR_FSP,
                          MSTAR_FSP_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void mstar_fsp_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    MStarFspState *s = MSTAR_FSP(dev);
    void *xip;

    memory_region_init_rom(&s->xip, OBJECT(dev), "mstar-fsp.xip",
                           MSTAR_FSP_XIP_SIZE, errp);
    if (*errp) {
        return;
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->xip);

    /* Blank flash reads as all ones */
    xip = memory_region_get_ram_ptr(&s->xip);
    memset(xip, 0xff, MSTAR_FSP_XIP_SIZE);

    if (s->blk) {
        int64_t size = blk_getlength(s->blk);

        if (size < 0 || size > MSTAR_FSP_XIP_SIZE) {
            error_setg(errp, "flash image must fit the 16 MiB XIP window");
            return;
        }
        if (blk_pread(s->blk, 0, size, xip, 0) < 0) {
            error_setg(errp, "failed to read the flash image");
            return;
        }
    }
}

static const Property mstar_fsp_properties[] = {
    DEFINE_PROP_DRIVE("drive", MStarFspState, blk),
};

static void mstar_fsp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mstar_fsp_realize;
    device_class_set_props(dc, mstar_fsp_properties);
    device_class_set_legacy_reset(dc, mstar_fsp_reset);
}

static const TypeInfo mstar_fsp_types[] = {
    {
        .name           = TYPE_MSTAR_FSP,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarFspState),
        .instance_init  = mstar_fsp_init,
        .class_init     = mstar_fsp_class_init,
    },
};

DEFINE_TYPES(mstar_fsp_types)
