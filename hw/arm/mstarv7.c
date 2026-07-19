/*
 * MStar/SigmaStar ARMv7 SoC base class
 *
 * The MStar/SigmaStar ARMv7 SoCs share a common lineage: one or two
 * Cortex-A7 cores, DRAM at the same base address and many hardware
 * blocks that reappear across the range. This abstract base class
 * holds what is common to every family; SoC families (infinity2m, ...)
 * subclass it and concrete SoCs subclass the families.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "hw/arm/mstarv7.h"
#include "hw/char/serial-mm.h"
#include "hw/intc/arm_gic_common.h"
#include "hw/misc/unimp.h"
#include "hw/core/qdev-properties.h"
#include "system/system.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/arm-powerctl.h"

/*
 * The "DID" block at 0x1f007000. Only DID_KEY, holding the boot-media
 * strap, is understood so far; everything else reads as zero.
 */
static uint64_t mstarv7_did_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarV7SoCState *s = MSTARV7_SOC(opaque);

    switch (addr) {
    case MSTARV7_DID_KEY:
        return s->did_key;
    default:
        qemu_log_mask(LOG_UNIMP, "mstarv7-did: unknown read 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void mstarv7_did_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "mstarv7-did: unknown write 0x%" HWADDR_PRIx
                  " = 0x%" PRIx64 "\n", addr, val);
}

static const MemoryRegionOps mstarv7_did_ops = {
    .read = mstarv7_did_read,
    .write = mstarv7_did_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * The MIU DDR controller. The IPL's DDR bring-up is fire and forget
 * for almost every register; the exceptions are the completion gates
 * (training, BIST) which read as done, and the DDR PLL frequency-set
 * pair which reads back what the IPL programs on real hardware.
 */
static uint64_t mstarv7_miu_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case MSTARV7_MIU_DIG_CNTRL0:
        return MSTARV7_MIU_DIG_CNTRL0_INITDONE;
    case MSTARV7_MIU_DIG_BIST_CTRL:
        /* Self test done, no error; emulated DRAM never fails */
        return MSTARV7_MIU_DIG_BIST_DONE;
    case MSTARV7_MIU_ANA_DDFSET_L:
        return MSTARV7_MIU_DDFSET_L_VALUE;
    case MSTARV7_MIU_ANA_DDFSET_H:
        return MSTARV7_MIU_DDFSET_H_VALUE;
    default:
        return 0;
    }
}

static void mstarv7_miu_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    /* No DRAM PHY to program; training writes are no-ops */
}

static const MemoryRegionOps mstarv7_miu_ops = {
    .read = mstarv7_miu_read,
    .write = mstarv7_miu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * The smpctrl secondary-core boot mailbox; see mstarv7.h.
 */
static uint64_t mstarv7_smpctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarV7SoCState *s = MSTARV7_SOC(opaque);

    return s->smpctrl[addr / 4];
}

static void mstarv7_smpctrl_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    MStarV7SoCState *s = MSTARV7_SOC(opaque);
    MStarV7SoCClass *msc = MSTARV7_SOC_GET_CLASS(opaque);

    s->smpctrl[addr / 4] = val;

    if (addr == MSTARV7_SMPCTRL_UNLOCK &&
        (val & 0xffff) == MSTARV7_SMPCTRL_UNLOCK_MAGIC &&
        msc->num_cpus > 1) {
        uint32_t entry = s->smpctrl[MSTARV7_SMPCTRL_BOOT_LOW / 4] |
                ((uint32_t)s->smpctrl[MSTARV7_SMPCTRL_BOOT_HIGH / 4] << 16);
        /*
         * Real hardware releases CPU1 from the mask ROM in Secure
         * state; enter at the highest exception level we have so the
         * Secure-only setup the kernel does on it works.
         */
        uint32_t el = arm_feature(&s->cpus[1].env, ARM_FEATURE_EL3) ? 3 : 1;
        int ret = arm_set_cpu_on(s->cpus[1].mp_affinity, entry, 0, el, false);

        if (ret != QEMU_ARM_POWERCTL_RET_SUCCESS) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mstarv7-smpctrl: failed to start CPU1 (%d)\n", ret);
        }
    }
}

static const MemoryRegionOps mstarv7_smpctrl_ops = {
    .read = mstarv7_smpctrl_read,
    .write = mstarv7_smpctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * The l3bridge write barrier: all writes are accepted and every
 * flush reads as already complete, which is true for us since our
 * memory accesses complete synchronously.
 */
static uint64_t mstarv7_l3bridge_read(void *opaque, hwaddr addr, unsigned size)
{
    if (addr == MSTARV7_L3BRIDGE_STATUS) {
        return MSTARV7_L3BRIDGE_STATUS_DONE;
    }
    return 0;
}

static void mstarv7_l3bridge_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
}

static const MemoryRegionOps mstarv7_l3bridge_ops = {
    .read = mstarv7_l3bridge_read,
    .write = mstarv7_l3bridge_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstarv7_soc_init(Object *obj)
{
    MStarV7SoCState *s = MSTARV7_SOC(obj);
    MStarV7SoCClass *msc = MSTARV7_SOC_GET_CLASS(obj);
    int i;

    for (i = 0; i < msc->num_cpus; i++) {
        object_initialize_child(obj, "cpu[*]", &s->cpus[i],
                                ARM_CPU_TYPE_NAME("cortex-a7"));
    }

    for (i = 0; i < MSTARV7_NUM_TIMERS; i++) {
        object_initialize_child(obj, "timer[*]", &s->timer[i],
                                TYPE_MSTAR_TIMER);
    }

    object_initialize_child(obj, "a7mpcore", &s->a7mpcore,
                            TYPE_A15MPCORE_PRIV);
    object_initialize_child(obj, "intc-irq", &s->intc_irq, TYPE_MSTAR_INTC);
    object_initialize_child(obj, "intc-fiq", &s->intc_fiq, TYPE_MSTAR_INTC);
    object_initialize_child(obj, "bdma", &s->bdma, TYPE_MSTAR_BDMA);
    object_initialize_child(obj, "fsp", &s->fsp, TYPE_MSTAR_FSP);
    object_initialize_child(obj, "sar", &s->sar, TYPE_MSTAR_SAR);

    for (i = 0; i < MSTARV7_NUM_I2C; i++) {
        object_initialize_child(obj, "i2c[*]", &s->i2c[i], TYPE_MSTAR_I2C);
    }

    object_initialize_child(obj, "clkgen", &s->clkgen, TYPE_MSTAR_REGBANK);
}

static void mstarv7_soc_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    MStarV7SoCState *s = MSTARV7_SOC(dev);
    MStarV7SoCClass *msc = MSTARV7_SOC_GET_CLASS(dev);
    int i;

    for (i = 0; i < msc->num_cpus; i++) {
        Object *cpu = OBJECT(&s->cpus[i]);

        object_property_set_int(cpu, "reset-cbar", MSTARV7_PERIPHBASE,
                                &error_abort);
        /*
         * On hardware every core runs the mask ROM and the
         * secondaries park in its smpctrl wait loop; the model keeps
         * them powered off until the mailbox posts an entry address.
         */
        object_property_set_bool(cpu, "start-powered-off", i > 0,
                                 &error_abort);
        if (!qdev_realize(DEVICE(cpu), NULL, errp)) {
            return;
        }
    }

    /*
     * The Cortex-A7 MPCore private region: SCU, GIC-400 distributor
     * at +0x1000 and CPU interface at +0x2000, and the timer PPIs.
     */
    qdev_prop_set_uint32(DEVICE(&s->a7mpcore), "num-cpu", msc->num_cpus);
    qdev_prop_set_uint32(DEVICE(&s->a7mpcore), "num-irq",
                         MSTARV7_GIC_NUM_SPI + GIC_INTERNAL);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->a7mpcore), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->a7mpcore), 0, MSTARV7_PERIPHBASE);
    for (i = 0; i < msc->num_cpus; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->a7mpcore);
        DeviceState *cpu = DEVICE(&s->cpus[i]);

        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(cpu, ARM_CPU_IRQ));
        sysbus_connect_irq(sbd, i + msc->num_cpus,
                           qdev_get_gpio_in(cpu, ARM_CPU_FIQ));
        sysbus_connect_irq(sbd, i + 2 * msc->num_cpus,
                           qdev_get_gpio_in(cpu, ARM_CPU_VIRQ));
        sysbus_connect_irq(sbd, i + 3 * msc->num_cpus,
                           qdev_get_gpio_in(cpu, ARM_CPU_VFIQ));
    }

    for (i = 0; i < 2; i++) {
        MStarIntcState *intc = i ? &s->intc_fiq : &s->intc_irq;
        unsigned int num = i ? MSTARV7_INTC_FIQ_NUM : MSTARV7_INTC_IRQ_NUM;
        unsigned int start = i ? MSTARV7_INTC_FIQ_START
                               : MSTARV7_INTC_IRQ_START;
        hwaddr base = i ? MSTARV7_INTC_FIQ_BASE : MSTARV7_INTC_IRQ_BASE;
        unsigned int line;

        qdev_prop_set_uint32(DEVICE(intc), "num-irqs", num);
        if (!sysbus_realize(SYS_BUS_DEVICE(intc), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(intc), 0, base);
        for (line = 0; line < num; line++) {
            /* The a7mpcore GPIO inputs are numbered by GIC SPI */
            sysbus_connect_irq(SYS_BUS_DEVICE(intc), line,
                               qdev_get_gpio_in(DEVICE(&s->a7mpcore),
                                                start + line));
        }
    }

    memory_region_init_ram(&s->imi, OBJECT(dev), "mstarv7.imi",
                           msc->imi_size, errp);
    if (*errp) {
        return;
    }
    memory_region_add_subregion(get_system_memory(), MSTARV7_IMI_BASE,
                                &s->imi);

    /*
     * Cover the register regions with low priority stubs so that
     * accesses to unmodelled registers read as zero and get logged
     * instead of faulting. Real device models overlay these.
     */
    create_unimplemented_device("mstarv7.riu",
                                MSTARV7_RIU_BASE, MSTARV7_RIU_SIZE);
    create_unimplemented_device("mstarv7.periphbase",
                                MSTARV7_PERIPHBASE, MSTARV7_PERIPHBASE_SIZE);

    memory_region_init_io(&s->did, OBJECT(dev), &mstarv7_did_ops, s,
                          "mstarv7.did", MSTARV7_DID_SIZE);
    memory_region_add_subregion(get_system_memory(), MSTARV7_DID_BASE,
                                &s->did);

    memory_region_init_io(&s->miu, OBJECT(dev), &mstarv7_miu_ops, s,
                          "mstarv7.miu", MSTARV7_MIU_SIZE);
    memory_region_add_subregion(get_system_memory(), MSTARV7_MIU_BASE,
                                &s->miu);

    memory_region_init_io(&s->smpctrl_mr, OBJECT(dev), &mstarv7_smpctrl_ops,
                          s, "mstarv7.smpctrl", MSTARV7_SMPCTRL_SIZE);
    memory_region_add_subregion(get_system_memory(), MSTARV7_SMPCTRL_BASE,
                                &s->smpctrl_mr);

    memory_region_init_io(&s->l3bridge, OBJECT(dev), &mstarv7_l3bridge_ops,
                          s, "mstarv7.l3bridge", MSTARV7_L3BRIDGE_SIZE);
    memory_region_add_subregion(get_system_memory(), MSTARV7_L3BRIDGE_BASE,
                                &s->l3bridge);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->bdma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->bdma), 0, MSTARV7_BDMA_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->fsp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fsp), 0, MSTARV7_FSP_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fsp), 1, MSTARV7_ISP_XIP_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sar), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sar), 0, MSTARV7_SAR_BASE);

    for (i = 0; i < MSTARV7_NUM_I2C; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->i2c[i]);

        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, MSTARV7_I2C_BASE + i * MSTARV7_I2C_STRIDE);
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->clkgen), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->clkgen), 0, MSTARV7_CLKGEN_BASE);

    serial_mm_init(get_system_memory(), MSTARV7_PM_UART_BASE,
                   MSTARV7_PM_UART_REGSHIFT,
                   qdev_get_gpio_in(DEVICE(&s->intc_irq),
                                    MSTARV7_PM_UART_INTC_IRQ),
                   MSTARV7_PM_UART_BAUDBASE, serial_hd(0),
                   DEVICE_LITTLE_ENDIAN);

    for (i = 0; i < MSTARV7_NUM_TIMERS; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->timer[i]);

        qdev_prop_set_uint32(DEVICE(sbd), "freq", msc->timer_freq);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0,
                        MSTARV7_TIMER_BASE + i * MSTARV7_TIMER_STRIDE);
    }
}

static const Property mstarv7_soc_properties[] = {
    DEFINE_PROP_UINT16("did-key", MStarV7SoCState, did_key,
                       MSTARV7_BOOT_MEDIA_SPI_NOR),
};

static void mstarv7_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mstarv7_soc_realize;
    device_class_set_props(dc, mstarv7_soc_properties);
    /* Reason: SoCs are only useful wired up inside a board */
    dc->user_creatable = false;
}

static const TypeInfo mstarv7_soc_types[] = {
    {
        .name           = TYPE_MSTARV7_SOC,
        .parent         = TYPE_DEVICE,
        .instance_size  = sizeof(MStarV7SoCState),
        .instance_init  = mstarv7_soc_init,
        .class_size     = sizeof(MStarV7SoCClass),
        .class_init     = mstarv7_soc_class_init,
        .abstract       = true,
    },
};

DEFINE_TYPES(mstarv7_soc_types)
