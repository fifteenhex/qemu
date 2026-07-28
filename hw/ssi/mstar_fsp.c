/*
 * MStar/SigmaStar FSP flash sequencer + ISP byte path
 *
 * The ISP SPI-NOR controller has two ways to reach the flash:
 *
 *   - the FSP sequencer (this bank at 0x1f002c00): the guest fills a
 *     write buffer, fires the trigger and polls the done flag; reply
 *     bytes land in a read buffer. Register names follow the vendor SDK
 *     (regSERFLASH.h) as relayed by the previous branch; the register
 *     the boot ROM polls at +0x1b8 behaves as the done flag.
 *
 *   - the ISP core byte path (the bank at 0x1f001000): the guest clocks
 *     the flash a byte at a time - writeb() to WDATA, readb() of RDATA
 *     via RDREQ, dropping chip select with CECLR. The u-boot SPL loads
 *     u-boot proper this way (drivers/spi/msc313_spinor.c), so it must
 *     run real transactions.
 *
 * Both paths drive a real SPI flash on an SSI bus, so the SPL/boot ROM
 * read back the actual image (from "-drive if=mtd"). The same image is
 * also mapped read-only in the XIP window at 0x14000000.
 *
 * The FSP bank spans two RIU banks: the sequencer at 0x1f002c00 and the
 * QSPI config bank at 0x1f002e00. The latter carries the flash write
 * protect control (WP_CTRL): the Linux SERFLASH driver clears it before
 * each write/erase and sets it again after, reading it back each time.
 * Both WP registers behave as plain readback storage.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/ssi/ssi.h"
#include "hw/ssi/mstar_fsp.h"

/* FSP sequencer registers (16-bit registers on a 4 byte stride) */
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

/* ISP core byte-path registers (from drivers/spi/msc313_spinor.c) */
#define ISP_PASSWORD        0x00
#define ISP_WDATA           0x10    /* byte to clock out */
#define ISP_RDATA           0x14    /* byte clocked in */
#define ISP_CLKDIV          0x18
#define ISP_CECLR           0x20    /* bit0: deassert chip select */
#define ISP_RDREQ           0x30    /* bit0: clock a byte in */
#define ISP_RD_DATARDY      0x54    /* bit0: read data ready */
#define ISP_WR_DATARDY      0x58    /* bit0: write data ready */
#define ISP_DATARDY_READY   (1 << 0)

static void mstar_fsp_cs_assert(MStarFspState *s)
{
    if (!s->cs_asserted) {
        qemu_set_irq(s->cs, 0);     /* active low: select the flash */
        s->cs_asserted = true;
    }
}

static void mstar_fsp_cs_release(MStarFspState *s)
{
    if (s->cs_asserted) {
        qemu_set_irq(s->cs, 1);     /* deselect */
        s->cs_asserted = false;
    }
}

static unsigned int mstar_fsp_size_sum(uint16_t val)
{
    /* WBF/RBF_SIZE pack three 4-bit per-phase byte counts. */
    return (val & 0xf) + ((val >> 4) & 0xf) + ((val >> 8) & 0xf);
}

/*
 * Run the FSP command buffer against the flash: clock out the write
 * bytes, then clock in the read bytes, and raise DONE.
 */
static void mstar_fsp_fire(MStarFspState *s)
{
    unsigned int nw = MIN(mstar_fsp_size_sum(s->regs[FSP_WBF_SIZE]), 10);
    unsigned int nr = MIN(mstar_fsp_size_sum(s->regs[FSP_RBF_SIZE]), 10);
    unsigned int i;

    mstar_fsp_cs_release(s);        /* finish any byte-path transaction */
    qemu_set_irq(s->cs, 0);         /* select */
    for (i = 0; i < nw; i++) {
        uint8_t byte = s->regs[FSP_WD_BASE + i / 2] >> ((i & 1) * 8);

        ssi_transfer(s->spi, byte);
    }
    for (i = 0; i < nr; i++) {
        uint8_t byte = ssi_transfer(s->spi, 0xff) & 0xff;
        unsigned int r = FSP_RD_BASE + i / 2;

        if (i & 1) {
            s->regs[r] = (s->regs[r] & 0x00ff) | (byte << 8);
        } else {
            s->regs[r] = (s->regs[r] & 0xff00) | byte;
        }
    }
    qemu_set_irq(s->cs, 1);         /* deselect */
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

/* ---------------------------------------------------- ISP core (byte path) */

static uint64_t mstar_isp_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarFspState *s = MSTAR_FSP(opaque);

    switch (addr) {
    case ISP_RDATA:
        return s->rdata;
    case ISP_RD_DATARDY:
    case ISP_WR_DATARDY:
        /* Transfers complete synchronously, so always report ready. */
        return ISP_DATARDY_READY;
    default:
        return 0;
    }
}

static void mstar_isp_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MStarFspState *s = MSTAR_FSP(opaque);

    switch (addr) {
    case ISP_WDATA:
        /* The controller pulls CS low on the first byte of a transaction. */
        mstar_fsp_cs_assert(s);
        ssi_transfer(s->spi, val & 0xff);
        break;
    case ISP_RDREQ:
        if (val & 1) {
            mstar_fsp_cs_assert(s);
            s->rdata = ssi_transfer(s->spi, 0xff) & 0xff;
        }
        break;
    case ISP_CECLR:
        if (val & 1) {
            mstar_fsp_cs_release(s);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps mstar_isp_ops = {
    .read = mstar_isp_read,
    .write = mstar_isp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * The u-boot driver clocks the flash a byte at a time: writeb() to WDATA
     * and readb() of RDATA / the DATARDY status. Accept 1-byte accesses so
     * those reach the handler (a 2-byte minimum drops the WDATA writes and
     * the status poll spins forever - "set mode: 0" then a hang).
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_fsp_reset(DeviceState *dev)
{
    MStarFspState *s = MSTAR_FSP(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->rdata = 0;
    s->cs_asserted = false;
    qemu_set_irq(s->cs, 1);         /* deselect the flash */
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
    DeviceState *flash;
    DriveInfo *dinfo;
    void *xip;

    /* mmio region 1: the XIP read window (a read-only view of the flash) */
    memory_region_init_rom(&s->xip, OBJECT(dev), "mstar-fsp.xip",
                           MSTAR_FSP_XIP_SIZE, errp);
    if (*errp) {
        return;
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->xip);
    xip = memory_region_get_ram_ptr(&s->xip);
    memset(xip, 0xff, MSTAR_FSP_XIP_SIZE);       /* blank flash reads as 1s */

    /* mmio region 2: the ISP core byte-path registers */
    memory_region_init_io(&s->isp, OBJECT(dev), &mstar_isp_ops, s,
                          "mstar-fsp.isp", MSTAR_ISP_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->isp);

    /*
     * Attach a real SPI-NOR flash so the byte path and the FSP sequencer
     * read the actual image. Prime the XIP window from the same "-drive
     * if=mtd" image before handing the block backend to the flash device.
     */
    s->spi = ssi_create_bus(dev, "spi");
    dinfo = drive_get(IF_MTD, 0, 0);
    flash = qdev_new("w25q128");
    if (dinfo) {
        BlockBackend *blk = blk_by_legacy_dinfo(dinfo);
        int64_t size = blk_getlength(blk);

        if (size < 0 || size > MSTAR_FSP_XIP_SIZE) {
            error_setg(errp, "flash image must fit the 16 MiB XIP window");
            return;
        }
        if (size > 0 && blk_pread(blk, 0, size, xip, 0) < 0) {
            error_setg(errp, "failed to read the flash image");
            return;
        }
        s->blk = blk;
        qdev_prop_set_drive_err(flash, "drive", blk, &error_fatal);
    }
    qdev_realize_and_unref(flash, BUS(s->spi), &error_fatal);
    s->cs = qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0);
}

static void mstar_fsp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mstar_fsp_realize;
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
