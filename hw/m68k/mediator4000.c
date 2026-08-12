/*
 * ElBox Mediator 4000, the Zorro III to PCI bridge.
 *
 * Modelled on the hardware interface Daniel Palmer's Linux
 * host-controller driver (drivers/pci/controller/pci-mediator4000.c)
 * programs; the register names below are that driver's #defines.  The
 * bridge appears on the Zorro bus as two logical AutoConfig boards:
 *
 *  - the Control board (product 0x21, 16MB): the bridge registers at
 *    offset 0 (the window-base byte at +0, the INTx status/mask byte
 *    at +4), the PCI configuration space window at +0x800000 (2KB per
 *    device slot, 256 bytes per function, little-endian byte order)
 *    and the PCI I/O space window at +0xC00000;
 *
 *  - the Window board (product 0x21|0x80, 256MB): a movable window
 *    into PCI memory space.  The window-base register selects address
 *    bits 31-28 of where the window points; the driver points it at
 *    the window board's own Zorro address so that PCI BARs allocated
 *    inside the board's resource appear 1:1.
 *
 * PCI INTA-INTD are wired to slot number modulo 4 (as the driver's
 * map_irq expects) and collect into the INTx status nybble; enabled
 * lines (mask nybble) assert the Amiga's /INT2 (the PORTS interrupt).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_host.h"
#include "hw/m68k/amiga.h"
#include "hw/m68k/zorro3.h"
#include "migration/vmstate.h"
#include "trace.h"

/* register layout, from the Linux driver's #defines */
#define MEDIATOR4000_CONTROL_WINDOW         0x0
#define MEDIATOR4000_CONTROL_WINDOW_MSB(v)  (((v) >> 4) & 0xf)  /* bits 7-4 */
#define MEDIATOR4000_CONTROL_IRQ            0x4
#define MEDIATOR4000_CONTROL_IRQ_STATUS     0x0f    /* bits 3-0, RO */
#define MEDIATOR4000_CONTROL_IRQ_MASK       0xf0    /* bits 7-4 */
#define MEDIATOR4000_CONTROL_REGS_SIZE      8
#define MEDIATOR4000_PCICONF                0x800000
#define MEDIATOR4000_PCICONF_SIZE           (4 * MiB)
#define MEDIATOR4000_PCICONF_DEV_STRIDE     (2 * KiB)
#define MEDIATOR4000_PCICONF_FUNC_STRIDE    256
#define MEDIATOR4000_PCIIO                  0xc00000
#define MEDIATOR4000_PCIIO_SIZE             (1 * MiB)
#define MEDIATOR4000_MAX_SLOTS              6

/* the two AutoConfig boards (see zorro.ids, manufacturer ElBox 0x089e) */
#define ZORRO_MANUF_ELBOX                   0x089e
#define ZORRO_PROD_MEDIATOR4000_CONTROL     0x21
#define ZORRO_PROD_MEDIATOR4000_WINDOW      (0x21 | 0x80)
#define MEDIATOR4000_CONTROL_BOARD_SIZE     (16 * MiB)
#define MEDIATOR4000_WINDOW_BOARD_SIZE      (256 * MiB)

#define TYPE_MEDIATOR4000 "mediator4000"
OBJECT_DECLARE_SIMPLE_TYPE(Mediator4000State, MEDIATOR4000)

#define TYPE_MEDIATOR4000_PCI_HOST "mediator4000-pci-host"
OBJECT_DECLARE_SIMPLE_TYPE(Mediator4000PCIHost, MEDIATOR4000_PCI_HOST)

struct Mediator4000PCIHost {
    PCIHostState parent_obj;
    Mediator4000State *mediator;
};

struct Mediator4000State {
    Zorro3Device parent_obj;

    Mediator4000PCIHost pcihost;

    /* PCI memory and I/O spaces behind the bridge */
    MemoryRegion pci_mem;
    MemoryRegion pci_io;
    /* bus-master DMA sees only PCI space, never Amiga RAM (real bridge) */
    AddressSpace pci_dma_as;

    /* the Control board body and its windows */
    MemoryRegion control_body;
    MemoryRegion control_filler;
    MemoryRegion regs;
    MemoryRegion conf;
    MemoryRegion io_window;
    /* the Window board body: a movable alias into PCI memory space */
    MemoryRegion window_body;

    Zorro3Board control_board;
    Zorro3Board window_board;

    uint8_t window_reg;
    uint8_t irq_mask;       /* MEDIATOR4000_CONTROL_IRQ bits 7-4 */
    uint8_t intx_levels;    /* MEDIATOR4000_CONTROL_IRQ bits 3-0 */
};

static void mediator_update_irq(Mediator4000State *s)
{
    int level = !!(s->intx_levels & (s->irq_mask >> 4));

    trace_mediator4000_irq(s->intx_levels, s->irq_mask, level);
    zorro3_device_set_int2(ZORRO3_DEVICE(s), level);
}

/* --- bridge registers (Control board offset 0) --- */

static uint64_t mediator_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    Mediator4000State *s = opaque;

    switch (addr) {
    case MEDIATOR4000_CONTROL_WINDOW:
        return s->window_reg;
    case MEDIATOR4000_CONTROL_IRQ:
        return (s->irq_mask & MEDIATOR4000_CONTROL_IRQ_MASK) |
               (s->intx_levels & MEDIATOR4000_CONTROL_IRQ_STATUS);
    default:
        qemu_log_mask(LOG_UNIMP, "mediator4000: unimplemented register "
                      "read 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void mediator_regs_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    Mediator4000State *s = opaque;

    switch (addr) {
    case MEDIATOR4000_CONTROL_WINDOW:
        /*
         * Bits 7-4 select address bits 31-28 of where in PCI memory
         * space the Window board's window points.
         */
        s->window_reg = val;
        trace_mediator4000_window(MEDIATOR4000_CONTROL_WINDOW_MSB(val));
        memory_region_set_alias_offset(&s->window_body,
            (hwaddr)MEDIATOR4000_CONTROL_WINDOW_MSB(val) << 28);
        break;
    case MEDIATOR4000_CONTROL_IRQ:
        /* the status nybble is the live INTx lines, only the mask writes */
        s->irq_mask = val & MEDIATOR4000_CONTROL_IRQ_MASK;
        mediator_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "mediator4000: unimplemented register "
                      "write 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps mediator_regs_ops = {
    .read = mediator_regs_read,
    .write = mediator_regs_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 1 },
};

/* --- PCI configuration space window --- */

static PCIDevice *mediator_conf_device(Mediator4000State *s, hwaddr addr,
                                       unsigned *where)
{
    PCIHostState *phb = PCI_HOST_BRIDGE(&s->pcihost);
    unsigned slot = addr / MEDIATOR4000_PCICONF_DEV_STRIDE;
    unsigned func = (addr % MEDIATOR4000_PCICONF_DEV_STRIDE) /
                    MEDIATOR4000_PCICONF_FUNC_STRIDE;

    *where = addr % MEDIATOR4000_PCICONF_FUNC_STRIDE;
    if (slot >= MEDIATOR4000_MAX_SLOTS) {
        return NULL;
    }
    return pci_find_device(phb->bus, 0, PCI_DEVFN(slot, func));
}

static uint64_t mediator_conf_read(void *opaque, hwaddr addr, unsigned size)
{
    Mediator4000State *s = opaque;
    unsigned where;
    PCIDevice *d = mediator_conf_device(s, addr, &where);
    uint64_t val;

    if (!d) {
        /* master abort: the bus floats */
        return (uint64_t)-1;
    }
    val = pci_host_config_read_common(d, where, pci_config_size(d), size);
    trace_mediator4000_conf_read(PCI_SLOT(d->devfn), PCI_FUNC(d->devfn),
                                 where, size, val);
    return val;
}

static void mediator_conf_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    Mediator4000State *s = opaque;
    unsigned where;
    PCIDevice *d = mediator_conf_device(s, addr, &where);

    if (!d) {
        return;
    }
    trace_mediator4000_conf_write(PCI_SLOT(d->devfn), PCI_FUNC(d->devfn),
                                  where, size, val);
    pci_host_config_write_common(d, where, pci_config_size(d), val, size);
}

static const MemoryRegionOps mediator_conf_ops = {
    .read = mediator_conf_read,
    .write = mediator_conf_write,
    /* PCI configuration data is little-endian on the bus */
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* --- PCI interrupts --- */

static int mediator_map_irq(PCIDevice *pci_dev, int irq_num)
{
    /*
     * INTx line = slot number modulo four, matching the driver's
     * pci_mediator4000_map_irq ("my 4 slot board seems to just have
     * slot number == irq").
     */
    return PCI_SLOT(pci_dev->devfn) % PCI_NUM_PINS;
}

static void mediator_set_irq(void *opaque, int irq_num, int level)
{
    Mediator4000State *s = opaque;

    s->intx_levels = deposit32(s->intx_levels, irq_num, 1, !!level);
    mediator_update_irq(s);
}

/* --- the PCI host bridge proper --- */

static AddressSpace *mediator_pci_dma_as(PCIBus *bus, void *opaque, int devfn)
{
    Mediator4000State *s = opaque;

    return &s->pci_dma_as;
}

static const PCIIOMMUOps mediator_pci_dma_ops = {
    .get_address_space = mediator_pci_dma_as,
};

static void mediator_pci_host_realize(DeviceState *dev, Error **errp)
{
    Mediator4000PCIHost *h = MEDIATOR4000_PCI_HOST(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    Mediator4000State *s = h->mediator;

    phb->bus = pci_register_root_bus(dev, "pci", mediator_set_irq,
                                     mediator_map_irq, s, &s->pci_mem,
                                     &s->pci_io, PCI_DEVFN(0, 0),
                                     PCI_NUM_PINS, TYPE_PCI_BUS);

    /*
     * The real Mediator does not forward PCI-initiated (bus-master) cycles
     * back to the Amiga: a card can only reach PCI space -- other cards'
     * BARs and the bridge's own windows -- not chip or fast RAM.  Without
     * an IOMMU, QEMU would default device DMA to the system address space
     * (Amiga RAM), so root every device's DMA at the PCI memory space
     * instead.  Bus-master reads/writes to RAM addresses then hit
     * unassigned PCI memory and fail, exactly as on the hardware; the CPU
     * still sees everything, so drivers move data by hand (or via swiotlb).
     */
    address_space_init(&s->pci_dma_as, &s->pci_mem, "mediator4000.pci-dma");
    pci_setup_iommu(phb->bus, &mediator_pci_dma_ops, s);
}

static void mediator_pci_host_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mediator_pci_host_realize;
    /* an internal part of the mediator4000 device */
    dc->user_creatable = false;
}

/* --- the card --- */

static void mediator_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    Mediator4000State *s = MEDIATOR4000(dev);
    Zorro3Bus *bus = ZORRO3_BUS(qdev_get_parent_bus(dev));
    Zorro3DeviceClass *zdc = ZORRO3_DEVICE_GET_CLASS(dev);

    zdc->parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    /* PCI memory space (banked through the Window board) and I/O space */
    memory_region_init(&s->pci_mem, OBJECT(s), "mediator4000.pci-mem",
                       UINT64_C(1) << 32);
    memory_region_init(&s->pci_io, OBJECT(s), "mediator4000.pci-io",
                       MEDIATOR4000_PCIIO_SIZE);

    s->pcihost.mediator = s;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pcihost), errp)) {
        return;
    }

    /*
     * The Control board body.  Undecoded parts of the 16MB board read
     * open bus like the rest of the Zorro space.
     */
    memory_region_init(&s->control_body, OBJECT(s), "mediator4000.control",
                       MEDIATOR4000_CONTROL_BOARD_SIZE);
    memory_region_init_io(&s->control_filler, OBJECT(s), &amiga_open_bus_ops,
                          NULL, "mediator4000.control-filler",
                          MEDIATOR4000_CONTROL_BOARD_SIZE);
    memory_region_add_subregion_overlap(&s->control_body, 0,
                                        &s->control_filler, -1);
    memory_region_init_io(&s->regs, OBJECT(s), &mediator_regs_ops, s,
                          "mediator4000.regs", MEDIATOR4000_CONTROL_REGS_SIZE);
    memory_region_add_subregion(&s->control_body, 0, &s->regs);
    memory_region_init_io(&s->conf, OBJECT(s), &mediator_conf_ops, s,
                          "mediator4000.pciconf", MEDIATOR4000_PCICONF_SIZE);
    memory_region_add_subregion(&s->control_body, MEDIATOR4000_PCICONF,
                                &s->conf);
    memory_region_init_alias(&s->io_window, OBJECT(s), "mediator4000.pciio",
                             &s->pci_io, 0, MEDIATOR4000_PCIIO_SIZE);
    memory_region_add_subregion(&s->control_body, MEDIATOR4000_PCIIO,
                                &s->io_window);

    /* the Window board body: the movable window into PCI memory space */
    memory_region_init_alias(&s->window_body, OBJECT(s), "mediator4000.window",
                             &s->pci_mem, 0, MEDIATOR4000_WINDOW_BOARD_SIZE);

    s->control_board = (Zorro3Board) {
        .er_type = ZORRO3_ERT_ZORROIII | ZORRO3_ERT_EXT_16MB,
        .er_product = ZORRO_PROD_MEDIATOR4000_CONTROL,
        .er_flags = ZORRO3_ERFF_ZORRO_III | ZORRO3_ERFF_EXTENDED |
                    ZORRO3_ERF_SUBSIZE_MATCH,
        .er_manufacturer = ZORRO_MANUF_ELBOX,
        .size = MEDIATOR4000_CONTROL_BOARD_SIZE,
        .body = &s->control_body,
    };
    s->window_board = (Zorro3Board) {
        .er_type = ZORRO3_ERT_ZORROIII | ZORRO3_ERT_EXT_256MB,
        .er_product = ZORRO_PROD_MEDIATOR4000_WINDOW,
        .er_flags = ZORRO3_ERFF_ZORRO_III | ZORRO3_ERFF_EXTENDED |
                    ZORRO3_ERF_SUBSIZE_MATCH,
        .er_manufacturer = ZORRO_MANUF_ELBOX,
        .size = MEDIATOR4000_WINDOW_BOARD_SIZE,
        .body = &s->window_body,
    };
    zorro3_bus_add_board(bus, &s->control_board);
    zorro3_bus_add_board(bus, &s->window_board);
}

static void mediator_reset(DeviceState *dev)
{
    Mediator4000State *s = MEDIATOR4000(dev);

    s->window_reg = 0;
    s->irq_mask = 0;
    memory_region_set_alias_offset(&s->window_body, 0);
    mediator_update_irq(s);
}

static void mediator_init(Object *obj)
{
    Mediator4000State *s = MEDIATOR4000(obj);

    object_initialize_child(obj, "pci-host", &s->pcihost,
                            TYPE_MEDIATOR4000_PCI_HOST);
}

static const VMStateDescription vmstate_mediator4000 = {
    .name = "mediator4000",
    .unmigratable = 1,
};

static void mediator_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    Zorro3DeviceClass *zdc = ZORRO3_DEVICE_CLASS(klass);

    device_class_set_parent_realize(dc, mediator_realize,
                                    &zdc->parent_realize);
    device_class_set_legacy_reset(dc, mediator_reset);
    dc->vmsd = &vmstate_mediator4000;
    dc->desc = "ElBox Mediator 4000 Zorro III to PCI bridge";
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo mediator4000_types[] = {
    {
        .name          = TYPE_MEDIATOR4000,
        .parent        = TYPE_ZORRO3_DEVICE,
        .instance_size = sizeof(Mediator4000State),
        .instance_init = mediator_init,
        .class_init    = mediator_class_init,
    },
    {
        .name          = TYPE_MEDIATOR4000_PCI_HOST,
        .parent        = TYPE_PCI_HOST_BRIDGE,
        .instance_size = sizeof(Mediator4000PCIHost),
        .class_init    = mediator_pci_host_class_init,
    },
};

DEFINE_TYPES(mediator4000_types)
