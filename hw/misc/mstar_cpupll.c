/*
 * MStar/SigmaStar CPU PLL
 *
 * The analog CPU PLL at 0x1f206400. The kernel derives the Cortex-A7
 * clock from it through the device tree's CLK_cpupll_clk, a
 * "sstar,complex-clock". Its recalc reads a loop divider and an output
 * divider and computes
 *
 *   cpu_hz = (216 MHz << 20) * 32 / (loop * (out_div + 1))
 *
 * The registers, 16-bit on the 4-byte RIU stride, relative to the base:
 *
 *   0x064 OUT_DIV               output divider (rate uses out_div + 1)
 *   0x148 LOOP_L / 0x14c LOOP_H loop divider, low / high half
 *   0x150, 0x15c, 0x160, 0x164 set-rate control (start / latch)
 *   0x174 LOCK                  bit 0: PLL locked
 *
 * Setting the rate (the cpufreq driver's clk_set_rate) writes the loop
 * divider, pulses the control registers and then spins reading the LOCK
 * register until bit 0 goes high; a real PLL sets it once it relocks.
 * Model the block as readback storage, seeded at reset with the 1.2 GHz
 * loop divider (out_div 0) the vendor bootloader leaves, and always
 * report the PLL as locked so set-rate completes immediately - otherwise
 * the kernel spins there forever.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/mstar_cpupll.h"

#define MSTAR_CPUPLL_LOOP_L     0x148
#define MSTAR_CPUPLL_LOOP_H     0x14c
#define MSTAR_CPUPLL_LOCK       0x174
#define MSTAR_CPUPLL_LOCK_DONE  (1 << 0)

/*
 * Loop divider for 1.2 GHz: (216 MHz << 20) * 32 / 1.2 GHz = 0x5c28f6,
 * with the output divider left at 0 (divide by out_div + 1 = 1).
 */
#define MSTAR_CPUPLL_LOOP_1200MHZ 0x5c28f6

static uint64_t mstar_cpupll_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarCpupllState *s = MSTAR_CPUPLL(opaque);

    if (addr == MSTAR_CPUPLL_LOCK) {
        /* The PLL relocks instantly here, so it always reads as locked */
        return s->regs[addr / 4] | MSTAR_CPUPLL_LOCK_DONE;
    }
    return s->regs[addr / 4];
}

static void mstar_cpupll_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MStarCpupllState *s = MSTAR_CPUPLL(opaque);

    s->regs[addr / 4] = val;
}

static const MemoryRegionOps mstar_cpupll_ops = {
    .read = mstar_cpupll_read,
    .write = mstar_cpupll_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_cpupll_reset(DeviceState *dev)
{
    MStarCpupllState *s = MSTAR_CPUPLL(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[MSTAR_CPUPLL_LOOP_L / 4] = MSTAR_CPUPLL_LOOP_1200MHZ & 0xffff;
    s->regs[MSTAR_CPUPLL_LOOP_H / 4] = MSTAR_CPUPLL_LOOP_1200MHZ >> 16;
}

static void mstar_cpupll_init(Object *obj)
{
    MStarCpupllState *s = MSTAR_CPUPLL(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_cpupll_ops, s,
                          TYPE_MSTAR_CPUPLL, MSTAR_CPUPLL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void mstar_cpupll_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_cpupll_reset);
}

static const TypeInfo mstar_cpupll_types[] = {
    {
        .name           = TYPE_MSTAR_CPUPLL,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarCpupllState),
        .instance_init  = mstar_cpupll_init,
        .class_init     = mstar_cpupll_class_init,
    },
};

DEFINE_TYPES(mstar_cpupll_types)
