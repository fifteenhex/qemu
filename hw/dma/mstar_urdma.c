/*
 * MStar/SigmaStar URDMA - the FUART's UART RX/TX ring-buffer DMA engine
 * (urdma@1f220600, "mstar,msc313-urdma").
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The URDMA sits between the "fast UART" (FUART, serial@220400) and DRAM: it
 * streams received bytes into an RX ring and drains a TX ring out of the FUART,
 * so the CPU can move UART traffic without per-byte PIO. The register layout and
 * semantics here follow the mainline 6.5 driver (drivers/dma/mstar/
 * mstar-msc313-urdma.c): 16-bit registers on a 4-byte stride, a REG_CTRL with
 * per-direction enable / sw-reset / busy bits, byte-granular ring pointers, and
 * a REG_STATUS with write-1-to-clear interrupt-latch bits.
 *
 * NB the dashcam RTOS we boot drives the FUART purely as a 16550 (PIO) and never
 * enables the URDMA, so in practice this block stays idle; it is modelled so the
 * bank is not an unmapped hole and so the Linux driver would initialise/transfer
 * cleanly if its DT node were enabled. The ring buffers live in DRAM at a MIU
 * bus address (DRAM offset), reconstructed as MSTAR_DRAM_BASE + busaddr.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "chardev/char-fe.h"
#include "system/dma.h"
#include "system/address-spaces.h"
#include "hw/arm/mstar.h"

/* REG_CTRL (0x00) bit fields (see the 6.5 driver's reg_field table). */
#define URDMA_CTRL_SW_RST       (1 << 0)
#define URDMA_CTRL_MODE         (1 << 1)   /* global urdma_mode enable */
#define URDMA_CTRL_TX_EN        (1 << 2)
#define URDMA_CTRL_RX_EN        (1 << 3)
#define URDMA_CTRL_TX_SW_RST    (1 << 6)
#define URDMA_CTRL_RX_SW_RST    (1 << 7)
#define URDMA_CTRL_TX_BUSY      (1 << 12)  /* read-only, we always report idle */
#define URDMA_CTRL_RX_BUSY      (1 << 13)

/* REG_STATUS (0x34) bit fields. */
#define URDMA_ST_RX_INTR_CLR    (1 << 0)   /* W1C */
#define URDMA_ST_RX_INTR_EN1    (1 << 1)
#define URDMA_ST_RX_INTR_EN2    (1 << 2)
#define URDMA_ST_RX_INTR1       (1 << 4)
#define URDMA_ST_RX_INTR2       (1 << 5)
#define URDMA_ST_RX_MCU_INTR    (1 << 7)
#define URDMA_ST_TX_INTR_CLR    (1 << 8)   /* W1C */
#define URDMA_ST_TX_INTR_EN     (1 << 9)
#define URDMA_ST_TX_MCU_INTR    (1 << 15)

#define REG_CTRL            0x00
#define REG_INTR_THRESHOLD  0x04
#define REG_TX_BUF_BASE_H   0x08
#define REG_TX_BUF_BASE_L   0x0c
#define REG_TX_BUF_SIZE     0x10
#define REG_TX_BUF_RPTR     0x14
#define REG_TX_BUF_WPTR     0x18
#define REG_TX_TIMEOUT      0x1c
#define REG_RX_BUF_BASE_H   0x20
#define REG_RX_BUF_BASE_L   0x24
#define REG_RX_BUF_SIZE     0x28
#define REG_RX_BUF_WPTR     0x2c
#define REG_RX_TIMEOUT      0x30
#define REG_STATUS          0x34

static void mstar_urdma_update_irq(MstarUrdmaState *s)
{
    bool rx = (s->status & URDMA_ST_RX_MCU_INTR) &&
              (s->status & (URDMA_ST_RX_INTR_EN1 | URDMA_ST_RX_INTR_EN2));
    bool tx = (s->status & URDMA_ST_TX_MCU_INTR) &&
              (s->status & URDMA_ST_TX_INTR_EN);

    qemu_set_irq(s->irq, rx || tx);
}

static hwaddr mstar_urdma_tx_base(MstarUrdmaState *s)
{
    /* MIU bus address (DRAM offset) -> physical. */
    return MSTAR_DRAM_BASE + (((uint32_t)s->tx_base_h << 16) | s->tx_base_l);
}

static hwaddr mstar_urdma_rx_base(MstarUrdmaState *s)
{
    return MSTAR_DRAM_BASE + (((uint32_t)s->rx_base_h << 16) | s->rx_base_l);
}

/* Ring size in bytes: the SIZE register counts 8-byte units. */
static uint32_t mstar_urdma_ring_bytes(uint16_t sz)
{
    return sz ? (uint32_t)sz * 8 : 4096;
}

/*
 * A write to TX_BUF_WPTR with a running TX channel starts the transfer: drain
 * bytes [rptr, wptr) out of the ring to the UART backend, advance rptr to wptr,
 * and raise the TX "done" (mcu) interrupt.
 */
static void mstar_urdma_tx_kick(MstarUrdmaState *s)
{
    uint32_t ring = mstar_urdma_ring_bytes(s->tx_size);
    hwaddr base = mstar_urdma_tx_base(s);
    uint32_t rptr = s->tx_rptr % ring;
    uint32_t wptr = s->tx_wptr % ring;

    if (!(s->ctrl & URDMA_CTRL_MODE) || !(s->ctrl & URDMA_CTRL_TX_EN)) {
        return;
    }

    while (rptr != wptr) {
        uint8_t b;

        dma_memory_read(&address_space_memory, base + rptr, &b, 1,
                        MEMTXATTRS_UNSPECIFIED);
        /* qemu_chr_fe_write_all tolerates a disconnected (NULL) backend. */
        qemu_chr_fe_write_all(&s->chr, &b, 1);
        rptr = (rptr + 1) % ring;
    }

    s->tx_rptr = wptr;
    s->status |= URDMA_ST_TX_MCU_INTR;
    mstar_urdma_update_irq(s);
}

/*
 * Backend RX: latch one byte, DMA it into the RX ring at the current wptr,
 * advance wptr, and raise the RX "done" interrupt. Only active once the RX
 * channel has been enabled with a configured ring.
 */
static int mstar_urdma_can_receive(void *opaque)
{
    MstarUrdmaState *s = opaque;

    return (s->ctrl & URDMA_CTRL_MODE) && (s->ctrl & URDMA_CTRL_RX_EN) &&
           !s->rx_pending;
}

static void mstar_urdma_receive(void *opaque, const uint8_t *buf, int size)
{
    MstarUrdmaState *s = opaque;
    uint32_t ring = mstar_urdma_ring_bytes(s->rx_size);
    hwaddr base = mstar_urdma_rx_base(s);
    int i;

    for (i = 0; i < size; i++) {
        uint32_t wptr = s->rx_wptr % ring;

        dma_memory_write(&address_space_memory, base + wptr, &buf[i], 1,
                         MEMTXATTRS_UNSPECIFIED);
        s->rx_wptr = (wptr + 1) % ring;
    }
    if (size > 0) {
        s->status |= URDMA_ST_RX_MCU_INTR | URDMA_ST_RX_INTR1;
        mstar_urdma_update_irq(s);
    }
}

static uint64_t mstar_urdma_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarUrdmaState *s = opaque;

    switch (addr) {
    case REG_CTRL:
        /* sw-reset bits self-clear; busy bits always read idle. */
        return s->ctrl & ~(URDMA_CTRL_SW_RST | URDMA_CTRL_TX_SW_RST |
                           URDMA_CTRL_RX_SW_RST | URDMA_CTRL_TX_BUSY |
                           URDMA_CTRL_RX_BUSY);
    case REG_INTR_THRESHOLD: return s->threshold;
    case REG_TX_BUF_BASE_H:  return s->tx_base_h;
    case REG_TX_BUF_BASE_L:  return s->tx_base_l;
    case REG_TX_BUF_SIZE:    return s->tx_size;
    case REG_TX_BUF_RPTR:    return s->tx_rptr;
    case REG_TX_BUF_WPTR:    return s->tx_wptr;
    case REG_TX_TIMEOUT:     return s->tx_timeout;
    case REG_RX_BUF_BASE_H:  return s->rx_base_h;
    case REG_RX_BUF_BASE_L:  return s->rx_base_l;
    case REG_RX_BUF_SIZE:    return s->rx_size;
    case REG_RX_BUF_WPTR:    return s->rx_wptr;
    case REG_RX_TIMEOUT:     return s->rx_timeout;
    case REG_STATUS:         return s->status;
    }
    return 0;
}

static void mstar_urdma_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MstarUrdmaState *s = opaque;
    uint16_t v = val;

    switch (addr) {
    case REG_CTRL:
        s->ctrl = v;
        if (v & (URDMA_CTRL_SW_RST | URDMA_CTRL_TX_SW_RST |
                 URDMA_CTRL_RX_SW_RST)) {
            /* Reset clears the pointers/status; sw-reset bits self-clear. */
            s->tx_rptr = s->tx_wptr = 0;
            s->rx_wptr = 0;
            s->status = 0;
            s->ctrl &= ~(URDMA_CTRL_SW_RST | URDMA_CTRL_TX_SW_RST |
                         URDMA_CTRL_RX_SW_RST);
            mstar_urdma_update_irq(s);
        }
        qemu_chr_fe_accept_input(&s->chr);
        break;
    case REG_INTR_THRESHOLD: s->threshold = v; break;
    case REG_TX_BUF_BASE_H:  s->tx_base_h = v; break;
    case REG_TX_BUF_BASE_L:  s->tx_base_l = v; break;
    case REG_TX_BUF_SIZE:    s->tx_size = v; break;
    case REG_TX_BUF_RPTR:    s->tx_rptr = v; break;
    case REG_TX_BUF_WPTR:
        s->tx_wptr = v;
        mstar_urdma_tx_kick(s);
        break;
    case REG_TX_TIMEOUT:     s->tx_timeout = v; break;
    case REG_RX_BUF_BASE_H:  s->rx_base_h = v; break;
    case REG_RX_BUF_BASE_L:  s->rx_base_l = v; break;
    case REG_RX_BUF_SIZE:    s->rx_size = v; break;
    case REG_RX_BUF_WPTR:    s->rx_wptr = v; break;
    case REG_RX_TIMEOUT:     s->rx_timeout = v; break;
    case REG_STATUS:
        /* bits 0/8 are write-1-to-clear ack bits; the rest are R/W enables. */
        if (v & URDMA_ST_RX_INTR_CLR) {
            s->status &= ~(URDMA_ST_RX_MCU_INTR | URDMA_ST_RX_INTR1 |
                           URDMA_ST_RX_INTR2);
        }
        if (v & URDMA_ST_TX_INTR_CLR) {
            s->status &= ~URDMA_ST_TX_MCU_INTR;
        }
        s->status = (s->status & ~(URDMA_ST_RX_INTR_EN1 | URDMA_ST_RX_INTR_EN2 |
                                   URDMA_ST_TX_INTR_EN)) |
                    (v & (URDMA_ST_RX_INTR_EN1 | URDMA_ST_RX_INTR_EN2 |
                          URDMA_ST_TX_INTR_EN));
        mstar_urdma_update_irq(s);
        qemu_chr_fe_accept_input(&s->chr);
        break;
    }
}

static const MemoryRegionOps mstar_urdma_ops = {
    .read = mstar_urdma_read,
    .write = mstar_urdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_urdma_reset(DeviceState *dev)
{
    MstarUrdmaState *s = MSTAR_URDMA(dev);

    s->ctrl = 0;
    s->threshold = 0;
    s->tx_base_h = s->tx_base_l = s->tx_size = 0;
    s->tx_rptr = s->tx_wptr = s->tx_timeout = 0;
    s->rx_base_h = s->rx_base_l = s->rx_size = 0;
    s->rx_wptr = s->rx_timeout = 0;
    s->status = 0;
    s->rx_pending = 0;
    qemu_set_irq(s->irq, 0);
}

static void mstar_urdma_realize(DeviceState *dev, Error **errp)
{
    MstarUrdmaState *s = MSTAR_URDMA(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mstar_urdma_ops, s,
                          TYPE_MSTAR_URDMA, MSTAR_URDMA_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    qemu_chr_fe_set_handlers(&s->chr, mstar_urdma_can_receive,
                             mstar_urdma_receive, NULL, NULL, s, NULL, true);
}

static const VMStateDescription vmstate_mstar_urdma = {
    .name = TYPE_MSTAR_URDMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(ctrl, MstarUrdmaState),
        VMSTATE_UINT16(threshold, MstarUrdmaState),
        VMSTATE_UINT16(tx_base_h, MstarUrdmaState),
        VMSTATE_UINT16(tx_base_l, MstarUrdmaState),
        VMSTATE_UINT16(tx_size, MstarUrdmaState),
        VMSTATE_UINT16(tx_rptr, MstarUrdmaState),
        VMSTATE_UINT16(tx_wptr, MstarUrdmaState),
        VMSTATE_UINT16(tx_timeout, MstarUrdmaState),
        VMSTATE_UINT16(rx_base_h, MstarUrdmaState),
        VMSTATE_UINT16(rx_base_l, MstarUrdmaState),
        VMSTATE_UINT16(rx_size, MstarUrdmaState),
        VMSTATE_UINT16(rx_wptr, MstarUrdmaState),
        VMSTATE_UINT16(rx_timeout, MstarUrdmaState),
        VMSTATE_UINT16(status, MstarUrdmaState),
        VMSTATE_UINT8(rx_pending, MstarUrdmaState),
        VMSTATE_UINT8(rx_byte, MstarUrdmaState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mstar_urdma_properties[] = {
    DEFINE_PROP_CHR("chardev", MstarUrdmaState, chr),
};

static void mstar_urdma_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mstar_urdma_realize;
    device_class_set_legacy_reset(dc, mstar_urdma_reset);
    dc->vmsd = &vmstate_mstar_urdma;
    device_class_set_props(dc, mstar_urdma_properties);
}

static const TypeInfo mstar_urdma_types[] = {
    {
        .name           = TYPE_MSTAR_URDMA,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MstarUrdmaState),
        .class_init     = mstar_urdma_class_init,
    },
};

DEFINE_TYPES(mstar_urdma_types)
