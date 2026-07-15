/*
 * MStar/SigmaStar FCIE SD/MMC host controller
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
#include "hw/arm/mstar.h"

/* -------------------------------------------------------------- sdio (FCIE) */

/* Register map from the mainline driver drivers/mmc/host/mstar-fcie.c. */
#define SDIO_INT             0x00
#define SDIO_INT_DATA_END    (1 << 0)
#define SDIO_INT_CMD_END     (1 << 1)
#define SDIO_INT_BUSY_END    (1 << 4)
#define SDIO_INT_R2N_RDY     (1 << 5)
#define SDIO_INTMASK         0x04
#define SDIO_DMA_ADDR_L      0x0c
#define SDIO_DMA_ADDR_H      0x10
#define SDIO_DMA_LEN_L       0x14
#define SDIO_DMA_LEN_H       0x18
#define SDIO_SD_CTL          0x30
#define SDIO_SD_CTL_RSPEN    (1 << 1)
#define SDIO_SD_CTL_CMDEN    (1 << 2)
#define SDIO_SD_CTL_DTRFEN   (1 << 3)
#define SDIO_SD_CTL_JOBDIR   (1 << 4)    /* 0 = read (card->mem), 1 = write */
#define SDIO_SD_CTL_ADMAEN   (1 << 5)
#define SDIO_SD_CTL_JOBSTART (1 << 6)
#define SDIO_SD_CTL_BUSYDETEN (1 << 8)
#define SDIO_SD_STS          0x34
#define SDIO_SD_STS_NORSP    (1 << 3)
#define SDIO_SD_STS_D0       (1 << 8)
#define SDIO_FIFO            0x80
#define SDIO_FIFO_END        0xc0
#define SDIO_RST             0xfc
#define SDIO_RST_NRST        (1 << 0)

/* DMA uses bus addresses; the MIU dma-ranges maps bus 0 to DRAM. */
static hwaddr msc313_sdio_dma_phys(uint32_t bus_addr)
{
    return MSTAR_DRAM_BASE + bus_addr;
}

static void msc313_sdio_update_irq(Msc313SdioState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[SDIO_INT / 4] & s->regs[SDIO_INTMASK / 4]));
}

static void msc313_sdio_xfer_one(Msc313SdioState *s, hwaddr phys,
                                 uint32_t len, bool write)
{
    g_autofree uint8_t *buf = g_malloc(len);

    if (write) {
        address_space_read(&address_space_memory, phys, MEMTXATTRS_UNSPECIFIED,
                           buf, len);
        sdbus_write_data(&s->sdbus, buf, len);
    } else {
        sdbus_read_data(&s->sdbus, buf, len);
        address_space_write(&address_space_memory, phys, MEMTXATTRS_UNSPECIFIED,
                            buf, len);
    }
}

static void msc313_sdio_data_xfer(Msc313SdioState *s, bool write)
{
    uint32_t dma = s->regs[SDIO_DMA_ADDR_L / 4] |
                   ((uint32_t)s->regs[SDIO_DMA_ADDR_H / 4] << 16);

    if (s->regs[SDIO_SD_CTL / 4] & SDIO_SD_CTL_ADMAEN) {
        /* ADMA: DMA_ADDR points at a list of 16-byte descriptors. */
        hwaddr desc = msc313_sdio_dma_phys(dma);
        unsigned int guard = 0;

        do {
            uint32_t ctrl = address_space_ldl_le(&address_space_memory, desc,
                                                 MEMTXATTRS_UNSPECIFIED, NULL);
            uint32_t daddr = address_space_ldl_le(&address_space_memory,
                                    desc + 4, MEMTXATTRS_UNSPECIFIED, NULL);
            uint32_t dlen = address_space_ldl_le(&address_space_memory,
                                    desc + 8, MEMTXATTRS_UNSPECIFIED, NULL);

            msc313_sdio_xfer_one(s, msc313_sdio_dma_phys(daddr), dlen, write);
            if (ctrl & 1) {         /* ADMA_DESC_CTRL_END */
                break;
            }
            desc += 16;
        } while (++guard < 4096);
    } else {
        uint32_t len = s->regs[SDIO_DMA_LEN_L / 4] |
                       ((uint32_t)s->regs[SDIO_DMA_LEN_H / 4] << 16);

        msc313_sdio_xfer_one(s, msc313_sdio_dma_phys(dma), len, write);
    }
}

static void msc313_sdio_run(Msc313SdioState *s)
{
    uint16_t ctl = s->regs[SDIO_SD_CTL / 4];
    bool docmd = ctl & SDIO_SD_CTL_CMDEN;
    uint8_t resp[16];
    int rlen = 0;

    /*
     * A job may carry a command, data, or both. Reads send the command with
     * the first data block (cmd_en + dtrf_en together); writes send the
     * command in a command-only job first, then issue a data-only job with
     * cmd_en clear - so only touch the card's command state when cmd_en is
     * set, otherwise the FIFO still holds the previous response.
     */
    if (docmd) {
        SDRequest req = {
            .cmd = s->fifo[0] & 0x3f,
            .arg = (s->fifo[1] << 24) | (s->fifo[2] << 16) |
                   (s->fifo[3] << 8) | s->fifo[4],
        };

        s->last_cmd = req.cmd;
        rlen = sdbus_do_command(&s->sdbus, &req, resp, sizeof(resp));
    }

    /* job start is self-clearing; reset the status (data line idle). */
    s->regs[SDIO_SD_CTL / 4] = ctl & ~SDIO_SD_CTL_JOBSTART;
    s->regs[SDIO_SD_STS / 4] = SDIO_SD_STS_D0;

    if (ctl & SDIO_SD_CTL_DTRFEN) {
        msc313_sdio_data_xfer(s, ctl & SDIO_SD_CTL_JOBDIR);
        /*
         * The DMA engine is programmed with a block count, so the controller
         * transfers exactly that many blocks and auto-issues a STOP for the
         * open-ended multi-block commands (the driver leans on this rather
         * than always sending its own CMD12). Return the card to tran state.
         */
        if (s->last_cmd == 18 || s->last_cmd == 25) {
            SDRequest stop = { .cmd = 12 };
            uint8_t sr[16];

            sdbus_do_command(&s->sdbus, &stop, sr, sizeof(sr));
        }
    }

    /* Rebuild the FIFO with the response so the guest can read it back. The
     * driver strips the first byte (command index for short responses, an
     * 0x3f header for R2) and takes the rest big-endian. */
    if (docmd) {
        memset(s->fifo, 0, sizeof(s->fifo));
        if (rlen == 4) {
            s->fifo[0] = s->last_cmd;
            memcpy(&s->fifo[1], resp, 4);
        } else if (rlen == 16) {
            s->fifo[0] = 0x3f;
            memcpy(&s->fifo[1], resp, 16);
        } else if (ctl & SDIO_SD_CTL_RSPEN) {
            s->regs[SDIO_SD_STS / 4] |= SDIO_SD_STS_NORSP;
        }
    }

    s->regs[SDIO_INT / 4] |= SDIO_INT_CMD_END;
    if (ctl & SDIO_SD_CTL_DTRFEN) {
        s->regs[SDIO_INT / 4] |= SDIO_INT_DATA_END;
    }
    if (ctl & SDIO_SD_CTL_BUSYDETEN) {
        s->regs[SDIO_INT / 4] |= SDIO_INT_BUSY_END;
    }
    msc313_sdio_update_irq(s);
}

static uint64_t msc313_sdio_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313SdioState *s = opaque;

    if (addr >= SDIO_FIFO && addr < SDIO_FIFO_END) {
        unsigned int b = (addr - SDIO_FIFO) / 2;

        return s->fifo[b] | (s->fifo[b + 1] << 8);
    }
    return s->regs[addr / 4];
}

static void msc313_sdio_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    Msc313SdioState *s = opaque;

    if (addr >= SDIO_FIFO && addr < SDIO_FIFO_END) {
        unsigned int b = (addr - SDIO_FIFO) / 2;

        s->fifo[b] = val & 0xff;
        s->fifo[b + 1] = (val >> 8) & 0xff;
        return;
    }

    switch (addr) {
    case SDIO_INT:
        s->regs[SDIO_INT / 4] &= ~(uint16_t)val;    /* write-1-to-clear */
        msc313_sdio_update_irq(s);
        break;
    case SDIO_INTMASK:
        s->regs[SDIO_INTMASK / 4] = val;
        msc313_sdio_update_irq(s);
        break;
    case SDIO_RST:
        /* nrst low -> reset status reads 0x7; nrst high -> 0 (reset done). */
        s->regs[SDIO_RST / 4] = (val & SDIO_RST_NRST) ? SDIO_RST_NRST
                                                      : (0x7 << 1);
        break;
    case SDIO_SD_CTL:
        s->regs[SDIO_SD_CTL / 4] = val;
        if (val & SDIO_SD_CTL_JOBSTART) {
            msc313_sdio_run(s);
        }
        break;
    default:
        s->regs[addr / 4] = val;
        break;
    }
}

static const MemoryRegionOps msc313_sdio_ops = {
    .read = msc313_sdio_read,
    .write = msc313_sdio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void msc313_sdio_reset_hold(Object *obj, ResetType type)
{
    Msc313SdioState *s = MSC313_SDIO(obj);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->fifo, 0, sizeof(s->fifo));
    s->regs[SDIO_RST / 4] = SDIO_RST_NRST;
    s->regs[SDIO_SD_STS / 4] = SDIO_SD_STS_D0;
    qemu_set_irq(s->irq, 0);
}

static void msc313_sdio_realize(DeviceState *dev, Error **errp)
{
    Msc313SdioState *s = MSC313_SDIO(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313_sdio_ops, s,
                          "mstar.sdio", MSTAR_SDIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_MSC313_SDIO_BUS, dev, "sd-bus");
}

static void msc313_sdio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313_sdio_realize;
    rc->phases.hold = msc313_sdio_reset_hold;
}

static const TypeInfo mstar_fcie_types[] = {
    {
        .name           = TYPE_MSC313_SDIO,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313SdioState),
        .class_init     = msc313_sdio_class_init,
    },
    {
        .name           = TYPE_MSC313_SDIO_BUS,
        .parent         = TYPE_SD_BUS,
        .instance_size  = sizeof(SDBus),
    },
};

DEFINE_TYPES(mstar_fcie_types)
