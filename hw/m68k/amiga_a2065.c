/*
 * Commodore A2065 Zorro II Ethernet card.
 *
 * A 64KB Zorro II I/O board carrying an AMD Am7990 LANCE and 32KB of
 * onboard RAM that the LANCE uses for its descriptor rings and packet
 * buffers.  The LANCE core is QEMU's shared PCnet/LANCE model; the
 * card provides its DMA path into the onboard RAM (with the big-endian
 * bus byte order the Am7990 sees on the Amiga), the RDP/RAP register
 * window, and the Zorro II autoconfig ROM so expansion.library finds
 * and configures it like the real card.
 *
 * The MAC address follows the real board: Commodore's 00:80:10 prefix
 * plus the low three bytes of the autoconfig serial number, which is
 * how a2065.device and the Linux a2065 driver derive it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "net/net.h"
#include "hw/net/pcnet.h"
#include "hw/m68k/amiga_a2065.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

/* board body: 64KB Zorro II I/O board */
#define A2065_BODY_SIZE     0x10000
#define A2065_LANCE_OFF     0x4000      /* RDP at +0, RAP at +2 */
#define A2065_RAM_OFF       0x8000
#define A2065_RAM_SIZE      0x8000      /* 32KB, wraps the LANCE DMA */
#define A2065_RAM_MASK      (A2065_RAM_SIZE - 1)

/* Zorro II autoconfig */
#define A2065_AUTOCONFIG_BASE   0xe80000
#define A2065_AUTOCONFIG_SIZE   0x100
#define ZORRO_MANUF_COMMODORE   0x0202  /* Commodore West Chester */
#define ZORRO_PROD_A2065        0x70
/* er_Type: Zorro II board (0xc0), size code 1 = 64KB */
#define A2065_ERT               (0xc0 | 0x01)

/* the LANCE's DMA into the onboard RAM; ledma-style byte handling */
static void a2065_mem_read(void *opaque, hwaddr addr, uint8_t *buf, int len,
                           int do_bswap)
{
    A2065State *s = AMIGA_A2065(opaque);
    int i;

    addr &= A2065_RAM_MASK;
    if (addr + len > A2065_RAM_SIZE) {
        len = A2065_RAM_SIZE - addr;
    }
    if (do_bswap) {
        memcpy(buf, s->rambuf + addr, len);
    } else {
        addr &= ~1;
        len &= ~1;
        memcpy(buf, s->rambuf + addr, len);
        for (i = 0; i < len; i += 2) {
            bswap16s((uint16_t *)(buf + i));
        }
    }
}

static void a2065_mem_write(void *opaque, hwaddr addr, uint8_t *buf, int len,
                            int do_bswap)
{
    A2065State *s = AMIGA_A2065(opaque);
    int i;

    addr &= A2065_RAM_MASK;
    if (addr + len > A2065_RAM_SIZE) {
        len = A2065_RAM_SIZE - addr;
    }
    if (do_bswap) {
        memcpy(s->rambuf + addr, buf, len);
    } else {
        addr &= ~1;
        len &= ~1;
        for (i = 0; i < len; i += 2) {
            uint16_t v = bswap16(*(uint16_t *)(buf + i));
            memcpy(s->rambuf + addr + i, &v, 2);
        }
    }
}

/* --- LANCE register window (RDP/RAP) --- */

static uint64_t a2065_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    A2065State *s = AMIGA_A2065(opaque);

    return pcnet_ioport_readw(&s->state, addr);
}

static void a2065_regs_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    A2065State *s = AMIGA_A2065(opaque);

    pcnet_ioport_writew(&s->state, addr, val);
}

static const MemoryRegionOps a2065_regs_ops = {
    .read = a2065_regs_read,
    .write = a2065_regs_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 2 },
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

/* --- Zorro II autoconfig --- */

/*
 * Configure the board at the base the OS wrote: map the 64KB body
 * there and drop out of the autoconfig window so the next slot (none)
 * reads open bus.
 */
static void a2065_configure(A2065State *s)
{
    if (s->base < 0x200000 || s->configured) {
        return;
    }
    s->configured = true;
    memory_region_add_subregion(get_system_memory(), s->base, &s->body);
    memory_region_set_enabled(&s->autoconfig, false);
}

static uint64_t a2065_autoconfig_read(void *opaque, hwaddr addr, unsigned size)
{
    A2065State *s = AMIGA_A2065(opaque);
    unsigned reg = addr & 0xff;
    unsigned byte = reg >> 2;
    uint8_t v = byte < sizeof(s->rom) ? s->rom[byte] : 0;
    uint8_t nibble = (reg & 2) ? (v & 0x0f) : (v >> 4);
    uint8_t result = nibble << 4;

    /* every register but er_Type is presented inverted */
    if (byte != 0) {
        result = ~result;
    }
    return result;
}

static void a2065_autoconfig_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    A2065State *s = AMIGA_A2065(opaque);
    unsigned reg = addr & 0xff;

    switch (reg) {
    case 0x48:  /* Zorro II ec_BaseAddress: the full byte is address 23-16 */
        s->base = (uint32_t)(val & 0xff) << 16;
        a2065_configure(s);
        break;
    case 0x4c:  /* shut up: the board disappears entirely */
        memory_region_set_enabled(&s->autoconfig, false);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps a2065_autoconfig_ops = {
    .read = a2065_autoconfig_read,
    .write = a2065_autoconfig_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 2 },
    .impl = { .min_access_size = 1, .max_access_size = 1 },
};

/* --- device --- */

static NetClientInfo net_a2065_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = pcnet_receive,
    .link_status_changed = pcnet_set_link_status,
};

static void a2065_reset(DeviceState *dev)
{
    A2065State *s = AMIGA_A2065(dev);

    pcnet_h_reset(&s->state);
}

static void a2065_realize(DeviceState *dev, Error **errp)
{
    A2065State *s = AMIGA_A2065(dev);
    PCNetState *p = &s->state;
    uint8_t *mac;
    uint32_t serial;

    /*
     * Force Commodore's OUI: the driver always derives the MAC as
     * 00:80:10 + the serial number's low 24 bits, so make the card's
     * serial and station address agree with that.
     */
    qemu_macaddr_default_if_unset(&p->conf.macaddr);
    mac = p->conf.macaddr.a;
    mac[0] = 0x00;
    mac[1] = 0x80;
    mac[2] = 0x10;
    serial = (mac[3] << 16) | (mac[4] << 8) | mac[5];

    s->rom[0] = A2065_ERT;                       /* er_Type */
    s->rom[1] = ZORRO_PROD_A2065;                /* er_Product */
    s->rom[4] = ZORRO_MANUF_COMMODORE >> 8;      /* er_Manufacturer hi */
    s->rom[5] = ZORRO_MANUF_COMMODORE & 0xff;    /* er_Manufacturer lo */
    s->rom[6] = serial >> 24;                    /* er_SerialNumber */
    s->rom[7] = serial >> 16;
    s->rom[8] = serial >> 8;
    s->rom[9] = serial;

    /* the 64KB board body: LANCE registers and onboard RAM */
    memory_region_init(&s->body, OBJECT(s), "a2065.body", A2065_BODY_SIZE);
    memory_region_init_io(&s->regs, OBJECT(s), &a2065_regs_ops, s,
                          "a2065.lance", 4);
    memory_region_add_subregion(&s->body, A2065_LANCE_OFF, &s->regs);
    memory_region_init_ram(&s->ram, OBJECT(s), "a2065.ram", A2065_RAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(&s->body, A2065_RAM_OFF, &s->ram);
    s->rambuf = memory_region_get_ram_ptr(&s->ram);

    /* the autoconfig window, live until the board is configured away */
    memory_region_init_io(&s->autoconfig, OBJECT(s), &a2065_autoconfig_ops, s,
                          "a2065.autoconfig", A2065_AUTOCONFIG_SIZE);
    memory_region_add_subregion(get_system_memory(), A2065_AUTOCONFIG_BASE,
                                &s->autoconfig);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &p->irq);
    p->phys_mem_read = a2065_mem_read;
    p->phys_mem_write = a2065_mem_write;
    p->dma_opaque = dev;
    pcnet_common_init(dev, p, &net_a2065_info);
}

static const VMStateDescription vmstate_a2065 = {
    .name = "amiga-a2065",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(state, A2065State, 0, vmstate_pcnet, PCNetState),
        VMSTATE_UINT32(base, A2065State),
        VMSTATE_BOOL(configured, A2065State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property a2065_properties[] = {
    DEFINE_NIC_PROPERTIES(A2065State, state.conf),
};

static void a2065_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = a2065_realize;
    device_class_set_legacy_reset(dc, a2065_reset);
    dc->vmsd = &vmstate_a2065;
    device_class_set_props(dc, a2065_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo a2065_info = {
    .name          = TYPE_AMIGA_A2065,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A2065State),
    .class_init    = a2065_class_init,
};

static void a2065_register_types(void)
{
    type_register_static(&a2065_info);
}

type_init(a2065_register_types)
