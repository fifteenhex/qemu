/*
 * Amiga custom chip register block (Agnus/Denise/Paula).
 *
 * This models the pieces of the chipset that Kickstart needs to get
 * going: the Paula interrupt controller (INTENA/INTREQ mapped onto the
 * 68k IPL lines), the beam counters (VPOSR/VHPOSR, plus the vertical
 * blank interrupt), and the Paula serial port wired to a chardev.
 * Display, blitter, copper, audio and disk DMA are not implemented;
 * writes to their registers are stored so later models can pick them
 * up, reads of unimplemented registers are logged.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "hw/m68k/amiga_custom.h"
#include "migration/vmstate.h"

/* PAL timing: 227 colour clocks (~64us) per line, 312 lines per frame */
#define LINE_NS         64000
#define FRAME_LINES     312
#define FRAME_NS        (LINE_NS * FRAME_LINES)
#define LINE_CCKS       227

#define REG_BLTDDAT     0x000
#define REG_DMACONR     0x002
#define REG_VPOSR       0x004
#define REG_VHPOSR      0x006
#define REG_JOY0DAT     0x00a
#define REG_JOY1DAT     0x00c
#define REG_ADKCONR     0x010
#define REG_POTGOR      0x016
#define REG_SERDATR     0x018
#define REG_DSKBYTR     0x01a
#define REG_INTENAR     0x01c
#define REG_INTREQR     0x01e
#define REG_SERDAT      0x030
#define REG_SERPER      0x032
#define REG_POTGO       0x034
#define REG_DENISEID    0x07c
#define REG_DMACON      0x096
#define REG_INTENA      0x09a
#define REG_INTREQ      0x09c
#define REG_ADKCON      0x09e

#define SERDATR_TSRE    (1 << 12)
#define SERDATR_TBE     (1 << 13)
#define SERDATR_RBF     (1 << 14)

/*
 * Paula funnels the fifteen interrupt sources onto six 68k levels.
 */
static const uint16_t ipl_masks[7] = {
    [1] = INT_TBE | INT_DSKBLK | INT_SOFT,
    [2] = INT_PORTS,
    [3] = INT_COPER | INT_VERTB | INT_BLIT,
    [4] = INT_AUD0 | INT_AUD1 | INT_AUD2 | INT_AUD3,
    [5] = INT_RBF | INT_DSKSYN,
    [6] = INT_EXTER,
};

static void amiga_custom_update_irq(AmigaCustomState *s)
{
    uint16_t active = 0;
    int level;

    if (s->intena & INT_INTEN) {
        active = s->intreq & s->intena & 0x3fff;
    }
    for (level = 6; level > 0; level--) {
        if (active & ipl_masks[level]) {
            break;
        }
    }
    qemu_set_irq(s->ipl, level);
}

static void amiga_custom_post_int(AmigaCustomState *s, uint16_t bits)
{
    s->intreq |= bits;
    amiga_custom_update_irq(s);
}

static uint16_t setclr(uint16_t reg, uint16_t val, uint16_t mask)
{
    if (val & 0x8000) {
        return reg | (val & mask);
    }
    return reg & ~(val & mask);
}

/* --- beam counters --- */

static void amiga_custom_beam_pos(AmigaCustomState *s, uint32_t *vpos,
                                  uint32_t *hpos)
{
    int64_t t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->frame_origin_ns;

    if (t < 0) {
        t = 0;
    }
    t %= FRAME_NS;
    *vpos = t / LINE_NS;
    *hpos = (t % LINE_NS) * LINE_CCKS / LINE_NS;
}

static void amiga_custom_vblank(void *opaque)
{
    AmigaCustomState *s = opaque;

    s->frame_origin_ns += FRAME_NS;
    timer_mod(&s->vblank_timer, s->frame_origin_ns + FRAME_NS);
    amiga_custom_post_int(s, INT_VERTB);
}

/* --- serial port --- */

static int amiga_custom_serial_can_receive(void *opaque)
{
    AmigaCustomState *s = opaque;

    return (s->intreq & INT_RBF) ? 0 : 1;
}

static void amiga_custom_serial_receive(void *opaque, const uint8_t *buf,
                                        int size)
{
    AmigaCustomState *s = opaque;

    s->serial_rx = buf[0];
    amiga_custom_post_int(s, INT_RBF);
}

/* --- register access --- */

static uint64_t amiga_custom_read(void *opaque, hwaddr addr, unsigned size)
{
    AmigaCustomState *s = opaque;
    uint32_t vpos, hpos;

    switch (addr & 0x1fe) {
    case REG_BLTDDAT:
        return 0;
    case REG_DMACONR:
        /* blitter never busy, always produced a zero result */
        return s->dmacon;
    case REG_VPOSR:
        amiga_custom_beam_pos(s, &vpos, &hpos);
        return 0x8000 | (s->agnus_id << 8) | ((vpos >> 8) & 7);
    case REG_VHPOSR:
        amiga_custom_beam_pos(s, &vpos, &hpos);
        return ((vpos & 0xff) << 8) | (hpos & 0xff);
    case REG_JOY0DAT:
    case REG_JOY1DAT:
        return 0;
    case REG_ADKCONR:
        return s->adkcon;
    case REG_POTGOR:
        /* all pot lines high: no mouse buttons pressed */
        return 0xffff;
    case REG_SERDATR:
        return SERDATR_TBE | SERDATR_TSRE |
               ((s->intreq & INT_RBF) ? SERDATR_RBF | 0x100 | s->serial_rx
                                      : 0);
    case REG_DSKBYTR:
        return 0;
    case REG_INTENAR:
        return s->intena;
    case REG_INTREQR:
        return s->intreq;
    case REG_DENISEID:
        return 0xff00 | s->denise_id;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "amiga-custom: unimplemented read 0x%03" HWADDR_PRIx "\n",
                      addr & 0x1fe);
        return 0;
    }
}

static void amiga_custom_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    AmigaCustomState *s = opaque;
    uint8_t ch;

    switch (addr & 0x1fe) {
    case REG_SERDAT:
        ch = val & 0xff;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        amiga_custom_post_int(s, INT_TBE);
        break;
    case REG_SERPER:
        s->serper = val;
        break;
    case REG_POTGO:
        s->potgo = val;
        break;
    case REG_DMACON:
        s->dmacon = setclr(s->dmacon, val, 0x07ff);
        break;
    case REG_INTENA:
        s->intena = setclr(s->intena, val, 0x7fff);
        amiga_custom_update_irq(s);
        break;
    case REG_INTREQ:
        s->intreq = setclr(s->intreq, val, 0x7fff);
        /* level-sensitive sources re-latch immediately */
        if (s->ciaa_level) {
            s->intreq |= INT_PORTS;
        }
        if (s->ciab_level) {
            s->intreq |= INT_EXTER;
        }
        amiga_custom_update_irq(s);
        break;
    case REG_ADKCON:
        s->adkcon = setclr(s->adkcon, val, 0x7fff);
        break;
    default:
        s->regs[(addr & 0x1fe) >> 1] = val;
        break;
    }
}

static const MemoryRegionOps amiga_custom_ops = {
    .read = amiga_custom_read,
    .write = amiga_custom_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 2,
        .max_access_size = 2,
    },
};

static void amiga_custom_cia_irq(void *opaque, int n, int level)
{
    AmigaCustomState *s = opaque;

    if (n == 0) {
        s->ciaa_level = level;
        if (level) {
            amiga_custom_post_int(s, INT_PORTS);
        }
    } else {
        s->ciab_level = level;
        if (level) {
            amiga_custom_post_int(s, INT_EXTER);
        }
    }
}

static void amiga_custom_reset(DeviceState *dev)
{
    AmigaCustomState *s = AMIGA_CUSTOM(dev);

    s->intena = 0;
    s->intreq = 0;
    s->dmacon = 0;
    s->adkcon = 0;
    s->serper = 0;
    s->potgo = 0;
    s->serial_rx = 0;
    s->ciaa_level = false;
    s->ciab_level = false;
    memset(s->regs, 0, sizeof(s->regs));
    s->frame_origin_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(&s->vblank_timer, s->frame_origin_ns + FRAME_NS);
    amiga_custom_update_irq(s);
}

static void amiga_custom_realize(DeviceState *dev, Error **errp)
{
    AmigaCustomState *s = AMIGA_CUSTOM(dev);

    timer_init_ns(&s->vblank_timer, QEMU_CLOCK_VIRTUAL,
                  amiga_custom_vblank, s);
    qemu_chr_fe_set_handlers(&s->chr, amiga_custom_serial_can_receive,
                             amiga_custom_serial_receive, NULL, NULL,
                             s, NULL, true);
}

static void amiga_custom_init(Object *obj)
{
    AmigaCustomState *s = AMIGA_CUSTOM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &amiga_custom_ops, s,
                          "amiga-custom", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->ipl);
    qdev_init_gpio_in_named(dev, amiga_custom_cia_irq, "cia-irq", 2);
}

static const VMStateDescription vmstate_amiga_custom = {
    .name = "amiga-custom",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(intena, AmigaCustomState),
        VMSTATE_UINT16(intreq, AmigaCustomState),
        VMSTATE_UINT16(dmacon, AmigaCustomState),
        VMSTATE_UINT16(adkcon, AmigaCustomState),
        VMSTATE_UINT16(serper, AmigaCustomState),
        VMSTATE_UINT16(potgo, AmigaCustomState),
        VMSTATE_UINT8(serial_rx, AmigaCustomState),
        VMSTATE_BOOL(ciaa_level, AmigaCustomState),
        VMSTATE_BOOL(ciab_level, AmigaCustomState),
        VMSTATE_UINT16_ARRAY(regs, AmigaCustomState, 0x100),
        VMSTATE_INT64(frame_origin_ns, AmigaCustomState),
        VMSTATE_TIMER(vblank_timer, AmigaCustomState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property amiga_custom_properties[] = {
    DEFINE_PROP_CHR("chardev", AmigaCustomState, chr),
    /* PAL ECS 2MB Agnus / ECS Denise by default */
    DEFINE_PROP_UINT32("agnus-id", AmigaCustomState, agnus_id, 0x22),
    DEFINE_PROP_UINT32("denise-id", AmigaCustomState, denise_id, 0xfc),
};

static void amiga_custom_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = amiga_custom_realize;
    dc->legacy_reset = amiga_custom_reset;
    dc->vmsd = &vmstate_amiga_custom;
    device_class_set_props(dc, amiga_custom_properties);
}

static const TypeInfo amiga_custom_info = {
    .name          = TYPE_AMIGA_CUSTOM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AmigaCustomState),
    .instance_init = amiga_custom_init,
    .class_init    = amiga_custom_class_init,
};

static void amiga_custom_register_types(void)
{
    type_register_static(&amiga_custom_info);
}

type_init(amiga_custom_register_types)
