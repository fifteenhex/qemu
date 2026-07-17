/*
 * MStar/SigmaStar BDMA engine
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
#include "hw/arm/mstar.h"

/* ------------------------------------------------------------------ bdma */

#define BDMA_CTRL           0x00
#define BDMA_CTRL_TRIG      (1 << 0)
#define BDMA_STATUS         0x04
#define BDMA_STATUS_BUSY    (1 << 1)
#define BDMA_STATUS_INT     (1 << 2)
#define BDMA_STATUS_DONE    (1 << 3)
#define BDMA_STATUS_RESULT0 (1 << 4)
#define BDMA_STATUS_MASK    0x1f
#define BDMA_CONFIG         0x08
#define BDMA_MISC           0x0c
#define BDMA_MISC_INT_EN    (1 << 1)
#define BDMA_SRC_ADDR_L     0x10
#define BDMA_SRC_ADDR_H     0x14
#define BDMA_DST_ADDR_L     0x18
#define BDMA_DST_ADDR_H     0x1c
#define BDMA_SIZE_L         0x20
#define BDMA_SIZE_H         0x24

/*
 * Slave (port) IDs in the CONFIG register's src/dst selects. The MIU/IMI
 * memory port is reachable through either of two DMA channels - id 0
 * (MIU_IMI_CH0) and id 1 (MIU_IMI_CH1) - both of which address the same memory
 * (which MIU/IMI is picked by the replace_miu field). The vendor kernel's flash
 * read uses CH1 for the destination, so both must translate as memory.
 */
#define BDMA_SLAVE_MIU      0x0
#define BDMA_SLAVE_MIU_CH1  0x1
#define BDMA_SLAVE_QSPI     0x5

/* MISC bits 12-13 select which memory the MIU port addresses. 0 is DRAM; the
 * boot ROM uses the on-chip SRAM (IMI) here to load the IPL before DRAM is up
 * (see "boot rom leaves this with IMI selected" in the Linux bdma driver). */
#define BDMA_MISC_MIU_SEL(misc) (((misc) >> 12) & 0x3)
#define BDMA_MIU_SEL_IMI    0x2

static hwaddr msc313_bdma_translate(unsigned int slave_id, unsigned int miu_sel,
                                    uint32_t addr)
{
    switch (slave_id) {
    case BDMA_SLAVE_MIU:
    case BDMA_SLAVE_MIU_CH1:
        /*
         * mercury5's mask ROM programs an absolute CPU address as the MIU
         * endpoint (IMI at 0xa0000000, or DRAM at 0x20000000+) and leaves
         * miu_sel clear, whereas msc313's ROM/kernel program a MIU-relative
         * offset and pick IMI vs DRAM via miu_sel. A relative offset is always
         * below DRAM_BASE (DRAM is at most 128MB), so treat anything at/above
         * it as an already-absolute address.
         */
        if (addr >= MSTAR_DRAM_BASE) {
            return addr;
        }
        if (miu_sel == BDMA_MIU_SEL_IMI) {
            return MSTAR_IMI_BASE + addr;
        }
        /* dma-ranges maps bus address 0 to DRAM at 0x20000000. */
        return MSTAR_DRAM_BASE + addr;
    case BDMA_SLAVE_QSPI:
        /*
         * The QSPI slave streams the flash through the ISP XIP window. The
         * address may be given flash-relative (the boot ROM and the kernel)
         * or as an absolute XIP address (the vendor IPL loading IPL_CUST);
         * mask to the window size so both map into the window once.
         */
        return MSTAR_ISP_XIP_BASE + (addr & (MSTAR_ISP_XIP_SIZE - 1));
    default:
        return addr;
    }
}

static void msc313_bdma_update_irq(Msc313BdmaState *s, unsigned int ch)
{
    Msc313BdmaChan *c = &s->chans[ch];
    bool active = (c->regs[BDMA_STATUS / 4] & BDMA_STATUS_INT) &&
                  (c->regs[BDMA_MISC / 4] & BDMA_MISC_INT_EN);

    qemu_set_irq(c->irq, active);
}

static void msc313_bdma_trigger(Msc313BdmaState *s, unsigned int ch)
{
    Msc313BdmaChan *c = &s->chans[ch];
    uint16_t *r = c->regs;
    unsigned int src_id = r[BDMA_CONFIG / 4] & 0xf;
    unsigned int dst_id = (r[BDMA_CONFIG / 4] >> 8) & 0xf;
    uint32_t src = r[BDMA_SRC_ADDR_L / 4] | ((uint32_t)r[BDMA_SRC_ADDR_H / 4] << 16);
    uint32_t dst = r[BDMA_DST_ADDR_L / 4] | ((uint32_t)r[BDMA_DST_ADDR_H / 4] << 16);
    uint32_t len = r[BDMA_SIZE_L / 4] | ((uint32_t)r[BDMA_SIZE_H / 4] << 16);
    unsigned int miu_sel = BDMA_MISC_MIU_SEL(r[BDMA_MISC / 4]);
    hwaddr src_phys = msc313_bdma_translate(src_id, miu_sel, src);
    hwaddr dst_phys = msc313_bdma_translate(dst_id, miu_sel, dst);
    uint8_t buf[1024];

    while (len) {
        uint32_t chunk = len < sizeof(buf) ? len : sizeof(buf);

        address_space_read(&address_space_memory, src_phys,
                           MEMTXATTRS_UNSPECIFIED, buf, chunk);
        address_space_write(&address_space_memory, dst_phys,
                            MEMTXATTRS_UNSPECIFIED, buf, chunk);
        src_phys += chunk;
        dst_phys += chunk;
        len -= chunk;
    }

    /* Completion: clear busy, latch the done/int/result flags, interrupt. */
    r[BDMA_STATUS / 4] = (r[BDMA_STATUS / 4] & ~BDMA_STATUS_BUSY) |
                         BDMA_STATUS_INT | BDMA_STATUS_DONE | BDMA_STATUS_RESULT0;
    msc313_bdma_update_irq(s, ch);
}

static uint64_t msc313_bdma_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313BdmaState *s = opaque;
    unsigned int ch = addr / MSTAR_BDMA_CHAN_SIZE;
    unsigned int off = addr % MSTAR_BDMA_CHAN_SIZE;

    if (ch >= MSTAR_BDMA_NUM_CHANNELS) {
        return 0;
    }
    return s->chans[ch].regs[off / 4];
}

static void msc313_bdma_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    Msc313BdmaState *s = opaque;
    unsigned int ch = addr / MSTAR_BDMA_CHAN_SIZE;
    unsigned int off = addr % MSTAR_BDMA_CHAN_SIZE;
    Msc313BdmaChan *c;

    if (ch >= MSTAR_BDMA_NUM_CHANNELS) {
        return;
    }
    c = &s->chans[ch];

    switch (off) {
    case BDMA_CTRL:
        /* The trigger bit self-clears; bit4 (stop) is a no-op for us. */
        c->regs[BDMA_CTRL / 4] = val & ~BDMA_CTRL_TRIG;
        if (val & BDMA_CTRL_TRIG) {
            msc313_bdma_trigger(s, ch);
        }
        break;
    case BDMA_STATUS:
        /* Status flags are write-1-to-clear. */
        c->regs[BDMA_STATUS / 4] &= ~(val & BDMA_STATUS_MASK);
        msc313_bdma_update_irq(s, ch);
        break;
    case BDMA_MISC:
        c->regs[BDMA_MISC / 4] = val;
        msc313_bdma_update_irq(s, ch);
        break;
    default:
        c->regs[off / 4] = val;
        break;
    }
}

static const MemoryRegionOps msc313_bdma_ops = {
    .read = msc313_bdma_read,
    .write = msc313_bdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void msc313_bdma_reset_hold(Object *obj, ResetType type)
{
    Msc313BdmaState *s = MSC313_BDMA(obj);
    unsigned int i;

    for (i = 0; i < MSTAR_BDMA_NUM_CHANNELS; i++) {
        memset(s->chans[i].regs, 0, sizeof(s->chans[i].regs));
        qemu_set_irq(s->chans[i].irq, 0);
    }
}

static void msc313_bdma_realize(DeviceState *dev, Error **errp)
{
    Msc313BdmaState *s = MSC313_BDMA(dev);
    unsigned int i;

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313_bdma_ops, s,
                          "mstar.bdma", MSTAR_BDMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (i = 0; i < MSTAR_BDMA_NUM_CHANNELS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->chans[i].irq);
    }
}

static void msc313_bdma_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313_bdma_realize;
    rc->phases.hold = msc313_bdma_reset_hold;
}

static const TypeInfo mstar_bdma_types[] = {
    {
        .name           = TYPE_MSC313_BDMA,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313BdmaState),
        .class_init     = msc313_bdma_class_init,
    },
};

DEFINE_TYPES(mstar_bdma_types)
