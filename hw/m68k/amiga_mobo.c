/*
 * Amiga big-box motherboard glue: the Ramsey memory controller and the
 * Fat Gary bus interface, which sit together in the 0x00de0000 page on
 * the A3000 and A4000.  Kickstart reads Ramsey's version to identify the
 * memory controller and pokes Gary's timeout registers during early
 * bus setup; both are otherwise just a handful of readable/writable
 * bytes as far as the OS is concerned.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/m68k/amiga_mobo.h"
#include "migration/vmstate.h"

#define GARY_TIMEOUT            0x0000
#define RAMSEY_CTRL             0x0003
#define RAMSEY_VERSION          0x0043
#define GARY_TOENB              0x1000
#define GARY_TOLW               0x1001
#define GARY_DEB                0x1002

#define AMIGA_MOBO_SIZE         0x2000

static uint64_t amiga_mobo_read(void *opaque, hwaddr addr, unsigned size)
{
    AmigaMoboState *s = opaque;

    switch (addr) {
    case GARY_TIMEOUT:
        /* bit 7: a watched bus access timed out since the last write */
        return (s->gary_timeout ? 0x80 : 0) | (s->gary_timeout_ctrl & 0x7f);
    case RAMSEY_CTRL:
        return s->ramsey_ctrl;
    case RAMSEY_VERSION:
        return s->ramsey_version;
    case GARY_TOENB:
    case GARY_TOLW:
    case GARY_DEB:
        return s->gary[addr - GARY_TOENB];
    default:
        qemu_log_mask(LOG_UNIMP,
                      "amiga-mobo: unimplemented read 0x%04" HWADDR_PRIx "\n",
                      addr);
        return 0;
    }
}

static void amiga_mobo_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    AmigaMoboState *s = opaque;

    switch (addr) {
    case GARY_TIMEOUT:
        /*
         * Writing configures the timeout response (bit 7 enables the
         * bus-error machine; scsi.device writes 0 to probe silently)
         * and clears the latch for the next watched access.
         */
        s->gary_timeout_ctrl = val;
        s->gary_timeout = false;
        break;
    case RAMSEY_CTRL:
        s->ramsey_ctrl = val;
        break;
    case GARY_TOENB:
    case GARY_TOLW:
    case GARY_DEB:
        /* only bit 7 is implemented in Gary's registers */
        s->gary[addr - GARY_TOENB] = val & 0x80;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "amiga-mobo: unimplemented write 0x%04" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps amiga_mobo_ops = {
    .read = amiga_mobo_read,
    .write = amiga_mobo_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * A vacant, Gary-watched slot: nothing answers, so the access times
 * out, the latch sets and the data bus floats (open bus reads).
 */
static uint64_t amiga_mobo_slot_read(void *opaque, hwaddr addr, unsigned size)
{
    AmigaMoboState *s = opaque;

    s->gary_timeout = true;
    return (uint64_t)-1;
}

static void amiga_mobo_slot_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    AmigaMoboState *s = opaque;

    s->gary_timeout = true;
}

static const MemoryRegionOps amiga_mobo_slot_ops = {
    .read = amiga_mobo_slot_read,
    .write = amiga_mobo_slot_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void amiga_mobo_realize(DeviceState *dev, Error **errp)
{
    AmigaMoboState *s = AMIGA_MOBO(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &amiga_mobo_ops, s,
                          "amiga.mobo", AMIGA_MOBO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    memory_region_init_io(&s->timeout_slot, OBJECT(s), &amiga_mobo_slot_ops,
                          s, "amiga.mobo.timeout-slot",
                          AMIGA_MOBO_TIMEOUT_SLOT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->timeout_slot);
}

static const VMStateDescription vmstate_amiga_mobo = {
    .name = "amiga-mobo",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(ramsey_ctrl, AmigaMoboState),
        VMSTATE_UINT8_ARRAY(gary, AmigaMoboState, 3),
        VMSTATE_UINT8(gary_timeout_ctrl, AmigaMoboState),
        VMSTATE_BOOL(gary_timeout, AmigaMoboState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property amiga_mobo_properties[] = {
    /* Ramsey-07 (0x0f) is what late A3000s and the A4000 carry */
    DEFINE_PROP_UINT8("ramsey-version", AmigaMoboState, ramsey_version, 0x0f),
};

static void amiga_mobo_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = amiga_mobo_realize;
    dc->vmsd = &vmstate_amiga_mobo;
    device_class_set_props(dc, amiga_mobo_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo amiga_mobo_info = {
    .name          = TYPE_AMIGA_MOBO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AmigaMoboState),
    .class_init    = amiga_mobo_class_init,
};

static void amiga_mobo_register_types(void)
{
    type_register_static(&amiga_mobo_info);
}

type_init(amiga_mobo_register_types)
