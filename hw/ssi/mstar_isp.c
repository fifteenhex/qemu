/*
 * MStar/SigmaStar ISP SPI-NOR controller
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/loader.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "qemu/datadir.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

/* ------------------------------------------------------------- isp (spi) */

/* Core registers (16-bit, from drivers/spi/spi-msc313-isp.c). */
#define ISP_PASSWORD        0x00
#define ISP_WDATA           0x10    /* byte to clock out */
#define ISP_RDATA           0x14    /* byte clocked in */
#define ISP_CLKDIV          0x18
#define ISP_CECLR           0x20    /* bit0: deassert chip select */
#define ISP_RDREQ           0x30    /* bit0: clock a byte in */
#define ISP_RD_DATARDY      0x54    /* bit0: read data ready */
#define ISP_WR_DATARDY      0x58    /* bit0: write data ready */
#define ISP_TRIGGER_MODE    0xa8
#define ISP_RST             0xfc
#define ISP_DATARDY_READY   (1 << 0)

static void msc313_isp_cs_assert(Msc313IspState *s)
{
    if (!s->cs_asserted) {
        qemu_set_irq(s->cs, 0);     /* active low: select the flash */
        s->cs_asserted = true;
    }
}

static void msc313_isp_cs_release(Msc313IspState *s)
{
    if (s->cs_asserted) {
        qemu_set_irq(s->cs, 1);     /* deselect */
        s->cs_asserted = false;
    }
}

static uint64_t msc313_isp_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313IspState *s = opaque;

    switch (addr) {
    case ISP_PASSWORD:
        return s->password;
    case ISP_RDATA:
        return s->rdata;
    case ISP_CLKDIV:
        return s->clkdiv;
    case ISP_TRIGGER_MODE:
        return s->trigger;
    case ISP_RST:
        return s->rst;
    case ISP_RD_DATARDY:
    case ISP_WR_DATARDY:
        /* Transfers complete synchronously, so always report ready. */
        return ISP_DATARDY_READY;
    }
    return 0;
}

static void msc313_isp_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Msc313IspState *s = opaque;

    switch (addr) {
    case ISP_PASSWORD:
        s->password = val;
        break;
    case ISP_WDATA:
        /* The controller pulls CS low on the first byte of a transaction. */
        msc313_isp_cs_assert(s);
        ssi_transfer(s->spi, val & 0xff);
        break;
    case ISP_RDREQ:
        if (val & 1) {
            msc313_isp_cs_assert(s);
            s->rdata = ssi_transfer(s->spi, 0xff) & 0xff;
        }
        break;
    case ISP_CECLR:
        if (val & 1) {
            msc313_isp_cs_release(s);
        }
        break;
    case ISP_CLKDIV:
        s->clkdiv = val;
        break;
    case ISP_TRIGGER_MODE:
        s->trigger = val;
        break;
    case ISP_RST:
        s->rst = val;
        break;
    }
}

static const MemoryRegionOps msc313_isp_ops = {
    .read = msc313_isp_read,
    .write = msc313_isp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static uint64_t msc313_isp_qspi_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313IspState *s = opaque;

    return s->qspi_regs[addr / 4];
}

static void msc313_isp_qspi_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    Msc313IspState *s = opaque;

    s->qspi_regs[addr / 4] = val;
}

static const MemoryRegionOps msc313_isp_qspi_ops = {
    .read = msc313_isp_qspi_read,
    .write = msc313_isp_qspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

/*
 * The memory-mapped XIP window. Model each access as the hardware does it: a
 * plain 0x03 READ with a 3-byte address issued on the SPI bus to the flash.
 */
static uint64_t msc313_isp_xip_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313IspState *s = opaque;
    uint64_t val = 0;
    unsigned int i;

    /*
     * The XIP window is a memory-mapped view of the flash. Serve it from the
     * cached image directly; this is both faster and equivalent to clocking
     * a 0x03 READ out to the flash. (Runtime erases/writes issued through the
     * byte interface are not reflected here.)
     */
    for (i = 0; i < size; i++) {
        uint32_t off = addr + i;
        uint8_t b = off < MSTAR_ISP_XIP_SIZE ? s->flash_cache[off] : 0xff;

        val |= (uint64_t)b << (8 * i);
    }
    return val;
}

static const MemoryRegionOps msc313_isp_xip_ops = {
    .read = msc313_isp_xip_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * The "fsp" flash controller (the ISP block's second register window at
 * 0x1f002c00). Register map from the vendor SDK (drivers/sstar/flash_isp,
 * regSERFLASH.h): a small write-data buffer (WD, regs 0x60-0x64, 2 bytes
 * each) and read-data buffer (RD, 0x65-0x69), the byte counts WBF/RBF_SIZE
 * (0x6a/0x6b, three 4-bit sub-counts), CTRL (0x6c), TRIGGER (0x6d, bit0 =
 * "fire"), DONE_FLAG (0x6e, bit0 = complete) and DONE_CLR (0x6f). The guest
 * fills the write buffer, fires, then polls DONE_FLAG. We run the whole
 * transaction on the flash synchronously and set DONE, so the poll always
 * completes.
 */
#define FSP_WD_BASE         0x60
#define FSP_RD_BASE         0x65
#define FSP_WBF_SIZE        0x6a
#define FSP_RBF_SIZE        0x6b
#define FSP_TRIGGER         0x6d
#define FSP_TRIGGER_FIRE    (1 << 0)
#define FSP_DONE_FLAG       0x6e
#define FSP_DONE            (1 << 0)
#define FSP_DONE_CLR        0x6f

static unsigned int fsp_size_sum(uint16_t v)
{
    /* WBF/RBF_SIZE pack three 4-bit per-phase byte counts. */
    return (v & 0xf) + ((v >> 4) & 0xf) + ((v >> 8) & 0xf);
}

static void msc313_isp_fsp_fire(Msc313IspState *s)
{
    unsigned int nw = MIN(fsp_size_sum(s->fsp_regs[FSP_WBF_SIZE]), 10);
    unsigned int nr = MIN(fsp_size_sum(s->fsp_regs[FSP_RBF_SIZE]), 10);
    unsigned int i;

    msc313_isp_cs_release(s);           /* finish any byte-path transaction */
    qemu_set_irq(s->cs, 0);             /* select */
    for (i = 0; i < nw; i++) {
        uint8_t b = s->fsp_regs[FSP_WD_BASE + i / 2] >> ((i & 1) * 8);

        ssi_transfer(s->spi, b);
    }
    for (i = 0; i < nr; i++) {
        uint8_t b = ssi_transfer(s->spi, 0xff) & 0xff;
        unsigned int r = FSP_RD_BASE + i / 2;

        if (i & 1) {
            s->fsp_regs[r] = (s->fsp_regs[r] & 0x00ff) | (b << 8);
        } else {
            s->fsp_regs[r] = (s->fsp_regs[r] & 0xff00) | b;
        }
    }
    qemu_set_irq(s->cs, 1);             /* deselect */
    s->fsp_regs[FSP_DONE_FLAG] |= FSP_DONE;
}

static uint64_t msc313_isp_fsp_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313IspState *s = opaque;

    return s->fsp_regs[addr / 4];
}

static void msc313_isp_fsp_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    Msc313IspState *s = opaque;
    unsigned int reg = addr / 4;

    s->fsp_regs[reg] = val;
    if (reg == FSP_TRIGGER && (val & FSP_TRIGGER_FIRE)) {
        msc313_isp_fsp_fire(s);
    } else if (reg == FSP_DONE_CLR && (val & 1)) {
        s->fsp_regs[FSP_DONE_FLAG] &= ~FSP_DONE;
    }
}

static const MemoryRegionOps msc313_isp_fsp_ops = {
    .read = msc313_isp_fsp_read,
    .write = msc313_isp_fsp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void msc313_isp_reset_hold(Object *obj, ResetType type)
{
    Msc313IspState *s = MSC313_ISP(obj);

    s->password = 0;
    s->clkdiv = 0;
    s->trigger = 0;
    s->rst = 0;
    s->rdata = 0;
    s->cs_asserted = false;
    memset(s->qspi_regs, 0, sizeof(s->qspi_regs));
    memset(s->fsp_regs, 0, sizeof(s->fsp_regs));
    qemu_set_irq(s->cs, 1);             /* deselect the flash */
}

static void msc313_isp_realize(DeviceState *dev, Error **errp)
{
    Msc313IspState *s = MSC313_ISP(dev);
    DeviceState *flash;
    DriveInfo *dinfo;

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313_isp_ops, s,
                          "mstar.isp", MSTAR_ISP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    memory_region_init_io(&s->fsp, OBJECT(dev), &msc313_isp_fsp_ops, s,
                          "mstar.isp-fsp", MSTAR_FSP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->fsp);

    memory_region_init_io(&s->qspi, OBJECT(dev), &msc313_isp_qspi_ops, s,
                          "mstar.isp-qspi", MSTAR_ISP_QSPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->qspi);

    memory_region_init_io(&s->xip, OBJECT(dev), &msc313_isp_xip_ops, s,
                          "mstar.isp-xip", MSTAR_ISP_XIP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->xip);

    s->spi = ssi_create_bus(dev, "spi");

    /* Attach a 16 MiB SPI-NOR flash, backed by "-drive if=mtd" if given. */
    flash = qdev_new("n25q128a13");
    dinfo = drive_get(IF_MTD, 0, 0);
    s->flash_cache = g_malloc(MSTAR_ISP_XIP_SIZE);
    memset(s->flash_cache, 0xff, MSTAR_ISP_XIP_SIZE);
    if (dinfo) {
        BlockBackend *blk = blk_by_legacy_dinfo(dinfo);
        int64_t len = blk_getlength(blk);

        if (len > MSTAR_ISP_XIP_SIZE) {
            len = MSTAR_ISP_XIP_SIZE;
        }
        if (len > 0) {
            blk_pread(blk, 0, len, s->flash_cache, 0);
        }
        qdev_prop_set_drive_err(flash, "drive", blk, &error_fatal);
    }
    qdev_realize_and_unref(flash, BUS(s->spi), &error_fatal);
    s->cs = qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0);
}

/* flash_cache is NOT migrated: realize refills it from the block backend,
 * which loadvm reverts to the same snapshot instant. */
static const VMStateDescription vmstate_mstar_msc313_isp = {
    .name = "mstar-msc313-isp",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(cs_asserted, Msc313IspState),
        VMSTATE_UINT16(rdata, Msc313IspState),
        VMSTATE_UINT16(password, Msc313IspState),
        VMSTATE_UINT16(clkdiv, Msc313IspState),
        VMSTATE_UINT16(trigger, Msc313IspState),
        VMSTATE_UINT16(rst, Msc313IspState),
        VMSTATE_UINT16_ARRAY(qspi_regs, Msc313IspState, MSTAR_ISP_QSPI_NUM_REGS),
        VMSTATE_UINT16_ARRAY(fsp_regs, Msc313IspState, MSTAR_ISP_FSP_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void msc313_isp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313_isp_realize;
    rc->phases.hold = msc313_isp_reset_hold;
    dc->vmsd = &vmstate_mstar_msc313_isp;
}

static const TypeInfo mstar_isp_types[] = {
    {
        .name           = TYPE_MSC313_ISP,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313IspState),
        .class_init     = msc313_isp_class_init,
    },
};

DEFINE_TYPES(mstar_isp_types)
