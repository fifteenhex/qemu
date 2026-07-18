/*
 * MStar/SigmaStar Armv7 SoCs and boards
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This models the common MStar/SigmaStar Armv7 SoC peripherals - Cortex-A
 * CPU(s), DRAM and the "pm" UART - enough to start a mainline kernel. The
 * per-block device models live in hw/<subsystem>/mstar_*.c; this file builds
 * the SoC container and the boards. The SoC and board types are split so that
 * support for more chips and boards can be added by defining new subclasses.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/mstar.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/char/serial-mm.h"
#include "hw/core/loader.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/arm-powerctl.h"
#include "hw/intc/arm_gic.h"
#include "qom/object.h"
#include "mstar-soc.h"

/*
 * Optional RIU I/O tracer for reverse-engineering. When the environment
 * variable MSTAR_IOLOG names a file, every access logged via mstar_iolog() is
 * appended to it as "R|W <phys> <size> <value>". Two sources feed it: a
 * catch-all overlay over the whole RIU (added in mstar_soc_realize, priority
 * -1, so it only sees registers no device has claimed) and explicit calls in
 * the display/dphy/dsi/mop register handlers (whose registers ARE claimed, so
 * the overlay would never see them).
 */
static FILE *mstar_iolog_fp;

void mstar_iolog(hwaddr phys, bool write, uint64_t val, unsigned size)
{
    FILE *fp = mstar_iolog_fp;

    /* stderr fallback: qemu's fopen() to a file can be sandbox-blocked. */
    if (!fp && getenv("MSTAR_IOLOG_STDERR")) {
        fp = stderr;
    }
    if (fp) {
        uint32_t pc = current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0;
        fprintf(fp, "%c %08x %u %0*x %08x\n", write ? 'W' : 'R',
                (uint32_t)phys, size, size * 2, (uint32_t)val, pc);
        fflush(fp);
    }
}

/* ----------------------------------------------------------- l3bridge */

static uint64_t mstar_l3bridge_read(void *opaque, hwaddr addr, unsigned size)
{
    /* Report every MIU flush as already complete. */
    if (addr == MSTAR_L3BRIDGE_STATUS) {
        return MSTAR_L3BRIDGE_STATUS_DONE;
    }
    return 0;
}

static void mstar_l3bridge_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
}

static const MemoryRegionOps mstar_l3bridge_ops = {
    .read = mstar_l3bridge_read,
    .write = mstar_l3bridge_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


/* --------------------------------------------------------------- did (chip) */

/*
 * The "did" block. The boot ROM reads DID_KEY (vendor register 0x70, i.e.
 * byte offset 0x1c0 at the 4-byte register stride) and takes bits[5:2] as
 * the boot-media strap:
 *   0x20 - SPI-NOR (read via the ISP/QSPI XIP window)
 *   0x10 - NAND
 *   0x08 - SPINAND / eMMC (a separate controller, not modelled)
 * Anything else makes the ROM print "... undefined! [HALT]". We report NOR,
 * which is what the breadbee-class boards boot from; that is all the ROM
 * needs from this block.
 *
 * mercury5's mask ROM reuses this register but also tests bit 0x800 to pick
 * the flash offset it loads the IPL/MCR5 header from (clear = 0x8000, set = 0);
 * a SoC that needs a different strap sets MStarSoCInfo::boot_strap.
 */
#define DID_BOOTMEDIA_REG   0x1c0
#define DID_BOOTMEDIA_NOR   0x20

static uint64_t mstar_did_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarSoCState *s = opaque;
    MStarSoCClass *sc = MSTAR_SOC_GET_CLASS(s);

    if (addr == DID_BOOTMEDIA_REG) {
        return sc->info.boot_strap ? sc->info.boot_strap : DID_BOOTMEDIA_NOR;
    }
    return 0;
}

static void mstar_did_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
}

static const MemoryRegionOps mstar_did_ops = {
    .read = mstar_did_read,
    .write = mstar_did_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


/* ------------------------------------------------------------------- miu */

/*
 * The MIU (Memory Interface Unit - the DDR controller). QEMU's DRAM is a plain
 * always-there RAM region, so there is no analog PHY to train or memory to
 * bring up; this model exists only so the mask-ROM IPL's DDR bring-up runs to
 * completion instead of spinning on a status poll.
 *
 * The 0x1000 window at 0x1f202000 is split into four 16-bit register banks
 * (registers at their native 2-byte stride; the IPL uses byte and halfword
 * accesses interchangeably):
 *
 *   0x1f202000  MIU_ANA        analog PHY: DDR PLL, DQ/DQS phase/drive training
 *   0x1f202200  MIU_ARB        arbiter: request groups 4-6, protection windows
 *   0x1f202400  MIU_DIG        digital: memory config, DRAM timings, MRx, BIST
 *   0x1f202c00  MIU_M5_GROUPS  extra arbiter request groups (pioneer3 "m5")
 *
 * How the DDR init happens (register names/offsets from the u-boot mstar ddr
 * driver drivers/ddr/mstar/{ana,arb,dig}.h + miu.c, itself reverse engineered
 * from the vendor IPL). The full bring-up (miu_configure_dram()) is:
 *
 *   1. hold the digital block in reset      (DIG SW_RST 0x3c)
 *   2. block every arbiter request mask     (DIG/ARB group REQ_MASK regs)
 *   3. reset the analog block               (ANA F0/48 "drive cal" sequence)
 *   4. start the DDR PLL / set the clock     (ANA 6c=0x400, 68=0x2004, 114=1,
 *                                            DDFSET_L 0x60=0x8000, _H 0x64=0x29)
 *   5. program the memory config + timings  (DIG CONFIG1..7 0x04-0x1c: memtype/
 *                                            buswidth/banks/cols, tRCD/tRP/tRAS,
 *                                            tWL/tWR..., mode regs MR0..3 0x20)
 *   6. set the arbiter request priorities   (DIG groups 0-3 0x80/c0/100/140,
 *                                            ARB groups 4-6)
 *   7. analog config + power-up             (ANA E0/D8/DC/EC..., then the ANA00
 *                                            power-up + 3C=5/f/5 pulse)
 *   8. run the "initial cycle"              (DIG CNTRL0 0x00: step RSTZ->CS->CKE
 *                                            ->INIT_MIU, poll INITDONE bit15)
 *   9. latch init-done                      (DIG SW_RST 0x3c |= SW_INIT_DONE
 *                                            bit3 - "if this is not set any DDR
 *                                            access locks up the CPU")
 *  10. run the memory BIST self test        (see below)
 *
 * The vendor IPL in the emulated Miyoo/SSD202 flash takes a shortened path
 * (captured live with MSTAR_IOLOG): it can't identify the PLL ("unknown
 * miupll") so it skips the analog PLL/training in steps 3-4/7-8 and jumps
 * straight to the arbiter setup (step 6: DIG groups 0-3 CTRL=0x8015,
 * CONFIG0=0x2008, PRIORITY0..3=0x3210/7654/ba98/fedc; ARB group6 0x1c0..0x1f0),
 * unmasks the requests (step 2 in reverse), latches init-done (step 9: DIG
 * SW_RST 0x3c=0x8c08), then runs the BIST.
 *
 * The BIST lives at MIU_DIG + 0x1c0.. (0x1f2025c0). Programming, from the
 * trace: PROTECT2_START 0x1a4=0x9000, pulse 0x1bc 1->0, then length 0x1c8=
 * 0xffff, 0x1cc=0x01fe, test pattern 0x1d0=0x5aa5, MIUSEL0 0x1e0=0, and finally
 * write bit0 of the control/status reg 0x1c0 to start. The IPL then polls 0x1c0
 * for bit15 = done, bits[14:13] = error (so 0x8000 = done, no error; 0xe000 =
 * done, failed). This is the one register the IPL reads-and-waits-on.
 *
 * What we model: DRAM training is instantaneous here, so report both completion
 * gates as finished with no error - BIST 0x1c0 -> 0x8000, and (for the full
 * u-boot/kernel path) the initial-cycle CNTRL0 -> INITDONE. Every other MIU
 * register is write-only to the IPL (fire and forget) or only read to
 * read-modify-write, so returning zero for them is fine.
 *
 * Note the IPL does NOT read the DRAM size from any MIU register: it sizes
 * memory by probing address aliasing - writing distinct markers at DRAM +0,
 * +32M, +64M, +128M and +256M and reading them back to find where the address
 * wraps. Real DRAM wraps above its size; QEMU's flat RAM does not, so the
 * machine mirrors RAM above its real size (see mstar_machine_init) to make the
 * probe detect the true amount instead of the 512MB "no wrap" maximum.
 */
#define MIU_ANA_BASE            0x000
#define MIU_ARB_BASE            0x200
#define MIU_DIG_BASE            0x400
#define MIU_M5_GROUPS_BASE      0xc00

#define MIU_DIG_CNTRL0          (MIU_DIG_BASE + 0x000)  /* init-cycle control */
#define MIU_DIG_CNTRL0_INITDONE (1 << 15)
#define MIU_DIG_BIST_CTRL       (MIU_DIG_BASE + 0x1c0)  /* start (W) / status (R) */
#define MIU_DIG_BIST_DONE       (1 << 15)               /* bits 14:13 = error */

/*
 * DDR-PLL frequency-set registers in the analog block. The 6.5 kernel's MIU
 * clock driver (drivers/memory/mstar-msc313_miu.c mstar_miu_ddrpll_recalc_rate)
 * divides the PLL base rate by ddfset = (DDFSET_H & 0xff)<<16 | DDFSET_L, and
 * panics with a divide-by-zero if the pair reads 0. Report the values the
 * vendor IPL programs during DDR bring-up (captured with MSTAR_IOLOG) so the
 * clock registers with a sane, non-zero rate.
 */
#define MIU_ANA_DDFSET_L        (MIU_ANA_BASE + 0x60)
#define MIU_ANA_DDFSET_H        (MIU_ANA_BASE + 0x64)

static uint64_t mstar_miu_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case MIU_DIG_BIST_CTRL:
        /* Memory self-test finished, no error (emulated DRAM never fails). */
        return MIU_DIG_BIST_DONE;
    case MIU_DIG_CNTRL0:
        /* Initial DRAM training cycle completed (full-config path). */
        return MIU_DIG_CNTRL0_INITDONE;
    case MIU_ANA_DDFSET_L:
        return 0x8000;
    case MIU_ANA_DDFSET_H:
        return 0x0029;
    }
    return 0;
}

static void mstar_miu_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
    /* No analog PHY / DRAM to program; the training writes are no-ops here. */
}

static const MemoryRegionOps mstar_miu_ops = {
    .read = mstar_miu_read,
    .write = mstar_miu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


/* --------------------------------------------------------------- smpctrl */

/*
 * Secondary-CPU boot mailbox (mstar,smpctrl). mstarv7_boot_secondary()
 * writes CPU1's entry address as two halfwords then an unlock magic; the
 * hardware then releases CPU1 to that address. Register byte offsets are
 * from arch/arm/mach-mstar/mstarv7.c.
 */
#define SMPCTRL_CPU1_BOOT_HIGH  0x4c
#define SMPCTRL_CPU1_BOOT_LOW   0x50
#define SMPCTRL_CPU1_UNLOCK     0x58
#define SMPCTRL_UNLOCK_MAGIC    0xbabe

static uint64_t mstar_smpctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarSoCState *s = opaque;

    /*
     * Return what was written. The vendor kernel's boot_secondary writes the
     * entry address a halfword at a time and spins reading it back until the
     * readback matches (see mach-sstar), so a read-as-zero register livelocks
     * it before it ever brings up CPU1.
     */
    return s->smpctrl_regs[addr / 4];
}

static void mstar_smpctrl_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    MStarSoCState *s = opaque;

    s->smpctrl_regs[addr / 4] = val;

    switch (addr) {
    case SMPCTRL_CPU1_BOOT_LOW:
        s->smp_bootaddr = (s->smp_bootaddr & 0xffff0000) | (val & 0xffff);
        break;
    case SMPCTRL_CPU1_BOOT_HIGH:
        s->smp_bootaddr = (s->smp_bootaddr & 0x0000ffff) | ((val & 0xffff) << 16);
        break;
    case SMPCTRL_CPU1_UNLOCK:
        if ((val & 0xffff) == SMPCTRL_UNLOCK_MAGIC) {
            /*
             * Real hardware releases the secondary from reset in Secure state,
             * so it can run the same Secure init the primary did (Monitor-mode
             * SCR/CNTVOFF setup). Start it at EL3 (Secure Monitor); starting it
             * Non-secure (EL1) makes those Secure-only accesses UNDEFINED and
             * the secondary oopses. Only meaningful when the CPU has EL3 (the
             * vendor-kernel path); fall back to EL1 otherwise.
             */
            uint32_t el = arm_feature(&s->cpu[1].env, ARM_FEATURE_EL3) ? 3 : 1;
            int ret = arm_set_cpu_on(s->cpu[1].mp_affinity, s->smp_bootaddr, 0,
                                     el, false /* AArch32 */);

            if (ret != QEMU_ARM_POWERCTL_RET_SUCCESS) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "mstar-smpctrl: could not start CPU1 (%d)\n", ret);
            }
        }
        break;
    }
}

static const MemoryRegionOps mstar_smpctrl_ops = {
    .read = mstar_smpctrl_read,
    .write = mstar_smpctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};


/* --------------------------------------------------------------- cpupll */

/*
 * CPU PLL (the vendor "CLK_cpupll" bank at 0x1f206400, vendor register base
 * 0x103200 at the RIU 2x stride). The clock driver reads an "LPF value" and a
 * post-divider and back-computes the CPU frequency:
 *
 *   lpf  = reg[0x1032a4] | reg[0x1032a6] << 16   (phys 0x206548 / 0x20654c)
 *   pdiv = reg[0x103232] + 1                      (phys 0x206464)
 *   rate = div64_u64(432000000 * 2^19, lpf) * 2 / pdiv * 32 / 2
 *
 * With the registers reading 0 the vendor kernel divides by zero and panics
 * ("Division by zero in kernel" in ms_cpuclk_recalc_rate). Return an LPF that
 * yields ~1 GHz with post_div = 2, i.e. lpf = (432MHz / (1GHz*2/32)) * 2^19 =
 * 0x374cc7. Real silicon holds these values after the boot ROM/IPL bring the
 * PLL up; we do the same so the recalc is well-defined.
 */
#define CPUPLL_LPF_LOW      0x148    /* 0x1032a4 << 1 - base */
#define CPUPLL_LPF_HIGH     0x14c    /* 0x1032a6 << 1 - base */
#define CPUPLL_POSTDIV      0x064    /* 0x103232 << 1 - base */
#define CPUPLL_LPF_DONE     0x174    /* 0x1032ba << 1 - base */

static uint64_t mstar_cpupll_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case CPUPLL_LPF_LOW:
        return 0x4cc7;
    case CPUPLL_LPF_HIGH:
        return 0x0037;
    case CPUPLL_POSTDIV:
        return 0x0001;      /* post_div = value + 1 = 2 */
    case CPUPLL_LPF_DONE:
        /*
         * ms_cpuclk_set_rate() programs the LPF then spins on bit0 of this
         * register waiting for the frequency switch to "settle". The switch is
         * instantaneous for us, so always report done.
         */
        return 0x0001;
    }
    return 0;
}

static void mstar_cpupll_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    /* The frequency-switch sequence writes here; nothing to model. */
}

static const MemoryRegionOps mstar_cpupll_ops = {
    .read = mstar_cpupll_read,
    .write = mstar_cpupll_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};


/* --------------------------------------------------------------- chipid */

/*
 * The "chipid" register (chip@1f003c00). The mask-ROM IPL reads the byte here
 * to identify the chip family and prints it as "D-<id>"; msc313e/infinity3 is
 * 0xc2, the SSD20xD/infinity2m is 0xf0. The offset within the bank is
 * SoC-specific (MStarSoCInfo::chipid_off): 0 for infinity, +0x198 (0x1f003d98)
 * for mercury5, whose IPL prints "Chip:M5U" for 0xee.
 */
static uint64_t mstar_chipid_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarSoCState *s = opaque;
    MStarSoCClass *sc = MSTAR_SOC_GET_CLASS(s);

    if (addr == sc->info.chipid_off) {
        return sc->info.chip_id;
    }
    return 0;
}

static void mstar_chipid_write(void *opaque, hwaddr addr, uint64_t v,
                               unsigned size)
{
}

static const MemoryRegionOps mstar_chipid_ops = {
    .read = mstar_chipid_read,
    .write = mstar_chipid_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* --------------------------------------------------------------- chiptop */

/*
 * The "chiptop" pin-mux / chip-strap block (pinctrl@1f203c00). Only one
 * register matters to the mask-ROM IPL: the package/variant strap "BOND" at
 * +0x120 (0x1f203d20). The IPL reads it to identify the chip - it prints
 * "D-<bond>" - and picks the matching DDR timing profile and PLL: with the
 * right strap (0x1e = SSD202D, 0x1d = SSD201) it reports e.g. "miupll_233MHz",
 * left at 0 it can't identify the part and prints "unknown miupll". (The DRAM
 * *size* is not read from here - the IPL probes it by aliasing; see the miu
 * block.) The rest of the block is stubbed (reads 0, writes dropped), matching
 * what the IPL and the pinctrl driver need.
 */
static uint64_t mstar_chiptop_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarSoCState *s = opaque;
    MStarSoCClass *sc = MSTAR_SOC_GET_CLASS(s);

    if (addr == MSTAR_CHIPTOP_BOND) {
        return sc->info.bond;
    }
    return 0;
}

static void mstar_chiptop_write(void *opaque, hwaddr addr, uint64_t v,
                                unsigned size)
{
}

static const MemoryRegionOps mstar_chiptop_ops = {
    .read = mstar_chiptop_read,
    .write = mstar_chiptop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


/* ------------------------------------------------------------------ SoC */

/* "fiq" mst-intc input line for each timer@6040/80/c0 (from the DT). */
static const unsigned int mstar_timer_hwirq[MSTAR_NUM_TIMERS] = { 0, 1, 12 };

static void mstar_soc_init(Object *obj)
{
    MStarSoCState *s = MSTAR_SOC(obj);
    MStarSoCClass *sc = MSTAR_SOC_GET_CLASS(obj);
    unsigned int i;

    for (i = 0; i < sc->info.num_cpus; i++) {
        object_initialize_child(obj, "cpu[*]", &s->cpu[i],
                                sc->info.cpu_type);
    }

    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GIC);
    object_initialize_child(obj, "intc-irq", &s->intc_irq, TYPE_MST_INTC);
    object_initialize_child(obj, "intc-fiq", &s->intc_fiq, TYPE_MST_INTC);

    for (i = 0; i < MSTAR_NUM_TIMERS; i++) {
        object_initialize_child(obj, "timer[*]", &s->timer[i],
                                TYPE_MSC313E_TIMER);
    }

    object_initialize_child(obj, "rtc", &s->rtc, TYPE_MSC313_RTC);
    object_initialize_child(obj, "sar", &s->sar, TYPE_MSC313_SAR);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_MSC313_GPIO);
    object_initialize_child(obj, "pm-gpio", &s->pm_gpio, TYPE_MSTAR_PM_GPIO);
    object_initialize_child(obj, "isp", &s->isp, TYPE_MSC313_ISP);
    object_initialize_child(obj, "bdma", &s->bdma, TYPE_MSC313_BDMA);
    object_initialize_child(obj, "clkgen", &s->clkgen, sc->info.clkgen_type);
    object_initialize_child(obj, "pinctrl", &s->pinctrl, sc->info.pinctrl_type);
    object_initialize_child(obj, "sdio", &s->sdio, TYPE_MSC313_SDIO);
    object_initialize_child(obj, "bach", &s->bach, TYPE_MSC313_BACH);
    object_initialize_child(obj, "emac", &s->emac, TYPE_MSTAR_EMAC);
    object_initialize_child(obj, "wdt", &s->wdt, TYPE_MSTAR_WDT);
    object_initialize_child(obj, "efuse", &s->efuse, TYPE_MSTAR_REGBANK);
    object_initialize_child(obj, "syscon", &s->syscon, TYPE_MSTAR_REGBANK);
    object_initialize_child(obj, "cmdq", &s->cmdq, TYPE_MSTAR_CMDQ);
    for (i = 0; i < MSTAR_NUM_I2C; i++) {
        object_initialize_child(obj, "i2c[*]", &s->i2c[i], TYPE_MSC313_I2C);
    }
    if (sc->info.has_display) {
        object_initialize_child(obj, "pwm", &s->pwm, TYPE_MSC313_PWM);
        object_initialize_child(obj, "disp", &s->disp, TYPE_MSC313_DISP);
        object_initialize_child(obj, "dphy", &s->dphy, TYPE_MSTAR_DPHY);
    }
}

static bool mstar_realize_intc(MstIntcState *intc, DeviceState *gicdev,
                               hwaddr base, uint32_t irq_start, uint32_t num,
                               Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(intc);
    unsigned int i;

    qdev_prop_set_uint32(DEVICE(intc), "irq-start", irq_start);
    qdev_prop_set_uint32(DEVICE(intc), "num-irqs", num);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, base);

    /* Each input forwards to GIC SPI (irq_start + line). */
    for (i = 0; i < num; i++) {
        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(gicdev, irq_start + i));
    }
    return true;
}

/*
 * Catch-all overlay for the RIU tracer: logs accesses no device claimed.
 * When MSTAR_IOLOG_UNIQUE is set, each (address, direction) is logged only
 * once, so a full boot yields the *set* of unhandled registers instead of
 * millions of poll hits - fast enough to run to the UI. The RIU window is
 * 0x400000 bytes; track first-seen at 4-byte granularity (2 bits per reg).
 */
static uint8_t *mstar_iolog_seen;   /* 0x400000/4 entries, bit0 read bit1 write */

bool mstar_iolog_first(hwaddr addr, bool write)
{
    unsigned int idx;
    uint8_t bit;

    if (!mstar_iolog_seen) {
        return true;
    }
    idx = (addr / 4) & 0xfffff;
    bit = write ? 2 : 1;
    if (mstar_iolog_seen[idx] & bit) {
        return false;
    }
    mstar_iolog_seen[idx] |= bit;
    return true;
}

static uint64_t mstar_iolog_catchall_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    if (mstar_iolog_first(addr, false)) {
        mstar_iolog(MSTAR_RIU_BASE + addr, false, 0, size);
    }
    return 0;
}

static void mstar_iolog_catchall_write(void *opaque, hwaddr addr, uint64_t val,
                                       unsigned size)
{
    if (mstar_iolog_first(addr, true)) {
        mstar_iolog(MSTAR_RIU_BASE + addr, true, val, size);
    }
}

static const MemoryRegionOps mstar_iolog_catchall_ops = {
    .read = mstar_iolog_catchall_read,
    .write = mstar_iolog_catchall_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void mstar_soc_realize(DeviceState *dev, Error **errp)
{
    MStarSoCState *s = MSTAR_SOC(dev);
    MStarSoCClass *sc = MSTAR_SOC_GET_CLASS(dev);
    DeviceState *gicdev = DEVICE(&s->gic);
    unsigned int i;

    for (i = 0; i < sc->info.num_cpus; i++) {
        /* The architected timer runs at the rate given in the device tree. */
        object_property_set_int(OBJECT(&s->cpu[i]), "cntfrq",
                                MSTAR_ARCH_TIMER_FREQ, &error_abort);
        /*
         * No EL2 for a kernel boot: the kernel starts at EL1 (SVC); with EL2
         * present QEMU's cortex-a7 would enter HYP and hang on the unwired HYP
         * timer. EL3 is only enabled for a mask-ROM boot: the ROM/IPL run in
         * Secure state and touch Secure-only registers (e.g. NSACR), which are
         * UNDEFINED without the Security Extensions.
         */
        object_property_set_bool(OBJECT(&s->cpu[i]), "has_el2",
                                 getenv("MSTAR_SECURE_KERNEL") != NULL,
                                 &error_abort);
        object_property_set_bool(OBJECT(&s->cpu[i]), "has_el3", s->secure_boot,
                                 &error_abort);
        /*
         * Secondary CPUs start held; the kernel releases them one at a time
         * through the smpctrl mailbox (see mstar_smpctrl_write).
         */
        if (i > 0) {
            object_property_set_bool(OBJECT(&s->cpu[i]), "start-powered-off",
                                     true, &error_abort);
        }
        if (!qdev_realize(DEVICE(&s->cpu[i]), NULL, errp)) {
            return;
        }
    }

    /* GIC-400 (GICv2). Model only the distributor + CPU interface. */
    qdev_prop_set_uint32(gicdev, "num-irq", MSTAR_GIC_NUM_SPI + GIC_INTERNAL);
    qdev_prop_set_uint32(gicdev, "revision", 2);
    qdev_prop_set_uint32(gicdev, "num-cpu", sc->info.num_cpus);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gic), errp)) {
        return;
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gic), 0, MSTAR_GIC_DIST_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gic), 1, MSTAR_GIC_CPU_BASE);

    for (i = 0; i < sc->info.num_cpus; i++) {
        DeviceState *cpudev = DEVICE(&s->cpu[i]);
        int ppibase = MSTAR_GIC_NUM_SPI + i * GIC_INTERNAL + GIC_NR_SGIS;

        /* CPU generic timer outputs -> GIC PPI inputs */
        qdev_connect_gpio_out(cpudev, GTIMER_PHYS,
                              qdev_get_gpio_in(gicdev,
                                       ppibase + MSTAR_GIC_PPI_PHYSTIMER));
        qdev_connect_gpio_out(cpudev, GTIMER_VIRT,
                              qdev_get_gpio_in(gicdev,
                                       ppibase + MSTAR_GIC_PPI_VIRTTIMER));

        /* GIC outputs -> CPU interrupt inputs */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i + sc->info.num_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
    }

    /* l3bridge (MIU write-flush) used by the SoC memory barrier. */
    memory_region_init_io(&s->l3bridge, OBJECT(s), &mstar_l3bridge_ops, s,
                          "mstar.l3bridge", MSTAR_L3BRIDGE_SIZE);
    memory_region_add_subregion(get_system_memory(), MSTAR_L3BRIDGE_BASE,
                                &s->l3bridge);

    /* On-chip SRAM ("IMI"), used by the boot ROM as scratch/IPL load area. */
    memory_region_init_ram(&s->imi, OBJECT(s), "mstar.imi", MSTAR_IMI_SIZE,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), MSTAR_IMI_BASE, &s->imi);

    /* smpctrl mailbox used to release the secondary CPU on multi-core SoCs. */
    if (sc->info.num_cpus > 1) {
        memory_region_init_io(&s->smpctrl, OBJECT(s), &mstar_smpctrl_ops, s,
                              "mstar.smpctrl", MSTAR_SMPCTRL_SIZE);
        memory_region_add_subregion(get_system_memory(), MSTAR_SMPCTRL_BASE,
                                    &s->smpctrl);
    }

    /* CPU PLL - realistic LPF/post-div so clock recalc doesn't divide by 0. */
    memory_region_init_io(&s->cpupll, OBJECT(s), &mstar_cpupll_ops, s,
                          "mstar.cpupll", MSTAR_CPUPLL_SIZE);
    memory_region_add_subregion(get_system_memory(), MSTAR_CPUPLL_BASE,
                                &s->cpupll);

    /* "did" chip block - the boot ROM reads the boot-media strap from here. */
    {
        MemoryRegion *did = g_new(MemoryRegion, 1);
        memory_region_init_io(did, OBJECT(s), &mstar_did_ops, s,
                              "mstar.did", 0x200);
        memory_region_add_subregion(get_system_memory(), MSTAR_DID_BASE, did);
    }

    /* MIU (DDR controller) - only enough for the mask-ROM IPL's memory BIST. */
    {
        MemoryRegion *miu = g_new(MemoryRegion, 1);
        memory_region_init_io(miu, OBJECT(s), &mstar_miu_ops, s,
                              "mstar.miu", MSTAR_MIU_SIZE);
        memory_region_add_subregion(get_system_memory(), MSTAR_MIU_BASE, miu);
    }

    /*
     * SCLDMA capture-status stub (MSC313 camera pipeline). Only on the
     * camera-class SoC (no GOP/MOP display) so it can't shadow anything on the
     * SSD20xD display machines. This is a first, partial piece: it lets the
     * capture busy-poll advance, but the IP-camera firmware still parks in its
     * H2BR request controller waiting for a real frame-done interrupt + sensor
     * data (see the linux-chenxing camera page for what a full model needs).
     */

    /* chipid: the CHIPID byte the mask-ROM IPL reads to identify the chip. */
    if (sc->info.chip_id) {
        MemoryRegion *chipid = g_new(MemoryRegion, 1);
        memory_region_init_io(chipid, OBJECT(s), &mstar_chipid_ops, s,
                              "mstar.chipid", MSTAR_CHIPID_SIZE);
        memory_region_add_subregion(get_system_memory(), MSTAR_CHIPID_BASE,
                                    chipid);
    }

    /* chiptop: the package BOND strap the IPL reads to size in-package DRAM. */
    if (sc->info.bond) {
        MemoryRegion *chiptop = g_new(MemoryRegion, 1);
        memory_region_init_io(chiptop, OBJECT(s), &mstar_chiptop_ops, s,
                              "mstar.chiptop", MSTAR_CHIPTOP_SIZE);
        memory_region_add_subregion(get_system_memory(), MSTAR_CHIPTOP_BASE,
                                    chiptop);
    }

    /* The two "mst-intc" instances between the peripherals and the GIC. */
    if (!mstar_realize_intc(&s->intc_irq, gicdev, MSTAR_INTC_IRQ_BASE,
                            MSTAR_INTC_IRQ_START, MSTAR_INTC_IRQ_NUM, errp) ||
        !mstar_realize_intc(&s->intc_fiq, gicdev, MSTAR_INTC_FIQ_BASE,
                            MSTAR_INTC_FIQ_START, MSTAR_INTC_FIQ_NUM, errp)) {
        return;
    }

    /*
     * emac (classic Atmel MACB/EMAC behind the RIU bus). A real NIC wired to a
     * QEMU netdev: attach with e.g. -nic user,model=mstar-emac,hostfwd=... . Its
     * interrupt is "irq" mst-intc line 26 (from the DT); wired here, after the
     * mst-intc is realized.
     */
    qemu_configure_nic_device(DEVICE(&s->emac), true, NULL);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->emac), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->emac), 0, MSTAR_EMAC_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->emac), 1, MSTAR_EMACPHY_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->emac), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_EMAC_HWIRQ));
    /* Second aperture the vendor camera driver uses for the same MAC regs. */
    {
        MemoryRegion *alias = g_new(MemoryRegion, 1);
        memory_region_init_alias(alias, OBJECT(s), "mstar.emac-alt",
                                 &s->emac.iomem, 0, MSTAR_EMAC_SIZE);
        memory_region_add_subregion(get_system_memory(), MSTAR_EMAC_ALT_BASE,
                                    alias);
    }

    /*
     * Watchdog (watchdog@6000), common to all these SoCs. Its own region at the
     * bottom of the 0x1f006000 bank (0x6000..0x603f), below the timers (+0x40)
     * and the emac-phy (+0x200). Its pre-timeout interrupt is a "fiq" mst-intc
     * line - 2 on infinity, but the mercury5 firmware routes it to line 34
     * (its "sys_watchDogHandler" is at the matching GIC INTID 162), so the line
     * is per-SoC (MStarSoCInfo::wdt_hwirq; 0 selects the infinity default).
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->wdt), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->wdt), 0, MSTAR_WDT_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->wdt), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_fiq),
                                        sc->info.wdt_hwirq ? sc->info.wdt_hwirq
                                                           : MSTAR_WDT_HWIRQ));

    /*
     * Passive register banks common to the family (RAM-backed, read-after-write
     * consistent): the read-only fuse array (efuse@4000) and the syscon/
     * simple-mfd bank (syscon@226600). Modelled so the firmware sees consistent
     * read-back rather than the catch-all's 0.
     */
    object_property_set_uint(OBJECT(&s->efuse), "size", MSTAR_EFUSE_SIZE,
                             &error_abort);
    object_property_set_bool(OBJECT(&s->efuse), "readonly", true, &error_abort);
    object_property_set_str(OBJECT(&s->efuse), "name", "mstar.efuse",
                            &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->efuse), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->efuse), 0, MSTAR_EFUSE_BASE);

    object_property_set_uint(OBJECT(&s->syscon), "size", MSTAR_SYSCON_SIZE,
                             &error_abort);
    object_property_set_str(OBJECT(&s->syscon), "name", "mstar.syscon",
                            &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->syscon), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->syscon), 0, MSTAR_SYSCON_BASE);

    /*
     * CMDQ command-queue engine (0x1f224000) - present on infinity/infinity2m/
     * infinity3. Named store/read-back region; register layout still being
     * mapped (see hw/misc/mstar_cmdq.c, MSTAR_CMDQ_DBG). The camera drives it as
     * the SCLIRQ capture command queue; the Miyoo display path touches it too.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->cmdq), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->cmdq), 0, MSTAR_CMDQ_BASE);

    /*
     * SoC PIT timers (timer@6040, free-running counters). Their input clock is
     * per-variant: the SSD20xD family runs them from the 432MHz clk_timer, the
     * msc313/infinity3 from the 12MHz xtal_div2. The vendor kernel programs its
     * clockevent using this rate, so getting it wrong makes every deadline land
     * in the past and storms the timer interrupt.
     */
    for (i = 0; i < MSTAR_NUM_TIMERS; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->timer[i]);

        object_property_set_int(OBJECT(&s->timer[i]), "freq",
                                sc->info.timer_freq, &error_abort);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, MSTAR_TIMER_BASE + i * MSTAR_TIMER_STRIDE);
        /* timer@6040/80/c0 interrupt on "fiq" mst-intc lines 0, 1 and 12. */
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(DEVICE(&s->intc_fiq),
                                            mstar_timer_hwirq[i]));
    }

    /* RTC - its alarm interrupt routed through the "irq" mst-intc. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rtc), 0, MSTAR_RTC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_RTC_HWIRQ));

    /* SAR ADC - its conversion-done interrupt routed through the "irq" intc. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sar), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sar), 0, MSTAR_SAR_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sar), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_SAR_HWIRQ));

    /*
     * GPIO pad register bank. On camera boards, GPIO8/GPIO9 are the bit-banged
     * sensor SCCB: the board sets s->gpio.gpioi2c (via MStarMachineClass.
     * has_gpioi2c) before realize so the gpio creates the i2c bus, and its
     * board_init attaches a sensor module to it (see mstar_msc313e_cam.c).
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio), 0, MSTAR_GPIO_BASE);

    /* PM-domain GPIO bank (carries the SD card-detect). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pm_gpio), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pm_gpio), 0, MSTAR_PM_GPIO_BASE);

    /* ISP SPI-NOR controller (core regs, qspi config, XIP window). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->isp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->isp), 0, MSTAR_ISP_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->isp), 1, MSTAR_FSP_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->isp), 2, MSTAR_ISP_QSPI_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->isp), 3, MSTAR_ISP_XIP_BASE);

    /* BDMA engine, its two channels interrupting via the "irq" mst-intc. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->bdma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->bdma), 0, MSTAR_BDMA_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->bdma), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_BDMA_CH0_HWIRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->bdma), 1,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_BDMA_CH1_HWIRQ));

    /* bach audio controller + its audiotop syscon (dummy, logs accesses). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->bach), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->bach), 0, MSTAR_BACH_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->bach), 1, MSTAR_AUDIOTOP_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->bach), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_BACH_HWIRQ));

    /*
     * clkgen and pinctrl: SoC-specific register-probe blocks that store the
     * registers and log anything the v6.5 driver does not describe (see
     * mstar_regprobe.c), for finding registers not yet in the mainline kernel.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->clkgen), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->clkgen), 0, MSTAR_CLKGEN_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pinctrl), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pinctrl), 0, MSTAR_PINCTRL_BASE);

    /* "sdio" FCIE SD/MMC host, with a card backed by "-drive if=sd" if given. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sdio), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sdio), 0, MSTAR_SDIO_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sdio), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_SDIO_HWIRQ));
    {
        DriveInfo *di = drive_get(IF_SD, 0, 0);

        if (di) {
            DeviceState *card = qdev_new(TYPE_SD_CARD);
            BusState *bus = qdev_get_child_bus(DEVICE(&s->sdio), "sd-bus");

            qdev_prop_set_drive_err(card, "drive",
                                    blk_by_legacy_dinfo(di), &error_fatal);
            qdev_realize_and_unref(card, bus, &error_fatal);
        }
    }

    /* "pwm" controller; its channel 0 is the panel backlight. */
    if (sc->info.has_display) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->pwm), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->pwm), 0, MSTAR_PWM_BASE);
    }

    /* "disp" GOP/display-top pipeline, scanned out to a QEMU console. */
    if (sc->info.has_display) {
        s->disp.backlight = &s->pwm;
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->disp), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->disp), 0, MSTAR_DISP_GOP_BASE);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->disp), 1, MSTAR_DISP_TOP_BASE);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->disp), 2, MSTAR_DISP_MOP_BASE);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->disp), 3, MSTAR_DISP_DSI_BASE);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->disp), 4, MSTAR_DISP_GE_BASE);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->dphy), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->dphy), 0, MSTAR_DPHY_BASE);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->disp), 0,
                           qdev_get_gpio_in(DEVICE(&s->intc_irq),
                                            MSTAR_DISP_HWIRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->disp), 1,
                           qdev_get_gpio_in(DEVICE(&s->intc_irq),
                                            MSTAR_DISP_GOP_HWIRQ));
    }

    /* HWI2C masters (transfers NAK until a slave is attached to the bus). */
    for (i = 0; i < MSTAR_NUM_I2C; i++) {
        static const hwaddr i2c_base[MSTAR_NUM_I2C] = {
            MSTAR_I2C0_BASE, MSTAR_I2C1_BASE,
        };

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2c[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2c[i]), 0, i2c_base[i]);
    }

    /* Optional RIU I/O tracer (MSTAR_IOLOG=<file>, or MSTAR_IOLOG_STDERR=1 when
     * a file cannot be opened under a sandbox): see mstar_iolog(). */
    if ((getenv("MSTAR_IOLOG") || getenv("MSTAR_IOLOG_STDERR")) &&
        !mstar_iolog_fp) {
        MemoryRegion *log = g_new(MemoryRegion, 1);

        if (getenv("MSTAR_IOLOG")) {
            mstar_iolog_fp = fopen(getenv("MSTAR_IOLOG"), "w");
        }
        if (getenv("MSTAR_IOLOG_UNIQUE") || getenv("MSTAR_IOLOG_STDERR")) {
            mstar_iolog_seen = g_malloc0(0x100000);   /* 0x400000/4 entries */
        }
        memory_region_init_io(log, OBJECT(s), &mstar_iolog_catchall_ops, s,
                              "mstar.iolog", 0x400000);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            MSTAR_RIU_BASE, log, -1);
    }

    /* "pm" UART - the infinity/IPL console, its IRQ routed through "irq" intc.
     * (uart1 @0x1f221200 is the mercury5 RTOS console and is modelled by the
     * mercury5 SoC itself, since that firmware drives it as a MStar-native UART
     * with an RX interrupt rather than a plain 16550.) */
    serial_mm_init(get_system_memory(), MSTAR_PM_UART_BASE,
                   MSTAR_PM_UART_REGSHIFT,
                   qdev_get_gpio_in(DEVICE(&s->intc_irq), MSTAR_PM_UART_HWIRQ),
                   MSTAR_PM_UART_CLK / 16, serial_hd(0), DEVICE_LITTLE_ENDIAN);
}

static void mstar_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mstar_soc_realize;
    /* SoCs are instantiated by their board, not directly by the user. */
    dc->user_creatable = false;
}

/*
 * The concrete SoC variants (infinity3/MSC313E, infinity2m/SSD20xD, SSD203D)
 * and their boards live in the per-family files hw/arm/mstar_<family>.c; each
 * subclasses TYPE_MSTAR_SOC / TYPE_MSTAR_MACHINE and fills in MStarSoCInfo.
 */

/* ------------------------------------------------------- abstract machine */

static struct arm_boot_info mstar_binfo;

static void mstar_machine_init(MachineState *machine)
{
    MStarMachineClass *mmc = MSTAR_MACHINE_GET_CLASS(machine);
    MStarSoCState *soc;

    soc = MSTAR_SOC(object_new(mmc->soc_type));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc));
    object_unref(OBJECT(soc));
    /* A mask-ROM boot (-bios) runs in Secure state, so give the CPU EL3. */
    soc->secure_boot = machine->firmware != NULL || getenv("MSTAR_SECURE_KERNEL");
    /* Camera boards drive a sensor over GPIO8/9 bit-banged SCCB (pre-realize). */
    soc->gpio.gpioi2c = mmc->has_gpioi2c;
    qdev_realize(DEVICE(soc), NULL, &error_fatal);

    /*
     * Board-specific devices not present on every board with this SoC (e.g. the
     * Miyoo Mini's alpu-fa auth chip, or a camera board's capture pipeline +
     * sensor). Each board wires its own in its machine file, so nothing leaks
     * onto the other boards.
     */
    if (mmc->board_init) {
        mmc->board_init(soc);
    }

    memory_region_add_subregion(get_system_memory(), MSTAR_DRAM_BASE,
                                machine->ram);

    if (machine->firmware) {
        /*
         * Real DRAM wraps (aliases) above its size, and the mask-ROM IPL relies
         * on that to size memory: it writes distinct markers at DRAM +0/+32M/
         * +64M/+128M/+256M and reads them back to find the wrap boundary (there
         * is no size register). QEMU's RAM is a flat region, so accesses above
         * the real size would just fall into unmapped space and the IPL would
         * detect no wrap and print the 512MB maximum. Mirror the RAM above its
         * real size, across the range the IPL probes, to reproduce the wrap so
         * it detects the true size (e.g. an SSD202D reports "128MB"). Only the
         * mask-ROM path needs this; a -kernel boot takes its size from the DTB.
         */
        uint64_t ram_size = machine->ram_size;
        uint64_t off;

        for (off = ram_size; off + ram_size <= 512 * MiB; off += ram_size) {
            MemoryRegion *mirror = g_new(MemoryRegion, 1);

            memory_region_init_alias(mirror, OBJECT(soc), "mstar.dram-mirror",
                                     machine->ram, 0, ram_size);
            memory_region_add_subregion(get_system_memory(),
                                        MSTAR_DRAM_BASE + off, mirror);
        }
    }

    if (machine->firmware) {
        /*
         * Boot ROM mode: map the mask ROM at address 0 and let the CPU reset
         * straight into it (no kernel boot wrapper). The ROM brings up the
         * SoC and chain-loads the next stage itself. As observed on the
         * msc313e mask ROM, the flow is:
         *
         *   1. Read the boot-media strap from DID_KEY (0x1f0071c0) bits[5:2]:
         *      0x20 selects SPI-NOR (the ISP/QSPI).
         *   2. Read the "IPL" from the SPI-NOR flash (the partition table
         *      points it at flash offset 0x4000) through the memory-mapped
         *      XIP window at 0x14000000, and copy it into IMI SRAM at
         *      0xa0000000.
         *   3. Check the "IPL_" magic at IMI+4 and jump to 0xa0000000.
         *
         * So to reach the IPL, pass the ROM with -bios and the NOR flash
         * image with -drive if=mtd.
         *
         * The IPL then runs its own DDR/PLL bring-up, prints its banner
         * ("IPL <ver>", "512MB", ...) on the pm UART, passes the MIU memory
         * BIST (see the "miu" block), decompresses the next stage and hands
         * off to u-boot. u-boot comes up on the console, runs its MDIO PHY
         * scan against the modelled "emac" (finds no PHY, fakes a link) and
         * reaches its command prompt.
         */
        MemoryRegion *rom = g_new(MemoryRegion, 1);
        char *fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);

        memory_region_init_rom(rom, NULL, "mstar.bootrom", MSTAR_BOOTROM_SIZE,
                               &error_fatal);
        memory_region_add_subregion(get_system_memory(), MSTAR_BOOTROM_BASE,
                                    rom);
        if (load_image_mr(fn ? fn : machine->firmware, rom) < 0) {
            error_report("could not load boot ROM '%s'", machine->firmware);
            exit(1);
        }
        g_free(fn);
    } else {
        mstar_binfo.loader_start = MSTAR_DRAM_BASE;
        mstar_binfo.ram_size = machine->ram_size;
        /* The vendor kernel expects to enter in Secure state (see above). */
        mstar_binfo.secure_boot = soc->secure_boot;
        arm_load_kernel(&soc->cpu[0], machine, &mstar_binfo);
    }
}

static void mstar_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = mstar_machine_init;
    mc->default_ram_id = "mstar.dram";
    mc->min_cpus = 1;
    mc->default_cpus = 1;
    /*
     * Peripherals other than the console (GIC, timer, ...) are not modelled
     * yet, so let the kernel poke at their addresses without aborting.
     */
    mc->ignore_memory_transaction_failures = true;
}

/* ----------------------------------------------------------------- Types */

/*
 * Only the abstract base SoC and machine live here. Concrete SoCs and boards
 * are registered by the per-family files (mstar_infinity3.c, mstar_infinity2m.c)
 * which subclass these.
 */
static const TypeInfo mstar_types[] = {
    {
        .name           = TYPE_MSTAR_SOC,
        .parent         = TYPE_DEVICE,
        .instance_size  = sizeof(MStarSoCState),
        .instance_init  = mstar_soc_init,
        .class_size     = sizeof(MStarSoCClass),
        .class_init     = mstar_soc_class_init,
        .abstract       = true,
    },
    {
        .name           = TYPE_MSTAR_MACHINE,
        .parent         = TYPE_MACHINE,
        .instance_size  = sizeof(MStarMachineState),
        .class_size     = sizeof(MStarMachineClass),
        .class_init     = mstar_machine_class_init,
        .abstract       = true,
        /* Make derived boards show up in qemu-system-arm/-aarch64. */
        .interfaces     = arm_machine_interfaces,
    },
};

DEFINE_TYPES(mstar_types)
