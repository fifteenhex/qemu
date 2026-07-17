/*
 * MStar/SigmaStar VIF - sensor video-input interface (infinity3 csi@1f240800)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The front-end that clocks pixel data in from the image sensor and hands it to
 * the ISP. On the MSC313E (infinity3) this single block at 0x1f240800 is the
 * unified receiver for either a MIPI CSI-2 sensor or a parallel (BT.601/656)
 * sensor - the retail firmware here drives it in parallel mode for the IMX323
 * ("IMX323_PARL"). The kernel/DT name is "csi@1f240800"; the vendor HAL calls it
 * the VIF.
 *
 * What the firmware actually does with it (captured live, MSTAR_IOLOG, from a
 * single bring-up function): a short burst of config writes, all read-back-
 * verified -
 *   0x00 = 0x020c   control: source select + sensor reset/power-down/enable
 *   0x04 = 0x0003
 *   0x08 = 0x0001
 *   0x0c = 0xffff   (0x0c=0xff, 0x0d=0x7f byte lanes)
 *   0x18 = 0x003f   format / sync polarity
 *   0x60 = 0x00001001 (32-bit), 0x64 = 0x00000008 (32-bit)
 *   0x74/0x80/0x84 = 0
 * then nothing more until frames flow. So a plain store/read-back register file
 * satisfies bring-up (the catch-all only got away with returning 0 because the
 * driver doesn't re-check most of these).
 *
 * Register semantics (control bits, and the 7-bit interrupt block) are named
 * from the Mercury5 SDK VIF header (mercury5_reg_vif.h, reg_vif_ch0_* /
 * reg_c_irq_*). The exact interrupt-register offsets are PROVISIONAL for the
 * infinity3 - this firmware never touches them (no frame source yet), so they
 * are modelled here for completeness/future use and marked to confirm by trace
 * once the capture path delivers frames. The interrupt output + the
 * mstar_vif_frame_irq() helper are wired so a frame source can later raise a
 * per-frame VIF interrupt.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

/* ---- control (reg 0x00), Mercury5 reg_vif_ch0_* bit layout ------------- */
#define VIF_CTRL0           0x00
#define  VIF_CTRL0_SW_RSTZ      (1 << 0)    /* sensor interface soft reset (0=rst) */
#define  VIF_CTRL0_IF_STATE_RST (1 << 1)
#define  VIF_CTRL0_SENSOR_RST   (1 << 2)    /* sensor reset pin */
#define  VIF_CTRL0_SENSOR_PWRDN (1 << 3)    /* sensor power-down pin */
#define  VIF_CTRL0_EN           (1 << 15)   /* channel enable */

/*
 * 7-bit interrupt block (reg_c_irq_* / reg_irq_*_status0). PROVISIONAL offsets
 * from the Mercury5 header (byte-offset*2): mask 0x2c, force 0x30, clr 0x34,
 * final(masked) status 0x38, raw status 0x3c. Confirm against infinity3 once a
 * frame source exercises them.
 */
#define VIF_IRQ_MASK0       0x2c
#define VIF_IRQ_FORCE0      0x30
#define VIF_IRQ_CLR0        0x34
#define VIF_IRQ_FINAL0      0x38
#define VIF_IRQ_RAW0        0x3c
#define VIF_IRQ_BITS        0x7f

/* Interrupt bit assignment is not yet decoded for infinity3; bit 0 is the
 * per-frame (frame-end) source used by mstar_vif_frame_irq(). */
#define VIF_IRQ_FRAME_END   (1 << 0)

static void mstar_vif_update_irq(MstarVifState *s)
{
    /* mask bit set => that source is masked off; line follows the rest. */
    qemu_set_irq(s->irq, (s->irq_raw & ~s->irq_mask & VIF_IRQ_BITS) != 0);
}

/*
 * Raise a VIF interrupt source (e.g. frame-end). Exported so a future capture
 * frame source can pulse a per-frame VIF interrupt. No-op visible effect until
 * the interrupt output is connected by the SoC.
 */
void mstar_vif_frame_irq(MstarVifState *s, unsigned bits)
{
    s->irq_raw |= bits & VIF_IRQ_BITS;
    mstar_vif_update_irq(s);
}

static uint64_t mstar_vif_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarVifState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    switch (addr) {
    case VIF_IRQ_RAW0:
        return s->irq_raw;
    case VIF_IRQ_FINAL0:
        return s->irq_raw & ~s->irq_mask & VIF_IRQ_BITS;
    case VIF_IRQ_MASK0:
        return s->irq_mask;
    }
    for (i = 0; i < size && addr + i < sizeof(s->store); i++) {
        val |= (uint64_t)s->store[addr + i] << (8 * i);
    }
    return val;
}

static void mstar_vif_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MstarVifState *s = opaque;
    unsigned i;

    switch (addr) {
    case VIF_IRQ_MASK0:
        s->irq_mask = val & VIF_IRQ_BITS;
        mstar_vif_update_irq(s);
        return;
    case VIF_IRQ_FORCE0:                 /* force interrupt source(s) */
        s->irq_raw |= val & VIF_IRQ_BITS;
        mstar_vif_update_irq(s);
        return;
    case VIF_IRQ_CLR0:                   /* write-1-to-clear raw status */
        s->irq_raw &= ~(val & VIF_IRQ_BITS);
        mstar_vif_update_irq(s);
        return;
    }
    for (i = 0; i < size && addr + i < sizeof(s->store); i++) {
        s->store[addr + i] = (val >> (8 * i)) & 0xff;
    }
}

static const MemoryRegionOps mstar_vif_ops = {
    .read = mstar_vif_read,
    .write = mstar_vif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,     /* firmware writes the control reg byte-wise */
    .valid.max_access_size = 4,     /* ... and regs 0x60/0x64 as 32-bit words */
};

static void mstar_vif_reset_hold(Object *obj, ResetType type)
{
    MstarVifState *s = MSTAR_VIF(obj);

    memset(s->store, 0, sizeof(s->store));
    s->irq_raw = 0;
    s->irq_mask = VIF_IRQ_BITS;         /* all sources masked out of reset */
    mstar_vif_update_irq(s);
}

static void mstar_vif_realize(DeviceState *dev, Error **errp)
{
    MstarVifState *s = MSTAR_VIF(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_vif_ops, s,
                          "mstar.vif", MSTAR_VIF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mstar_vif = {
    .name = "mstar-vif",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(store, MstarVifState, MSTAR_VIF_SIZE),
        VMSTATE_UINT8(irq_raw, MstarVifState),
        VMSTATE_UINT8(irq_mask, MstarVifState),
        VMSTATE_END_OF_LIST()
    },
};

static void mstar_vif_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_vif_realize;
    dc->vmsd = &vmstate_mstar_vif;
    rc->phases.hold = mstar_vif_reset_hold;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo mstar_vif_types[] = {
    {
        .name           = TYPE_MSTAR_VIF,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MstarVifState),
        .class_init     = mstar_vif_class_init,
    },
};

DEFINE_TYPES(mstar_vif_types)
