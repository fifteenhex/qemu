/*
 * MStar/SigmaStar on-die camera capture pipeline (fake frame delivery)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The scaler-DMA + ISP + HVSP capture blocks the camera firmware polls while
 * bringing up video capture, plus a timer that fakes ~25fps frame delivery.
 * Extracted from hw/arm/mstar_infinity3.c so it can be shared by the MSC313E
 * (infinity3) and mercury5 SoCs; the three block base offsets and the ISP
 * frame-counter offset are properties, since they differ between families.
 *
 * There is no real sensor, so the model keeps the firmware's verify/poll loops
 * happy: store/read-back registers, a toggling SCLDMA double-buffer status, a
 * frame counter that advances once capture is set up (armed lazily on the first
 * ISP frame-counter poll), a scaler clock heartbeat, and - opt-in via env - the
 * scaler-DMA / image-ISP frame-done interrupts. See the register notes inline;
 * the exact offsets were decoded from the vendor vmlinux (task #15 RE).
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

/* SCLDMA: double-buffer / frame-done status register (byte offset). */
#define CAMCAP_SCLDMA_STATUS    0xfc
/* HVSP: ISP frame-done interrupt status window (see the read handler). */
#define CAMCAP_HVSP_ISTAT1      0x4ac       /* int status 1 (bit14 = frame) */
#define CAMCAP_HVSP_CLKHB       0x5e8       /* scaler/idclk clock heartbeat */

static inline void camcap_iolog(MstarCamCapState *s, uint32_t base, hwaddr addr,
                                bool w, uint64_t val, unsigned size)
{
    if (mstar_iolog_first(base + addr, w)) {
        mstar_iolog(MSTAR_RIU_BASE + base + addr, w, val, size);
    }
}

/* ------------------------------------------------------------- SCLDMA */

static uint64_t camcap_scldma_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarCamCapState *s = opaque;

    camcap_iolog(s, s->scldma_base, addr, false, 0, size);
    if (addr == CAMCAP_SCLDMA_STATUS) {
        return s->scldma_status ^= 0xffff;      /* double-buffer status */
    }
    return s->scldma_regs[(addr >> 1) & 0xff];
}

static void camcap_scldma_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    MstarCamCapState *s = opaque;

    camcap_iolog(s, s->scldma_base, addr, true, val, size);
    s->scldma_regs[(addr >> 1) & 0xff] = val;
}

static const MemoryRegionOps camcap_scldma_ops = {
    .read = camcap_scldma_read,
    .write = camcap_scldma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* --------------------------------------------------------------- ISP */

/*
 * ISP core. The firmware verifies most of its config by write-then-read-back,
 * so plain store/read-back keeps those loops happy. The frame-counter triple
 * (framecnt_off-4/off/+4) returns the fake captured-frame counter, and reaching
 * it means capture is set up, so the frame timer is armed there.
 */
static uint64_t camcap_isppoll_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarCamCapState *s = opaque;

    camcap_iolog(s, s->isppoll_base, addr, false, 0, size);
    if (addr == s->framecnt_off || addr == s->framecnt_off - 4 ||
        addr == s->framecnt_off + 4) {
        if (!timer_pending(s->timer)) {
            timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 33);
        }
        return s->frame_count;
    }
    return s->isppoll_regs[(addr >> 1) & 0x1fff];
}

static void camcap_isppoll_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    MstarCamCapState *s = opaque;

    camcap_iolog(s, s->isppoll_base, addr, true, val, size);
    s->isppoll_regs[(addr >> 1) & 0x1fff] = val;
}

static const MemoryRegionOps camcap_isppoll_ops = {
    .read = camcap_isppoll_read,
    .write = camcap_isppoll_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* -------------------------------------------------------------- HVSP */

static uint64_t camcap_hvsp_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarCamCapState *s = opaque;

    camcap_iolog(s, s->hvsp_base, addr, false, 0, size);
    /*
     * SCL/ISP frame-done interrupt status. The kernel ISP ISR computes three
     * masks as (status & ~intmask) and drops the IRQ as "[ISP] False
     * interrupt?" if all are 0. The frame-complete handler runs on status1
     * bit14 (0x4000): report it set/unmasked/one-shot while a frame IRQ is
     * pending (cleared when the ISR reads it, to avoid a re-entry storm).
     */
    if (s->isp_frame_pending) {
        switch (addr) {
        case CAMCAP_HVSP_ISTAT1:
            s->isp_frame_pending = false;
            qemu_set_irq(s->isp_img_irq, 0);
            return 0x4000;
        case 0x4a0: case 0x4b4: case 0x528:     /* int-mask regs: unmasked */
        case 0x4bc: case 0x534:                 /* status 2/3: idle */
            return 0x0000;
        }
    }
    if (addr == CAMCAP_HVSP_CLKHB) {
        return s->hvsp_hb ^= 0xffff;            /* "clock is running" heartbeat */
    }
    return s->hvsp_regs[(addr >> 1) & 0xfff];
}

static void camcap_hvsp_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MstarCamCapState *s = opaque;

    camcap_iolog(s, s->hvsp_base, addr, true, val, size);
    s->hvsp_regs[(addr >> 1) & 0xfff] = val;
}

static const MemoryRegionOps camcap_hvsp_ops = {
    .read = camcap_hvsp_read,
    .write = camcap_hvsp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* ------------------------------------------------------- frame delivery */

/*
 * Fake a captured video frame ~25fps. Phase 0 (frame boundary): advance the
 * counter, mark the SCLDMA double-buffer ready, raise whichever frame-done IRQ
 * lines are enabled (opt-in via env, for exercising the ISR paths). Phase 1
 * (~1ms later): lower them - the mst-intc is level-triggered, so keep the high
 * window short. Then wait ~39ms for the next frame.
 *
 *   MSTAR_SCLDMA_IRQ - SCLINTR / scaler-DMA done
 *   MSTAR_ISP_IRQ    - image-ISP frame-done (drives the vendor ISP IntCount)
 * The frame counter + SCLDMA status also drive the app's poll() path with no
 * interrupt at all (default).
 */
static void camcap_tick(void *opaque)
{
    MstarCamCapState *s = opaque;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    if (s->frame_phase == 0) {
        s->frame_count++;
        s->scldma_status = 0xffff;
        if (getenv("MSTAR_SCLDMA_IRQ")) {
            qemu_set_irq(s->scldma_irq, 1);
        }
        if (getenv("MSTAR_ISP_IRQ")) {
            s->isp_frame_pending = true;
            qemu_set_irq(s->isp_img_irq, 1);
        }
        s->frame_phase = 1;
        timer_mod(s->timer, now + 1);
        return;
    }

    qemu_set_irq(s->scldma_irq, 0);
    qemu_set_irq(s->isp_img_irq, 0);
    s->frame_phase = 0;
    timer_mod(s->timer, now + 39);
}

/* ------------------------------------------------------------- device */

static void mstar_camcap_reset_hold(Object *obj, ResetType type)
{
    MstarCamCapState *s = MSTAR_CAMCAP(obj);

    memset(s->scldma_regs, 0, sizeof(s->scldma_regs));
    memset(s->isppoll_regs, 0, sizeof(s->isppoll_regs));
    memset(s->hvsp_regs, 0, sizeof(s->hvsp_regs));
    s->scldma_status = 0;
    s->hvsp_hb = 0;
    s->isp_frame_pending = false;
    s->frame_count = 0;
    s->frame_phase = 0;
    qemu_set_irq(s->scldma_irq, 0);
    qemu_set_irq(s->isp_img_irq, 0);
}

static void mstar_camcap_realize(DeviceState *dev, Error **errp)
{
    MstarCamCapState *s = MSTAR_CAMCAP(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->scldma, OBJECT(dev), &camcap_scldma_ops, s,
                          "mstar.scldma", 0x200);
    sysbus_init_mmio(sbd, &s->scldma);
    memory_region_init_io(&s->isppoll, OBJECT(dev), &camcap_isppoll_ops, s,
                          "mstar.isppoll", 0x4000);
    sysbus_init_mmio(sbd, &s->isppoll);
    memory_region_init_io(&s->hvsp, OBJECT(dev), &camcap_hvsp_ops, s,
                          "mstar.hvsp", 0x2000);
    sysbus_init_mmio(sbd, &s->hvsp);

    sysbus_init_irq(sbd, &s->scldma_irq);       /* idx 0: scaler-DMA done */
    sysbus_init_irq(sbd, &s->isp_img_irq);      /* idx 1: image-ISP frame-done */

    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, camcap_tick, s);
}

static const VMStateDescription vmstate_mstar_camcap = {
    .name = "mstar-camcap",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(scldma_regs, MstarCamCapState, 0x100),
        VMSTATE_UINT16_ARRAY(isppoll_regs, MstarCamCapState, 0x2000),
        VMSTATE_UINT16_ARRAY(hvsp_regs, MstarCamCapState, 0x1000),
        VMSTATE_UINT16(scldma_status, MstarCamCapState),
        VMSTATE_UINT16(hvsp_hb, MstarCamCapState),
        VMSTATE_BOOL(isp_frame_pending, MstarCamCapState),
        VMSTATE_UINT32(frame_count, MstarCamCapState),
        VMSTATE_INT32(frame_phase, MstarCamCapState),
        VMSTATE_TIMER_PTR(timer, MstarCamCapState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mstar_camcap_props[] = {
    DEFINE_PROP_UINT32("scldma-base", MstarCamCapState, scldma_base, 0x280400),
    DEFINE_PROP_UINT32("isppoll-base", MstarCamCapState, isppoll_base, 0x242000),
    DEFINE_PROP_UINT32("hvsp-base", MstarCamCapState, hvsp_base, 0x260000),
    DEFINE_PROP_UINT32("framecnt-off", MstarCamCapState, framecnt_off, 0x1050),
};

static void mstar_camcap_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mstar_camcap_realize;
    rc->phases.hold = mstar_camcap_reset_hold;
    dc->vmsd = &vmstate_mstar_camcap;
    device_class_set_props(dc, mstar_camcap_props);
}

static const TypeInfo mstar_camcap_types[] = {
    {
        .name           = TYPE_MSTAR_CAMCAP,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MstarCamCapState),
        .class_init     = mstar_camcap_class_init,
    },
};

DEFINE_TYPES(mstar_camcap_types)
