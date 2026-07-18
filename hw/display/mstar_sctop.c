/*
 * MStar/SigmaStar mercury5 SC_TOP - scaler/display-top interrupt bank
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The interrupt latch/mask bank of the mercury5 SCL (scaler/display) subsystem,
 * RIU bank 0x1218 (0x1f243000), signalling GIC SPI 20 (vendor INT_IRQ_SC_TOP).
 * Layout reverse engineered from the 70mai RTOS's ISR (registered at GIC INTID
 * 84; status reader at RTOS 0x2023245c, register token = RIU address >> 1):
 *
 *   0x40..0x4c  raw/latched status, one 16-bit word each; writing 1s clears
 *   0x50..0x5c  final status: raw & ~mask (what the ISR reads and dispatches)
 *   0x70..0x7c  masks (the firmware unmasks only bit1 of word0 = the display
 *               frame/vsync interrupt; everything else stays masked)
 *   0x144/0x174 per-SCL-instance status (instance 1/2), polled - store/readback
 *
 * A 60Hz timer latches the vsync bit so the firmware's display task gets its
 * frame heartbeat; the IRQ line is level = any unmasked latched bit.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "hw/display/mstar_sctop.h"

#define SCTOP_RAW(i)        (0x40 + (i) * 4)
#define SCTOP_STATUS(i)     (0x50 + (i) * 4)
#define SCTOP_MASK(i)       (0x70 + (i) * 4)
/*
 * word0 bit1 = the frame/vsync line source the firmware unmasks. (Bit0 also
 * looks frame-related - the ISR has a path keyed on it - but latching it makes
 * the firmware sys_Abort with error ID 22, so leave it clear.)
 */
#define SCTOP_VSYNC_BITS    (1 << 1)

#define SCTOP_REFRESH_NS (NANOSECONDS_PER_SECOND / 60)

static uint16_t mstar_sctop_mask(MstarScTopState *s, int i)
{
    return s->regs[SCTOP_MASK(i) / 4];
}

static void mstar_sctop_update_irq(MstarScTopState *s)
{
    int i;
    uint16_t pending = 0;

    for (i = 0; i < MSTAR_SCTOP_NWORDS; i++) {
        pending |= s->raw[i] & ~mstar_sctop_mask(s, i);
    }
    qemu_set_irq(s->irq, !!pending);
}

static void mstar_sctop_vsync(void *opaque)
{
    MstarScTopState *s = opaque;

    s->raw[0] |= SCTOP_VSYNC_BITS;
    mstar_sctop_update_irq(s);
    timer_mod(s->vsync,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCTOP_REFRESH_NS);
}

static uint64_t mstar_sctop_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarScTopState *s = opaque;

    if (addr >= SCTOP_RAW(0) && addr < SCTOP_RAW(MSTAR_SCTOP_NWORDS)) {
        return s->raw[(addr - SCTOP_RAW(0)) / 4];
    }
    if (addr >= SCTOP_STATUS(0) && addr < SCTOP_STATUS(MSTAR_SCTOP_NWORDS)) {
        /*
         * The ISR reads the status raw and dispatches every pending bit; the
         * masks only gate the interrupt line (the firmware unmasks just bit1
         * to get the line, but its frame handling keys off bit0).
         */
        return s->raw[(addr - SCTOP_STATUS(0)) / 4];
    }
    return s->regs[addr / 4];
}

static void mstar_sctop_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MstarScTopState *s = opaque;

    if (addr >= SCTOP_RAW(0) && addr < SCTOP_RAW(MSTAR_SCTOP_NWORDS)) {
        s->raw[(addr - SCTOP_RAW(0)) / 4] &= ~val;      /* write-1-to-clear */
        mstar_sctop_update_irq(s);
        return;
    }
    s->regs[addr / 4] = val;
    if (addr >= SCTOP_MASK(0) && addr < SCTOP_MASK(MSTAR_SCTOP_NWORDS)) {
        mstar_sctop_update_irq(s);
    }
}

static const MemoryRegionOps mstar_sctop_ops = {
    .read = mstar_sctop_read,
    .write = mstar_sctop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_sctop_reset_hold(Object *obj, ResetType type)
{
    MstarScTopState *s = MSTAR_SCTOP(obj);
    int i;

    memset(s->raw, 0, sizeof(s->raw));
    memset(s->regs, 0, sizeof(s->regs));
    for (i = 0; i < MSTAR_SCTOP_NWORDS; i++) {
        s->regs[SCTOP_MASK(i) / 4] = 0xffff;            /* all masked */
    }
    qemu_set_irq(s->irq, 0);
    timer_mod(s->vsync,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCTOP_REFRESH_NS);
}

static void mstar_sctop_realize(DeviceState *dev, Error **errp)
{
    MstarScTopState *s = MSTAR_SCTOP(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &mstar_sctop_ops, s,
                          TYPE_MSTAR_SCTOP, MSTAR_SCTOP_REGSIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->vsync = timer_new_ns(QEMU_CLOCK_VIRTUAL, mstar_sctop_vsync, s);
}

static const VMStateDescription vmstate_mstar_sctop = {
    .name = "mstar-sctop",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(raw, MstarScTopState, MSTAR_SCTOP_NWORDS),
        VMSTATE_UINT16_ARRAY(regs, MstarScTopState, MSTAR_SCTOP_REGSIZE / 4),
        VMSTATE_TIMER_PTR(vsync, MstarScTopState),
        VMSTATE_END_OF_LIST()
    },
};

static void mstar_sctop_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_sctop_realize;
    rc->phases.hold = mstar_sctop_reset_hold;
    dc->vmsd = &vmstate_mstar_sctop;
}

static const TypeInfo mstar_sctop_types[] = {
    {
        .name           = TYPE_MSTAR_SCTOP,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MstarScTopState),
        .class_init     = mstar_sctop_class_init,
    },
};

DEFINE_TYPES(mstar_sctop_types)
