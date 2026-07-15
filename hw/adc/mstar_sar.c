/*
 * MStar/SigmaStar SAR ADC
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

/* ------------------------------------------------------------- sar (ADC) */

/*
 * Register map from the mainline driver drivers/iio/adc/msc313e_sar.c
 * (16-bit registers at the usual 4-byte RIU stride). A read of an analog
 * channel is a one-shot: the driver selects a channel and pulses CTRL.START,
 * waits, then reads the per-channel result register. We synthesise a fixed
 * sample per channel (there is no analog world to measure) and, on the
 * START edge, latch it into the result register and raise the done interrupt.
 * The four SAR pads also form a small GPIO/pinctrl block (loopback modelled
 * like the msc313-gpio) which the same driver registers.
 */
#define SAR_CTRL            0x00
#define SAR_CTRL_CHANNEL    0x0007  /* bits 0-2 */
#define SAR_CTRL_START      (1 << 7)
#define SAR_CTRL_LOAD       (1 << 14)  /* latch result; self-clears when done */
#define SAR_SAMPLE_PERIOD   0x04
#define SAR_GPIO_CTRL       0x44    /* en bits 0-3, oen (output disable) 8-11 */
#define SAR_GPIO_DATA       0x48    /* value bits 0-3, in bits 8-11 */
#define SAR_INT_MASK        0x50
#define SAR_INT_CLR         0x54
#define SAR_INT_FORCE       0x58
#define SAR_INT_STATUS      0x5c
#define SAR_VREF_SEL        0x64
#define SAR_INT_DONE        (1 << 0)
#define SAR_CH_RESULT(ch)   (0x100 + (ch) * 4)  /* ch 0-3 voltage, 6 temp */

static void msc313_sar_update_irq(Msc313SarState *s)
{
    uint16_t active = s->regs[SAR_INT_STATUS / 4] & ~s->regs[SAR_INT_MASK / 4];

    qemu_set_irq(s->irq, active != 0);
}

static uint64_t msc313_sar_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313SarState *s = opaque;

    if (addr == SAR_GPIO_DATA) {
        /*
         * Bits 0-3 are the driven output values, 8-11 the pin levels. As in
         * the msc313-gpio, an output-enabled pad (oen clear) loops its value
         * back to its input; an input pad has no external source (reads 0).
         */
        uint16_t ctrl = s->regs[SAR_GPIO_CTRL / 4];
        uint16_t val = s->regs[SAR_GPIO_DATA / 4] & 0x000f;
        uint16_t oen = (ctrl >> 8) & 0x0f;
        uint16_t in = val & ~oen;

        return val | (in << 8);
    }
    return s->regs[(addr & 0x1ff) / 4];
}

static void msc313_sar_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Msc313SarState *s = opaque;
    unsigned int idx = (addr & 0x1ff) / 4;
    uint16_t old = s->regs[idx];

    switch (addr) {
    case SAR_CTRL:
        /*
         * A conversion is triggered by a rising START (mainline driver) or by
         * writing LOAD (bit 14, vendor u-boot). Conversion is instantaneous
         * here, so LOAD self-clears: the vendor code polls it to clear as the
         * "done" flag, and reads the result once it does.
         */
        s->regs[idx] = val & ~SAR_CTRL_LOAD;
        if (((val & SAR_CTRL_START) && !(old & SAR_CTRL_START)) ||
            (val & SAR_CTRL_LOAD)) {
            unsigned int ch = val & SAR_CTRL_CHANNEL;

            s->regs[SAR_CH_RESULT(ch) / 4] = s->chan_input[ch];
            s->regs[SAR_INT_STATUS / 4] |= SAR_INT_DONE;
            msc313_sar_update_irq(s);
        }
        break;
    case SAR_INT_CLR:
        /* Write-1-to-clear the corresponding interrupt status bits. */
        s->regs[SAR_INT_STATUS / 4] &= ~(uint16_t)val;
        msc313_sar_update_irq(s);
        break;
    case SAR_INT_FORCE:
        s->regs[idx] = val;
        s->regs[SAR_INT_STATUS / 4] |= val;
        msc313_sar_update_irq(s);
        break;
    case SAR_INT_MASK:
        s->regs[idx] = val;
        msc313_sar_update_irq(s);
        break;
    default:
        s->regs[idx] = val;
        break;
    }
}

static const MemoryRegionOps msc313_sar_ops = {
    .read = msc313_sar_read,
    .write = msc313_sar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void msc313_sar_reset_hold(Object *obj, ResetType type)
{
    Msc313SarState *s = MSC313_SAR(obj);

    memset(s->regs, 0, sizeof(s->regs));
    /*
     * Synthesised samples: distinct per voltage channel so a driver can tell
     * the channel select works, and a temperature-channel raw value that maps
     * (via the driver's 1220*(400-raw)+25000 mdeg formula) to ~37 C.
     */
    s->chan_input[0] = 512;
    s->chan_input[1] = 600;
    s->chan_input[2] = 700;
    s->chan_input[3] = 800;
    s->chan_input[6] = 390;
    qemu_set_irq(s->irq, 0);
}

static void msc313_sar_realize(DeviceState *dev, Error **errp)
{
    Msc313SarState *s = MSC313_SAR(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313_sar_ops, s,
                          "mstar.msc313-sar", MSTAR_SAR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void msc313_sar_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313_sar_realize;
    rc->phases.hold = msc313_sar_reset_hold;
}

static const TypeInfo mstar_sar_types[] = {
    {
        .name           = TYPE_MSC313_SAR,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313SarState),
        .class_init     = msc313_sar_class_init,
    },
};

DEFINE_TYPES(mstar_sar_types)
