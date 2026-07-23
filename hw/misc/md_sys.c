/* SPDX-License-Identifier: MIT */
/*
 * QEMU Sega MegaDrive Z80 area and system control registers.
 *
 * There is no Z80 CPU here (yet): the 8 KB of sound RAM is readable and
 * writable so games can upload their sound driver, the bus arbiter always
 * grants the bus immediately, and the YM2612 accepts writes and always
 * reads back "not busy".  Enough for games whose 68k side just drives the
 * sound hardware and gets on with the game.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "migration/vmstate.h"
#include "system/reset.h"

#include "hw/misc/md_sys.h"

/* Offsets within the Z80 window at 0xA00000 */
#define MD_Z80_RAM_END      0x4000  /* 8 KB RAM, mirrored to 16 K */
#define MD_YM2612_BASE      0x4000
#define MD_YM2612_END       0x6000
#define MD_Z80_BANK         0x6000

/* Offsets within the control window at 0xA11000 */
#define MD_CTRL_MEMMODE     0x000
#define MD_CTRL_BUSREQ      0x100
#define MD_CTRL_RESET       0x200

static uint64_t md_sys_z80_read(void *opaque, hwaddr offset, unsigned size)
{
    MDSysState *s = MD_SYS(opaque);

    if (offset < MD_Z80_RAM_END) {
        return s->z80_ram[offset & (MD_SYS_Z80_RAM_SIZE - 1)];
    }
    if (offset >= MD_YM2612_BASE && offset < MD_YM2612_END) {
        /* YM2612 status: never busy, timers never expired */
        return 0x00;
    }
    qemu_log_mask(LOG_UNIMP,
        "md_sys: z80 area read at offset 0x%04" HWADDR_PRIx "\n", offset);
    return 0xFF;
}

static void md_sys_z80_write(void *opaque, hwaddr offset, uint64_t val,
                             unsigned size)
{
    MDSysState *s = MD_SYS(opaque);

    if (offset < MD_Z80_RAM_END) {
        s->z80_ram[offset & (MD_SYS_Z80_RAM_SIZE - 1)] = val & 0xFF;
        return;
    }
    if (offset >= MD_YM2612_BASE && offset < MD_YM2612_END) {
        unsigned port = (offset >> 1) & 1;

        if (offset & 1) {
            qemu_log_mask(LOG_UNIMP, "md_sys: ym2612 reg[%u][0x%02x] = 0x%02x\n",
                          port, s->ym_addr[port], (unsigned)(val & 0xFF));
        } else {
            s->ym_addr[port] = val & 0xFF;
        }
        return;
    }
    if (offset >= MD_Z80_BANK && offset < MD_Z80_BANK + 0x100) {
        /* Z80 banking register - no Z80, nothing to bank */
        return;
    }
    qemu_log_mask(LOG_UNIMP,
        "md_sys: z80 area write 0x%02" PRIx64 " at offset 0x%04" HWADDR_PRIx "\n",
        val, offset);
}

static uint64_t md_sys_ctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    MDSysState *s = MD_SYS(opaque);

    switch (offset & ~0xFF) {
    case MD_CTRL_BUSREQ:
        /*
         * Bit 0 (of the high byte on hardware, mirrored to both bytes by
         * most bus setups): 0 = bus granted to the 68k.  With no Z80 the
         * grant is instant; while the Z80 "runs" the line reads busy.
         */
        return s->z80_busreq ? 0x0000 : 0x0101;
    case MD_CTRL_RESET:
        return 0x0000;
    case MD_CTRL_MEMMODE:
        return 0x0000;
    default:
        qemu_log_mask(LOG_UNIMP,
            "md_sys: ctrl read at offset 0x%03" HWADDR_PRIx "\n", offset);
        return 0x0000;
    }
}

static void md_sys_ctrl_write(void *opaque, hwaddr offset, uint64_t val,
                              unsigned size)
{
    MDSysState *s = MD_SYS(opaque);

    switch (offset & ~0xFF) {
    case MD_CTRL_BUSREQ:
        s->z80_busreq = (val & 0x0101) != 0;
        break;
    case MD_CTRL_RESET:
        s->z80_reset = (val & 0x0101) == 0;
        break;
    case MD_CTRL_MEMMODE:
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
            "md_sys: ctrl write 0x%04" PRIx64 " at offset 0x%03" HWADDR_PRIx "\n",
            val, offset);
        break;
    }
}

static const MemoryRegionOps md_sys_z80_ops = {
    .read       = md_sys_z80_read,
    .write      = md_sys_z80_write,
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

static const MemoryRegionOps md_sys_ctrl_ops = {
    .read       = md_sys_ctrl_read,
    .write      = md_sys_ctrl_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

static const VMStateDescription vmstate_md_sys = {
    .name    = "md-sys",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(z80_ram, MDSysState, MD_SYS_Z80_RAM_SIZE),
        VMSTATE_BOOL(z80_busreq,     MDSysState),
        VMSTATE_BOOL(z80_reset,      MDSysState),
        VMSTATE_UINT8_ARRAY(ym_addr, MDSysState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void md_sys_reset(DeviceState *dev)
{
    MDSysState *s = MD_SYS(dev);

    memset(s->z80_ram, 0, sizeof(s->z80_ram));
    s->z80_busreq = false;
    s->z80_reset  = true;
    s->ym_addr[0] = 0;
    s->ym_addr[1] = 0;
}

static void md_sys_realize(DeviceState *dev, Error **errp)
{
    MDSysState   *s   = MD_SYS(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->z80_iomem, OBJECT(s), &md_sys_z80_ops, s,
                          "md-sys-z80", 0x10000);
    sysbus_init_mmio(sbd, &s->z80_iomem);

    memory_region_init_io(&s->ctrl_iomem, OBJECT(s), &md_sys_ctrl_ops, s,
                          "md-sys-ctrl", 0x300);
    sysbus_init_mmio(sbd, &s->ctrl_iomem);
}

static void md_sys_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize      = md_sys_realize;
    dc->legacy_reset = md_sys_reset;
    dc->vmsd         = &vmstate_md_sys;
    dc->desc         = "Sega MegaDrive Z80 area / system control";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo md_sys_info = {
    .name          = TYPE_MD_SYS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MDSysState),
    .class_init    = md_sys_class_init,
};

static void md_sys_register_types(void)
{
    type_register_static(&md_sys_info);
}

type_init(md_sys_register_types)
