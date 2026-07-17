/*
 * MStar/SigmaStar mercury5 SoCs (SSC8336, ...)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The mercury5 family is SigmaStar's dual Cortex-A7 camera/dashcam line (e.g.
 * the SSC8336, used in 70mai dashcams). It subclasses the common SoC base in
 * mstar.c like the other families; the boards live in their own machine files
 * (mstar_70mai.c).
 *
 * mercury5 is close to infinity3/MSC313E (the same ISP/SCL/VIF capture pipeline,
 * see the Mercury5 SDK headers we used for the camera work) but dual-core and
 * with its own clkgen/pinctrl register maps. The mask ROM loads the IPL from
 * flash offset 0 (MCR5 header) via the BDMA into IMI - gated by the boot-media
 * strap in did@7000 (see boot_strap below) - then the IPL brings up DRAM and
 * jumps to the kernel. With the chip-id + BOND strap below it boots all the way
 * through the IPL ("Chip:M5U ... Jump to Kernel,PC=>0x20008000").
 *
 * Block layout differences from infinity that the boot exercises (rest is shared
 * with the msc313 models):
 *   chip-id   0x1f003d98 (infinity: 0x1f003c00)  -> 0xee = "M5U"
 *   BOND      0x1f207818 GPIO-bank pad (infinity: chiptop+0x120)
 *   0x1f004xxx  USB utmi0 PHY (NOT a DDR/MIU controller)
 *   0x1f001c00  pmsleep; 0x1f200800 boot mailbox/scratch
 *   0x1f206000  mpll/miupll/lpll (clock+DRAM PLL setup)
 *   0x1f226xxx  MIU DDR-PHY DQS calibration
 *   0x1f221000  pm_uart (IPL console); 0x1f221200 uart1 (kernel console)
 * These are all currently served by the catch-all (reads 0/writes dropped),
 * which is enough for the IPL; a full kernel boot will need uart1 + a DTB.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "mstar-soc.h"

/*
 * mercury5-specific register shims, each layered (overlap) over the shared
 * banks to feed the vendor boot ROM / IPL / firmware a fixed value it polls for.
 * The firmware drives many blocks with a "write trigger, spin on a done/ready
 * bit" idiom; the underlying hardware isn't modelled, so we just report the
 * awaited bit(s) set and let the firmware march on.
 *
 *  - BOND strap (0x1f207818): unlike infinity (chiptop+0x120), the mercury5 IPL
 *    reads the package/DRAM strap from a GPIO-bank pad and uses the low nibble
 *    with the chip-id at 0x1f003d98: 0xee("M5U")+0xb -> MIU profile 0x31,
 *    0xd9("M5")+2 -> 4, else "Unknown BoundID to Miu [HALT]". 70mai (SSC8336N)
 *    is M5U, so report 0xb (chip 0xee is set via MStarSoCInfo).
 *  - uart1 status (0x1f221238 = serial@221200 + 0x38): the firmware's UART putc
 *    polls this MStar-native status reg - bit2 before programming the baud
 *    divisor, bit1 before each char - which QEMU's 16550 leaves at 0 (scratch
 *    reg). Report both set; chars still flow through the 16550 THR underneath.
 *  - dsi cmd-done (0x1f34420c = dsi@1f344200 + 0x0c): the firmware sends the
 *    ST7701S panel init commands over MIPI-DSI, spinning on bit1 for "sent".
 */
typedef struct Mercury5Shim {
    const char *name;
    hwaddr addr;
    uint64_t size;
    uint64_t val;
} Mercury5Shim;

static const Mercury5Shim mercury5_shims[] = {
    { "mstar.mercury5-bound",        MSTAR_RIU_BASE + 0x207818, 4, 0x0b },
    { "mstar.mercury5-uart1-status", MSTAR_RIU_BASE + 0x221238, 8, 0x06 },
    { "mstar.mercury5-dsi-done",     MSTAR_RIU_BASE + 0x34420c, 4, 0x02 },
};

static uint64_t mercury5_shim_read(void *opaque, hwaddr addr, unsigned size)
{
    return ((const Mercury5Shim *)opaque)->val;
}

static void mercury5_shim_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
}

static const MemoryRegionOps mercury5_shim_ops = {
    .read = mercury5_shim_read,
    .write = mercury5_shim_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_mercury5_soc_realize(DeviceState *dev, Error **errp)
{
    MStarSoCState *s = MSTAR_SOC(dev);
    MStarSoCClass *sc = MSTAR_SOC_GET_CLASS(dev);
    unsigned int i;

    sc->parent_realize(dev, errp);
    if (*errp) {
        return;
    }

    for (i = 0; i < ARRAY_SIZE(mercury5_shims); i++) {
        const Mercury5Shim *sh = &mercury5_shims[i];
        MemoryRegion *mr = g_new0(MemoryRegion, 1);

        memory_region_init_io(mr, OBJECT(s), &mercury5_shim_ops,
                              (void *)sh, sh->name, sh->size);
        memory_region_add_subregion_overlap(get_system_memory(), sh->addr,
                                            mr, 10);
    }
}

static void mstar_mercury5_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    MStarSoCClass *sc = MSTAR_SOC_CLASS(oc);

    /* Dual Cortex-A7 (secondary released via the smpctrl mailbox, as SSD20xD). */
    sc->info.cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    sc->info.num_cpus = 2;
    sc->info.has_display = false;       /* TODO: confirm from firmware/board */
    sc->info.timer_freq = 12000000;     /* TODO: confirm mercury5 PIT clock */
    sc->info.bond = 0;                  /* mercury5 reads BOND from a GPIO pad,
                                         * not chiptop+0x120; see the realize */
    sc->info.chip_id = 0xee;            /* IPL @0x1f003d98: 0xee -> "Chip:M5U" */
    sc->info.chipid_off = 0x198;       /* mercury5 chip-id is at 0x1f003d98 */
    /*
     * did@7000 reg 0x1c0: 0x20 = NOR boot media (bits[5:2]) plus bit 0x800,
     * which this ROM tests to load the IPL/MCR5 header from flash offset 0
     * (clear would make it use offset 0x8000). The 70mai image has its MCR5
     * partition header at offset 0, so the ROM must take the offset-0 path.
     */
    sc->info.boot_strap = 0x20 | 0x800;
    /*
     * TODO: mercury5 has its own clkgen/pinctrl register maps; reuse the
     * infinity3 (msc313) reg-probe tables for now (mercury5 is closest to it),
     * and add mercury5-specific tables once the firmware's register use is
     * captured.
     */
    sc->info.clkgen_type = TYPE_MSC313_CLKGEN;
    sc->info.pinctrl_type = TYPE_MSC313_PINCTRL;

    /* Chain the common realize, then add the mercury5-specific blocks. */
    device_class_set_parent_realize(dc, mstar_mercury5_soc_realize,
                                    &sc->parent_realize);
}

static void mstar_ssc8336_soc_class_init(ObjectClass *oc, const void *data)
{
    /*
     * SSC8336 (SSC8336N): a concrete mercury5 SoC. Inherits the family defaults
     * above; any SSC8336-specific info (chip-id, bond, extra blocks) is filled
     * in here once the boot ROM + firmware are available.
     */
}

static const TypeInfo mstar_mercury5_types[] = {
    {
        .name           = TYPE_MSTAR_MERCURY5_SOC,
        .parent         = TYPE_MSTAR_SOC,
        .class_init     = mstar_mercury5_soc_class_init,
        .abstract       = true,         /* a family base; use a concrete SoC */
    },
    {
        .name           = TYPE_MSTAR_SSC8336_SOC,
        .parent         = TYPE_MSTAR_MERCURY5_SOC,
        .class_init     = mstar_ssc8336_soc_class_init,
    },
};

DEFINE_TYPES(mstar_mercury5_types)
