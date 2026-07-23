/*
 * Commodore Amiga 3000.
 *
 * 68030 + 68882 at 25MHz, ECS chipset with 2MB chip RAM, motherboard
 * fast RAM below 0x08000000 behind the Ramsey memory controller, Fat
 * Gary bus glue, and a WD33C93A SCSI controller behind the SuperDMAC.
 *
 * Memory map (motherboard):
 *   0x00000000  chip RAM (2MB)
 *   0x00bfd000  CIA-B
 *   0x00bfe000  CIA-A
 *   0x00dd0000  SuperDMAC + WD33C93A SCSI
 *   0x00de0000  Ramsey control/version
 *   0x00de1000  Fat Gary
 *   0x00dff000  custom chips
 *   0x00f80000  Kickstart ROM (512KB)
 *   0x07ffffff  motherboard fast RAM, growing down from 0x08000000
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/m68k/amiga.h"
#include "hw/m68k/a3000_sdmac.h"
#include "hw/scsi/wd33c93.h"
#include "hw/core/or-irq.h"
#include "hw/m68k/amiga_a2065.h"
#include "net/net.h"
#include "target/m68k/cpu.h"

#define A3000_SUPERDMAC_BASE    0xdd0000
#define A3000_RAMSEY_BASE       0xde0000
#define A3000_FASTRAM_TOP       0x08000000

#define RAMSEY_CTRL             0x0003
#define RAMSEY_VERSION          0x0043
#define GARY_TOENB              0x1000
#define GARY_TOLW               0x1001
#define GARY_DEB                0x1002

#define RAMSEY_VERSION_07       0x0f

#define TYPE_A3000_MACHINE MACHINE_TYPE_NAME("a3000")
OBJECT_DECLARE_SIMPLE_TYPE(A3000MachineState, A3000_MACHINE)

struct A3000MachineState {
    AmigaMachineState parent_obj;

    MemoryRegion mobo;
    MemoryRegion z3_open_bus;
    uint8_t ramsey_ctrl;
    uint8_t gary[3];
};

/* Ramsey memory controller and Fat Gary bus glue */
static uint64_t a3000_mobo_read(void *opaque, hwaddr addr, unsigned size)
{
    A3000MachineState *s = opaque;

    switch (addr) {
    case RAMSEY_CTRL:
        return s->ramsey_ctrl;
    case RAMSEY_VERSION:
        return RAMSEY_VERSION_07;
    case GARY_TOENB:
    case GARY_TOLW:
    case GARY_DEB:
        return s->gary[addr - GARY_TOENB];
    default:
        qemu_log_mask(LOG_UNIMP,
                      "a3000: unimplemented mobo read 0x%04" HWADDR_PRIx "\n",
                      addr);
        return 0;
    }
}

static void a3000_mobo_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    A3000MachineState *s = opaque;

    switch (addr) {
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
                      "a3000: unimplemented mobo write 0x%04" HWADDR_PRIx
                      " = 0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps a3000_mobo_ops = {
    .read = a3000_mobo_read,
    .write = a3000_mobo_write,
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

static void a3000_board_init(AmigaMachineState *ams)
{
    A3000MachineState *s = A3000_MACHINE(ams);
    MachineState *machine = MACHINE(ams);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *sbic_dev, *sdmac_dev, *a2065_dev, *int2_dev;
    NICInfo *nd;

    /* motherboard fast RAM sits below 0x08000000, sized by Kickstart */
    if (machine->ram_size > 16 * MiB) {
        error_report("a3000: motherboard fast RAM is limited to 16MB");
        exit(1);
    }
    if (machine->ram_size) {
        memory_region_add_subregion(sysmem,
                                    A3000_FASTRAM_TOP - machine->ram_size,
                                    machine->ram);
    }

    memory_region_init_io(&s->mobo, OBJECT(s), &a3000_mobo_ops, s,
                          "a3000.mobo", 0x2000);
    memory_region_add_subregion(sysmem, A3000_RAMSEY_BASE, &s->mobo);

    /* WD33C93A SBIC behind the SuperDMAC, interrupting on INT2 */
    sbic_dev = qdev_new(TYPE_WD33C93);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sbic_dev), &error_fatal);
    sdmac_dev = qdev_new(TYPE_A3000_SDMAC);
    object_property_set_link(OBJECT(sdmac_dev), "sbic", OBJECT(sbic_dev),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sdmac_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(sdmac_dev), 0, A3000_SUPERDMAC_BASE);
    /*
     * The SuperDMAC and the A2065 both raise INT2, so OR them onto the
     * custom chip's single ports-irq (INT2) input.
     */
    int2_dev = qdev_new(TYPE_OR_IRQ);
    qdev_prop_set_uint16(int2_dev, "num-lines", 2);
    qdev_realize_and_unref(int2_dev, NULL, &error_fatal);
    qdev_connect_gpio_out(int2_dev, 0,
                          qdev_get_gpio_in_named(ams->custom, "ports-irq", 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(sdmac_dev), 0,
                       qdev_get_gpio_in(int2_dev, 0));
    scsi_bus_legacy_handle_cmdline(&WD33C93(sbic_dev)->bus);

    /* A2065 Ethernet: a Zorro II card, autoconfigured by the OS */
    a2065_dev = qdev_new(TYPE_AMIGA_A2065);
    nd = qemu_find_nic_info(TYPE_AMIGA_A2065, true, NULL);
    if (nd) {
        qdev_set_nic_properties(a2065_dev, nd);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(a2065_dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(a2065_dev), 0,
                       qdev_get_gpio_in(int2_dev, 1));

    /*
     * Zorro III configuration space: with no boards present,
     * expansion.library must read open bus (0xff, "no board") here or
     * the config chain scan dies with a bus error.
     */
    memory_region_init_io(&s->z3_open_bus, OBJECT(s), &amiga_open_bus_ops,
                          NULL, "a3000.z3-config-open-bus", 0x01000000);
    memory_region_add_subregion(sysmem, 0xff000000, &s->z3_open_bus);
}

static void a3000_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AmigaMachineClass *amc = AMIGA_MACHINE_CLASS(oc);

    mc->desc = "Commodore Amiga 3000 (68030)";
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->default_ram_size = 8 * MiB;
    mc->default_ram_id = "amiga.fastram";
    mc->block_default_type = IF_SCSI;

    amc->rom_base = 0xf80000;
    amc->rom_size = 512 * KiB;
    amc->chipram_size = 2 * MiB;
    /* Ramsey/Gary answer every cycle below the Zorro III space */
    amc->open_bus_size = 0x10000000;
    amc->board_init = a3000_board_init;
}

static const TypeInfo a3000_machine_types[] = {
    {
        .name          = TYPE_A3000_MACHINE,
        .parent        = TYPE_AMIGA_MACHINE,
        .instance_size = sizeof(A3000MachineState),
        .class_init    = a3000_machine_class_init,
    },
};

DEFINE_TYPES(a3000_machine_types)
