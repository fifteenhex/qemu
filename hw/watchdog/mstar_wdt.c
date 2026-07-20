/*
 * MStar/SigmaStar watchdog timer
 *
 * The watchdog at 0x1f006000 (mainline device-tree compatible
 * "sstar,infinity-wdt"; the vendor kernel reserves it in /proc/iomem
 * as /soc/watchdog). A free-running counter clocked off the crystal
 * counts up; when it reaches the programmed MAX period the block
 * resets the SoC. Software feeds it by writing the CLR register and
 * disarms it by programming MAX to zero.
 *
 * The register meanings are taken from the mainline msc313e watchdog
 * driver and confirmed against what the vendor kernel writes at boot -
 * it feeds the counter (CLR = 1) and then parks the block with a zero
 * MAX period, i.e. disabled - pending validation against real silicon:
 *
 *   0x00 CLR            write feeds / restarts the counter
 *   0x10 MAX_L / 0x14 MAX_H  timeout period, in counter ticks
 *
 * When MAX is non-zero the model arms a host timer that fires after
 * the corresponding wall-clock interval and performs the configured
 * watchdog action (a system reset by default); a CLR write restarts
 * it, and a zero MAX cancels it.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "system/watchdog.h"
#include "hw/watchdog/mstar_wdt.h"

#define MSTAR_WDT_CLR       0x00
#define MSTAR_WDT_MAX_L     0x10
#define MSTAR_WDT_MAX_H     0x14

/* Default counter clock: the 12MHz crystal the block is wired to */
#define MSTAR_WDT_DEFAULT_FREQ 12000000

static uint32_t mstar_wdt_max(MStarWdtState *s)
{
    return s->regs[MSTAR_WDT_MAX_L / 4] |
           ((uint32_t)s->regs[MSTAR_WDT_MAX_H / 4] << 16);
}

/* (Re)arm the reset timer from the current MAX period, or cancel it */
static void mstar_wdt_update(MStarWdtState *s)
{
    uint32_t max = mstar_wdt_max(s);

    if (max) {
        timer_mod(&s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  muldiv64(max, NANOSECONDS_PER_SECOND, s->freq));
    } else {
        timer_del(&s->timer);
    }
}

static void mstar_wdt_expire(void *opaque)
{
    watchdog_perform_action();
}

static uint64_t mstar_wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarWdtState *s = MSTAR_WDT(opaque);

    return s->regs[addr / 4];
}

static void mstar_wdt_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MStarWdtState *s = MSTAR_WDT(opaque);

    switch (addr) {
    case MSTAR_WDT_CLR:
        /* A feed restarts the counter, i.e. re-arms from zero */
        s->regs[addr / 4] = val;
        mstar_wdt_update(s);
        break;
    case MSTAR_WDT_MAX_L:
    case MSTAR_WDT_MAX_H:
        s->regs[addr / 4] = val;
        mstar_wdt_update(s);
        break;
    default:
        s->regs[addr / 4] = val;
        break;
    }
}

static const MemoryRegionOps mstar_wdt_ops = {
    .read = mstar_wdt_read,
    .write = mstar_wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_wdt_reset(DeviceState *dev)
{
    MStarWdtState *s = MSTAR_WDT(dev);

    memset(s->regs, 0, sizeof(s->regs));
    timer_del(&s->timer);
}

static void mstar_wdt_init(Object *obj)
{
    MStarWdtState *s = MSTAR_WDT(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_wdt_ops, s,
                          TYPE_MSTAR_WDT, MSTAR_WDT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, mstar_wdt_expire, s);
}

static const Property mstar_wdt_properties[] = {
    DEFINE_PROP_UINT32("freq", MStarWdtState, freq, MSTAR_WDT_DEFAULT_FREQ),
};

static void mstar_wdt_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_wdt_reset);
    device_class_set_props(dc, mstar_wdt_properties);
}

static const TypeInfo mstar_wdt_types[] = {
    {
        .name           = TYPE_MSTAR_WDT,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarWdtState),
        .instance_init  = mstar_wdt_init,
        .class_init     = mstar_wdt_class_init,
    },
};

DEFINE_TYPES(mstar_wdt_types)
