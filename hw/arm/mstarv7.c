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

/*
 * The chiptop block: the pinctrl pad-mux plus the chip straps. The
 * pad-mux registers read back what software writes; the package bond
 * strap at +0x120 reads the SoC's bond value.
 */
static uint64_t mstarv7_chiptop_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarV7SoCState *s = MSTARV7_SOC(opaque);
    MStarV7SoCClass *msc = MSTARV7_SOC_GET_CLASS(opaque);

    if (addr == MSTARV7_CHIPTOP_BOND) {
        return msc->bond;
    }
    return s->chiptop_regs[addr / 4];
}

static void mstarv7_chiptop_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    MStarV7SoCState *s = MSTARV7_SOC(opaque);

    s->chiptop_regs[addr / 4] = val;
}

static const MemoryRegionOps mstarv7_chiptop_ops = {
    .read = mstarv7_chiptop_read,
    .write = mstarv7_chiptop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * Bases of the display-pipeline config/timing banks, with the role
 * each was traced to (see display.rst). All plain readback banks.
 */
static const hwaddr mstarv7_disp_cfg_base[MSTARV7_NUM_DISP_CFG] = {
    MSTARV7_RIU_BASE + 0x224c00,    /* disp front: mux/enable, clip windows */
    MSTARV7_RIU_BASE + 0x224e00,    /* disp front: continued */
    MSTARV7_RIU_BASE + 0x225200,    /* panel (pnl) timing generator */
    MSTARV7_RIU_BASE + 0x226600,    /* mipi_tx_dsi / hdmi clock gates */
    MSTARV7_RIU_BASE + 0x246200,    /* GOP plane 0 config */
    MSTARV7_RIU_BASE + 0x246400,    /* GOP plane 1 config */
    MSTARV7_RIU_BASE + 0x281000,    /* GE front / display mux */
    MSTARV7_RIU_BASE + 0x281a00,    /* MOP overlay windows */
    MSTARV7_RIU_BASE + 0x283e00,    /* scaler front */
    MSTARV7_RIU_BASE + 0x284200,    /* scaler / colour plane 0 */
    MSTARV7_RIU_BASE + 0x284a00,    /* scaler / colour plane 1 */
    MSTARV7_RIU_BASE + 0x285200,    /* scaler / colour plane 2 */
    MSTARV7_RIU_BASE + 0x2a4a00,    /* mipi/dsi analog */
    MSTARV7_RIU_BASE + 0x2a4c00,    /* mipi/dsi analog */
    MSTARV7_RIU_BASE + 0x2a4e00,    /* mipi/dsi analog */
};

/*
 * Reset defaults for the readback banks, captured from a real SSD202D
 * (Miyoo Mini) via contrib/mstarpoker - the values these banks power up
 * holding, which the vendor code reads back. See VALIDATION.md; only the
 * non-zero registers are listed (the rest reset to 0). Registers the boot
 * ROM itself programs (clkgen +0xc4 / pm_clkgen +0x24, the UART pad-mux
 * and clock; see bootrom.rst) are deliberately excluded - they are not
 * reset values, and the ROM writes them on every boot regardless.
 *
 * clkgen +0xc4 reads 0x1108 on hardware but only 0x08 here: the ROM
 * writes 0x08 (the UART clock-mux select) and bits 0x1100 are read-only
 * strap/status bits the generic readback bank does not model. Left as a
 * known 1-register gap rather than special-casing it.
 */
static const MStarRegDefault mstarv7_clkgen_defaults[] = {
    { 0x008, 0x0001 }, { 0x00c, 0x0001 }, { 0x0cc, 0x1101 },
    { 0x0d0, 0x0211 }, { 0x0d4, 0x45a9 }, { 0x0dc, 0x0101 }, { 0x108, 0x0001 },
    { 0x114, 0x0001 }, { 0x118, 0x0101 }, { 0x144, 0x0001 }, { 0x148, 0x0001 },
    { 0x14c, 0x0101 }, { 0x150, 0x0001 }, { 0x154, 0x0101 }, { 0x158, 0x0101 },
    { 0x15c, 0x0001 }, { 0x180, 0x0001 }, { 0x184, 0x0001 }, { 0x18c, 0x0009 },
    { 0x1a8, 0x0001 }, { 0x1b0, 0x0100 }, { 0x1b4, 0x003d }, { 0x1b8, 0x0001 },
    { 0x1bc, 0x0001 }, { 0x1c4, 0xffff }, { 0x1cc, 0x0a40 }, { 0x1f8, 0x0101 },
    { 0x1fc, 0xffff },
};

static const MStarRegDefault mstarv7_pm_clkgen_defaults[] = {
    { 0x000, 0x0003 }, { 0x020, 0x00ff }, { 0x050, 0x000f },
    { 0x058, 0x003f }, { 0x070, 0x0300 }, { 0x07c, 0x1800 }, { 0x0b8, 0x00ff },
    { 0x0c0, 0x0003 }, { 0x0d4, 0x0008 }, { 0x0dc, 0xffff }, { 0x0ec, 0xffff },
    { 0x0f0, 0xffff }, { 0x0f4, 0x003f }, { 0x124, 0x0020 }, { 0x128, 0x000f },
    { 0x130, 0x000f }, { 0x158, 0x003f }, { 0x184, 0x3c3f }, { 0x188, 0x4000 },
    { 0x18c, 0x0600 }, { 0x190, 0x2000 }, { 0x194, 0x01ff }, { 0x198, 0x0006 },
    { 0x1bc, 0x0003 },
};

/*
 * chiptop reset defaults (base 0x1f203c00). The bond strap at +0x120 is
 * driven by the SoC class, not stored, so it is not listed here. The UART
 * pad-mux at +0x14c (0x1f203d4c) is written by the boot ROM and excluded.
 */
static const MStarRegDefault mstarv7_chiptop_defaults[] = {
    { 0x004, 0x0200 }, { 0x074, 0x7ff2 }, { 0x078, 0x800d }, { 0x080, 0xffff },
    { 0x088, 0xffff }, { 0x0c4, 0x7fff }, { 0x0c8, 0x7f00 }, { 0x0e0, 0xffff },
    { 0x0e4, 0x0fff }, { 0x0e8, 0xffff }, { 0x0ec, 0x0fff }, { 0x0f0, 0xffff },
    { 0x0f4, 0x0fff }, { 0x100, 0x0003 }, { 0x110, 0xffff }, { 0x114, 0xffff },
    { 0x118, 0xffff }, { 0x150, 0x0054 }, { 0x194, 0x1006 }, { 0x1cc, 0xffff },
    { 0x1f0, 0xffff }, { 0x1f4, 0x0003 }, { 0x1f8, 0x000f },
};

/* The chip-version bank: only +0x19c (0x1f003d9c) is known. */
static const MStarRegDefault mstarv7_chipver_defaults[] = {
    { MSTARV7_CHIPVER_REG, MSTARV7_CHIPVER_VALUE },
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
    s->clkgen.defaults = mstarv7_clkgen_defaults;
    s->clkgen.num_defaults = ARRAY_SIZE(mstarv7_clkgen_defaults);
    object_initialize_child(obj, "pm-clkgen", &s->pm_clkgen, TYPE_MSTAR_REGBANK);
    s->pm_clkgen.defaults = mstarv7_pm_clkgen_defaults;
    s->pm_clkgen.num_defaults = ARRAY_SIZE(mstarv7_pm_clkgen_defaults);
    object_initialize_child(obj, "chipver", &s->chipver, TYPE_MSTAR_REGBANK);
    s->chipver.defaults = mstarv7_chipver_defaults;
    s->chipver.num_defaults = ARRAY_SIZE(mstarv7_chipver_defaults);
    for (i = 0; i < MSTARV7_NUM_DISP_CFG; i++) {
        object_initialize_child(obj, "disp-cfg[*]", &s->disp_cfg[i],
                                TYPE_MSTAR_REGBANK);
    }
    object_initialize_child(obj, "cpupll", &s->cpupll, TYPE_MSTAR_CPUPLL);
    object_initialize_child(obj, "pwm", &s->pwm, TYPE_MSTAR_PWM);
    object_initialize_child(obj, "wdt", &s->wdt, TYPE_MSTAR_WDT);
    object_initialize_child(obj, "dsi", &s->dsi, TYPE_MSTAR_DSI);
    object_initialize_child(obj, "dphy", &s->dphy, TYPE_MSTAR_DPHY);
    object_initialize_child(obj, "disp", &s->disp, TYPE_MSTAR_DISP);
    object_initialize_child(obj, "ge", &s->ge, TYPE_MSTAR_GE);
    object_initialize_child(obj, "bach", &s->bach, TYPE_MSTAR_BACH);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_MSTAR_GPIO);
    object_initialize_child(obj, "rtcpwc", &s->rtcpwc, TYPE_MSTAR_RTCPWC);
    object_initialize_child(obj, "fcie", &s->fcie, TYPE_MSTAR_FCIE);
}

/*
 * The timers' counter-reached-MAX interrupts land on the "FIQ"
 * mst-intc: timer@6040/6080/60c0 on lines 0, 1 and 12.
 */
static const unsigned int mstarv7_timer_intc_fiq[MSTARV7_NUM_TIMERS] = {
    0, 1, 12
};

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

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pm_clkgen), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pm_clkgen), 0, MSTARV7_PM_CLKGEN_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->chipver), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->chipver), 0, MSTARV7_CHIPVER_BASE);

    for (i = 0; i < MSTARV7_NUM_DISP_CFG; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->disp_cfg[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->disp_cfg[i]), 0,
                        mstarv7_disp_cfg_base[i]);
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->cpupll), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->cpupll), 0, MSTARV7_CPUPLL_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pwm), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pwm), 0, MSTARV7_PWM_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->wdt), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->wdt), 0, MSTARV7_WDT_BASE);

    memory_region_init_io(&s->chiptop, OBJECT(dev), &mstarv7_chiptop_ops, s,
                          "mstarv7.chiptop", MSTARV7_CHIPTOP_SIZE);
    memory_region_add_subregion(get_system_memory(), MSTARV7_CHIPTOP_BASE,
                                &s->chiptop);
    for (i = 0; i < ARRAY_SIZE(mstarv7_chiptop_defaults); i++) {
        s->chiptop_regs[mstarv7_chiptop_defaults[i].offset / 4] =
            mstarv7_chiptop_defaults[i].value;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dsi), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dsi), 0, MSTARV7_DSI_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dphy), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dphy), 0, MSTARV7_DPHY_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->disp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->disp), 0, MSTARV7_DISP_GOP_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->disp), 1, MSTARV7_DISP_TOP_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->disp), 2, MSTARV7_DISP_MOP_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->disp), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq),
                                        MSTARV7_DISP_TOP_INTC_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->disp), 1,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq),
                                        MSTARV7_DISP_GOP_INTC_IRQ));

    object_property_set_uint(OBJECT(&s->ge), "dram-base", MSTARV7_MIU0_BASE,
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ge), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ge), 0, MSTARV7_DISP_GE_BASE);

    object_property_set_uint(OBJECT(&s->bach), "dram-base", MSTARV7_MIU0_BASE,
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->bach), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->bach), 0, MSTARV7_BACH_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->bach), 1, MSTARV7_AUDIOTOP_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->bach), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq),
                                        MSTARV7_BACH_INTC_IRQ));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio), 0, MSTARV7_GPIO_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio), 1, MSTARV7_PM_GPIO_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtcpwc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rtcpwc), 0, MSTARV7_RTCPWC_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->fcie), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fcie), 0, MSTARV7_FCIE_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->fcie), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq),
                                        MSTARV7_FCIE_INTC_IRQ));

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
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(DEVICE(&s->intc_fiq),
                                            mstarv7_timer_intc_fiq[i]));
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
