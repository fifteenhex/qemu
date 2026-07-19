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
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "hw/arm/mstarv7.h"
#include "hw/char/serial-mm.h"
#include "hw/misc/unimp.h"
#include "system/system.h"
#include "target/arm/cpu-qom.h"

static void mstarv7_soc_init(Object *obj)
{
    MStarV7SoCState *s = MSTARV7_SOC(obj);
    MStarV7SoCClass *msc = MSTARV7_SOC_GET_CLASS(obj);
    int i;

    for (i = 0; i < msc->num_cpus; i++) {
        object_initialize_child(obj, "cpu[*]", &s->cpus[i],
                                ARM_CPU_TYPE_NAME("cortex-a7"));
    }
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
         * Secondary cores are held in reset until released by software,
         * only the boot core comes out of reset running.
         */
        object_property_set_bool(cpu, "start-powered-off", i > 0,
                                 &error_abort);
        if (!qdev_realize(DEVICE(cpu), NULL, errp)) {
            return;
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

    /* TODO: wire up the interrupt once the GIC is modelled */
    serial_mm_init(get_system_memory(), MSTARV7_PM_UART_BASE,
                   MSTARV7_PM_UART_REGSHIFT, NULL, MSTARV7_PM_UART_BAUDBASE,
                   serial_hd(0), DEVICE_LITTLE_ENDIAN);
}

static void mstarv7_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mstarv7_soc_realize;
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
