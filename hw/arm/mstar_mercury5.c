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
#include "qemu/timer.h"
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
    /*
     * The BOND strap (0x1f207818) is created separately in realize because its
     * value is per-SoC (M5U 0x0b vs M5 0x01); see MStarSoCInfo::bound_id.
     */
    { "mstar.mercury5-dsi-done", MSTAR_RIU_BASE + 0x34420c, 4, 0x02 },
    /*
     * NB the boot/power-source status at 0x1f006848 (bit2 = external DC/ACC
     * power) used to be shimmed here; it lives inside the rtcpwc bank
     * (0x1f006800) and is now served by TYPE_MSTAR_RTCPWC.
     */
    /*
     * USB UTMI PHY calibration done (CA_END). The Mstar USB host bring-up polls
     * "UTMI_base + 0x3c*2" (= +0x78) bit1 == CA_END and spins until set (see the
     * 6.5 kernel drivers/usb/host/ehci-mstar.c:333
     *   while ((readb(UTMI_base+0x3c*2) & BIT1) == 0);  // polling CA_END
     * ). With no PHY modelled the catch-all returns 0 and the host task hangs.
     * There are two USB ports: UTMI0 @0x1f004400 (RTOS poll 0x2014bfb4, during
     * MDrv_Usb_Init) and UTMI1 @0x1f285200 (RTOS poll 0x2023ff4c, in the HighWork
     * host task). Report CA_END done on both so USB init proceeds; without UTMI1
     * the HighWork task spins forever and trips the RTOS "Task timeout" watchdog.
     */
    { "mstar.mercury5-utmi0-caend", MSTAR_RIU_BASE + 0x004478, 4, 0x02 },
    { "mstar.mercury5-utmi1-caend", MSTAR_RIU_BASE + 0x285278, 4, 0x02 },
    /*
     * Trigger/done handshake block at 0x1f004a00 (RTOS setup at 0x201fe8a0): the
     * firmware arms it (saves 0x4a30, clears bit0 of 0x4a20) then spins on
     * "0x1f004a28 & BIT12 == done" (poll at 0x201fe8f0). With the block unmodelled
     * the catch-all returns 0, so the awaited bit never sets and the owning RTOS
     * task blocks - the single hottest unmodelled access, ~18k reads/boot (it
     * yields each spin, so other tasks keep running). Report the done bit set so
     * it proceeds. The block sits in the 0x1f004xxx USB/PM region; the exact
     * subsystem is not yet identified (the function has no A32 callers).
     */
    { "mstar.mercury5-4a28-done", MSTAR_RIU_BASE + 0x004a28, 4, 0x1000 },
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
/*
 * HVSP2 (scaler engine 2, @0x1f244000) has the same DrvSclHvsp completion-poll
 * window as HVSP1 (same accessor code 0x20237fa8/fb4), at 0x1f2441e0..0x1f2441ff.
 * The firmware writes a command/index into a channel register and polls the same
 * register for the completed count to echo it; unlike HVSP1 it does not use the
 * indirect cmd/wdata/readback ports, so plain per-register store+echo is enough.
 * Without it the display pipeline re-polls it ~4357x/boot (non-fatal but wasteful).
 */
#define M5_HVSP2_QSTAT_BASE (MSTAR_RIU_BASE + 0x2441e0)
#define M5_HVSP2_QSTAT_SIZE 0x20

typedef struct Mercury5HvspQstat {
    MemoryRegion mr;
    hwaddr base;                /* RIU base of this window (for iolog) */
    bool readback_ports;        /* HVSP1: +0x08/+0x14 echo the +0x04/+0x10 wdata */
    uint16_t regs[M5_HVSP2_QSTAT_SIZE / 4];
    uint16_t count[2];          /* per-channel fake completed-frame counter */
} Mercury5HvspQstat;

static uint64_t mercury5_hvsp_qstat_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    Mercury5HvspQstat *q = opaque;

    uint64_t val = q->regs[addr / 4];

    /*
     * On HVSP1 each channel is an indirect access port into engine-internal
     * state: command reg = entry select (+0x800 prepare, +0x1000 latch), +0x04
     * = write data, +0x08 = readback. The firmware writes a value, latches, and
     * spins until the readback matches (RTOS 0x20235ff8..0x20236074). With no
     * engine behind it, reflect the last written data so the verify passes.
     * HVSP2 polls the command register directly, so its reads just echo (below).
     */
    if (q->readback_ports) {
        switch (addr) {
        case 0x08:                              /* ch0 readback */
            val = q->regs[0x04 / 4];
            break;
        case 0x14:                              /* ch1 readback */
            val = q->regs[0x10 / 4];
            break;
        }
    }
    mstar_iolog(q->base + addr, false, val, size);
    return val;
}

static void mercury5_hvsp_qstat_write(void *opaque, hwaddr addr, uint64_t val,
                                      unsigned size)
{
    Mercury5HvspQstat *q = opaque;

    mstar_iolog(q->base + addr, true, val, size);
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
        VMSTATE_UINT16_ARRAY(regs, Mercury5HvspQstat, M5_HVSP2_QSTAT_SIZE / 4),
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

/* DEBUG: square-wave a mst-intc "irq" line to test an unmodelled interrupt. */
typedef struct {
    MStarSoCState *s;
    QEMUTimer *t;
    int line;
    int ms;
    bool hi;
} Mercury5Fire;

static void mercury5_fire_cb(void *opaque)
{
    Mercury5Fire *f = opaque;

    f->hi = !f->hi;
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&f->s->intc_irq), f->line), f->hi);
    timer_mod(f->t, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + f->ms);
}

static void mercury5_fire_arm(MStarSoCState *s)
{
    Mercury5Fire *f = g_new0(Mercury5Fire, 1);

    f->s = s;
    f->line = atoi(getenv("M5_FIRE_LINE"));
    f->ms = getenv("M5_FIRE_MS") ? atoi(getenv("M5_FIRE_MS")) : 20;
    f->t = timer_new_ms(QEMU_CLOCK_VIRTUAL, mercury5_fire_cb, f);
    fprintf(stderr, "[M5_FIRE] pulsing intc irq line %d every %d ms\n",
            f->line, f->ms);
    timer_mod(f->t, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + f->ms);
}

/* DEBUG: periodically write a byte into DRAM (M5_POKE="addr:val[:ms]") to force
 * a firmware flag/gate and see if the boot advances. A gdb-free memory poke. */
typedef struct { uint32_t addr; uint32_t val; int ms; QEMUTimer *t; } Mercury5Poke;

static void mercury5_poke_cb(void *opaque)
{
    Mercury5Poke *p = opaque;
    uint8_t b = p->val;

    address_space_write(&address_space_memory, p->addr, MEMTXATTRS_UNSPECIFIED,
                        &b, 1);
    timer_mod(p->t, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + p->ms);
}

static void mercury5_poke_arm(void)
{
    Mercury5Poke *p = g_new0(Mercury5Poke, 1);
    char *spec = g_strdup(getenv("M5_POKE"));
    char *c1 = strchr(spec, ':');
    char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;

    if (c2) {
        *c2 = 0;
    }
    if (c1) {
        *c1 = 0;
    }
    p->addr = strtoul(spec, NULL, 0);
    p->val = c1 ? strtoul(c1 + 1, NULL, 0) : 1;
    p->ms = c2 ? atoi(c2 + 1) : 200;
    p->t = timer_new_ms(QEMU_CLOCK_VIRTUAL, mercury5_poke_cb, p);
    fprintf(stderr, "[M5_POKE] writing [%#x]=%#x every %d ms\n",
            p->addr, p->val, p->ms);
    timer_mod(p->t, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 8000);
}

/* DEBUG: MSTAR_GPIO_HIGH="ADDR[:VAL[:SIZE]]" forces a single MMIO word to read
 * VAL (default 1, SIZE default 4) - to bisect which unmodelled GPIO INPUT (our
 * stub returns 0) the firmware reads and expects non-zero at boot. */
typedef struct { uint32_t val; } Mercury5Force;
static uint64_t mercury5_force_read(void *opaque, hwaddr addr, unsigned size)
{
    return ((Mercury5Force *)opaque)->val;
}
static void mercury5_force_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
}
static const MemoryRegionOps mercury5_force_ops = {
    .read = mercury5_force_read,
    .write = mercury5_force_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
};

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
    if (getenv("MSTAR_GPIO_HIGH")) {
        char *spec = g_strdup(getenv("MSTAR_GPIO_HIGH"));
        char *c1 = strchr(spec, ':');
        char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        Mercury5Force *f = g_new0(Mercury5Force, 1);
        MemoryRegion *mr = g_new0(MemoryRegion, 1);
        hwaddr addr;
        unsigned sz;

        if (c2) { *c2 = 0; }
        if (c1) { *c1 = 0; }
        addr = strtoul(spec, NULL, 0);
        f->val = c1 ? strtoul(c1 + 1, NULL, 0) : 1;
        sz = c2 ? atoi(c2 + 1) : 4;
        memory_region_init_io(mr, OBJECT(s), &mercury5_force_ops, f,
                              "m5.gpio-force", sz);
        memory_region_add_subregion_overlap(get_system_memory(), addr, mr, 20);
        fprintf(stderr, "[MSTAR_GPIO_HIGH] force [%#" HWADDR_PRIx "]=%#x sz=%u\n",
                addr, f->val, sz);
    }

    for (i = 0; i < ARRAY_SIZE(mercury5_shims); i++) {
        const Mercury5Shim *sh = &mercury5_shims[i];
        MemoryRegion *mr = g_new0(MemoryRegion, 1);

        memory_region_init_io(mr, OBJECT(s), &mercury5_shim_ops,
                              (void *)sh, sh->name, sh->size);
        memory_region_add_subregion_overlap(get_system_memory(), sh->addr,
                                            mr, 10);
    }

    /* Per-SoC BOND strap shim (M5U 0x0b, M5 0x01) at 0x1f207818. */
    {
        Mercury5Shim *bound = g_new0(Mercury5Shim, 1);
        MemoryRegion *mr = g_new0(MemoryRegion, 1);

        bound->name = "mstar.mercury5-bound";
        bound->addr = MSTAR_RIU_BASE + 0x207818;
        bound->size = 4;
        bound->val = sc->info.bound_id ? sc->info.bound_id : 0x0b;
        memory_region_init_io(mr, OBJECT(s), &mercury5_shim_ops, bound,
                              bound->name, bound->size);
        memory_region_add_subregion_overlap(get_system_memory(), bound->addr,
                                            mr, 10);
    }

    {
        static const struct {
            const char *name;
            hwaddr base;
            uint64_t size;
            bool readback_ports;
        } hvsps[] = {
            { "mstar.mercury5-hvsp1-qstat", M5_HVSP1_QSTAT_BASE,
              M5_HVSP1_QSTAT_SIZE, true },
            { "mstar.mercury5-hvsp2-qstat", M5_HVSP2_QSTAT_BASE,
              M5_HVSP2_QSTAT_SIZE, false },
        };
        unsigned j;

        for (j = 0; j < ARRAY_SIZE(hvsps); j++) {
            Mercury5HvspQstat *q = g_new0(Mercury5HvspQstat, 1);

            q->base = hvsps[j].base;
            q->readback_ports = hvsps[j].readback_ports;
            memory_region_init_io(&q->mr, OBJECT(s), &mercury5_hvsp_qstat_ops, q,
                                  hvsps[j].name, hvsps[j].size);
            memory_region_add_subregion_overlap(get_system_memory(),
                                                hvsps[j].base, &q->mr, 10);
            vmstate_register(NULL, j, &vmstate_mercury5_hvsp_qstat, q);
        }
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

    /*
     * DEBUG: periodic "irq" mst-intc line pulser to pin an unmodelled interrupt.
     * M5_FIRE_LINE=<line> square-waves that line every M5_FIRE_MS ms (default
     * 20) so a candidate completion IRQ can be tested against the boot profile.
     * (line = INTID - 64 for the irq intc; e.g. INTID 105 -> line 41.)
     */
    if (getenv("M5_FIRE_LINE")) {
        mercury5_fire_arm(s);
    }
    if (getenv("M5_POKE")) {
        mercury5_poke_arm();
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
    /*
     * The mercury5 bach reader uses a different control-bit layout than the
     * msc313 one the base model assumes (queue/trigger on EN bit12 not bit13,
     * no EN bit15 enable, CTRL0 int-clear/underrun-IE/empty-IE on bit0/2/4 not
     * bit8/13/10). Without this the reader underrun IRQ (INTID 106) never fires
     * and the audio-subsystem init - and thus APK_BeepTask - hangs on its
     * completion event. Layout captured from the 70mai firmware (MSTAR_IOLOG).
     */
    sc->info.bach_mercury5 = true;
    /* rtcpwc RTC power/wake controller @0x1f006800 (subsumes the pwrsrc shim). */
    sc->info.has_rtcpwc = true;
    /* mercury5 has 4 HWI2C masters (adds i2c@222a00/222c00 = DrvI2c channels
     * 2/3, where the camera sensor lives). */
    sc->info.num_i2c = 4;

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

static void mstar_ssc8336_m5_soc_class_init(ObjectClass *oc, const void *data)
{
    MStarSoCClass *sc = MSTAR_SOC_CLASS(oc);

    /*
     * The mercury5 "M5" (chip 0xd9) variant of the SSC8336, as used by the
     * mirrorcam - vs the 70mai's "M5U" (0xee) above. The IPL identifies the chip
     * at 0x1f003d98 and maps chip 0xd9 + BOND 0x01 to its MIU/DRAM profile - the
     * real device's IPL prints "Chip:M5 Bound:0001 ... RAM Size 64MB" (0xd9 with
     * the M5U BOND 0x0b would "Unknown BoundID to Miu [HALT]"). It also carries a
     * smaller 4MB SPI-NOR (flash ID 0x204016) instead of 16MB.
     */
    sc->info.chip_id = 0xd9;
    sc->info.bound_id = 0x01;
    sc->info.flash_model = "mstar-nor-32m";  /* 4MB, JEDEC 0x204016 */
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
    {
        .name           = TYPE_MSTAR_SSC8336_M5_SOC,
        .parent         = TYPE_MSTAR_MERCURY5_SOC,
        .class_init     = mstar_ssc8336_m5_soc_class_init,
    },
};

DEFINE_TYPES(mstar_mercury5_types)
