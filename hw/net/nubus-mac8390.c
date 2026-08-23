/*
 * NuBus DP8390 ("mac8390") Ethernet card for classic 68k Macintoshes.
 *
 * Models the shared-memory, memory-mapped variant of the National
 * Semiconductor DP8390 used by the "Apple/Asante/sane" family of NuBus cards
 * (this one is a Macronix MX98803, decl-ROM name "Network_Ethernet_Apple_TFL",
 * MAC 00:A0:4B:xx).  Unlike an NE2000, the CPU does not use a programmed-I/O
 * data port: the DP8390's ring RAM is directly memory-mapped and accessed 16
 * bits at a time, and the DP8390 registers are memory-mapped with a "back4"
 * spacing - register R at reg_base + (15 - R) * 4, byte wide.
 *
 * Layout inside the card's NuBus slot space, matching Linux/U-Boot mac8390:
 *
 *   base  = slot_addr | ((slot & 0xf) << 20)         (Apple minor-space base)
 *   RAM   = base + MinorBaseOS (0xD0000, from the decl ROM)
 *   regs  = RAM  + 0x10000
 *
 * The declaration ROM is supplied as a NuBus "bus image" via the romfile=
 * property (the raw 64K chip dump spread onto byte lanes 0 and 2; see
 * scripts/mk-mac8390-busrom.py).  The register file is driven straight into
 * the reusable ne2000 core.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "hw/net/nubus-mac8390.h"

/* MinorBaseOS carried by this card's declaration ROM. */
#define MAC8390_MINOR_BASE  0x000D0000
/* DP8390 register window sits 0x10000 above the shared RAM (Apple layout). */
#define MAC8390_REG_OFF     0x00010000
/* The RAM decode fills the whole gap up to the registers; real RAM aliases
 * within it, which is exactly what mac8390_memsize() probes for. */
#define MAC8390_RAM_WINDOW  0x00010000

/*
 * DP8390 registers, "back4" spacing: register R at offset (15 - R) * 4, byte
 * wide.  Drive the shared ne2000 core directly.
 */
static uint64_t mac8390_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    NubusMac8390State *s = opaque;
    unsigned reg = 15 - ((addr >> 2) & 0xf);

    return ne2000_ioport_read(&s->ne2000, reg);
}

static void mac8390_reg_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    NubusMac8390State *s = opaque;
    unsigned reg = 15 - ((addr >> 2) & 0xf);

    ne2000_ioport_write(&s->ne2000, reg, val & 0xff);
}

static const MemoryRegionOps mac8390_reg_ops = {
    .read = mac8390_reg_read,
    .write = mac8390_reg_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 1 },
};

/*
 * Shared ring RAM.  Backed by the ne2000 core's mem[] so the DP8390 receive
 * path and the CPU see one buffer.  Accessed as a big-endian byte array (the
 * driver moves packets a 16-bit word at a time, MSB first); the real RAM is
 * small and aliases across the decoded window.
 */
static uint64_t mac8390_ram_read(void *opaque, hwaddr addr, unsigned size)
{
    NubusMac8390State *s = opaque;
    uint32_t mask = s->ramsize - 1;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | s->ne2000.mem[(addr + i) & mask];
    }
    return v;
}

static void mac8390_ram_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    NubusMac8390State *s = opaque;
    uint32_t mask = s->ramsize - 1;
    unsigned i;

    for (i = 0; i < size; i++) {
        s->ne2000.mem[(addr + i) & mask] = val >> (8 * (size - 1 - i));
    }
}

static const MemoryRegionOps mac8390_ram_ops = {
    .read = mac8390_ram_read,
    .write = mac8390_ram_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 2 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* DP8390 interrupt -> NuBus slot interrupt. */
static void mac8390_set_irq(void *opaque, int n, int level)
{
    NubusMac8390State *s = opaque;

    nubus_set_irq(&s->parent_obj, level);
}

/*
 * Receive filter.
 *
 * ne2000_receive() filters unicast against the NE2000 PROM copy in mem[0,2,
 * 4,...]; but a mac8390 card puts its transmit buffer at page 0 (tx_start_page
 * = 0), so that area holds the last transmitted frame, not the station address.
 * Filter here against the DP8390 PAR (s->phys, which the driver programs) using
 * the RXCR bits, then hand the frame to the core with its own filter forced
 * open so it just does the ring insertion.
 */
static ssize_t mac8390_receive(NetClientState *nc, const uint8_t *buf,
                               size_t size)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    NE2000State *s = qemu_get_nic_opaque(nc);
    ssize_t ret;
    uint8_t saved;

    if (!(s->rxcr & 0x10)) {                 /* not promiscuous */
        if (!memcmp(buf, bcast, 6)) {
            if (!(s->rxcr & 0x04)) {         /* broadcast not accepted */
                return size;
            }
        } else if (buf[0] & 0x01) {
            if (!(s->rxcr & 0x08)) {         /* multicast not accepted */
                return size;
            }
        } else if (memcmp(buf, s->phys, 6) != 0) {
            return size;                     /* unicast not addressed to us */
        }
    }

    saved = s->rxcr;
    s->rxcr |= 0x10;                         /* skip the core's mem[] filter */
    ret = ne2000_receive(nc, buf, size);
    s->rxcr = saved;
    return ret;
}

static NetClientInfo net_mac8390_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = mac8390_receive,
};

static void mac8390_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    NubusDevice *nd = NUBUS_DEVICE(dev);
    NubusMac8390State *s = NUBUS_MAC8390(dev);
    NubusMac8390Class *mc = NUBUS_MAC8390_GET_CLASS(dev);
    NE2000State *ne = &s->ne2000;
    hwaddr base;

    /* chain to TYPE_NUBUS_DEVICE realize (loads romfile, maps slot space) */
    mc->parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    if (s->ramsize < 0x1000 || s->ramsize > NE2000_MEM_SIZE ||
        (s->ramsize & (s->ramsize - 1))) {
        error_setg(errp, "ramsize must be a power of two in [4K, %uK]",
                   (unsigned)(NE2000_MEM_SIZE / 1024));
        return;
    }

    /*
     * Physical NuBus decode: slot base + MinorBaseOS, with the registers
     * 0x10000 above the RAM (the same slot-relative convention the Radius
     * framebuffer uses).  This is what an MMU-off accessor - U-Boot proper,
     * via the SPL's ROM-page-table translation - reaches.
     */
    base = MAC8390_MINOR_BASE;

    memory_region_init_io(&s->ram, OBJECT(dev), &mac8390_ram_ops, s,
                          "nubus-mac8390.ram", MAC8390_RAM_WINDOW);
    memory_region_add_subregion(&nd->slot_mem, base, &s->ram);

    memory_region_init_io(&s->reg, OBJECT(dev), &mac8390_reg_ops, s,
                          "nubus-mac8390.reg", 0x40);
    memory_region_add_subregion(&nd->slot_mem, base + MAC8390_REG_OFF,
                                &s->reg);

    /*
     * The Apple mac8390 driver addresses the card in the "slotted" logical
     * view, base | (slot << 20), which on real hardware the ROM's slot-space
     * MMU maps back onto the physical decode above.  QEMU does not model that
     * remap, so a driver running with its own flat/identity mapping (e.g.
     * Linux here) hits the slotted address directly - mirror the windows
     * there too so the card answers either way.
     */
    if (nd->slot) {
        hwaddr slotted = ((hwaddr)nd->slot << 20) + MAC8390_MINOR_BASE;

        memory_region_init_alias(&s->ram_slot, OBJECT(dev),
                                 "nubus-mac8390.ram-slot", &s->ram, 0,
                                 MAC8390_RAM_WINDOW);
        memory_region_add_subregion(&nd->slot_mem, slotted, &s->ram_slot);
        memory_region_init_alias(&s->reg_slot, OBJECT(dev),
                                 "nubus-mac8390.reg-slot", &s->reg, 0, 0x40);
        memory_region_add_subregion(&nd->slot_mem, slotted + MAC8390_REG_OFF,
                                    &s->reg_slot);
    }

    ne->irq = qemu_allocate_irq(mac8390_set_irq, s, 0);

    qemu_macaddr_default_if_unset(&ne->c.macaddr);
    ne2000_reset(ne);

    ne->nic = qemu_new_nic(&net_mac8390_info, &ne->c,
                           object_get_typename(OBJECT(dev)), dev->id,
                           &dev->mem_reentrancy_guard, ne);
    qemu_format_nic_info_str(qemu_get_queue(ne->nic), ne->c.macaddr.a);
}

static const VMStateDescription vmstate_mac8390 = {
    .name = "nubus-mac8390",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(ne2000, NubusMac8390State, 0, vmstate_ne2000,
                       NE2000State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property mac8390_properties[] = {
    DEFINE_NIC_PROPERTIES(NubusMac8390State, ne2000.c),
    /* populated ring RAM aliased across the decode window; Linux/U-Boot
     * mac8390 probe this to 32K, so back the whole ring 1:1 */
    DEFINE_PROP_UINT32("ramsize", NubusMac8390State, ramsize, 0x8000),
};

static void mac8390_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    NubusMac8390Class *mc = NUBUS_MAC8390_CLASS(oc);

    dc->desc = "NuBus DP8390 (mac8390) Ethernet";
    device_class_set_parent_realize(dc, mac8390_realize, &mc->parent_realize);
    dc->vmsd = &vmstate_mac8390;
    device_class_set_props(dc, mac8390_properties);
}

static const TypeInfo mac8390_type_info = {
    .name = TYPE_NUBUS_MAC8390,
    .parent = TYPE_NUBUS_DEVICE,
    .instance_size = sizeof(NubusMac8390State),
    .class_size = sizeof(NubusMac8390Class),
    .class_init = mac8390_class_init,
};

static void mac8390_register_types(void)
{
    type_register_static(&mac8390_type_info);
}

type_init(mac8390_register_types)
