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
 *   0x1f221000  pm_uart (IPL console); 0x1f221200 uart1 (RTOS/CLI console)
 * uart1 is modelled below (a MStar-native UART, not a plain 16550); the rest of
 * these are served by the catch-all (reads 0/writes dropped), which is enough
 * for the IPL and the RTOS to boot to its serial CLI.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/display/mstar_gop.h"
#include "hw/display/mstar_sctop.h"
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
 *  - dsi cmd-done (0x1f34420c = dsi@1f344200 + 0x0c): the firmware sends the
 *    ST7701S panel init commands over MIPI-DSI, spinning on bit1 for "sent".
 *
 * The uart1 status register (serial@221200 + 0x38) is handled separately below
 * because it needs to reflect the live RX state, not a constant.
 */
typedef struct Mercury5Shim {
    const char *name;
    hwaddr addr;
    uint64_t size;
    uint64_t val;
} Mercury5Shim;

static const Mercury5Shim mercury5_shims[] = {
    { "mstar.mercury5-bound",    MSTAR_RIU_BASE + 0x207818, 4, 0x0b },
    { "mstar.mercury5-dsi-done", MSTAR_RIU_BASE + 0x34420c, 4, 0x02 },
    /*
     * Boot/power-source status (read at RTOS 0x200a1b18): bit2 = external
     * power (car ACC/DC-in) present. The app's boot gate powers straight off
     * unless a boot reason exists (power key down, or this bit); the normal
     * dashcam flow - and the auto-record UI path - keys off external power.
     */
    { "mstar.mercury5-pwrsrc",   MSTAR_RIU_BASE + 0x006848, 1, 0x04 },
    /*
     * USB UTMI0 PHY calibration done (0x1f004478 bit1). The USB host bring-up
     * (MDrv_Usb_Init, after "1 [cfg bl:0]") programs the PHY at 0x1f004400 and
     * busy-waits on bit1 of 0x1f004478 for calibration-done (RTOS 0x2014bfb4);
     * with no PHY modelled the catch-all returns 0 and the boot hangs there.
     * Report done so the USB init - and the boot sequence past it - proceeds.
     */
    { "mstar.mercury5-usbphy-done", MSTAR_RIU_BASE + 0x004478, 4, 0x02 },
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

/*
 * HVSP1 (scaler engine 1, SDK bank 0x121100) queue-status window,
 * 0x1f2423e8..0x1f2423ff. The display/JPG_PB pipeline pushes frames through the
 * scaler and waits for completion by writing an index query + kick (bit12) into
 * a channel's command register and polling its status register until the
 * hardware's completed-frame counter echoes the requested index (RTOS wait loop
 * at 0x20236404; accessors read reg16 tokens 0x1211f4..0x1211fe). There is no
 * scaler here, so complete instantly: the status register echoes the last
 * command (kick bit stripped). Without this the pipeline re-kicks forever and
 * the boot screen never advances.
 *
 *   +0x00 ch0 command   +0x08 ch0 status (echo)
 *   +0x0c ch1 command   +0x14 ch1 status (echo)
 */
#define M5_HVSP1_QSTAT_BASE (MSTAR_RIU_BASE + 0x2423e8)
#define M5_HVSP1_QSTAT_SIZE 0x18

typedef struct Mercury5HvspQstat {
    MemoryRegion mr;
    uint16_t regs[M5_HVSP1_QSTAT_SIZE / 4];
    uint16_t count[2];          /* per-channel fake completed-frame counter */
} Mercury5HvspQstat;

static uint64_t mercury5_hvsp_qstat_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    Mercury5HvspQstat *q = opaque;

    uint64_t val = q->regs[addr / 4];

    /*
     * Each channel is an indirect access port into engine-internal state:
     * command reg = entry select (+0x800 prepare, +0x1000 latch), +0x04 =
     * write data, +0x08 = readback. The firmware writes a value, latches, and
     * spins until the readback matches (RTOS 0x20235ff8..0x20236074). With no
     * engine behind it, reflect the last written data so the verify passes.
     */
    switch (addr) {
    case 0x08:                                  /* ch0 readback */
        val = q->regs[0x04 / 4];
        break;
    case 0x14:                                  /* ch1 readback */
        val = q->regs[0x10 / 4];
        break;
    }
    mstar_iolog(M5_HVSP1_QSTAT_BASE + addr, false, val, size);
    return val;
}

static void mercury5_hvsp_qstat_write(void *opaque, hwaddr addr, uint64_t val,
                                      unsigned size)
{
    Mercury5HvspQstat *q = opaque;

    mstar_iolog(M5_HVSP1_QSTAT_BASE + addr, true, val, size);
    q->regs[addr / 4] = val;
}

static const MemoryRegionOps mercury5_hvsp_qstat_ops = {
    .read = mercury5_hvsp_qstat_read,
    .write = mercury5_hvsp_qstat_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};


static const VMStateDescription vmstate_mercury5_hvsp_qstat = {
    .name = "mstar.mercury5-hvsp1-qstat",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, Mercury5HvspQstat, M5_HVSP1_QSTAT_SIZE / 4),
        VMSTATE_UINT16_ARRAY(count, Mercury5HvspQstat, 2),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * uart1 (serial@221200): the mercury5 RTOS console. The firmware drives it as a
 * MStar-native UART, not a plain 16550, so we model it directly instead of using
 * serial_mm_init. What the firmware actually uses:
 *   +0x00  RBR/THR  - read a received byte / write a byte to transmit
 *          (also DLL when DLAB is set during baud programming; ignored)
 *   +0x18  LCR      - bit7 = DLAB (the only bit we track)
 *   +0x38  status   - bit1 "TX ready" (per char), bit2 "ok to set baud",
 *                     bit3 "RX data available" (the getchar/ISR test this)
 * The console CLI reads keystrokes from an RX interrupt (mst-intc "irq" line 35,
 * = uart1's GIC_SPI 35), so a received byte raises the IRQ; reading the RBR
 * clears it. This is what makes the interactive CLI (rr/wr register access,
 * sensor/LCD register R/W, memory dump, ...) usable.
 */
typedef struct Mercury5Uart {
    MemoryRegion mr;
    CharFrontend chr;
    qemu_irq irq;
    uint8_t rx;
    bool rx_valid;
    bool dlab;
} Mercury5Uart;

#define M5UART_ST_TXRDY   0x06     /* bit1 (per char) | bit2 (baud program) */
#define M5UART_ST_RXAVAIL 0x08     /* bit3: a byte is waiting in the RBR */

static uint64_t mercury5_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    Mercury5Uart *u = opaque;

    if (getenv("M5UART_TRACE"))
        fprintf(stderr, "[m5uart] R +0x%02x (rx_valid=%d)\n", (unsigned)addr, u->rx_valid);
    switch (addr) {
    case 0x00:                                  /* RBR (DLAB clear) */
        if (!u->dlab) {
            u->rx_valid = false;
            qemu_set_irq(u->irq, 0);
            qemu_chr_fe_accept_input(&u->chr);
            return u->rx;
        }
        return 0;                               /* DLL */
    case 0x28:                                  /* 16550-style LSR, for probers */
        return (u->rx_valid ? 0x01 : 0) | 0x60; /* DR | THRE | TEMT */
    case 0x38:                                  /* MStar-native status */
        return M5UART_ST_TXRDY | (u->rx_valid ? M5UART_ST_RXAVAIL : 0);
    }
    return 0;
}

static void mercury5_uart_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    Mercury5Uart *u = opaque;
    uint8_t ch;

    switch (addr) {
    case 0x00:                                  /* THR (DLAB clear) */
        if (!u->dlab) {
            ch = val;
            qemu_chr_fe_write_all(&u->chr, &ch, 1);
        }
        break;
    case 0x18:                                  /* LCR */
        u->dlab = !!(val & 0x80);
        break;
    }
}

static const MemoryRegionOps mercury5_uart_ops = {
    .read = mercury5_uart_read,
    .write = mercury5_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static int mercury5_uart_can_rx(void *opaque)
{
    Mercury5Uart *u = opaque;

    return !u->rx_valid;                         /* single-byte holding register */
}

static void mercury5_uart_rx(void *opaque, const uint8_t *buf, int size)
{
    Mercury5Uart *u = opaque;

    if (size > 0) {
        u->rx = buf[0];
        u->rx_valid = true;
        qemu_set_irq(u->irq, 1);                 /* wake the console RX ISR */
        if (getenv("M5UART_TRACE"))
            fprintf(stderr, "[m5uart] RX byte 0x%02x latched, irq raised\n", buf[0]);
    }
}


static const VMStateDescription vmstate_mercury5_uart = {
    .name = "mstar.mercury5-uart1",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(rx, Mercury5Uart),
        VMSTATE_BOOL(rx_valid, Mercury5Uart),
        VMSTATE_BOOL(dlab, Mercury5Uart),
        VMSTATE_END_OF_LIST()
    },
};

static void mercury5_uart_event(void *opaque, QEMUChrEvent event)
{
}

static void mstar_mercury5_soc_realize(DeviceState *dev, Error **errp)
{
    MStarSoCState *s = MSTAR_SOC(dev);
    MStarSoCClass *sc = MSTAR_SOC_GET_CLASS(dev);
    Mercury5Uart *uart;
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

    {
        Mercury5HvspQstat *q = g_new0(Mercury5HvspQstat, 1);

        memory_region_init_io(&q->mr, OBJECT(s), &mercury5_hvsp_qstat_ops, q,
                              "mstar.mercury5-hvsp1-qstat", M5_HVSP1_QSTAT_SIZE);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            M5_HVSP1_QSTAT_BASE, &q->mr, 10);
        vmstate_register(NULL, 0, &vmstate_mercury5_hvsp_qstat, q);
    }

    uart = g_new0(Mercury5Uart, 1);
    uart->irq = qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_UART1_HWIRQ);
    qemu_chr_fe_init(&uart->chr, serial_hd(1), &error_abort);
    qemu_chr_fe_set_handlers(&uart->chr, mercury5_uart_can_rx, mercury5_uart_rx,
                             mercury5_uart_event, NULL, uart, NULL, true);
    memory_region_init_io(&uart->mr, OBJECT(s), &mercury5_uart_ops, uart,
                          "mstar.mercury5-uart1", 0x100);
    memory_region_add_subregion_overlap(get_system_memory(), MSTAR_UART1_BASE,
                                        &uart->mr, 10);
    vmstate_register(NULL, 0, &vmstate_mercury5_uart, uart);

    /*
     * GOP (graphics output plane) - the 70mai composites its UI through it (SDK
     * gop_hal.lib). The region spans the SCL_GOP + DEC_GOP1..3 instances
     * (0x1f246200..0x1f2479ff, SDK REG_GOP_00/10/20/30_BASE); the whole region
     * is store/read-back (so HalGopGetStrechWinSize reads the sizes the firmware
     * wrote, clearing the "over Stretch window" spam). The instance the firmware
     * actually enables for the OSD is DEC_GOP2 (main @0x1f246e00, GWIN bank
     * @0x1f247000 with bit0 set + format ARGB8888); its window register layout
     * (GWIN enable/addr at bank+0x00/0x04/0x08, stretch at main+0xc0/0xc4)
     * matches the SSD20xD GOP, so it is scanned out to the console. The OSD
     * pixels themselves are drawn by the GE 2D blitter (not yet modelled), so
     * the buffer stays blank for now. Its vsync IRQ is left unconnected (the
     * RTOS doesn't use fbdev).
     */
    {
        DeviceState *gop = qdev_new(TYPE_MSTAR_GOP);

        qdev_prop_set_uint32(gop, "regsize", 0x1800);
        qdev_prop_set_uint32(gop, "winoff", 0xc00);   /* DEC_GOP2 @0x1f246e00 */
        sysbus_realize_and_unref(SYS_BUS_DEVICE(gop), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(gop), 0, MSTAR_RIU_BASE + 0x246200);
    }

    /*
     * SC_TOP - the SCL/display-top interrupt bank (RIU bank 0x1218). The RTOS
     * registers its display ISR at GIC INTID 84 (= SPI 20, vendor
     * INT_IRQ_SC_TOP) and unmasks only the frame/vsync bit; the display task's
     * frame heartbeat comes from this interrupt.
     */
    {
        DeviceState *sctop = qdev_new(TYPE_MSTAR_SCTOP);

        sysbus_realize_and_unref(SYS_BUS_DEVICE(sctop), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(sctop), 0, MSTAR_RIU_BASE + 0x243000);
        /* mst-intc "irq" line 20 -> GIC INTID 84 (vendor INT_IRQ_SC_TOP). */
        sysbus_connect_irq(SYS_BUS_DEVICE(sctop), 0,
                           qdev_get_gpio_in(DEVICE(&s->intc_irq), 20));
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
     * The mercury5 firmware registers its "sys_watchDogHandler" at GIC INTID
     * 162, which is beyond both mst-intcs (irq = INTID 64..127, fiq = 128..159)
     * and this model's 128-SPI GIC (max INTID 159) - i.e. it is wired straight
     * to a GIC SPI, not through the mst-intc. There is no valid mst-intc line
     * for it, so leave the watchdog on the shared default (fiq line 2). The
     * firmware kicks WDT_CLR at 0x1f006000 continuously (the hot loop at RTOS
     * 0x20239be0) so the pre-timeout interrupt never actually fires anyway.
     */
    /*
     * TODO: mercury5 has its own clkgen/pinctrl register maps; reuse the
     * infinity3 (msc313) reg-probe tables for now (mercury5 is closest to it),
     * and add mercury5-specific tables once the firmware's register use is
     * captured.
     */
    sc->info.clkgen_type = TYPE_MSC313_CLKGEN;
    sc->info.pinctrl_type = TYPE_MSC313_PINCTRL;
    /*
     * SD/MMC host: mercury5 places the FCIE at sd@1f282600 (6.5 dtsi
     * mstar-mercury5.dtsi) - 0x600 above the SSD20xD base - on GIC_SPI 60
     * (mst-intc "irq" line 60). The card-detect (PM-GPIO SD_SDZ @0x1f001f1c bit2,
     * active-low) is already modelled at the shared PM_GPIO base; attach a card
     * with -drive if=sd.
     */
    sc->info.sdio_base = MSTAR_RIU_BASE + 0x282600;
    sc->info.sdio_irq = 60;

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
