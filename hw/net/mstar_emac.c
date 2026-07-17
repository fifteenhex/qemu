/*
 * MStar/SigmaStar "emac" 10/100 Ethernet MAC (emac@1f2a2000)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A classic Atmel MACB/EMAC (AT91RM9200 style) behind the MStar RIU bus. The
 * register interface and behaviour are taken from the mainline 6.5 macb driver
 * (drivers/net/ethernet/cadence/, msc313e_config: MACB_CAPS_MACB_IS_EMAC |
 * MACB_CAPS_MSTAR_RIU | MACB_CAPS_MSTAR_TXQ) and include/soc/mstar/riuxiu.h:
 *
 *   - Each 32-bit MACB register at byte offset G is split into two 16-bit
 *     halves accessed through the RIU: the low half at RIU byte offset 2*G,
 *     the high half at 2*G+4. riu_writel writes the low half first, so a
 *     register's side effect is triggered when its high half is written.
 *   - Transmit uses the rm9200 path: write the buffer address to TAR then the
 *     length to TCR, which sends the frame. TSR reports the TX FIFO as free.
 *   - Receive uses the classic descriptor ring at RBQP: 2-word descriptors
 *     { addr, ctrl }, addr bit0 = "used" (1 = owned by host), bit1 = wrap,
 *     addr[31:2] = buffer; ctrl gets the frame length + SOF/EOF on receive.
 *   - MDIO reads (MAN register) return a permanently-linked PHY so the driver
 *     brings the link up.
 *
 * This is enough for the guest to actually send and receive packets against a
 * QEMU netdev (e.g. -nic user,model=mstar-emac,hostfwd=...), so services in a
 * camera firmware (RTSP/HTTP) can be reached from the host.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "net/net.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "exec/cpu-common.h"
#include "hw/arm/mstar.h"

/* MACB/EMAC register byte offsets (offset G in the 32-bit register space). */
#define EMAC_NCR    0x00    /* network control: RE bit2, TE bit3, MPE bit4 */
#define EMAC_NCFGR  0x04
#define EMAC_NSR    0x08    /* network status: IDLE (MDIO done) bit2 */
#define EMAC_TAR    0x0c    /* rm9200 transmit buffer address */
#define EMAC_TCR    0x10    /* rm9200 transmit length -> sends the frame */
#define EMAC_TSR    0x14    /* transmit status */
#define EMAC_RBQP   0x18    /* receive descriptor ring base */
#define EMAC_TBQP   0x1c
#define EMAC_RSR    0x20    /* receive status: BNA b0, REC b1, OVR b2 (W1C) */
#define EMAC_ISR    0x24    /* interrupt status (read-clear) */
#define EMAC_IER    0x28    /* interrupt enable */
#define EMAC_IDR    0x2c    /* interrupt disable */
#define EMAC_IMR    0x30    /* interrupt mask */
#define EMAC_MAN    0x34    /* PHY maintenance (MDIO) */
#define EMAC_SA1B   0x98
#define EMAC_SA1T   0x9c

#define NCR_RE      (1 << 2)
#define NCR_TE      (1 << 3)

#define NSR_IDLE    (1 << 2)

/* Interrupt / status bits (ISR, RSR). */
#define ISR_RCOMP       (1 << 1)
#define ISR_RXUBR       (1 << 2)
#define ISR_RM9200_TBRE (1 << 6)
#define ISR_TCOMP       (1 << 7)
#define RSR_BNA         (1 << 0)
#define RSR_REC         (1 << 1)
#define RSR_OVR         (1 << 2)

/*
 * TSR value reported to the msc313 TX-queue-free helper (msc313_txqfree): with
 * the FIFO-idle bits (3,4 and 7..12) all set it computes 4 free slots, i.e. the
 * transmit path is always ready. TX is instantaneous here so that is accurate.
 */
#define TSR_ALL_FREE    0x1f98

/* RX descriptor addr/ctrl bits. */
#define RXD_USED    (1 << 0)
#define RXD_WRAP    (1 << 1)
#define RXD_SOF     (1 << 14)
#define RXD_EOF     (1 << 15)

#define AT91ETHER_MAX_RX_DESCR 256      /* vendor driver's RX ring depth */

/* Reconstruct a 32-bit register value from its two RIU 16-bit halves. */
static uint32_t emac_reg(MstarEmacState *s, unsigned int g)
{
    return s->raw[g] | ((uint32_t)s->raw[g + 2] << 16);
}

/*
 * The emac DMA sees MIU-relative addresses: the driver programs RBQP/TAR and
 * builds the descriptor buffer pointers as (physical - DRAM_BASE), e.g. RBQP =
 * 0x03fb0000 for a ring in DRAM at 0x23fb0000. Translate back to a physical
 * address for the QEMU DMA. Addresses that are already >= DRAM_BASE are passed
 * through unchanged.
 */
static hwaddr emac_dma(hwaddr a)
{
    return a < MSTAR_DRAM_BASE ? a + MSTAR_DRAM_BASE : a;
}

static void mstar_emac_update_irq(MstarEmacState *s)
{
    qemu_set_irq(s->irq, (s->isr & s->ien) != 0);
}

/* A permanently-linked 10/100 PHY answering at a single MDIO address. */
static uint16_t mstar_emac_phy_read(MstarEmacState *s, unsigned int phya,
                                    unsigned int reg)
{
    if (phya != s->phy_addr) {
        return 0xffff;                  /* no device at this address */
    }
    switch (reg) {
    case 0x00:                  /* BMCR: autoneg enable */
        return 0x1000;
    case 0x01:                  /* BMSR: link up, autoneg complete, 10/100 caps */
        return 0x796d;
    case 0x02:                  /* PHYID1 */
        return 0x0141;
    case 0x03:                  /* PHYID2 */
        return 0x0e10;
    case 0x04:                  /* ANAR */
        return 0x01e1;
    case 0x05:                  /* ANLPAR: partner 100/10 full/half */
        return 0x45e1;
    default:
        return 0x0000;
    }
}

/* rm9200 transmit: read the frame from guest memory at TAR and send it. */
static void mstar_emac_do_tx(MstarEmacState *s)
{
    hwaddr addr = emac_dma(emac_reg(s, EMAC_TAR));
    uint32_t len = emac_reg(s, EMAC_TCR) & 0x7ff;
    g_autofree uint8_t *buf = NULL;

    if (!(emac_reg(s, EMAC_NCR) & NCR_TE) || len == 0 || len > 2048) {
        return;
    }
    buf = g_malloc(len);
    address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                       buf, len);
    if (getenv("MSTAR_EMAC_DBG")) {
        int i, n = len < 34 ? len : 34;
        fprintf(stderr, "[emac] TX %u bytes from 0x%08x:", len, (uint32_t)addr);
        for (i = 0; i < n; i++) {
            fprintf(stderr, "%s%02x", (i == 6 || i == 12 || i == 14) ? " " : "",
                    buf[i]);
        }
        fprintf(stderr, "\n");
    }
    qemu_send_packet(qemu_get_queue(s->nic), buf, len);

    /* Transmit complete; the FIFO is free again. */
    s->isr |= ISR_TCOMP | ISR_RM9200_TBRE;
    mstar_emac_update_irq(s);
}

/* MDIO access via the MAN register (Clause 22). */
static void mstar_emac_do_mdio(MstarEmacState *s)
{
    uint32_t man = emac_reg(s, EMAC_MAN);
    unsigned int rw = (man >> 28) & 3;
    unsigned int phya = (man >> 23) & 0x1f;
    unsigned int reg = (man >> 18) & 0x1f;

    if (rw == 2) {                          /* read */
        s->man = (man & 0xffff0000) | mstar_emac_phy_read(s, phya, reg);
    } else {                                /* write: just record it */
        s->man = man;
    }
}

/*
 * The MAC register file is reachable two ways (both put register byte-offset G
 * at bus offset 2*G): the mainline driver uses RIU 16-bit half-lanes (low at
 * 2*G, high at 2*G+4), the vendor camera driver uses XIU full 32-bit accesses
 * (readl/writel at 2*G). Decode by access size: a 4-byte access is the whole
 * register, a 2-byte access is one RIU half.
 */
static uint64_t mstar_emac_read(void *opaque, hwaddr riu, unsigned size)
{
    MstarEmacState *s = opaque;
    bool high = (size == 2) && (riu & 4);    /* RIU high 16-bit half */
    unsigned int g = high ? (riu - 4) / 2 : riu / 2;   /* MACB register offset */
    uint32_t v;

    switch (g) {
    case EMAC_NSR:
        v = NSR_IDLE;                        /* MDIO logic always idle/ready */
        break;
    case EMAC_TSR:
        v = TSR_ALL_FREE;                    /* transmit FIFO always free */
        break;
    case EMAC_RSR:
        v = s->rsr;
        break;
    case EMAC_ISR:
        v = s->isr;
        s->isr = 0;                          /* read-clear */
        mstar_emac_update_irq(s);
        break;
    case EMAC_IMR:
        v = ~s->ien;                         /* IMR reports masked (disabled) bits */
        break;
    case EMAC_MAN:
        v = s->man;
        break;
    default:
        v = emac_reg(s, g);
        break;
    }
    if (size == 4) {
        return v;                            /* XIU: whole 32-bit register */
    }
    return high ? (v >> 16) & 0xffff : v & 0xffff;
}

static void mstar_emac_write(void *opaque, hwaddr riu, uint64_t val,
                             unsigned size)
{
    MstarEmacState *s = opaque;
    bool xiu = (size == 4);
    bool high = !xiu && (riu & 4);
    unsigned int g = high ? (riu - 4) / 2 : riu / 2;

    if (xiu) {
        /* XIU: whole register at 2*G - store both RIU halves. */
        s->raw[riu / 2] = val & 0xffff;
        s->raw[riu / 2 + 2] = (val >> 16) & 0xffff;
    } else {
        s->raw[riu / 2] = val & 0xffff;
    }

    if (getenv("MSTAR_EMAC_DBG")) {
        fprintf(stderr, "[emac] W riu=0x%03x (reg 0x%02x %s) = 0x%0*x\n",
                (unsigned)riu, g, xiu ? "xiu" : high ? "hi" : "lo",
                xiu ? 8 : 4, (unsigned)val);
    }

    /*
     * Side effects fire when the whole 32-bit register value is present: for an
     * XIU write that is this access; for RIU half-writes it is the high half
     * (written last), so ignore the low half.
     */
    if (!xiu && !high) {
        return;
    }

    switch (g) {
    case EMAC_NCR:
        /*
         * The driver's RX errata toggles RE off then on; while it is off
         * can_receive() is false and the net layer queues incoming frames.
         * Flush them once RE comes back so RX does not stall after a burst.
         */
        if (emac_reg(s, EMAC_NCR) & NCR_RE) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        break;
    case EMAC_TCR:
        mstar_emac_do_tx(s);
        break;
    case EMAC_MAN:
        mstar_emac_do_mdio(s);
        break;
    case EMAC_IER:
        s->ien |= emac_reg(s, EMAC_IER);
        mstar_emac_update_irq(s);
        break;
    case EMAC_IDR:
        s->ien &= ~emac_reg(s, EMAC_IDR);
        mstar_emac_update_irq(s);
        break;
    case EMAC_RSR:
        s->rsr &= ~emac_reg(s, EMAC_RSR);    /* write-1-to-clear */
        break;
    default:
        break;
    }
}

static const MemoryRegionOps mstar_emac_ops = {
    .read = mstar_emac_read,
    .write = mstar_emac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

/*
 * Integrated-PHY register block (phys 0x1f006000). The vendor kernel reads the
 * internal PHY's registers straight out of the table at MSTAR_EMACPHY_TABLE
 * (reg N at +N*4) rather than doing MDIO, so present the PHY here too. Only the
 * standard MDIO register table matters for link bring-up; the analog-config
 * registers elsewhere in the block are write-only to the driver.
 */
static uint64_t mstar_emacphy_read(void *opaque, hwaddr off, unsigned size)
{
    MstarEmacState *s = opaque;

    if (off >= MSTAR_EMACPHY_TABLE &&
        off < MSTAR_EMACPHY_TABLE + sizeof(s->phy_regs)) {
        return s->phy_regs[(off - MSTAR_EMACPHY_TABLE) >> 2];
    }
    return 0;
}

static void mstar_emacphy_write(void *opaque, hwaddr off, uint64_t val,
                                unsigned size)
{
    MstarEmacState *s = opaque;

    if (off >= MSTAR_EMACPHY_TABLE &&
        off < MSTAR_EMACPHY_TABLE + sizeof(s->phy_regs)) {
        unsigned int reg = (off - MSTAR_EMACPHY_TABLE) >> 2;

        if (reg != 0x01) {              /* BMSR is read-only status */
            s->phy_regs[reg] = val;
        }
    }
}

static const MemoryRegionOps mstar_emacphy_ops = {
    .read = mstar_emacphy_read,
    .write = mstar_emacphy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static bool mstar_emac_can_receive(NetClientState *nc)
{
    MstarEmacState *s = qemu_get_nic_opaque(nc);

    return (emac_reg(s, EMAC_NCR) & NCR_RE) != 0;
}

static ssize_t mstar_emac_receive(NetClientState *nc, const uint8_t *buf,
                                  size_t size)
{
    MstarEmacState *s = qemu_get_nic_opaque(nc);
    hwaddr ring = emac_dma(emac_reg(s, EMAC_RBQP));
    hwaddr desc = ring + s->rx_idx * 8;
    uint32_t addr_w, ctrl_w;

    if (!(emac_reg(s, EMAC_NCR) & NCR_RE)) {
        return -1;
    }
    if (size > 0x600) {
        size = 0x600;                        /* AT91ETHER_MAX_RBUFF_SZ */
    }

    addr_w = ldl_le_phys(&address_space_memory, desc);
    if (getenv("MSTAR_EMAC_DBG")) {
        fprintf(stderr, "[emac] RX %zu bytes idx=%u %s dst %02x:%02x:%02x:%02x:%02x:%02x src %02x:%02x:%02x:%02x:%02x:%02x\n",
                size, s->rx_idx, (addr_w & RXD_USED) ? "FULL-DROP" : "ok",
                buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
                buf[6], buf[7], buf[8], buf[9], buf[10], buf[11]);
    }
    if (addr_w & RXD_USED) {
        /*
         * No free descriptor: report overrun and ask the net layer to hold the
         * packet - it retries via qemu_flush_queued_packets() (see the poll).
         */
        s->rsr |= RSR_OVR | RSR_BNA;
        s->isr |= ISR_RXUBR;
        mstar_emac_update_irq(s);
        return 0;
    }

    /* DMA the frame into the descriptor's buffer and hand it to the host. */
    address_space_write(&address_space_memory, emac_dma(addr_w & ~3u),
                        MEMTXATTRS_UNSPECIFIED, buf, size);
    ctrl_w = (size & 0xfff) | RXD_SOF | RXD_EOF;
    stl_le_phys(&address_space_memory, desc + 4, ctrl_w);
    stl_le_phys(&address_space_memory, desc, addr_w | RXD_USED);

    if (addr_w & RXD_WRAP) {
        s->rx_idx = 0;
    } else if (++s->rx_idx >= AT91ETHER_MAX_RX_DESCR) {
        s->rx_idx = 0;
    }

    s->rsr |= RSR_REC;
    s->isr |= ISR_RCOMP;
    mstar_emac_update_irq(s);
    return size;
}

static NetClientInfo net_mstar_emac_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = mstar_emac_can_receive,
    .receive = mstar_emac_receive,
};

static void mstar_emac_reset_hold(Object *obj, ResetType type)
{
    MstarEmacState *s = MSTAR_EMAC(obj);

    memset(s->raw, 0, sizeof(s->raw));
    s->isr = 0;
    s->ien = 0;
    s->rsr = 0;
    s->man = 0;
    s->rx_idx = 0;

    /* A permanently-linked 10/100 PHY in the integrated-PHY register table. */
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    s->phy_regs[0x00] = 0x3100;         /* BMCR */
    s->phy_regs[0x01] = 0x796d;         /* BMSR: link up, autoneg complete */
    s->phy_regs[0x02] = 0x0141;         /* PHYID1 */
    s->phy_regs[0x03] = 0x0e10;         /* PHYID2 */
    s->phy_regs[0x04] = 0x01e1;         /* ANAR */
    s->phy_regs[0x05] = 0x45e1;         /* ANLPAR: partner 100/10 full/half */

    mstar_emac_update_irq(s);
}

static void mstar_emac_realize(DeviceState *dev, Error **errp)
{
    MstarEmacState *s = MSTAR_EMAC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_emac_ops, s,
                          "mstar.emac", MSTAR_EMAC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    memory_region_init_io(&s->phy_iomem, OBJECT(dev), &mstar_emacphy_ops, s,
                          "mstar.emac-phy", MSTAR_EMACPHY_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->phy_iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_mstar_emac_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static const VMStateDescription vmstate_mstar_emac = {
    .name = "mstar-emac",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(raw, MstarEmacState, 0x1000 / 2),
        VMSTATE_UINT32(isr, MstarEmacState),
        VMSTATE_UINT32(ien, MstarEmacState),
        VMSTATE_UINT32(rsr, MstarEmacState),
        VMSTATE_UINT32(man, MstarEmacState),
        VMSTATE_UINT32(rx_idx, MstarEmacState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mstar_emac_props[] = {
    DEFINE_NIC_PROPERTIES(MstarEmacState, conf),
    DEFINE_PROP_UINT8("phy-addr", MstarEmacState, phy_addr, 0),
};

static void mstar_emac_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_emac_realize;
    dc->vmsd = &vmstate_mstar_emac;
    rc->phases.hold = mstar_emac_reset_hold;
    device_class_set_props(dc, mstar_emac_props);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo mstar_emac_types[] = {
    {
        .name           = TYPE_MSTAR_EMAC,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MstarEmacState),
        .class_init     = mstar_emac_class_init,
    },
};

DEFINE_TYPES(mstar_emac_types)
