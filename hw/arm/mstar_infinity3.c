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

/* ------------------------------------------------------- camera capture */

/*
 * SCLDMA - the scaler output DMA that writes captured frames to DRAM on the
 * MSC313 camera pipeline (sensor -> CSI -> ISP -> HVSP scaler -> SCLDMA; see
 * the linux-chenxing camera page). The IP-camera firmware busy-polls the
 * double-buffer / frame-done status at offset 0xfc waiting for a completed
 * frame. There is no real sensor in the model, so toggle that status on each
 * read to let the capture "request controller" advance past the poll; other
 * registers just store/read back.
 */
static uint16_t mstar_scldma_regs[0x200 / 2];
static uint16_t mstar_scldma_status;

/*
 * Debug: while an ISP frame-done IRQ is pending (between the tick raising and
 * lowering it), MSTAR_ISP_TRACE logs every read the ISP/HVSP/SCLDMA regions
 * see - i.e. exactly the registers the kernel ISP ISR reads for its status
 * snapshot. This is how we locate the frame-done status registers.
 */
static bool mstar_isp_in_irq;
static bool mstar_isp_frame_pending;    /* frame-done status latched, one-shot */

static inline void isp_trace(hwaddr absaddr, unsigned size)
{
    if (mstar_isp_in_irq && getenv("MSTAR_ISP_TRACE")) {
        fprintf(stderr, "[isptrace] R %08x sz%u\n",
                (unsigned)(MSTAR_RIU_BASE + absaddr), size);
    }
}

static uint64_t mstar_scldma_read(void *opaque, hwaddr addr, unsigned size)
{
    if (mstar_iolog_first(0x280400 + addr, false)) mstar_iolog(MSTAR_RIU_BASE + 0x280400 + addr, false, 0, size);
    isp_trace(0x280400 + addr, size);
    if (addr == 0xfc) {
        return mstar_scldma_status ^= 0xffff;   /* double-buffer status */
    }
    return mstar_scldma_regs[(addr >> 1) & 0xff];
}

static void mstar_scldma_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    if (mstar_iolog_first(0x280400 + addr, true)) mstar_iolog(MSTAR_RIU_BASE + 0x280400 + addr, true, val, size);
    mstar_scldma_regs[(addr >> 1) & 0xff] = val;
}

static const MemoryRegionOps mstar_scldma_ops = {
    .read = mstar_scldma_read,
    .write = mstar_scldma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * ISP core (0x1f242000..0x1f246000). The camera firmware verifies most of its
 * ISP config by writing then reading back, so a plain store/read-back keeps
 * those loops happy. Two special cases:
 *  - the frame-counter triple at 0x1f24304c/50/54 (region offset 0x104c/50/54)
 *    returns the fake captured-frame counter, and reaching it means capture is
 *    set up, so we start delivering frames.
 */
#define ISP_BASE_OFF   0x2000               /* 0x1f242000 within this region */
#define ISP_FRAMECNT   (0x3050 - ISP_BASE_OFF)  /* 0x1050 */
static uint16_t mstar_isppoll_regs[0x4000 / 2];
/*
 * Image-ISP frame-done interrupt (GIC 89), fired by the frame tick when
 * MSTAR_ISP_IRQ is set. This runs the real kernel ISP ISR cleanly, but the ISR
 * then reads three interrupt-status registers and, finding them 0, logs
 * "[ISP] False interrupt?" and drops the frame. Decoded from vmlinux:
 *   THalISPGetIntStatus1 = ldrb [base+0x14] & 1   (status 1, bit0)
 *   THalISPGetIntStatus2 = ldrh [base+0x60] | ldrh[base+0x64]<<16
 *   THalISPGetIntStatus3 = ldr  [base3+0]
 * where "base" is *(global 0xc03fc018 + 0x28) - an ioremap set up by
 * THal_PQ_init_riu_base, NOT plain 0x1f242000 (setting 0x1f242014 had no
 * effect). To make a frame register we must map that bank and set the
 * frame-done status bit; that is the remaining ISP-modelling work (task #15).
 */

static uint64_t mstar_isppoll_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarSoCState *s = opaque;

    if (mstar_iolog_first(0x242000 + addr, false)) mstar_iolog(MSTAR_RIU_BASE + 0x242000 + addr, false, 0, size);
    isp_trace(0x242000 + addr, size);
    if (addr == ISP_FRAMECNT || addr == ISP_FRAMECNT - 4 ||
        addr == ISP_FRAMECNT + 4) {
        if (!timer_pending(s->scldma_timer)) {
            timer_mod(s->scldma_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 33);
        }
        return s->frame_count;
    }
    return mstar_isppoll_regs[(addr >> 1) & 0x1fff];
}

static void mstar_isppoll_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    if (mstar_iolog_first(0x242000 + addr, true)) mstar_iolog(MSTAR_RIU_BASE + 0x242000 + addr, true, val, size);
    mstar_isppoll_regs[(addr >> 1) & 0x1fff] = val;
}

static const MemoryRegionOps mstar_isppoll_ops = {
    .read = mstar_isppoll_read,
    .write = mstar_isppoll_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * HVSP / SCL scaler (0x1f260000..0x1f262000). Store/read-back for config, and
 * report the scaler/idclk clock-ready status (polled at 0x1f2605e8 by
 * Hal_HVSP_SetIdclkOnOff / Hal_SCLDMA_CLKInit) as always ready.
 */
static uint16_t mstar_hvsp_regs[0x2000 / 2];

static uint64_t mstar_hvsp_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarSoCState *s = opaque;

    if (mstar_iolog_first(0x260000 + addr, false)) mstar_iolog(MSTAR_RIU_BASE + 0x260000 + addr, false, 0, size);
    isp_trace(0x260000 + addr, size);
    /*
     * SCL/ISP frame-done interrupt status (base 0x1f260400). The kernel ISP ISR
     * computes three masks as (status & ~intmask) and drops the IRQ as
     * "[ISP] False interrupt?" if all three are 0. Decoded from vmlinux:
     *   mask1 = u16[0x4ac] & ~u16[0x4a0]
     *   mask2 = (u8[0x4bc]&0xf) & ~(u8[0x4b4]&0xf)
     *   mask3 = u16[0x534] & ~u16[0x528]
     * The frame-complete handler runs on mask1 bit 14 (0x4000): it advances the
     * ISP frame counter and pulls the captured frame via THalISPGetVDOSData.
     * So while an ISP frame IRQ is pending report status1 bit14 set, unmasked,
     * and one-shot (cleared once the ISR reads it) to avoid a re-entry storm.
     * Experimental (MSTAR_ISP_IRQ).
     */
    if (mstar_isp_frame_pending) {
        switch (addr) {
        case 0x4ac:                             /* int status 1: frame-done */
            /* One-shot ack: clear pending and deassert the level line now that
             * the ISR has taken the frame, so it does not re-enter and storm. */
            mstar_isp_frame_pending = false;
            mstar_isp_in_irq = false;
            qemu_set_irq(s->isp_img_irq, 0);
            return 0x4000;
        case 0x4a0: case 0x4b4: case 0x528:     /* int-mask regs: unmasked */
        case 0x4bc: case 0x534:                 /* status 2/3: idle */
            return 0x0000;
        }
    }
    if (addr == 0x5e8) {
        /* SCLDMA/HVSP clock heartbeat: toggle so a "clock is running" wait
         * (Hal_SCLDMA_CLKInit / Hal_HVSP_SetIdclkOnOff) sees it change. */
        static uint16_t hb;
        return hb ^= 0xffff;
    }
    return mstar_hvsp_regs[(addr >> 1) & 0xfff];
}

static void mstar_hvsp_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    if (mstar_iolog_first(0x260000 + addr, true)) mstar_iolog(MSTAR_RIU_BASE + 0x260000 + addr, true, val, size);
    mstar_hvsp_regs[(addr >> 1) & 0xfff] = val;
}

static const MemoryRegionOps mstar_hvsp_ops = {
    .read = mstar_hvsp_read,
    .write = mstar_hvsp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * Fake a captured video frame: the MSC313 camera scaler-DMA raises its
 * frame-done interrupt (GIC SPI 52 = "irq" mst-intc line 20) ~30x/s. Raise the
 * line, then lower it shortly after (the mst-intc is level-triggered, so a
 * zero-width pulse is lost); the driver ISR then dequeues a buffer.
 */
static void mstar_scldma_tick(void *opaque)
{
    MStarSoCState *s = opaque;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    /*
     * Two-phase ~25fps frame pulse. Phase 0 (frame boundary): advance the
     * captured-frame counter, mark the SCLDMA double-buffer ready, and raise
     * whichever frame-done interrupt lines are enabled. Phase 1 (~1ms later):
     * lower them again - the mst-intc is level-triggered, so we keep the high
     * window short to bound ISR re-entry while still not being a lost 0-width
     * pulse. Then wait ~39ms for the next frame.
     *
     * Interrupt lines (opt-in via env, for experimenting with the ISR paths):
     *   MSTAR_SCLDMA_IRQ - SCLINTR / scaler-DMA done  ("irq" line 20, GIC 84)
     *   MSTAR_ISP_IRQ    - image-ISP frame-done       ("irq" line 25, GIC 89),
     *                      the line that drives the vendor ISP IntCount.
     * The frame counter + SCLDMA status also drive the app's poll() path with
     * no interrupt at all (default).
     */
    if (s->frame_phase == 0) {
        s->frame_count++;
        mstar_scldma_status = 0xffff;
        if (getenv("MSTAR_SCLDMA_IRQ")) {
            qemu_set_irq(s->scldma_irq, 1);
        }
        if (getenv("MSTAR_ISP_IRQ")) {
            mstar_isp_in_irq = true;        /* window for MSTAR_ISP_TRACE */
            mstar_isp_frame_pending = true; /* latch a frame-done status */
            qemu_set_irq(s->isp_img_irq, 1);
        }
        s->frame_phase = 1;
        timer_mod(s->scldma_timer, now + 1);
        return;
    }

    qemu_set_irq(s->scldma_irq, 0);
    qemu_set_irq(s->isp_img_irq, 0);
    mstar_isp_in_irq = false;
    s->frame_phase = 0;
    timer_mod(s->scldma_timer, now + 39);
}

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

    memory_region_init_io(&s->scldma, OBJECT(s), &mstar_scldma_ops, s,
                          "mstar.scldma", 0x200);
    memory_region_add_subregion(get_system_memory(),
                                MSTAR_RIU_BASE + 0x280400, &s->scldma);
    memory_region_init_io(&s->isppoll, OBJECT(s), &mstar_isppoll_ops, s,
                          "mstar.isppoll", 0x4000);
    memory_region_add_subregion(get_system_memory(),
                                MSTAR_RIU_BASE + 0x242000, &s->isppoll);
    memory_region_init_io(&s->hvsp, OBJECT(s), &mstar_hvsp_ops, s,
                          "mstar.hvsp", 0x2000);
    memory_region_add_subregion(get_system_memory(),
                                MSTAR_RIU_BASE + 0x260000, &s->hvsp);
    /* SCLDMA frame-done IRQ shares the "irq" mst-intc line 20 (SPI 52). */
    s->scldma_irq = qdev_get_gpio_in(DEVICE(&s->intc_irq),
                                     MSTAR_DISP_GOP_HWIRQ);
    /* Image-ISP frame-done IRQ: "irq" mst-intc line 25 (GIC 89). */
    s->isp_img_irq = qdev_get_gpio_in(DEVICE(&s->intc_irq),
                                      MSTAR_ISP_IMG_HWIRQ);
    s->scldma_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mstar_scldma_tick, s);

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
