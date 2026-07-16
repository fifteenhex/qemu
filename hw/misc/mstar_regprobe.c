/*
 * MStar/SigmaStar register-probe blocks (clkgen, pinctrl)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A minimal reverse-engineering aid: a block of 16-bit registers (4-byte
 * stride) that stores/returns what the firmware writes and logs (LOG_UNIMP,
 * i.e. -d unimp) any access to a register - or any write of bits within a
 * register - that the mainline Linux driver does not describe. That surfaces
 * the parts of the clkgen and pinctrl blocks the firmware uses but the v6.5
 * kernel does not yet know about.
 *
 * The set of "described" registers is SoC- and block-specific, so there is a
 * concrete type per (block, SoC): the clkgen and pinctrl blocks each have an
 * msc313 (infinity3) and an ssd20xd (infinity2m/SSD202D) variant, whose tables
 * are derived from drivers/clk/mstar/clk-msc313-clkgen*.h and
 * drivers/pinctrl/mstar/pinctrl-*.h respectively.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/arm/mstar.h"

/* ------------------------------------------------------------ clkgen tables */

/*
 * clkgen mux registers: known = gate bit | parent-select field | deglitch bit,
 * as described by MSC313_MUX_PARENT_DATA(..., offset, gate, mux_shift,
 * mux_width, deglitch) in the Linux driver headers.
 */
static const MstarRegProbeReg msc313_clkgen_known[] = {
    { 0x04, "mcu/riubrdg", 0x0d1d },
    { 0x5c, "miu",         0x001d },
    { 0x64, "ddr_syn",     0x000d },
    { 0xc4, "uart0/1",     0x0d0d },
    { 0xc8, "spi",         0x001d },
    { 0xcc, "mspi0/1",     0x0d0d },
    { 0xd0, "fuart",       0x00dd },
    { 0xdc, "miic0/1",     0x0d0d },
    { 0x108, "emac_ahb",   0x000d },
    { 0x114, "sdio",       0x001d },
    { 0x180, "bdma",       0x001d },
    { 0x184, "aesdma/isp", 0x1d1d },
    { 0x1a8, "jpe",        0x000d },
};

/*
 * SSD20xD clkgen. The COMMON muxes are shared with msc313 except that mcu and
 * spi have a 3-bit parent field here (and a deglitch bit at 5), and there are
 * extra muxes: mspi_movedma (shares 0xcc), ge, sc_pixel, mipi_tx_dsi. The
 * mop/sata/dec/disp clocks the driver also lists were not resolved to
 * offsets and are intentionally left out, so they show up in the log.
 */
static const MstarRegProbeReg ssd20xd_clkgen_known[] = {
    { 0x04, "mcu(w3)/riubrdg", 0x0d3d },
    { 0x5c, "miu",             0x001d },
    { 0x64, "ddr_syn",         0x000d },
    { 0xc4, "uart0/1",         0x0d0d },
    { 0xc8, "spi(w3)",         0x003d },
    { 0xcc, "mspi0/1/movedma", 0xdd0d },
    { 0xd0, "fuart",           0x00dd },
    { 0xdc, "miic0/1",         0x0d0d },
    { 0x108, "emac_ahb",       0x000d },
    { 0x114, "sdio",           0x001d },
    { 0x144, "ge",             0x001d },
    { 0x180, "bdma",           0x001d },
    { 0x184, "aesdma",         0x001d },
    { 0x18c, "sc_pixel",       0x003d },
    { 0x1a8, "jpe",            0x000d },
    { 0x1bc, "mipi_tx_dsi",    0x001d },
};

/* ----------------------------------------------------------- pinctrl tables */

/*
 * pinctrl: the function-select mux registers plus the per-pin pull/drive
 * config registers (pinctrl-mstar.h REG_* / pinctrl-msc313.h). The bit layout
 * varies per pin, so only the register offset is tracked (known == whole reg).
 */
#define WHOLE 0xffff

static const MstarRegProbeReg msc313_pinctrl_known[] = {
    { 0x0c, "uarts",        WHOLE },
    { 0x1c, "pwms",         WHOLE },
    { 0x20, "sdio_nand",    WHOLE },
    { 0x24, "i2cs",         WHOLE },
    { 0x30, "spis",         WHOLE },
    { 0x3c, "eth_jtag",     WHOLE },
    { 0x54, "sensor_config", WHOLE },
    { 0x58, "tx_mipi_uart2", WHOLE },
    { 0x94, "i2c1_pull_en", WHOLE },
    { 0x98, "i2c1_pull_dir", WHOLE },
    { 0x9c, "i2c1_drive",   WHOLE },
    { 0xa8, "spi_drive",    WHOLE },
    { 0xc8, "sdio_pulldrive", WHOLE },
    { 0xe0, "sr_inputen0",  WHOLE },
    { 0xe4, "sr_inputen1",  WHOLE },
    { 0xe8, "sr_pull_en0",  WHOLE },
    { 0xec, "sr_pull_en1",  WHOLE },
    { 0xf0, "sr_pull_dir0", WHOLE },
    { 0xf4, "sr_pull_dir1", WHOLE },
    { 0xf8, "sr_drive0",    WHOLE },
    { 0xfc, "sr_drive1",    WHOLE },
};

/* SSD20xD: the common function/config registers, plus TTL/TX_MIPI (0x34) and
 * ETH (0x38); it has no sensor/BT656 pins (0x54/0x58). */
static const MstarRegProbeReg ssd20xd_pinctrl_known[] = {
    { 0x0c, "uarts",        WHOLE },
    { 0x1c, "pwms",         WHOLE },
    { 0x20, "sdio_nand",    WHOLE },
    { 0x24, "i2cs",         WHOLE },
    { 0x30, "spis",         WHOLE },
    { 0x34, "ttl/tx_mipi",  WHOLE },
    { 0x38, "eth/eth1",     WHOLE },
    { 0x3c, "eth_jtag",     WHOLE },
    { 0x94, "i2c1_pull_en", WHOLE },
    { 0x98, "i2c1_pull_dir", WHOLE },
    { 0x9c, "i2c1_drive",   WHOLE },
    { 0xa8, "spi_drive",    WHOLE },
    { 0xc8, "sdio_pulldrive", WHOLE },
    { 0xe0, "sr_inputen0",  WHOLE },
    { 0xe4, "sr_inputen1",  WHOLE },
    { 0xe8, "sr_pull_en0",  WHOLE },
    { 0xec, "sr_pull_en1",  WHOLE },
    { 0xf0, "sr_pull_dir0", WHOLE },
    { 0xf4, "sr_pull_dir1", WHOLE },
    { 0xf8, "sr_drive0",    WHOLE },
    { 0xfc, "sr_drive1",    WHOLE },
};

/* -------------------------------------------------------------- device core */

static const MstarRegProbeReg *mstar_regprobe_lookup(MstarRegProbeClass *c,
                                                     hwaddr off)
{
    unsigned int i;

    for (i = 0; i < c->n_known; i++) {
        if (c->known[i].offset == off) {
            return &c->known[i];
        }
    }
    return NULL;
}

static uint64_t mstar_regprobe_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarRegProbeState *s = opaque;
    MstarRegProbeClass *c = MSTAR_REGPROBE_GET_CLASS(s);

    if (!mstar_regprobe_lookup(c, addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "mstar-%s: read of register 0x%03x not in the v6.5 "
                      "driver\n", c->label, (unsigned)addr);
    }
    return s->regs[addr / 4];
}

static void mstar_regprobe_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    MstarRegProbeState *s = opaque;
    MstarRegProbeClass *c = MSTAR_REGPROBE_GET_CLASS(s);
    const MstarRegProbeReg *reg = mstar_regprobe_lookup(c, addr);

    if (!reg) {
        qemu_log_mask(LOG_UNIMP,
                      "mstar-%s: write 0x%04x to register 0x%03x not in the "
                      "v6.5 driver\n", c->label, (unsigned)(val & 0xffff),
                      (unsigned)addr);
    } else if (val & ~(uint64_t)reg->known) {
        qemu_log_mask(LOG_UNIMP,
                      "mstar-%s: write 0x%04x to %s (0x%03x) sets bits 0x%04x "
                      "the v6.5 driver does not describe\n", c->label,
                      (unsigned)(val & 0xffff), reg->name, (unsigned)addr,
                      (unsigned)(val & ~reg->known & 0xffff));
    }
    s->regs[addr / 4] = val;
}

static const MemoryRegionOps mstar_regprobe_ops = {
    .read = mstar_regprobe_read,
    .write = mstar_regprobe_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void mstar_regprobe_reset_hold(Object *obj, ResetType type)
{
    MstarRegProbeState *s = MSTAR_REGPROBE(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mstar_regprobe_realize(DeviceState *dev, Error **errp)
{
    MstarRegProbeState *s = MSTAR_REGPROBE(dev);
    MstarRegProbeClass *c = MSTAR_REGPROBE_GET_CLASS(s);

    memory_region_init_io(&s->iomem, OBJECT(dev), &mstar_regprobe_ops, s,
                          c->label, c->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void mstar_regprobe_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_regprobe_realize;
    rc->phases.hold = mstar_regprobe_reset_hold;
}

/* Fill in a concrete (block, SoC) type's table/size/label. */
static void mstar_msc313_clkgen_class_init(ObjectClass *oc, const void *data)
{
    MstarRegProbeClass *c = MSTAR_REGPROBE_CLASS(oc);

    c->label = "clkgen";
    c->size = MSTAR_CLKGEN_SIZE;
    c->known = msc313_clkgen_known;
    c->n_known = ARRAY_SIZE(msc313_clkgen_known);
}

static void mstar_ssd20xd_clkgen_class_init(ObjectClass *oc, const void *data)
{
    MstarRegProbeClass *c = MSTAR_REGPROBE_CLASS(oc);

    c->label = "clkgen";
    c->size = MSTAR_CLKGEN_SIZE;
    c->known = ssd20xd_clkgen_known;
    c->n_known = ARRAY_SIZE(ssd20xd_clkgen_known);
}

static void mstar_msc313_pinctrl_class_init(ObjectClass *oc, const void *data)
{
    MstarRegProbeClass *c = MSTAR_REGPROBE_CLASS(oc);

    c->label = "pinctrl";
    c->size = MSTAR_PINCTRL_SIZE;
    c->known = msc313_pinctrl_known;
    c->n_known = ARRAY_SIZE(msc313_pinctrl_known);
}

static void mstar_ssd20xd_pinctrl_class_init(ObjectClass *oc, const void *data)
{
    MstarRegProbeClass *c = MSTAR_REGPROBE_CLASS(oc);

    c->label = "pinctrl";
    c->size = MSTAR_PINCTRL_SIZE;
    c->known = ssd20xd_pinctrl_known;
    c->n_known = ARRAY_SIZE(ssd20xd_pinctrl_known);
}

static const TypeInfo mstar_regprobe_types[] = {
    {
        .name           = TYPE_MSTAR_REGPROBE,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MstarRegProbeState),
        .class_size     = sizeof(MstarRegProbeClass),
        .class_init     = mstar_regprobe_class_init,
        .abstract       = true,
    },
    {
        .name           = TYPE_MSC313_CLKGEN,
        .parent         = TYPE_MSTAR_REGPROBE,
        .class_init     = mstar_msc313_clkgen_class_init,
    },
    {
        .name           = TYPE_SSD20XD_CLKGEN,
        .parent         = TYPE_MSTAR_REGPROBE,
        .class_init     = mstar_ssd20xd_clkgen_class_init,
    },
    {
        .name           = TYPE_MSC313_PINCTRL,
        .parent         = TYPE_MSTAR_REGPROBE,
        .class_init     = mstar_msc313_pinctrl_class_init,
    },
    {
        .name           = TYPE_SSD20XD_PINCTRL,
        .parent         = TYPE_MSTAR_REGPROBE,
        .class_init     = mstar_ssd20xd_pinctrl_class_init,
    },
};

DEFINE_TYPES(mstar_regprobe_types)
