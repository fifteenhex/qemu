/*
 * Amiga 3000 SuperDMAC (SCSI DMA controller).
 *
 * The SuperDMAC sits between the CPU, chip/fast RAM and the WD33C93A
 * SBIC: it decodes the SBIC's two host registers inside its own
 * register window, funnels the SBIC interrupt onto INT2 (gated by
 * CNTR_INTEN and reported in ISTR), and moves data-phase bytes between
 * memory and the SBIC with a FIFO.
 *
 * Register map (from the Linux amiga a3000 driver, confirmed against
 * the Kickstart 3.1 scsi.device):
 *
 *   0x02 DAWR    0x04 WTC     0x0a CNTR    0x0c ACR
 *   0x12 ST_DMA  0x16 FLUSH   0x1a CINT    0x1e ISTR   0x3e SP_DMA
 *
 * The SBIC is partially decoded through 0x40-0x7f on the odd byte
 * lane, SASR at 4n+1 and the data register at 4n+3; scsi.device
 * relies on the mirrors (its interrupt handler reads SASR at 0x49).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/m68k/a3000_sdmac.h"
#include "migration/vmstate.h"
#include "system/dma.h"

#define REG_DAWR        0x02
#define REG_WTC         0x04
#define REG_CNTR        0x0a
#define REG_ACR         0x0c
#define REG_ST_DMA      0x12
#define REG_FLUSH       0x16
#define REG_CINT        0x1a
#define REG_ISTR        0x1e
#define REG_SP_DMA      0x3e
#define REG_SBIC_BASE   0x40

#define CNTR_TCEN       (1 << 5)    /* terminal count enable */
#define CNTR_PREST      (1 << 4)    /* peripheral reset */
#define CNTR_PDMD       (1 << 3)    /* peripheral device mode */
#define CNTR_INTEN      (1 << 2)    /* interrupt enable */
#define CNTR_DDIR       (1 << 1)    /* direction: 1 = to memory */
#define CNTR_IO_DX      (1 << 0)

#define ISTR_INT_F      (1 << 7)    /* an interrupt condition is present */
#define ISTR_INTS       (1 << 6)    /* SBIC interrupt */
#define ISTR_E_INT      (1 << 5)    /* end-of-process */
#define ISTR_INT_P      (1 << 4)    /* INT2 asserted */
#define ISTR_UE_INT     (1 << 3)    /* fifo underrun */
#define ISTR_OE_INT     (1 << 2)    /* fifo overrun */
#define ISTR_FF_FLG     (1 << 1)    /* fifo full */
#define ISTR_FE_FLG     (1 << 0)    /* fifo empty */

static bool a3000_sdmac_int_cond(A3000SDMACState *s)
{
    return s->sbic_int || s->e_int;
}

static void a3000_sdmac_update_irq(A3000SDMACState *s)
{
    qemu_set_irq(s->irq, (s->cntr & CNTR_INTEN) && a3000_sdmac_int_cond(s));
}

static uint8_t a3000_sdmac_istr(A3000SDMACState *s)
{
    uint8_t istr = ISTR_FE_FLG;

    if (s->sbic_int) {
        istr |= ISTR_INTS;
    }
    if (s->e_int) {
        istr |= ISTR_E_INT;
    }
    if (a3000_sdmac_int_cond(s)) {
        istr |= ISTR_INT_F;
        if (s->cntr & CNTR_INTEN) {
            istr |= ISTR_INT_P;
        }
    }
    return istr;
}

/*
 * Move data between memory (at ACR) and the SBIC while the SBIC keeps
 * DRQ up.  With CNTR_TCEN the word transfer count limits the transfer
 * and raises end-of-process at terminal count; without it the SBIC's
 * own transfer count terminates the exchange.
 */
static void a3000_sdmac_run(A3000SDMACState *s)
{
    uint8_t buf[512];

    while (s->dma_active && s->drq) {
        int dir = wd33c93_dma_dir(s->sbic);
        size_t chunk = sizeof(buf);
        size_t n;

        if (!dir) {
            break;
        }
        if ((dir > 0) != !!(s->cntr & CNTR_DDIR)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "a3000-sdmac: dma direction mismatch (CNTR 0x%x)\n",
                          s->cntr);
        }
        if (s->cntr & CNTR_TCEN) {
            if (!s->wtc) {
                break;
            }
            chunk = MIN(chunk, (size_t)s->wtc * 2);
        }
        if (dir > 0) {
            n = wd33c93_dma_pull(s->sbic, buf, chunk);
            if (!n) {
                break;
            }
            dma_memory_write(&address_space_memory, s->acr, buf, n,
                             MEMTXATTRS_UNSPECIFIED);
        } else {
            dma_memory_read(&address_space_memory, s->acr, buf, chunk,
                            MEMTXATTRS_UNSPECIFIED);
            n = wd33c93_dma_push(s->sbic, buf, chunk);
            if (!n) {
                break;
            }
        }
        s->acr += n;
        if (s->cntr & CNTR_TCEN) {
            uint32_t words = (n + 1) / 2;

            s->wtc = s->wtc > words ? s->wtc - words : 0;
            if (!s->wtc) {
                s->dma_active = false;
                s->e_int = true;
                a3000_sdmac_update_irq(s);
            }
        }
    }
}

static uint64_t a3000_sdmac_read(void *opaque, hwaddr addr, unsigned size)
{
    A3000SDMACState *s = opaque;

    if (addr >= REG_SBIC_BASE) {
        if (addr & 1) {
            return wd33c93_io_read(s->sbic, (addr & 2) ?
                                   WD33C93_REG_BUS_DATA :
                                   WD33C93_REG_BUS_ADDR);
        }
        /* even bytes float */
        return 0xff;
    }

    switch (addr) {
    case REG_DAWR + 1:
        return s->dawr;
    case REG_WTC ... REG_WTC + 3:
        return s->wtc >> (8 * (3 - (addr - REG_WTC)));
    case REG_CNTR:
        return s->cntr >> 8;
    case REG_CNTR + 1:
        return s->cntr;
    case REG_ACR ... REG_ACR + 3:
        return s->acr >> (8 * (3 - (addr - REG_ACR)));
    case REG_ISTR:
        return 0;
    case REG_ISTR + 1:
        return a3000_sdmac_istr(s);
    default:
        qemu_log_mask(LOG_UNIMP,
                      "a3000-sdmac: unimplemented read 0x%02" HWADDR_PRIx "\n",
                      addr);
        return 0;
    }
}

static void a3000_sdmac_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    A3000SDMACState *s = opaque;
    uint16_t old_cntr;

    if (addr >= REG_SBIC_BASE) {
        if (addr & 1) {
            wd33c93_io_write(s->sbic, (addr & 2) ? WD33C93_REG_BUS_DATA :
                                                   WD33C93_REG_BUS_ADDR,
                             val);
        }
        return;
    }

    switch (addr) {
    case REG_DAWR:
        break;
    case REG_DAWR + 1:
        s->dawr = val;
        break;
    case REG_WTC ... REG_WTC + 3:
        s->wtc = deposit32(s->wtc, 8 * (3 - (addr - REG_WTC)), 8, val);
        break;
    case REG_CNTR:
        s->cntr = deposit32(s->cntr, 8, 8, val);
        break;
    case REG_CNTR + 1:
        old_cntr = s->cntr;
        s->cntr = deposit32(s->cntr, 0, 8, val);
        if ((s->cntr & CNTR_PREST) && !(old_cntr & CNTR_PREST)) {
            device_cold_reset(DEVICE(s->sbic));
        }
        a3000_sdmac_update_irq(s);
        break;
    case REG_ACR ... REG_ACR + 3:
        s->acr = deposit32(s->acr, 8 * (3 - (addr - REG_ACR)), 8, val);
        break;
    case REG_ST_DMA:
    case REG_ST_DMA + 1:
        s->dma_active = true;
        a3000_sdmac_run(s);
        break;
    case REG_FLUSH:
    case REG_FLUSH + 1:
        s->dma_active = false;
        break;
    case REG_CINT:
    case REG_CINT + 1:
        s->e_int = false;
        a3000_sdmac_update_irq(s);
        break;
    case REG_SP_DMA:
    case REG_SP_DMA + 1:
        s->dma_active = false;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "a3000-sdmac: unimplemented write 0x%02" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps a3000_sdmac_ops = {
    .read = a3000_sdmac_read,
    .write = a3000_sdmac_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void a3000_sdmac_sbic_irq(void *opaque, int n, int level)
{
    A3000SDMACState *s = opaque;

    s->sbic_int = level;
    a3000_sdmac_update_irq(s);
}

static void a3000_sdmac_sbic_drq(void *opaque, int n, int level)
{
    A3000SDMACState *s = opaque;

    s->drq = level;
    if (level) {
        a3000_sdmac_run(s);
    }
}

static void a3000_sdmac_reset(DeviceState *dev)
{
    A3000SDMACState *s = A3000_SDMAC(dev);

    s->dawr = 0;
    s->cntr = 0;
    s->wtc = 0;
    s->acr = 0;
    s->dma_active = false;
    s->e_int = false;
    a3000_sdmac_update_irq(s);
}

static void a3000_sdmac_realize(DeviceState *dev, Error **errp)
{
    A3000SDMACState *s = A3000_SDMAC(dev);

    if (!s->sbic) {
        error_setg(errp, "a3000-sdmac: 'sbic' link not set");
        return;
    }
    qdev_connect_gpio_out_named(DEVICE(s->sbic), "irq", 0,
                                qdev_get_gpio_in_named(dev, "sbic-irq", 0));
    qdev_connect_gpio_out_named(DEVICE(s->sbic), "drq", 0,
                                qdev_get_gpio_in_named(dev, "sbic-drq", 0));
}

static void a3000_sdmac_init(Object *obj)
{
    A3000SDMACState *s = A3000_SDMAC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &a3000_sdmac_ops, s,
                          TYPE_A3000_SDMAC, 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in_named(dev, a3000_sdmac_sbic_irq, "sbic-irq", 1);
    qdev_init_gpio_in_named(dev, a3000_sdmac_sbic_drq, "sbic-drq", 1);
}

static const VMStateDescription vmstate_a3000_sdmac = {
    .name = "a3000-sdmac",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(dawr, A3000SDMACState),
        VMSTATE_UINT16(cntr, A3000SDMACState),
        VMSTATE_UINT32(wtc, A3000SDMACState),
        VMSTATE_UINT32(acr, A3000SDMACState),
        VMSTATE_BOOL(dma_active, A3000SDMACState),
        VMSTATE_BOOL(e_int, A3000SDMACState),
        VMSTATE_BOOL(sbic_int, A3000SDMACState),
        VMSTATE_BOOL(drq, A3000SDMACState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property a3000_sdmac_properties[] = {
    DEFINE_PROP_LINK("sbic", A3000SDMACState, sbic, TYPE_WD33C93,
                     WD33C93State *),
};

static void a3000_sdmac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = a3000_sdmac_realize;
    device_class_set_legacy_reset(dc, a3000_sdmac_reset);
    dc->vmsd = &vmstate_a3000_sdmac;
    device_class_set_props(dc, a3000_sdmac_properties);
}

static const TypeInfo a3000_sdmac_info = {
    .name          = TYPE_A3000_SDMAC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A3000SDMACState),
    .instance_init = a3000_sdmac_init,
    .class_init    = a3000_sdmac_class_init,
};

static void a3000_sdmac_register_types(void)
{
    type_register_static(&a3000_sdmac_info);
}

type_init(a3000_sdmac_register_types)
