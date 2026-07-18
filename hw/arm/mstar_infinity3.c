/*
 * MStar/SigmaStar infinity3 (MSC313E) SoC
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The infinity3/MSC313E is a single-core Cortex-A7 camera SoC (no SSD20xD
 * display pipeline). It subclasses the common SoC base in mstar.c and adds the
 * on-die camera capture pipeline (ISP -> HVSP scaler -> scaler-DMA). Those
 * blocks are part of the chip, so they are present on every MSC313E board; a
 * board only differs in whether an actual sensor is wired to the SCCB bus (the
 * camera board attaches one, the BreadBee does not). The boards themselves live
 * in their own files (mstar_breadbee.c, mstar_msc313e_cam.c).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "qemu/timer.h"
#include "mstar-soc.h"

/* ------------------------------------------------------------------ SoC */

/*
 * infinity3 realize: chain to the common SoC realize, then add the MSC313E's
 * on-die camera capture blocks (present on every MSC313E board). The frame
 * timer is armed lazily on the first ISP frame-counter poll, and only fires if
 * a sensor has actually been configured - so on a board with no sensor wired
 * (the BreadBee) these blocks sit idle.
 */
static void mstar_infinity3_soc_realize(DeviceState *dev, Error **errp)
{
    MStarSoCState *s = MSTAR_SOC(dev);
    MStarSoCClass *sc = MSTAR_SOC_GET_CLASS(dev);

    sc->parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    /*
     * The scaler-DMA + ISP + HVSP capture pipeline (shared TYPE_MSTAR_CAMCAP).
     * infinity3 layout: scldma@1f280400, isppoll@1f242000, hvsp@1f260000, ISP
     * frame-counter at isppoll+0x1050. Frame-done IRQs share the "irq" mst-intc
     * lines 20 (scaler-DMA, SPI 52) and 25 (image-ISP, GIC 89).
     */
    object_initialize_child(OBJECT(s), "camcap", &s->camcap, TYPE_MSTAR_CAMCAP);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->camcap), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->camcap), 0, MSTAR_RIU_BASE + 0x280400);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->camcap), 1, MSTAR_RIU_BASE + 0x242000);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->camcap), 2, MSTAR_RIU_BASE + 0x260000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->camcap), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq),
                                        MSTAR_DISP_GOP_HWIRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->camcap), 1,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq),
                                        MSTAR_ISP_IMG_HWIRQ));

    /*
     * VIF: the sensor video-input front-end (csi@1f240800). Modelled as a proper
     * block (store/read-back + a 7-bit interrupt) so the firmware's bring-up
     * reads back what it wrote; its interrupt output is left for a future frame
     * source to drive via mstar_vif_frame_irq().
     */
    object_initialize_child(OBJECT(s), "vif", &s->vif, TYPE_MSTAR_VIF);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->vif), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->vif), 0, MSTAR_VIF_BASE);

    /*
     * The rest of the on-die camera capture/codec pipeline. These sit downstream
     * of the scaler/SCLDMA stall so the firmware does not reach them yet; give
     * each a named store/read-back stub (instead of the logging catch-all) so
     * the regions are identified and read-back consistent, ready to grow real
     * behaviour (trigger/done + the per-block interrupt below). DT names, bases
     * and GIC SPIs from cam.dtb:
     *   isp_sc_vif@263200            scaler-side VIF
     *   jpe@264000     SPI 0x3d      JPEG encoder
     *   mfe@264800     SPI 0x3c      multi-format (H.264) encoder
     *   vhe@265000     SPI 0x35      H.265 encoder (two banks 0x265000/0x265200)
     *   ive@2a4000     SPI 0x58      intelligent video engine (two banks)
     */
    static const struct {
        hwaddr off; uint32_t size; const char *name;
    } cam_stubs[] = {
        { 0x263200, 0x200, "mstar.isp_sc_vif" },
        { 0x264000, 0x200, "mstar.jpe" },
        { 0x264800, 0x200, "mstar.mfe" },
        { 0x265000, 0x400, "mstar.vhe" },
        { 0x2a4000, 0x400, "mstar.ive" },
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(cam_stubs); i++) {
        DeviceState *dev = qdev_new(TYPE_MSTAR_REGBANK);

        qdev_prop_set_uint32(dev, "size", cam_stubs[i].size);
        qdev_prop_set_string(dev, "name", cam_stubs[i].name);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0,
                        MSTAR_RIU_BASE + cam_stubs[i].off);
    }
}

static void mstar_infinity3_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    MStarSoCClass *sc = MSTAR_SOC_CLASS(oc);

    sc->info.cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    sc->info.num_cpus = 1;
    sc->info.timer_freq = 12000000;     /* xtal_div2 */
    sc->info.chip_id = 0xc2;            /* MSC313E */
    sc->info.clkgen_type = TYPE_MSC313_CLKGEN;
    sc->info.pinctrl_type = TYPE_MSC313_PINCTRL;
    /* Chain the common realize, then add the on-die camera capture blocks. */
    device_class_set_parent_realize(dc, mstar_infinity3_soc_realize,
                                    &sc->parent_realize);
}

static const TypeInfo mstar_infinity3_types[] = {
    {
        .name           = TYPE_MSTAR_INFINITY3_SOC,
        .parent         = TYPE_MSTAR_SOC,
        .class_init     = mstar_infinity3_soc_class_init,
    },
};

DEFINE_TYPES(mstar_infinity3_types)
