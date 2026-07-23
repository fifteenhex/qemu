/*
 * Cirrus Logic CD2401 four channel multi-protocol serial controller
 *
 * Async-mode model driven by what the ELTEC E17 RMON firmware needs:
 *
 *  - the per channel register file is banked by CAR (0xee); RMON
 *    programs CMR/COR/baud (TCOR/TBPR at 0xc0/0xc3, RCOR/RBPR at
 *    0xc8/0xcb, from a 20MHz CLK) and scratch tests the DMA address
 *    registers around 0x40,
 *  - GFRCR (0x81) must read back nonzero ("firmware loaded"),
 *  - interrupts are consumed either from the ISR or by polling the
 *    board's acknowledge port (second mmio region, offset 0xfb; the
 *    E17 decodes it at 0xfec660fb): the byte is (channel << 2) |
 *    type, types as in LIVR bits 1:0 (1=modem, 2=tx, 3=rx).
 *    Service context is the acknowledged channel, independent of CAR:
 *    tx loads TFTC (0x80) and writes TDR (0xf8), rx reads a count
 *    from RFOC (0x30) and that many bytes from RDR (0xf8), and the
 *    service ends with a write to REOIR/TEOIR/MEOIR (0x84/85/86).
 *
 * Registers this model does not interpret behave as plain storage.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/char/cd2401.h"
#include "migration/vmstate.h"

#define CD2401_RFOC     0x30    /* receive FIFO output count */
#define CD2401_TFTC     0x80    /* transmit FIFO transfer count */
#define CD2401_GFRCR    0x81    /* global firmware revision code */
#define CD2401_REOIR    0x84    /* receive end of interrupt */
#define CD2401_TEOIR    0x85    /* transmit end of interrupt */
#define CD2401_MEOIR    0x86    /* modem end of interrupt */
#define CD2401_LIVR     0x09    /* local interrupt vector, per channel */
#define CD2401_IER      0x11    /* interrupt enable, per channel */
#define CD2401_CCR      0x13    /* channel command register */
#define CD2401_CAR      0xee    /* channel access register */
#define CD2401_DR       0xf8    /* rx/tx data register */

#define CD2401_GFRCR_REV    0x26    /* any nonzero revision will do */
#define CD2401_IER_TXD      0x01    /* tx data interrupt enable */
#define CD2401_TX_FIFO_LEN  16

static CD2401Channel *cd2401_car_chan(CD2401State *s)
{
    return &s->chan[s->gregs[CD2401_CAR & 0x7f] & (CD2401_NR_CHAN - 1)];
}

/*
 * Find the highest priority pending interrupt: receive data beats
 * transmit ready, lower channels beat higher ones.  Returns the
 * acknowledge byte — the channel's LIVR with the interrupt type in
 * bits 1:0 — or 0 if nothing is pending.  (RMON sets LIVR to
 * 0x50 | channel << 2, so the channel rides along in bits 3:2.)
 */
static uint8_t cd2401_pending(CD2401State *s, int *chan, int *type)
{
    int i;

    for (i = 0; i < CD2401_NR_CHAN; i++) {
        if (s->chan[i].rx_count > 0) {
            *chan = i;
            *type = CD2401_INT_RX;
            return (s->chan[i].regs[CD2401_LIVR] & ~3) | CD2401_INT_RX;
        }
    }
    for (i = 0; i < CD2401_NR_CHAN; i++) {
        if (s->chan[i].regs[CD2401_IER] & CD2401_IER_TXD) {
            *chan = i;
            *type = CD2401_INT_TX;
            return (s->chan[i].regs[CD2401_LIVR] & ~3) | CD2401_INT_TX;
        }
    }
    return 0;
}

static uint64_t cd2401_read(void *opaque, hwaddr addr, unsigned size)
{
    CD2401State *s = opaque;
    CD2401Channel *svc = &s->chan[s->svc_chan];
    uint8_t val;

    addr &= 0xff;
    switch (addr) {
    case CD2401_RFOC:
        return s->svc_type == CD2401_INT_RX ? svc->rx_count : 0;
    case CD2401_TFTC:
        return s->svc_type == CD2401_INT_TX ? CD2401_TX_FIFO_LEN : 0;
    case CD2401_DR:
        if (s->svc_type == CD2401_INT_RX && svc->rx_count > 0) {
            val = svc->rx_fifo[0];
            svc->rx_count--;
            memmove(svc->rx_fifo, svc->rx_fifo + 1, svc->rx_count);
            qemu_chr_fe_accept_input(&svc->chr);
            return val;
        }
        return 0;
    }
    if (addr < 0x80) {
        return cd2401_car_chan(s)->regs[addr];
    }
    return s->gregs[addr & 0x7f];
}

static void cd2401_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    CD2401State *s = opaque;
    CD2401Channel *svc = &s->chan[s->svc_chan];
    uint8_t b = val;

    addr &= 0xff;
    switch (addr) {
    case CD2401_DR:
        if (s->svc_type == CD2401_INT_TX) {
            qemu_chr_fe_write_all(&svc->chr, &b, 1);
            return;
        }
        qemu_log_mask(LOG_UNIMP, "cd2401: data write outside tx service\n");
        return;
    case CD2401_REOIR:
    case CD2401_TEOIR:
    case CD2401_MEOIR:
        s->svc_type = CD2401_INT_NONE;
        return;
    case CD2401_GFRCR:
        /* read only: keep the revision visible */
        return;
    case CD2401_CCR:
        /*
         * Channel commands (reset, enable/disable rx/tx, ...) have no
         * work to do here; the chip clears the register on completion
         * and the firmware polls for that.
         */
        return;
    }
    if (addr < 0x80) {
        cd2401_car_chan(s)->regs[addr] = b;
    } else {
        s->gregs[addr & 0x7f] = b;
    }
}

static const MemoryRegionOps cd2401_ops = {
    .read = cd2401_read,
    .write = cd2401_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * The board's interrupt acknowledge port: reading it latches the
 * highest priority pending interrupt as the current service context.
 */
static uint64_t cd2401_iack_read(void *opaque, hwaddr addr, unsigned size)
{
    CD2401State *s = opaque;

    if (addr == 0xfb) {
        return cd2401_pending(s, &s->svc_chan, &s->svc_type);
    }
    qemu_log_mask(LOG_UNIMP, "cd2401: iack read @0x%02" HWADDR_PRIx "\n",
                  addr);
    return 0;
}

static void cd2401_iack_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "cd2401: iack write @0x%02" HWADDR_PRIx
                  " = 0x%" PRIx64 "\n", addr, val);
}

static const MemoryRegionOps cd2401_iack_ops = {
    .read = cd2401_iack_read,
    .write = cd2401_iack_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static int cd2401_can_receive(void *opaque)
{
    CD2401Channel *c = opaque;

    return CD2401_RX_FIFO_LEN - c->rx_count;
}

static void cd2401_receive(void *opaque, const uint8_t *buf, int size)
{
    CD2401Channel *c = opaque;
    int n = MIN(size, CD2401_RX_FIFO_LEN - c->rx_count);

    memcpy(c->rx_fifo + c->rx_count, buf, n);
    c->rx_count += n;
}

static void cd2401_reset(DeviceState *dev)
{
    CD2401State *s = CD2401(dev);
    int i;

    for (i = 0; i < CD2401_NR_CHAN; i++) {
        memset(s->chan[i].regs, 0, sizeof(s->chan[i].regs));
        s->chan[i].rx_count = 0;
    }
    memset(s->gregs, 0, sizeof(s->gregs));
    s->gregs[CD2401_GFRCR & 0x7f] = CD2401_GFRCR_REV;
    s->svc_chan = 0;
    s->svc_type = CD2401_INT_NONE;
}

static void cd2401_realize(DeviceState *dev, Error **errp)
{
    CD2401State *s = CD2401(dev);
    int i;

    for (i = 0; i < CD2401_NR_CHAN; i++) {
        qemu_chr_fe_set_handlers(&s->chan[i].chr, cd2401_can_receive,
                                 cd2401_receive, NULL, NULL,
                                 &s->chan[i], NULL, true);
    }
}

static void cd2401_init(Object *obj)
{
    CD2401State *s = CD2401(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &cd2401_ops, s, "cd2401", 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    memory_region_init_io(&s->iack, obj, &cd2401_iack_ops, s,
                          "cd2401.iack", 0x100);
    sysbus_init_mmio(sbd, &s->iack);
}

static const VMStateDescription vmstate_cd2401_chan = {
    .name = "cd2401/chan",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, CD2401Channel, 0x80),
        VMSTATE_UINT8_ARRAY(rx_fifo, CD2401Channel, CD2401_RX_FIFO_LEN),
        VMSTATE_INT32(rx_count, CD2401Channel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_cd2401 = {
    .name = "cd2401",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(chan, CD2401State, CD2401_NR_CHAN, 1,
                             vmstate_cd2401_chan, CD2401Channel),
        VMSTATE_UINT8_ARRAY(gregs, CD2401State, 0x80),
        VMSTATE_INT32(svc_chan, CD2401State),
        VMSTATE_INT32(svc_type, CD2401State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property cd2401_properties[] = {
    DEFINE_PROP_CHR("chrA", CD2401State, chan[0].chr),
    DEFINE_PROP_CHR("chrB", CD2401State, chan[1].chr),
    DEFINE_PROP_CHR("chrC", CD2401State, chan[2].chr),
    DEFINE_PROP_CHR("chrD", CD2401State, chan[3].chr),
};

static void cd2401_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Cirrus Logic CD2401 serial controller";
    dc->realize = cd2401_realize;
    device_class_set_legacy_reset(dc, cd2401_reset);
    dc->vmsd = &vmstate_cd2401;
    device_class_set_props(dc, cd2401_properties);
}

static const TypeInfo cd2401_info = {
    .name = TYPE_CD2401,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CD2401State),
    .instance_init = cd2401_init,
    .class_init = cd2401_class_init,
};

static void cd2401_register_types(void)
{
    type_register_static(&cd2401_info);
}

type_init(cd2401_register_types)
