/*
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/misc/dragonball_pll.h"
#include "migration/vmstate.h"
#include "hw/irq.h"

/* For clk32 bit */
#include "sysemu/sysemu.h"
#include "qemu/timer.h"

#define OSCFREQ 32768
#define NSPERTICK (NANOSECONDS_PER_SECOND / OSCFREQ)

static uint64_t dragonball_pll_read(void *opaque, hwaddr addr, unsigned size)
{
    DragonBallPLLState *s = opaque;
    int clk32 = ((uint64_t)qemu_clock_get_ns(rtc_clock) / NSPERTICK) & 1;
    uint16_t val;

    switch(addr) {
    case DRAGONBALL_PLL_PLLCR:
        return s->pllcr;
    case DRAGONBALL_PLL_PLLFSR:
        val = (clk32 << 15) | s->pllfsr;
        if (size == 1)
            val >>= 8;
        return val;
    }

    return 0;
}

static void dragonball_pll_update(DragonBallPLLState *s)
{
    bool prescale;
    unsigned int pc, qc, divisor, freq;

    prescale = s->pllcr & DRAGONBALL_PLL_PLLCR_PRESC;
    qc = (s->pllfsr >> DRAGONBALL_PLL_PLLFSR_QC_SHIFT) & DRAGONBALL_PLL_PLLFSR_QC_MASK;
    pc = s->pllfsr & DRAGONBALL_PLL_PLLFSR_PC_MASK;
    divisor = (14 * (pc + 1)) + (qc + 1);
    freq = 32786 * divisor;

    if (prescale)
        freq /= 2;

    printf("\npllcr: 0x%04x, pllfsr: 0x%04x\n",
           (unsigned) s->pllcr, (unsigned) s->pllfsr);
    printf("prescale: %d, pc 0x%02x, qc 0x%02x, divisor: %d, freq: %dHz\n",
           prescale, pc, qc, divisor, freq);
}

static void dragonball_pll_write(void *opaque, hwaddr addr, uint64_t value,
        unsigned size)
{
    DragonBallPLLState *s = opaque;

    switch(addr) {
    case DRAGONBALL_PLL_PLLCR:
        s->pllcr = value;
        dragonball_pll_update(s);
        break;
    case DRAGONBALL_PLL_PLLFSR:
        s->pllfsr = (value & 0xFFF);
        dragonball_pll_update(s);
        break;
    }
}

static const MemoryRegionOps dragonball_pll_ops = {
    .read = dragonball_pll_read,
    .write = dragonball_pll_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_pll_reset(DeviceState *dev)
{
    DragonBallPLLState *s = DRAGONBALL_PLL(dev);

    s->pllcr = 0x2420;
    s->pllfsr = 0x0123;
}

static void dragonball_pll_realize(DeviceState *dev, Error **errp)
{
    DragonBallPLLState *s = DRAGONBALL_PLL(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &dragonball_pll_ops, s,
                          TYPE_DRAGONBALL_PLL, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static const VMStateDescription vmstate_dragonball_pll = {
    .name = "dragonball_pll",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
            VMSTATE_END_OF_LIST()
        }
};

static Property dragonball_pll_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void dragonball_pll_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->legacy_reset = dragonball_pll_reset;
    device_class_set_props(dc, dragonball_pll_properties);
    dc->realize = dragonball_pll_realize;
    dc->vmsd = &vmstate_dragonball_pll;
}

static const TypeInfo dragonball_pll_info = {
    .name          = TYPE_DRAGONBALL_PLL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DragonBallPLLState),
    .class_init    = dragonball_pll_class_init,
};

static void dragonball_pll_register_types(void)
{
    type_register_static(&dragonball_pll_info);
}

type_init(dragonball_pll_register_types)
