/*
 * MStar/SigmaStar FCIE SD/MMC host controller
 *
 * The "sdio"/FCIE host controller (mstar,msc313-sdio). Register map
 * from the mainline driver drivers/mmc/host/mstar-fcie.c: a bank of
 * 16-bit registers plus a small command/response FIFO. The guest
 * loads a command into the FIFO, programs SD_CTL and fires the job;
 * the model runs the request on a QEMU SD card (over the standard SD
 * bus) and posts the completion. Data moves by a DMA engine that
 * either transfers one contiguous run or walks an ADMA descriptor
 * list.
 *
 * The vendor driver resets the controller before use and polls the
 * reset register: with the register unmodelled it reported
 * "IP Reset Switch Low Fail" and retried forever.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "hw/arm/mstarv7.h"
#include "hw/sd/mstar_fcie.h"

/* Register map (mainline mstar-fcie) */
#define FCIE_INT             0x00
#define FCIE_INT_DATA_END    (1 << 0)
#define FCIE_INT_CMD_END     (1 << 1)
#define FCIE_INT_BUSY_END    (1 << 4)
#define FCIE_INT_R2N_RDY     (1 << 5)
#define FCIE_INTMASK         0x04
#define FCIE_DMA_ADDR_L      0x0c
#define FCIE_DMA_ADDR_H      0x10
#define FCIE_DMA_LEN_L       0x14
#define FCIE_DMA_LEN_H       0x18
#define FCIE_SD_CTL          0x30
#define FCIE_SD_CTL_RSPEN    (1 << 1)
#define FCIE_SD_CTL_CMDEN    (1 << 2)
#define FCIE_SD_CTL_DTRFEN   (1 << 3)
#define FCIE_SD_CTL_JOBDIR   (1 << 4)    /* 0 = read (card->mem), 1 = write */
#define FCIE_SD_CTL_ADMAEN   (1 << 5)
#define FCIE_SD_CTL_JOBSTART (1 << 6)
#define FCIE_SD_CTL_BUSYDETEN (1 << 8)
#define FCIE_SD_STS          0x34
#define FCIE_SD_STS_NORSP    (1 << 3)
#define FCIE_SD_STS_D0       (1 << 8)
#define FCIE_SD_STS_ERRMASK  0x3f        /* CRC/timeout/no-response, W1C */
#define FCIE_FIFO            0x80
#define FCIE_FIFO_END        0xc0
#define FCIE_RST             0xfc
#define FCIE_RST_NRST        (1 << 0)

/* DMA uses bus addresses; the MIU dma-ranges maps bus 0 to DRAM */
static hwaddr mstar_fcie_dma_phys(uint32_t bus_addr)
{
    return MSTARV7_MIU0_BASE + bus_addr;
}

static void mstar_fcie_update_irq(MStarFcieState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[FCIE_INT / 4] & s->regs[FCIE_INTMASK / 4]));
}

static void mstar_fcie_xfer_one(MStarFcieState *s, hwaddr phys,
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

static void mstar_fcie_data_xfer(MStarFcieState *s, bool write)
{
    uint32_t dma = s->regs[FCIE_DMA_ADDR_L / 4] |
                   ((uint32_t)s->regs[FCIE_DMA_ADDR_H / 4] << 16);

    if (s->regs[FCIE_SD_CTL / 4] & FCIE_SD_CTL_ADMAEN) {
        /* ADMA: DMA_ADDR points at a list of 16-byte descriptors */
        hwaddr desc = mstar_fcie_dma_phys(dma);
        unsigned int guard = 0;

        do {
            uint32_t ctrl = address_space_ldl_le(&address_space_memory, desc,
                                                 MEMTXATTRS_UNSPECIFIED, NULL);
            uint32_t daddr = address_space_ldl_le(&address_space_memory,
                                    desc + 4, MEMTXATTRS_UNSPECIFIED, NULL);
            uint32_t dlen = address_space_ldl_le(&address_space_memory,
                                    desc + 8, MEMTXATTRS_UNSPECIFIED, NULL);

            mstar_fcie_xfer_one(s, mstar_fcie_dma_phys(daddr), dlen, write);
            if (ctrl & 1) {     /* ADMA_DESC_CTRL_END */
                break;
            }
            desc += 16;
        } while (++guard < 4096);
    } else {
        uint32_t len = s->regs[FCIE_DMA_LEN_L / 4] |
                       ((uint32_t)s->regs[FCIE_DMA_LEN_H / 4] << 16);

        mstar_fcie_xfer_one(s, mstar_fcie_dma_phys(dma), len, write);
    }
}

static void mstar_fcie_run(MStarFcieState *s)
{
    uint16_t ctl = s->regs[FCIE_SD_CTL / 4];
    bool docmd = ctl & FCIE_SD_CTL_CMDEN;
    uint8_t resp[16];
    int rlen = 0;

    /*
     * A job may carry a command, data, or both. Reads send the
     * command with the first data block (cmd_en + dtrf_en together);
     * writes send a command-only job first, then a data-only job with
     * cmd_en clear - so only touch the card's command state when
     * cmd_en is set, otherwise the FIFO still holds the last response.
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

    /* job start is self-clearing; data line idle */
    s->regs[FCIE_SD_CTL / 4] = ctl & ~FCIE_SD_CTL_JOBSTART;
    s->regs[FCIE_SD_STS / 4] = FCIE_SD_STS_D0;

    if (ctl & FCIE_SD_CTL_DTRFEN) {
        mstar_fcie_data_xfer(s, ctl & FCIE_SD_CTL_JOBDIR);
        /*
         * The DMA engine has a block count, so the controller
         * auto-issues a STOP for the open-ended multi-block commands
         * (the driver leans on this rather than sending its own CMD12).
         */
        if (s->last_cmd == 18 || s->last_cmd == 25) {
            SDRequest stop = { .cmd = 12 };
            uint8_t sr[16];

            sdbus_do_command(&s->sdbus, &stop, sr, sizeof(sr));
        }
    }

    /*
     * Rebuild the FIFO with the response so the guest can read it
     * back. The driver strips the first byte (command index for short
     * responses, an 0x3f header for R2) and takes the rest big-endian.
     */
    if (docmd) {
        memset(s->fifo, 0, sizeof(s->fifo));
        if (rlen == 4) {
            s->fifo[0] = s->last_cmd;
            memcpy(&s->fifo[1], resp, 4);
        } else if (rlen == 16) {
            s->fifo[0] = 0x3f;
            memcpy(&s->fifo[1], resp, 16);
        } else if (s->last_cmd == 12) {
            /* A STOP for a transfer already auto-stopped: report ready */
            s->fifo[0] = 12;
            s->fifo[1] = 0x00;
            s->fifo[2] = 0x00;
            s->fifo[3] = 0x09;
            s->fifo[4] = 0x00;
        } else if (ctl & FCIE_SD_CTL_RSPEN) {
            s->regs[FCIE_SD_STS / 4] |= FCIE_SD_STS_NORSP;
        }
    }

    s->regs[FCIE_INT / 4] |= FCIE_INT_CMD_END;
    if (ctl & FCIE_SD_CTL_DTRFEN) {
        s->regs[FCIE_INT / 4] |= FCIE_INT_DATA_END;
    }
    if (ctl & FCIE_SD_CTL_BUSYDETEN) {
        s->regs[FCIE_INT / 4] |= FCIE_INT_BUSY_END;
    }
    mstar_fcie_update_irq(s);
}

static uint64_t mstar_fcie_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarFcieState *s = MSTAR_FCIE(opaque);
    uint16_t regval;
    unsigned int byteoff;

    if (addr >= FCIE_FIFO && addr < FCIE_FIFO_END) {
        /*
         * The command/response FIFO is 16-bit words at a 4 byte stride,
         * each holding two bytes. The mainline driver reads whole
         * words; the vendor SDK reads the response a byte at a time, so
         * byte accesses must be served the right half of the word.
         */
        unsigned int word = (addr - FCIE_FIFO) / 4;

        regval = s->fifo[word * 2] | (s->fifo[word * 2 + 1] << 8);
        byteoff = (addr - FCIE_FIFO) & 3;
    } else {
        regval = s->regs[addr / 4];
        byteoff = addr & 3;
    }

    return (regval >> (8 * byteoff)) & (size == 1 ? 0xff : 0xffff);
}

static void mstar_fcie_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MStarFcieState *s = MSTAR_FCIE(opaque);

    if (addr >= FCIE_FIFO && addr < FCIE_FIFO_END) {
        unsigned int b = (addr - FCIE_FIFO) / 2;

        s->fifo[b] = val & 0xff;
        s->fifo[b + 1] = (val >> 8) & 0xff;
        return;
    }

    switch (addr) {
    case FCIE_INT:
        s->regs[FCIE_INT / 4] &= ~(uint16_t)val;    /* write-1-to-clear */
        mstar_fcie_update_irq(s);
        break;
    case FCIE_INTMASK:
        s->regs[FCIE_INTMASK / 4] = val;
        mstar_fcie_update_irq(s);
        break;
    case FCIE_RST:
        /* nrst high -> reset done (reads NRST); nrst low -> reset active */
        s->regs[FCIE_RST / 4] = (val & FCIE_RST_NRST) ? FCIE_RST_NRST
                                                      : (0x7 << 1);
        break;
    case FCIE_SD_STS:
        /* error bits are write-1-to-clear; D0 is live status */
        s->regs[FCIE_SD_STS / 4] &= ~((uint16_t)val & FCIE_SD_STS_ERRMASK);
        break;
    case FCIE_SD_CTL:
        s->regs[FCIE_SD_CTL / 4] = val;
        if (val & FCIE_SD_CTL_JOBSTART) {
            mstar_fcie_run(s);
        }
        break;
    default:
        s->regs[addr / 4] = val;
        break;
    }
}

static const MemoryRegionOps mstar_fcie_ops = {
    .read = mstar_fcie_read,
    .write = mstar_fcie_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,     /* vendor driver reads the response as bytes */
    .valid.max_access_size = 4,
};

static void mstar_fcie_reset_hold(Object *obj, ResetType type)
{
    MStarFcieState *s = MSTAR_FCIE(obj);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->fifo, 0, sizeof(s->fifo));
    s->regs[FCIE_RST / 4] = FCIE_RST_NRST;
    s->regs[FCIE_SD_STS / 4] = FCIE_SD_STS_D0;
    qemu_set_irq(s->irq, 0);
}

static void mstar_fcie_realize(DeviceState *dev, Error **errp)
{
    MStarFcieState *s = MSTAR_FCIE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_fcie_ops, s,
                          TYPE_MSTAR_FCIE, MSTAR_FCIE_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_MSTAR_FCIE_BUS, dev, "sd-bus");
}

static void mstar_fcie_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_fcie_realize;
    rc->phases.hold = mstar_fcie_reset_hold;
}

static const TypeInfo mstar_fcie_types[] = {
    {
        .name           = TYPE_MSTAR_FCIE,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarFcieState),
        .class_init     = mstar_fcie_class_init,
    },
    {
        .name           = TYPE_MSTAR_FCIE_BUS,
        .parent         = TYPE_SD_BUS,
        .instance_size  = sizeof(SDBus),
    },
};

DEFINE_TYPES(mstar_fcie_types)
