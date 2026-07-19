/*
 * MStar/SigmaStar BDMA engine
 *
 * A simple two channel byte DMA engine that moves data between the
 * memory ports (DRAM or IMI SRAM) and peripherals like the SPI NOR
 * QSPI port. The boot ROM uses channel 0 to copy the IPL from flash
 * into IMI. Register layout and port ids follow the previous branch
 * and the mainline Linux bdma driver; the parts the boot ROM
 * exercises (QSPI source, IMI destination via the MISC select, the
 * done flag) behave as documented there.
 *
 * Transfers complete synchronously inside the trigger write. The
 * per channel interrupt is not wired up anywhere yet.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/core/irq.h"
#include "hw/arm/mstarv7.h"
#include "hw/dma/mstar_bdma.h"

/* Register byte offsets within a channel (16-bit regs, 4 byte stride) */
#define BDMA_CTRL           0x00
#define BDMA_CTRL_TRIG      (1 << 0)
#define BDMA_STATUS         0x04    /* write-1-to-clear */
#define BDMA_STATUS_BUSY    (1 << 1)
#define BDMA_STATUS_INT     (1 << 2)
#define BDMA_STATUS_DONE    (1 << 3)
#define BDMA_STATUS_RESULT0 (1 << 4)
#define BDMA_STATUS_MASK    0x1f
#define BDMA_CONFIG         0x08    /* [3:0] source port, [11:8] dest port */
#define BDMA_MISC           0x0c
#define BDMA_MISC_INT_EN    (1 << 1)
#define BDMA_SRC_ADDR_L     0x10
#define BDMA_SRC_ADDR_H     0x14
#define BDMA_DST_ADDR_L     0x18
#define BDMA_DST_ADDR_H     0x1c
#define BDMA_SIZE_L         0x20
#define BDMA_SIZE_H         0x24

/* Port ids in CONFIG */
#define BDMA_PORT_MIU       0x0
#define BDMA_PORT_MIU_CH1   0x1
#define BDMA_PORT_QSPI      0x5

/* MISC bits 13:12 pick the memory behind the MIU port */
#define BDMA_MISC_MIU_SEL(misc) (((misc) >> 12) & 0x3)
#define BDMA_MIU_SEL_IMI    0x2

static uint32_t bdma_reg32(MStarBdmaChan *chan, unsigned int low)
{
    return chan->regs[low / 4] | ((uint32_t)chan->regs[low / 4 + 1] << 16);
}

static bool mstar_bdma_translate(unsigned int port, unsigned int miu_sel,
                                 uint32_t addr, hwaddr *phys)
{
    switch (port) {
    case BDMA_PORT_MIU:
    case BDMA_PORT_MIU_CH1:
        if (miu_sel == BDMA_MIU_SEL_IMI) {
            *phys = MSTARV7_IMI_BASE + addr;
        } else {
            /* Bus address 0 is the start of DRAM */
            *phys = MSTARV7_MIU0_BASE + addr;
        }
        return true;
    case BDMA_PORT_QSPI:
        /*
         * The QSPI port streams the flash contents; addresses may be
         * flash relative or absolute XIP addresses, so mask down to
         * the window.
         */
        *phys = MSTARV7_ISP_XIP_BASE + (addr % MSTAR_FSP_XIP_SIZE);
        return true;
    default:
        qemu_log_mask(LOG_UNIMP, "mstar-bdma: unknown port %u\n", port);
        return false;
    }
}

static void mstar_bdma_trigger(MStarBdmaChan *chan)
{
    unsigned int src_port = chan->regs[BDMA_CONFIG / 4] & 0xf;
    unsigned int dst_port = (chan->regs[BDMA_CONFIG / 4] >> 8) & 0xf;
    unsigned int miu_sel = BDMA_MISC_MIU_SEL(chan->regs[BDMA_MISC / 4]);
    uint32_t len = bdma_reg32(chan, BDMA_SIZE_L);
    hwaddr src, dst;
    uint8_t buf[1024];

    if (!mstar_bdma_translate(src_port, miu_sel,
                              bdma_reg32(chan, BDMA_SRC_ADDR_L), &src) ||
        !mstar_bdma_translate(dst_port, miu_sel,
                              bdma_reg32(chan, BDMA_DST_ADDR_L), &dst)) {
        return;
    }

    while (len) {
        uint32_t chunk = MIN(len, sizeof(buf));

        address_space_read(&address_space_memory, src,
                           MEMTXATTRS_UNSPECIFIED, buf, chunk);
        address_space_write(&address_space_memory, dst,
                            MEMTXATTRS_UNSPECIFIED, buf, chunk);
        src += chunk;
        dst += chunk;
        len -= chunk;
    }

    chan->regs[BDMA_STATUS / 4] = (chan->regs[BDMA_STATUS / 4] &
                                   ~BDMA_STATUS_BUSY) |
                                  BDMA_STATUS_INT | BDMA_STATUS_DONE |
                                  BDMA_STATUS_RESULT0;
}

static uint64_t mstar_bdma_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarBdmaState *s = MSTAR_BDMA(opaque);
    unsigned int ch = addr / MSTAR_BDMA_CHAN_SIZE;

    return s->chans[ch].regs[(addr % MSTAR_BDMA_CHAN_SIZE) / 4];
}

static void mstar_bdma_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MStarBdmaState *s = MSTAR_BDMA(opaque);
    MStarBdmaChan *chan = &s->chans[addr / MSTAR_BDMA_CHAN_SIZE];

    switch (addr % MSTAR_BDMA_CHAN_SIZE) {
    case BDMA_CTRL:
        /* The trigger bit self clears */
        chan->regs[BDMA_CTRL / 4] = val & ~BDMA_CTRL_TRIG;
        if (val & BDMA_CTRL_TRIG) {
            mstar_bdma_trigger(chan);
        }
        break;
    case BDMA_STATUS:
        chan->regs[BDMA_STATUS / 4] &= ~(val & BDMA_STATUS_MASK);
        break;
    default:
        chan->regs[(addr % MSTAR_BDMA_CHAN_SIZE) / 4] = val;
        break;
    }
}

static const MemoryRegionOps mstar_bdma_ops = {
    .read = mstar_bdma_read,
    .write = mstar_bdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_bdma_reset(DeviceState *dev)
{
    MStarBdmaState *s = MSTAR_BDMA(dev);
    int i;

    for (i = 0; i < MSTAR_BDMA_NUM_CHANNELS; i++) {
        memset(s->chans[i].regs, 0, sizeof(s->chans[i].regs));
    }
}

static void mstar_bdma_init(Object *obj)
{
    MStarBdmaState *s = MSTAR_BDMA(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &mstar_bdma_ops, s,
                          TYPE_MSTAR_BDMA, MSTAR_BDMA_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);

    for (i = 0; i < MSTAR_BDMA_NUM_CHANNELS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->chans[i].irq);
    }
}

static void mstar_bdma_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_bdma_reset);
}

static const TypeInfo mstar_bdma_types[] = {
    {
        .name           = TYPE_MSTAR_BDMA,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarBdmaState),
        .instance_init  = mstar_bdma_init,
        .class_init     = mstar_bdma_class_init,
    },
};

DEFINE_TYPES(mstar_bdma_types)
