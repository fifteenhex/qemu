/*
 * Sun 3/80 ("sun3x") system emulation.
 *
 * A Motorola 68030 workstation using the CPU's on-chip PMMU (the same unit
 * already exercised by HP9000/340 and Apollo) — NOT the discrete Sun-3 MMU of
 * the sun3-60.  Boots the Linux/m68k `sun3x` kernel (CONFIG_SUN3X) to a serial
 * shell over the Zilog Z8530 (zs) via the resident boot PROM console.
 *
 * The kernel is entered MMU-off at its ELF entry in RAM (PAGE_OFFSET==0, so
 * kernel VA==PA), with a standard m68k bootinfo block appended after the
 * image; head.S builds the page tables and engages the PMMU.  In addition the
 * sun3x head.S path expects a resident boot PROM: it copies the PROM's page
 * table (root pointer read indirectly through 0xfefe00d4) for the mapped
 * window 0xfee00000..0xff000000, and both head.S and the kernel console reach
 * the serial port only through the PROM's putchar/getchar romvec routines.
 * We therefore synthesise a minimal PROM (romvec + identity page table + a few
 * hand-assembled m68k stubs that poke the zs).  See sun3x-NOTES.md.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/reset.h"
#include "system/system.h"
#include "system/rtc.h"
#include "exec/target_page.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/escc.h"
#include "hw/scsi/ncr5380.h"
#include "hw/scsi/scsi.h"
#include "system/blockdev.h"
#include "hw/intc/m68k_irqc.h"
#include "chardev/char.h"
#include "qemu/timer.h"
#include "qemu/datadir.h"
#include "system/address-spaces.h"
#include "target/m68k/cpu.h"
#include "elf.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "bootinfo.h"

/* --- physical memory map (see sun3x-NOTES.md) --- */
#define SUN3X_RAM_BASE          0x00000000
#define SUN3X_RAM_DEFAULT       (16 * MiB)
#define SUN3X_RAM_MAX           (64 * MiB)

#define SUN3X_IOMMU             0x60000000
#define SUN3X_IOMMU_SIZE        0x8000      /* DVMA I/O mapper (PROM walks to
                                             * 0x600079fc); ~8K PTEs x 4 bytes */
#define SUN3X_ENAREG            0x61000000  /* enable/copro block base */
#define SUN3X_ENA_SIZE          0x2000      /* covers INTREG @+0x1400, DIAG @+0x1800 */
#define SUN3X_BUSERR            0x61000400  /* OBIO_BUSERRREG */
#define SUN3X_BE_TIMEOUT        0x20        /* bus timeout latch (bit 5) */

/*
 * The monitor's periodic (level-7 NMI) clock.  Its handler reaches the clock
 * registers through virtual addresses 0xfef06010 (pending) and 0xfef0b400
 * (ack); once the monitor has installed its page tables those map to these
 * physical OBIO addresses (found by walking the live 68030 page tables with
 * m68k_cpu_get_phys_addr_debug).  We place the clock status/ack device there,
 * on the physical side of the monitor's MMU, so the handler actually sees it.
 */
#define SUN3X_CLK_STAT          0x64000810  /* phys of virt 0xfef06010 */
#define SUN3X_CLK_ACK           0x61002800  /* phys of virt 0xfef0b400 */
#define SUN3X_CLK_STAT_VA       0xfef06010  /* the monitor's virtual address */
#define SUN3X_INTREG            0x61001400  /* offset 0x1400 within the block */
#define SUN3X_ZS1               0x62000000  /* console Z8530 */
#define SUN3X_ZS2               0x62002000  /* keyboard/mouse Z8530 (idle) */
#define SUN3X_ZS_SIZE           0x2000
#define SUN3X_EEPROM            0x64000000  /* NVRAM: idprom @+0x7d8, clock @+0xf8 */
#define SUN3X_EEPROM_SIZE       0x800
#define SUN3X_IDPROM_OFF        0x7d8
#define SUN3X_CLOCK_OFF         0xf8

/* PROM window: identity-mapped, holds romvec + page table + stubs */
#define SUN3X_PROM_PHYS         0xfee00000
#define SUN3X_PROM_SIZE         0x200000
#define SUN3X_PROM_BASE         0xfefe0000  /* romvec (SUN3X_PROM_BASE) */

/*
 * SunOS (real-PROM) mode: when a boot PROM image is supplied (-bios), the
 * synthesised Linux romvec is replaced by the genuine Sun 3/80 boot PROM.
 * The real PROM decodes as the top 128 KiB of the 0xfee00000 window; the
 * 0xfef00000..0xfefe0000 region just below it is the monitor's private
 * scratch/stack/VBR RAM (reset SSP=0xfef60c00, VBR set to 0xfef60c00).
 * Unmapped physical space raises a genuine 68030 bus error (no catch-all
 * background), which the PROM self-test relies on to size memory and probe
 * for optional devices.
 */
#define SUN3X_ROM_SIZE          0x20000     /* 128 KiB genuine boot PROM */
#define SUN3X_PROMRAM_BASE      0xfef00000  /* monitor scratch/stack/VBR */
#define SUN3X_PROMRAM_SIZE      (SUN3X_PROM_BASE - SUN3X_PROMRAM_BASE)

/*
 * The genuine PROM is also decoded low, at 0x63000000: the cold-start
 * self-test rebases its own data/vector references there (it sets
 * VBR := 0x63000000 and reads its TC sentinel from 0x63007860), so its
 * exception vectors (e.g. bus error, vector 2 -> ROM+8) resolve through it
 * while it probes the bus.
 */
#define SUN3X_PROM_ALIAS        0x63000000
#define SUN3X_PROM_ALIAS_SIZE   0x1000000   /* 16 MiB: ROM shadow + reloc RAM */

/* offsets within the PROM region (relative to SUN3X_PROM_PHYS) */
#define OFF(va)                 ((va) - SUN3X_PROM_PHYS)
#define ROMVEC_VA               0xfefe0000
#define D4_VA                   0xfefe00d4  /* head.S reads *(0xfefe00d4) */
#define PTP_VA                  0xfefe0200  /* page-table-pointer word */
#define PGTABLE_VA              0xfefe0400  /* 256 x u32 page descriptors */
#define PUTCHAR_VA              0xfefe0800
#define GETCHAR_VA              0xfefe0840
#define HALT_VA                 0xfefe0880
#define MONID_VA                0xfefe08a0

/* romvec field offsets used by the Linux sun3x port (openprom.h) */
#define RV_GETCHAR              20
#define RV_PUTCHAR              24
#define RV_NBGETCHAR            28
#define RV_NBPUTCHAR            32
#define RV_MONID                76
#define RV_REBOOT               96
#define RV_ABORT                152

/* 68030 short page descriptor bits for the identity PROM mapping */
#define SUN3X_PROM_PTE_FLAGS    0x59  /* PRESENT|ACCESSED|DIRTY|NOCACHE030 */

/* IDPROM: struct idprom, checksum = xor of bytes[0..14] */
#define SM_SUN3X                0x40
#define SM_3_80                 0x02
#define SUN3X_MACHTYPE          (SM_SUN3X | SM_3_80)     /* 0x42 */

/* interrupt register bits */
#define SUN3X_INT_ENABLE_ALL    0x01
#define SUN3X_INT_SOFT_1        0x02        /* software interrupt, level 1 */
#define SUN3X_INT_SOFT_2        0x04        /* software interrupt, level 2 */
#define SUN3X_INT_SOFT_3        0x08        /* software interrupt, level 3 */
#define SUN3X_INT_ENABLE_5      0x20

/* clock tick rate (Linux/m68k CONFIG_HZ default) */
#define SUN3X_HZ                100

/* Z8530 (ESCC) channel-A register byte offsets for it_shift==1:
 *   channel = (addr >> 2) & 1 (A == 1), reg = (addr >> 1) & 1 (ctrl 0/data 1) */
#define SUN3X_ZS_FREQ           4915200

typedef struct Sun3xState {
    M68kCPU *cpu;
    DeviceState *irqc;

    MemoryRegion bg;            /* absorb/zero background */
    MemoryRegion iommu;         /* DVMA page table scratch */
    MemoryRegion enareg;        /* enable/copro + LED block (absorbed) */
    MemoryRegion intreg_mr;     /* the interrupt/enable register */
    MemoryRegion eeprom;        /* NVRAM (idprom + clock) */
    MemoryRegion prom;          /* synthesised (Linux) or real (SunOS) PROM */
    MemoryRegion prom_alias;    /* SunOS: PROM also decoded at 0x63000000 */
    MemoryRegion promram;       /* SunOS: monitor scratch/stack/VBR RAM */
    MemoryRegion obio[6];       /* SunOS: misc OBIO registers (absorbed) */
    MemoryRegion memhole;       /* SunOS: absent memory banks (read 0) */
    MemoryRegion buserr_mr;     /* SunOS: bus error register @ 0x61000400 */
    MemoryRegion memreg_mr;     /* SunOS: memory error register @ 0x61001000 */
    MemoryRegion idprobe_hole;  /* SunOS: bus-error hole @ 0x61001800 (see below) */
    MemoryRegion idprobe_hole2; /* SunOS: bus-error hole @ 0x61000c00 (kernel) */
    MemoryRegion busfault;      /* SunOS: background bus-timeout (latches) */
    MemoryRegion clkstat;       /* SunOS: clock int-pending @ 0xfef06010 */
    MemoryRegion clkack;        /* SunOS: clock int-ack     @ 0xfef0b400 */

    NCR5380State scsi;          /* SunOS: "si" NCR5380 @ 0x66000000 */
    MemoryRegion sireg;         /* SunOS: si DMA/CSR block @ 0x66001000 */
    uint32_t si_dma_addr;       /* si DMA address (into DVMA space) */
    uint32_t si_dma_count;      /* si DMA byte count */
    uint16_t si_csr;            /* si control/status register */
    uint8_t si_fifo[16];        /* si data FIFO (probe/DMA staging) */
    uint8_t si_fifo_head, si_fifo_tail, si_fifo_count;
    uint8_t si_reg20;           /* si scratch register @ +0x20 (read-back) */
    uint8_t si_reg24;           /* si dma-control scratch @ +0x24 */
    uint8_t si_reg2c;           /* si scratch @ +0x2c */
    MemoryRegion siscratch;     /* si scratch overlay reclaiming +0x20/+0x24 */

    bool sunos;                 /* real-PROM (SunOS) mode */
    uint8_t buserr;             /* SunOS bus error register latch */
    uint8_t memreg;             /* SunOS memory error/control register */
    bool clk_pending;           /* SunOS level-7 clock interrupt pending */
    bool clk_enabled;           /* SunOS: monitor has mapped in its clock */
    uint8_t *promram_ptr;       /* SunOS: host ptr to the monitor scratch RAM */

    QEMUTimer *tick;
    uint8_t intreg;
    bool tick_latch;

    uint32_t reset_pc;
    uint32_t reset_sp;
} Sun3xState;

/* --- interrupt / timer --- */

static void sun3x_update_irq(Sun3xState *s)
{
    bool master = s->intreg & SUN3X_INT_ENABLE_ALL;
    /* Linux shim: the periodic clock is level-5, gated on the interrupt reg. */
    bool level5 = !s->sunos && s->tick_latch && master &&
                  (s->intreg & SUN3X_INT_ENABLE_5);

    /* level 5 autovector -> IRQC gpio index 4 (LEVEL_1..7 are 0-based) */
    qemu_set_irq(qdev_get_gpio_in(s->irqc, 5 - 1), level5);

    /*
     * SunOS/monitor: the boot-countdown clock is the level-7 (NMI) auto-vector.
     * It goes live only once the monitor has installed its page tables (the
     * clock registers become reachable through the monitor's virtual address
     * 0xfef06010, which maps to our clock device at SUN3X_CLK_STAT) — this keeps
     * it inert during the self-test (identity mapping) and off until the monitor
     * owns the clock and has set up its interrupt stack.  Delivery is a held
     * level cleared by the handler's ack (SUN3X_CLK_ACK); the ack clears the
     * latch so the next tick re-asserts the level and re-arms the NMI edge.
     */
    if (s->sunos) {
        /* latch "clock enabled" the first time the monitor's page tables map
         * its clock register through to our device; thereafter fire every tick
         * (re-checking the live mapping each tick flakes while the handler runs
         * in a different context). */
        if (!s->clk_enabled &&
            m68k_cpu_get_phys_addr_debug(CPU(s->cpu),
                SUN3X_CLK_STAT_VA) == SUN3X_CLK_STAT) {
            s->clk_enabled = true;
        }
        /*
         * Only deliver the edge while the monitor's level-7 vector
         * (VBR+0x7c, translated through its live page tables) holds a
         * handler.  The clock-register mapping can appear a tick or
         * two before the monitor copies its vector table into the
         * currently mapped vector page; an NMI in that window vectors
         * through 0 and storms the ISP into a double bus fault.  Real
         * hardware has no such window: the monitor only programs the
         * clock chip's interrupt enable once the handler is in place.
         */
        bool vec_ok = false;
        if (s->clk_enabled) {
            CPUM68KState *env = &s->cpu->env;
            hwaddr vph = m68k_cpu_get_phys_addr_debug(CPU(s->cpu),
                                                      env->vbr + 0x7c);
            if (vph != -1) {
                vec_ok = ldl_be_phys(&address_space_memory, vph) != 0;
            }
        }
        qemu_set_irq(qdev_get_gpio_in(s->irqc, 7 - 1),
                     s->clk_pending && s->clk_enabled && vec_ok);
    }

    /*
     * Software interrupts (SunOS/PROM): writing bit 1/2/3 of the interrupt
     * register raises an autovector interrupt at level 1/2/3.  The Linux shim
     * never sets these bits, so gate on SunOS mode to keep it byte-identical.
     */
    if (s->sunos) {
        qemu_set_irq(qdev_get_gpio_in(s->irqc, 1 - 1),
                     master && (s->intreg & SUN3X_INT_SOFT_1));
        qemu_set_irq(qdev_get_gpio_in(s->irqc, 2 - 1),
                     master && (s->intreg & SUN3X_INT_SOFT_2));
        qemu_set_irq(qdev_get_gpio_in(s->irqc, 3 - 1),
                     master && (s->intreg & SUN3X_INT_SOFT_3));
    }
}

static void sun3x_tick(void *opaque)
{
    Sun3xState *s = opaque;

    s->tick_latch = true;

    /*
     * Re-arm the SunOS level-7 (NMI) clock edge.  Drop then raise the pending
     * latch so sun3x_update_irq() deasserts then re-asserts level 7, giving the
     * NMI a fresh 0->7 edge every tick.  This does not depend on the monitor's
     * clock handler acking us (its ack goes through the monitor's MMU, which
     * may map the ack register to a different context than we can see), so the
     * boot-countdown tick counter advances reliably.
     */
    s->clk_pending = false;
    sun3x_update_irq(s);
    s->clk_pending = true;
    sun3x_update_irq(s);

    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / SUN3X_HZ);
}

/* --- SunOS periodic-clock (level 7) status / ack, in the monitor window --- */


static uint64_t sun3x_clkstat_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;

    /* the monitor's level-7 handler reads this and only advances its tick
     * counter when it is non-zero (clock interrupt pending) */
    return s->clk_pending ? 0x01 : 0x00;
}

static void sun3x_clkstat_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
}

static const MemoryRegionOps sun3x_clkstat_ops = {
    .read = sun3x_clkstat_read,
    .write = sun3x_clkstat_write,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .endianness = DEVICE_BIG_ENDIAN,
};

static uint64_t sun3x_clkack_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0x80;
}

static void sun3x_clkack_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    Sun3xState *s = opaque;

    /* the handler acks the clock by pulsing this register; clear the latch */    s->clk_pending = false;
    sun3x_update_irq(s);
}

static const MemoryRegionOps sun3x_clkack_ops = {
    .read = sun3x_clkack_read,
    .write = sun3x_clkack_write,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- SunOS "si" NCR5380 DMA/CSR block (0x66000008.. and 0x66001000) --- */

/* si control/status register bits (authoritative, NetBSD sun3 sireg.h) */
#define SI_CSR_DMA_ACTIVE   0x8000  /* r/o */
#define SI_CSR_DMA_CONFLICT 0x4000  /* r/o */
#define SI_CSR_DMA_BUS_ERR  0x2000  /* r/o */
#define SI_CSR_ID           0x1000  /* r/o: 1=SCSI-3/EMULEX */
#define SI_CSR_FIFO_FULL    0x0800  /* r/o */
#define SI_CSR_FIFO_EMPTY   0x0400  /* r/o */
#define SI_CSR_SBC_IP       0x0200  /* r/o: 5380 interrupt pending */
#define SI_CSR_DMA_IP       0x0100  /* r/o: DMA interrupt pending */
#define SI_CSR_DMA_EN       0x0010  /* r/w: DMA enable */
#define SI_CSR_SEND         0x0008  /* r/w: DMA direction 1=to device */
#define SI_CSR_INTR_EN      0x0004  /* r/w: interrupt enable */
#define SI_CSR_FIFO_RES     0x0002  /* r/w: fifo reset */
#define SI_CSR_SCSI_RES     0x0001  /* r/w: scsi bus reset */
#define SI_CSR_CTRL_BITS    0x001f  /* r/w control bits */

/*
 * Translate a sun3x DVMA address to a physical main-memory address using the
 * IOMMU page table the PROM builds at SUN3X_IOMMU: 16 MiB DVMA space, 8 KiB
 * pages, 2048 PTEs of 4 bytes.  PTE valid when the low bits are set;
 * phys = (pte & 0xffffe000) | (dva & 0x1fff).
 */
static bool sun3x_dvma_to_phys(uint32_t dva, hwaddr *pa)
{
    uint32_t idx = (dva >> 13) & 0x7ff;
    uint32_t pte = ldl_be_phys(&address_space_memory, SUN3X_IOMMU + idx * 4);

    if (!(pte & 0x3)) {
        return false;               /* invalid mapping */
    }
    *pa = ((hwaddr)(pte & 0xffffe000)) | (dva & 0x1fff);
    return true;
}

/*
 * Run the si pseudo-DMA engine: move up to si_dma_count bytes between the
 * NCR5380 data phase and DVMA memory (through the IOMMU).  SI_CSR_SEND set
 * means data-out (initiator -> target).  Sets DMA_IP when the run drains.
 */
static void sun3x_si_dma_run(Sun3xState *s)
{
    NCR5380State *n = &s->scsi;
    bool out;

    /*
     * Direction comes from the 5380's live data phase, NOT the si_csr bits.
     * The PROM's DMA-arm (0xfeff0e04) encodes a data-IN read as si_csr|=0x300
     * (SBC_IP|DMA_IP) and a data-OUT write as si_csr|=0x200 (SBC_IP only) —
     * the opposite of a naive SEND/DMA_IP reading — so inferring the direction
     * from those bits would run the transfer the wrong way and move nothing.
     * The bus phase (set by the SCSI command executed at selection) is
     * authoritative.
     */
    if (ncr5380_pdma_ready(n, false)) {
        out = false;                    /* data-in  (READ/INQUIRY/...) */
    } else if (ncr5380_pdma_ready(n, true)) {
        out = true;                     /* data-out (WRITE/MODE SELECT) */
    } else {
        s->si_csr |= SI_CSR_DMA_IP;     /* nothing to move */
        return;
    }

    while (s->si_dma_count > 0 && ncr5380_pdma_ready(n, out)) {
        hwaddr pa;

        if (!sun3x_dvma_to_phys(s->si_dma_addr, &pa)) {
            s->si_csr |= SI_CSR_DMA_BUS_ERR;
            break;
        }
        if (out) {
            uint8_t b = ldub_phys(&address_space_memory, pa);
            ncr5380_pdma_write(n, b);
        } else {
            uint8_t b = ncr5380_pdma_read(n);
            stb_phys(&address_space_memory, pa, b);
        }
        s->si_dma_addr++;
        s->si_dma_count--;
    }
    s->si_csr |= SI_CSR_DMA_IP;      /* DMA interrupt / transfer done */
}

/* addr is relative to 0x66000000; the DMA/CSR block starts at +0x1000 */
static uint64_t sun3x_sireg_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;
    uint64_t v = 0;

    switch (addr) {
    case 0x00:              /* DMA transfer count, low byte */
        v = s->si_dma_count & 0xff;
        break;
    case 0x04:              /* DMA transfer count, high byte */
        v = (s->si_dma_count >> 8) & 0xff;
        break;
    case 0x20:              /* si scratch register (read-back register test) */
        v = s->si_reg20;
        break;
    case 0x24:              /* si dma-control scratch (read-back) */
        v = s->si_reg24;
        break;
    case 0x2c:              /* si scratch (read-back; failure tolerated) */
        v = s->si_reg2c;
        break;
    case 0x1000: {          /* si control/status register.
                             * Progress gate needs bit0(0x1)=1, FIFO_RES(0x2)=0,
                             * FIFO_EMPTY(0x400)=0.  SBC_IP/DMA_IP are the
                             * driver's DMA-sequencing latches; the 0x40
                             * handshake bit reads back clear (completes at
                             * once). */
        uint16_t v16 = SI_CSR_ID | 0x0001;
        v16 |= (s->si_csr & (SI_CSR_SBC_IP | SI_CSR_DMA_IP));
        v = v16;
        break;
    }
    case 0x1004:            /* si DMA address (HW auto-increments during xfer) */
        v = s->si_dma_addr;
        break;
    case 0x1008:            /* si DMA byte count (register test) */
        v = s->si_dma_count;
        break;
    default:
        v = 0;
        break;
    }
    qemu_log_mask(CPU_LOG_MMU, "si rd  @%08x.%u = %08x\n",
                  0x66000000 + (uint32_t)addr, size, (uint32_t)v);
    return v;
}

static void sun3x_sireg_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    Sun3xState *s = opaque;

    switch (addr) {
    case 0x00:              /* DMA transfer count, low byte */
        s->si_dma_count = (s->si_dma_count & 0xff00) | (val & 0xff);
        break;
    case 0x04:              /* DMA transfer count, high byte */
        s->si_dma_count = (s->si_dma_count & 0x00ff) | ((val & 0xff) << 8);
        break;
    case 0x20:
        s->si_reg20 = val;
        break;
    case 0x24:
        s->si_reg24 = val;
        break;
    case 0x2c:
        s->si_reg2c = val;
        break;
    case 0x1000: {
        uint16_t old = s->si_csr;
        s->si_csr = val & 0x03ff;
        if (val & SI_CSR_FIFO_RES) {
            s->si_fifo_head = s->si_fifo_tail = s->si_fifo_count = 0;
        }
        /*
         * The driver arms a transfer by writing SBC_IP(0x200) [plus DMA_IP-bit
         * 0x100 for data-out] after loading dma_addr and the count latches.
         * On that rising edge, run the pseudo-DMA engine now that the 5380 is
         * in its data phase; the engine latches DMA_IP on completion and
         * advances dma_addr by the transfer length — which is the byte count
         * the PROM reads back (dma_addr - start) to decide the transfer
         * succeeded.  Mark the 5380's data phase drained so its reg2 now reads
         * the "transfer complete" phase (3) and the driver finalises it.
         */
        /*
         * The driver arms a transfer by writing SBC_IP(0x200) after loading
         * dma_addr and the count latches.  Run the pseudo-DMA engine once per
         * command: gate on sun_dma_done (cleared by ncr5380_do_command for
         * each new command) rather than a rising-edge of SBC_IP — si_csr is
         * not reliably cleared to 0 between commands (its bits are synthesised
         * on read), so a stale SBC_IP from the previous transfer would
         * otherwise swallow the next arm's edge and the DMA would never run.
         */
        if ((val & SI_CSR_SBC_IP) && !s->scsi.sun_dma_done &&
            (ncr5380_pdma_ready(&s->scsi, false) ||
             ncr5380_pdma_ready(&s->scsi, true))) {
            sun3x_si_dma_run(s);
            s->scsi.sun_dma_done = true;
        }
        /*
         * The driver's data-complete path (0xfeff0e6e) clears the DMA
         * interrupt latches (si_csr &= ~0x0300) once it has read the byte
         * count back.  On that falling edge, advance the SCSI bus to STATUS
         * so the driver's status/message handshake can run.
         */
        if (s->scsi.sun_dma_done &&
            (old & (SI_CSR_SBC_IP | SI_CSR_DMA_IP)) &&
            !(val & (SI_CSR_SBC_IP | SI_CSR_DMA_IP))) {
            ncr5380_sun_to_status(&s->scsi);
        }
        break;
    }
    case 0x1004:
        s->si_dma_addr = val;
        break;
    case 0x1008:
        s->si_dma_count = val;
        break;
    default:
        break;
    }
    qemu_log_mask(CPU_LOG_MMU, "si wr  @%08x.%u = %08x\n",
                  0x66000000 + (uint32_t)addr, size, (uint32_t)val);
}

static const MemoryRegionOps sun3x_sireg_ops = {
    .read = sun3x_sireg_read,
    .write = sun3x_sireg_write,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * The NCR5380 sits at 0x66000008..0x66000027 (reg0 = data @ +0x08), which
 * overlaps the si scratch registers at +0x20/+0x24.  Reclaim those two words
 * at higher priority and forward them to the sireg handlers above.
 */
static uint64_t sun3x_siscratch_read(void *opaque, hwaddr addr, unsigned size)
{
    return sun3x_sireg_read(opaque, addr + 0x20, size);
}

static void sun3x_siscratch_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    sun3x_sireg_write(opaque, addr + 0x20, val, size);
}

static const MemoryRegionOps sun3x_siscratch_ops = {
    .read = sun3x_siscratch_read,
    .write = sun3x_siscratch_write,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

static uint64_t sun3x_intreg_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;

    return s->intreg;
}

static void sun3x_intreg_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    Sun3xState *s = opaque;

    /* the tick handler acks by pulsing the level-5 enable low->high */
    if (!(val & SUN3X_INT_ENABLE_5)) {
        s->tick_latch = false;
    }
    s->intreg = val;
    sun3x_update_irq(s);
}

static const MemoryRegionOps sun3x_intreg_ops = {
    .read = sun3x_intreg_read,
    .write = sun3x_intreg_write,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- enable/copro/LED block: reads 0, writes absorbed --- */

static uint64_t sun3x_absorb_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void sun3x_absorb_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
}

static const MemoryRegionOps sun3x_absorb_ops = {
    .read = sun3x_absorb_read,
    .write = sun3x_absorb_write,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * Absent-memory (floating bus) reads.  A depopulated 3/80 memory bank still
 * ACKs the access (no bus error) but the data lines float high, so a read
 * returns all-ones.  The PROM's memory sizer relies on this: after writing a
 * test pattern to a bank it reads back and treats 0xffffffff as "bank empty"
 * (cleanly absent); any other value is flagged "Bank is partially filled" and
 * drops it into an interactive diagnostic.  Returning 0 here is therefore
 * wrong — absent banks must read as -1.
 */
static uint64_t sun3x_memhole_read(void *opaque, hwaddr addr, unsigned size)
{
    return (size >= 8) ? ~0ULL : (~0ULL >> (64 - size * 8));
}

static const MemoryRegionOps sun3x_memhole_ops = {
    .read = sun3x_memhole_read,
    .write = sun3x_absorb_write,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- SunOS bus error register (0x61000400) + bus-timeout background --- */

static uint64_t sun3x_buserr_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;

    return s->buserr;
}

static void sun3x_buserr_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    Sun3xState *s = opaque;

    /* the PROM writes 0 here to clear the latched fault bits */
    s->buserr = val;
}

static const MemoryRegionOps sun3x_buserr_ops = {
    .read = sun3x_buserr_read,
    .write = sun3x_buserr_write,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * Memory error/control register (0x61001000).  The PROM's Parity Memory test
 * writes control bits (e.g. 0x40) and reads them back; modelled as a simple
 * read/write-back byte (the low control bits), which is enough for the
 * self-test to progress.  A faithful parity-error latch is future work.
 */
static uint64_t sun3x_memreg_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;

    return s->memreg;
}

static void sun3x_memreg_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    Sun3xState *s = opaque;

    s->memreg = val;
}

static const MemoryRegionOps sun3x_memreg_ops = {
    .read = sun3x_memreg_read,
    .write = sun3x_memreg_write,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * Background "bus timeout": any access that decodes to nothing (a nonexistent
 * on-board/VME address) latches the TIMEOUT bit in the bus error register and
 * terminates in a genuine 68030 bus error.  The PROM's Bus Error Register test
 * reads a nonexistent address (0x40000000) and then requires TIMEOUT (bit 5)
 * to be set; memory sizing and device probing likewise depend on the fault.
 * Installed at the lowest priority so RAM, the memhole, OBIO and the PROM
 * windows all take precedence.
 */
static MemTxResult sun3x_busfault_read(void *opaque, hwaddr addr,
                                       uint64_t *data, unsigned size,
                                       MemTxAttrs attrs)
{
    Sun3xState *s = opaque;

    s->buserr |= SUN3X_BE_TIMEOUT;
    *data = 0;
    return MEMTX_DECODE_ERROR;   /* -> m68k bus error (access fault) */
}

static MemTxResult sun3x_busfault_write(void *opaque, hwaddr addr,
                                        uint64_t val, unsigned size,
                                        MemTxAttrs attrs)
{
    Sun3xState *s = opaque;

    s->buserr |= SUN3X_BE_TIMEOUT;
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps sun3x_busfault_ops = {
    .read_with_attrs = sun3x_busfault_read,
    .write_with_attrs = sun3x_busfault_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- IDPROM + Mostek clock (in the NVRAM) --- */

static void sun3x_build_nvram(Sun3xState *s)
{
    uint8_t *p = memory_region_get_ram_ptr(&s->eeprom);
    uint8_t *id = p + SUN3X_IDPROM_OFF;
    /* the genuine 3/80 clock is the Mostek MK48T02 at NVRAM +0x7f8
     * (OBIO_CLOCK2); the Linux shim reads it at +0xf8. */
    uint8_t *clk = p + (s->sunos ? 0x7f8 : SUN3X_CLOCK_OFF);
    struct tm tm;
    uint8_t sum = 0;
    int i;

    memset(p, 0, SUN3X_EEPROM_SIZE);

    /*
     * EEPROM configuration bytes (SunOS mode).  Byte 0x1f selects the console
     * device; force ttya so the genuine monitor talks to the zs serial port
     * and does not wait on an (absent) on-board framebuffer + keyboard.
     */
    if (s->sunos) {
        p[0x1f] = 0x10;         /* EE_CONS_TTYA */
    }

    /* idprom */
    id[0] = 0x01;               /* id_format */
    id[1] = SUN3X_MACHTYPE;     /* Sun 3/80 */
    id[2] = 0x08; id[3] = 0x00; id[4] = 0x20;   /* Sun OUI ethaddr */
    id[5] = 0x11; id[6] = 0x22; id[7] = 0x33;
    /* bytes 8..14 date/serial left plausible (zero); byte15 = checksum */
    for (i = 0; i <= 0x0e; i++) {
        sum ^= id[i];
    }
    id[0x0f] = sum;

    /* Mostek-style clock: csr, sec, min, hour, wday, mday, month, year (BCD) */
    qemu_get_timedate(&tm, 0);
#define BCD(x) ((((x) / 10) << 4) | ((x) % 10))
    clk[0] = 0;                         /* csr */
    clk[1] = BCD(tm.tm_sec);
    clk[2] = BCD(tm.tm_min);
    clk[3] = BCD(tm.tm_hour);
    clk[4] = BCD(tm.tm_wday);
    clk[5] = BCD(tm.tm_mday);
    clk[6] = BCD(tm.tm_mon + 1);
    clk[7] = BCD(tm.tm_year % 100);
#undef BCD
}

/* --- the synthesised boot PROM --- */

/*
 * Hand-assembled m68k stubs (big-endian).  They drive channel A of the Z8530
 * at 0x62000000 (it_shift==1: ch-A CTRL @ +4, ch-A DATA @ +6).  The minimal
 * PROM does no zs init, and the ESCC drops TX unless WR5.TXEN and blocks RX
 * unless WR3.RXEN, so each routine (re)enables its direction first.
 */
static const uint8_t sun3x_putchar_code[] = {
    /* pv_putchar/pv_nbputchar: char = low byte of arg long at 4(sp) */
    0x13, 0xfc, 0x00, 0x05, 0x62, 0x00, 0x00, 0x04, /* move.b #5,0x62000004  (sel WR5) */
    0x13, 0xfc, 0x00, 0x68, 0x62, 0x00, 0x00, 0x04, /* move.b #0x68,..       (TXEN|Tx8) */
    0x12, 0x39, 0x62, 0x00, 0x00, 0x04,             /* poll: move.b 0x62000004,d1 (RR0) */
    0x08, 0x01, 0x00, 0x02,                         /* btst #2,d1            (TXEMPTY) */
    0x67, 0xf4,                                     /* beq.s poll */
    0x12, 0x2f, 0x00, 0x07,                         /* move.b (7,sp),d1      (char) */
    0x13, 0xc1, 0x62, 0x00, 0x00, 0x06,             /* move.b d1,0x62000006  (DATA) */
    0x70, 0x00,                                     /* moveq #0,d0 */
    0x4e, 0x75,                                     /* rts */
};

static const uint8_t sun3x_getchar_code[] = {
    /* pv_getchar/pv_nbgetchar: return char in d0, or -1 if none */
    0x13, 0xfc, 0x00, 0x03, 0x62, 0x00, 0x00, 0x04, /* move.b #3,0x62000004  (sel WR3) */
    0x13, 0xfc, 0x00, 0xc1, 0x62, 0x00, 0x00, 0x04, /* move.b #0xc1,..       (RXEN|Rx8) */
    0x12, 0x39, 0x62, 0x00, 0x00, 0x04,             /* move.b 0x62000004,d1  (RR0) */
    0x08, 0x01, 0x00, 0x00,                         /* btst #0,d1            (RXAV) */
    0x66, 0x04,                                     /* bne.s haschar */
    0x70, 0xff,                                     /* moveq #-1,d0 */
    0x4e, 0x75,                                     /* rts */
    0x10, 0x39, 0x62, 0x00, 0x00, 0x06,             /* haschar: move.b 0x62000006,d0 */
    0x02, 0x80, 0x00, 0x00, 0x00, 0xff,             /* andi.l #0xff,d0 */
    0x4e, 0x75,                                     /* rts */
};

static const uint8_t sun3x_halt_code[] = {
    0x60, 0xfe,                                     /* bra.s . */
};

static void sun3x_build_prom(Sun3xState *s)
{
    uint8_t *p = memory_region_get_ram_ptr(&s->prom);
    uint32_t i;

    memset(p, 0, SUN3X_PROM_SIZE);

    /* romvec function pointers */
    stl_be_p(p + OFF(ROMVEC_VA) + RV_GETCHAR,   GETCHAR_VA);
    stl_be_p(p + OFF(ROMVEC_VA) + RV_PUTCHAR,   PUTCHAR_VA);
    stl_be_p(p + OFF(ROMVEC_VA) + RV_NBGETCHAR, GETCHAR_VA);
    stl_be_p(p + OFF(ROMVEC_VA) + RV_NBPUTCHAR, PUTCHAR_VA);
    stl_be_p(p + OFF(ROMVEC_VA) + RV_MONID,     MONID_VA);
    stl_be_p(p + OFF(ROMVEC_VA) + RV_REBOOT,    HALT_VA);
    stl_be_p(p + OFF(ROMVEC_VA) + RV_ABORT,     HALT_VA);

    /* head.S: a1 = *(0xfefe00d4); a1 = *a1; copy 256 descriptors from a1 */
    stl_be_p(p + OFF(D4_VA),  PTP_VA);
    stl_be_p(p + OFF(PTP_VA), PGTABLE_VA);

    /* identity page table for the 0xfee00000..0xff000000 window (8K pages) */
    for (i = 0; i < 256; i++) {
        stl_be_p(p + OFF(PGTABLE_VA) + i * 4,
                 (SUN3X_PROM_PHYS + i * 0x2000) | SUN3X_PROM_PTE_FLAGS);
    }

    /* stub routines + monitor id string */
    memcpy(p + OFF(PUTCHAR_VA), sun3x_putchar_code, sizeof(sun3x_putchar_code));
    memcpy(p + OFF(GETCHAR_VA), sun3x_getchar_code, sizeof(sun3x_getchar_code));
    memcpy(p + OFF(HALT_VA),    sun3x_halt_code,    sizeof(sun3x_halt_code));
    strcpy((char *)(p + OFF(MONID_VA)), "QEMU sun3x");
}

/* --- boot --- */

static void sun3x_cpu_reset(void *opaque)
{
    Sun3xState *s = opaque;
    CPUM68KState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->pc = s->reset_pc;
    env->aregs[7] = s->reset_sp;
    env->sp[env->current_sp] = s->reset_sp;
}

static void sun3x_load_kernel(Sun3xState *s, MachineState *machine)
{
    CPUState *cs = CPU(s->cpu);
    uint64_t elf_entry, high;
    ssize_t kernel_size;
    hwaddr parameters_base;
    hwaddr top_of_ram = SUN3X_RAM_BASE + machine->ram_size;
    void *param_blob, *param_ptr;

    /* PAGE_OFFSET==0: kernel VA==PA, RAM at phys 0, so no bias needed. */
    kernel_size = load_elf(machine->kernel_filename, NULL, NULL, NULL,
                           &elf_entry, NULL, &high, NULL, ELFDATA2MSB,
                           EM_68K, 0, 0);
    if (kernel_size < 0) {
        error_report("could not load kernel '%s'", machine->kernel_filename);
        exit(1);
    }
    s->reset_pc = elf_entry;

    parameters_base = (high + 1) & ~1;
    param_blob = g_malloc(machine->kernel_cmdline ?
                          strlen(machine->kernel_cmdline) + 1024 : 1024);
    param_ptr = param_blob;

    BOOTINFO1(param_ptr, BI_MACHTYPE, MACH_SUN3X);
    BOOTINFO1(param_ptr, BI_MMUTYPE, MMU_68030);
    BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68030);
    BOOTINFO2(param_ptr, BI_MEMCHUNK, SUN3X_RAM_BASE, machine->ram_size);

    if (machine->kernel_cmdline) {
        BOOTINFOSTR(param_ptr, BI_COMMAND_LINE, machine->kernel_cmdline);
    }

    if (machine->initrd_filename) {
        int64_t initrd_size = get_image_size(machine->initrd_filename, NULL);
        hwaddr initrd_base;

        if (initrd_size < 0) {
            error_report("could not load initial ram disk '%s'",
                         machine->initrd_filename);
            exit(1);
        }
        initrd_base = (top_of_ram - initrd_size) & TARGET_PAGE_MASK;
        load_image_targphys(machine->initrd_filename, initrd_base,
                            top_of_ram - initrd_base, &error_fatal);
        BOOTINFO2(param_ptr, BI_RAMDISK, initrd_base, initrd_size);
        top_of_ram = initrd_base;
    }

    BOOTINFO0(param_ptr, BI_LAST);
    rom_add_blob_fixed_as("bootinfo", param_blob, param_ptr - param_blob,
                          parameters_base, cs->as);
    g_free(param_blob);

    s->reset_sp = (top_of_ram - 0x1000) & ~3;
}

static void sun3x_init(MachineState *machine)
{
    Sun3xState *s = g_new0(Sun3xState, 1);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *dev;
    SysBusDevice *sbd;

    if (machine->ram_size > SUN3X_RAM_MAX) {
        error_report("sun3x supports at most %d MiB of RAM",
                     (int)(SUN3X_RAM_MAX / MiB));
        exit(1);
    }

    s->sunos = machine->firmware != NULL;

    s->cpu = M68K_CPU(cpu_create(machine->cpu_type));

    /* autovector interrupt controller */
    s->irqc = qdev_new(TYPE_M68K_IRQC);
    object_property_set_link(OBJECT(s->irqc), "m68k-cpu", OBJECT(s->cpu),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->irqc), &error_fatal);

    /*
     * Linux shim mode: zero/absorb background over the whole space (the Linux
     * sun3x port does no bus-error probing; RAM comes from BI_MEMCHUNK and
     * devices are at fixed addrs).  SunOS mode deliberately omits this so that
     * unmapped accesses raise genuine 68030 bus errors, which the real PROM
     * self-test uses to size memory and probe for optional devices.
     */
    if (!s->sunos) {
        memory_region_init_io(&s->bg, NULL, &sun3x_absorb_ops, s,
                              "sun3x.bg", UINT64_MAX);
        memory_region_add_subregion_overlap(sysmem, 0, &s->bg, -1);
    }

    /* main RAM */
    memory_region_add_subregion(sysmem, SUN3X_RAM_BASE, machine->ram);

    /* DVMA IOMMU page table (scratch RAM) */
    memory_region_init_ram(&s->iommu, NULL, "sun3x.iommu",
                           SUN3X_IOMMU_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, SUN3X_IOMMU, &s->iommu);

    /* enable/copro/LED block, with the interrupt register overlaid */
    memory_region_init_io(&s->enareg, NULL, &sun3x_absorb_ops, s,
                          "sun3x.enareg", SUN3X_ENA_SIZE);
    memory_region_add_subregion(sysmem, SUN3X_ENAREG, &s->enareg);
    memory_region_init_io(&s->intreg_mr, NULL, &sun3x_intreg_ops, s,
                          "sun3x.intreg", 1);
    memory_region_add_subregion_overlap(sysmem, SUN3X_INTREG,
                                        &s->intreg_mr, 1);

    /* NVRAM: idprom + Mostek clock */
    memory_region_init_ram(&s->eeprom, NULL, "sun3x.eeprom",
                           SUN3X_EEPROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, SUN3X_EEPROM, &s->eeprom);
    sun3x_build_nvram(s);

    /*
     * Console Z8530.  In the Linux shim the console is ZS1 (0x62000000, driven
     * by the synthesised PROM stubs).  The genuine Sun 3/80 PROM instead uses
     * the higher-addressed zs (0x62002000) as ttya/ttyb, exactly like the 3/60
     * (hw/m68k/sun3.c) — so in SunOS mode wire serial_hd(0/1) there and leave
     * ZS1 (keyboard/mouse) idle.
     */
    dev = qdev_new(TYPE_ESCC);
    qdev_prop_set_uint32(dev, "disabled", 0);
    qdev_prop_set_uint32(dev, "frequency", SUN3X_ZS_FREQ);
    qdev_prop_set_uint32(dev, "it_shift", 1);
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));
    qdev_prop_set_uint32(dev, "chnAtype", escc_serial);
    qdev_prop_set_uint32(dev, "chnBtype", escc_serial);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, s->sunos ? SUN3X_ZS2 : SUN3X_ZS1);
    /*
     * zs interrupt is autovector level 6 on the Sun-3; the genuine monitor and
     * SunOS drive an interrupt-driven console through it.  Only wire it in
     * SunOS mode -- the Linux shim leaves the console zs un-vectored exactly as
     * before, keeping the stock '-M sun3x' path byte-for-byte unchanged.
     */
    if (s->sunos) {
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->irqc, 6 - 1));
        sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(s->irqc, 6 - 1));
    }

    /* the other Z8530 (keyboard/mouse in SunOS mode), left idle */
    dev = qdev_new(TYPE_ESCC);
    qdev_prop_set_uint32(dev, "disabled", 0);
    qdev_prop_set_uint32(dev, "frequency", SUN3X_ZS_FREQ);
    qdev_prop_set_uint32(dev, "it_shift", 1);
    qdev_prop_set_chr(dev, "chrA", NULL);
    qdev_prop_set_chr(dev, "chrB", NULL);
    qdev_prop_set_uint32(dev, "chnAtype", escc_serial);
    qdev_prop_set_uint32(dev, "chnBtype", escc_serial);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, s->sunos ? SUN3X_ZS1 : SUN3X_ZS2);

    if (!s->sunos) {
        /* synthesised boot PROM window (identity mapped) */
        memory_region_init_ram(&s->prom, NULL, "sun3x.prom",
                               SUN3X_PROM_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, SUN3X_PROM_PHYS, &s->prom);
        sun3x_build_prom(s);
    } else {
        /* genuine Sun 3/80 boot PROM (128 KiB ROM) at 0xfefe0000, with its
         * private scratch/stack/VBR RAM in the 0xfef00000 window below it. */
        char *filename;
        int rsize;

        memory_region_init_ram(&s->promram, NULL, "sun3x.promram",
                               SUN3X_PROMRAM_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, SUN3X_PROMRAM_BASE, &s->promram);
        s->promram_ptr = memory_region_get_ram_ptr(&s->promram);

        memory_region_init_rom(&s->prom, NULL, "sun3x.prom",
                               SUN3X_ROM_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, SUN3X_PROM_BASE, &s->prom);

        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        rsize = filename ? load_image_size(filename,
                    memory_region_get_ram_ptr(&s->prom), SUN3X_ROM_SIZE) : -1;
        if (rsize != SUN3X_ROM_SIZE) {
            error_report("sun3x: cannot load %d-byte boot PROM '%s'",
                         SUN3X_ROM_SIZE, machine->firmware);
            exit(1);
        }
        g_free(filename);

        /*
         * The PROM is also decoded low, at 0x63000000, but NOT as a read-only
         * alias: this is the monitor's relocation window.  The cold-start reads
         * its exception vectors / TC sentinel here (so it is shadow-initialised
         * with the ROM image), then the monitor copies itself into this RAM and
         * runs from it (patching e.g. the level-5 vector to its clock handler),
         * jumping well past the first 128 KiB.  Model it as writable RAM
         * preloaded with the ROM so the self-copy sticks and execution beyond
         * 128 KiB lands on real code instead of a bus-timeout NOP-slide.
         */
        memory_region_init_ram(&s->prom_alias, NULL, "sun3x.prom.shadow",
                               SUN3X_PROM_ALIAS_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, SUN3X_PROM_ALIAS, &s->prom_alias);
        memcpy(memory_region_get_ram_ptr(&s->prom_alias),
               memory_region_get_ram_ptr(&s->prom), SUN3X_ROM_SIZE);

        /*
         * Low-priority absorb over the whole OBIO (Type-1) device window
         * (0x58000000..0x7bffffff, see NetBSD sun3x obio.h).  The PROM
         * self-test pokes many on-board device registers (LANCE ethernet at
         * 0x65002000, EMULEX SCSI+DMA at 0x66000000/0x66001000, the FDC at
         * 0x6e000000, the printer port at 0x6f00003c, cache-tag RAMs, ...)
         * before it installs its own bus-error probe handler, so an unmapped
         * access here would be a fatal early bus error.  Absorbed (read 0 /
         * write ignored); the real modelled registers (iommu, enable/diag/
         * memerr/interrupt, zs, eeprom) overlay this at higher priority.  Main
         * memory (sized low) and the OBMEM framebuffer stay unmapped, so RAM
         * sizing still bus-errors correctly and the PROM finds no framebuffer
         * and selects the ttya serial console.
         */
        memory_region_init_io(&s->obio[0], NULL, &sun3x_absorb_ops, s,
                              "sun3x.obio", 0x24000000);
        memory_region_add_subregion_overlap(sysmem, 0x58000000,
                                             &s->obio[0], -1);

        /* real bus error register (overlaid on the enareg absorb block) */
        memory_region_init_io(&s->buserr_mr, NULL, &sun3x_buserr_ops, s,
                              "sun3x.buserr", 1);
        memory_region_add_subregion_overlap(sysmem, SUN3X_BUSERR,
                                            &s->buserr_mr, 1);

        /* memory error/control register (overlaid on the enareg absorb) */
        memory_region_init_io(&s->memreg_mr, NULL, &sun3x_memreg_ops, s,
                              "sun3x.memreg", 1);
        memory_region_add_subregion_overlap(sysmem, 0x61001000,
                                            &s->memreg_mr, 1);

        /*
         * Bus-error hole at 0x61001800 (overlaid on the enareg absorb).
         * The SunOS standalone boot's IDPROM reader (ufsboot 0x211a68) first
         * probes the *sun3* IDPROM location — virtual 0xfef1cc00, which the
         * monitor's page tables map to physical 0x61001800 — with a
         * bus-error-guarded word read; only if that faults does it fall back
         * to the *sun3x* IDPROM at virtual 0xfef047d8 (= physical 0x640007d8,
         * the MK48T02, which sun3x_build_nvram fills with a valid format=1
         * IDPROM).  On a real 3/80 the sun3 location is absent and bus-errors,
         * forcing the fallback.  Our broad enareg absorb was ACKing it (read
         * 0), so the boot took format 0 -> "bad idprom format (0 s.b. 1)" and
         * later failed the root open.  Fault it like the real hardware.  The
         * PROM self-test never touches 0x61001800 (verified), and the real
         * registers here sit at +0x400/+0x1000/+0x1400, so this window is free.
         */
        memory_region_init_io(&s->idprobe_hole, NULL, &sun3x_busfault_ops, s,
                              "sun3x.idprobe-hole", 0x800);
        memory_region_add_subregion_overlap(sysmem, 0x61001800,
                                            &s->idprobe_hole, 2);

        /*
         * Bus-error hole at 0x61000c00 (overlaid on the enareg absorb).  The
         * SunOS *kernel* (vmunix) has its own IDPROM reader (idprom_fetch,
         * text 0xf8061842): it probes the *sun3* discrete IDPROM at virtual
         * 0xfedf8c00 — which the kernel's page tables map to physical
         * 0x61000c00 — with a bus-error-guarded peek (0xf8059d92).  If that
         * peek succeeds it uses the sun3 IDPROM (mode 1, reads 0xfedf8c00);
         * only if it faults does it fall back to the *sun3x* IDPROM (mode 2,
         * virtual 0xfedfa7d8 = physical 0x640007d8, our valid MK48T02 format=1
         * machtype=0x42 Sun-3/80 IDPROM).  Our broad enareg absorb ACKed the
         * probe (read 0), so the kernel took the sun3 IDPROM, saw format byte
         * 0 -> "INVALID FORMAT TYPE/CODE IN ID PROM", defaulted the machine
         * type to SUN3X_470, and double-faulted.  Fault it like the real 3/80
         * (the sun3 IDPROM is absent) so the kernel uses the sun3x IDPROM.
         * 0x61000c00 sits in the free gap between DIAGREG (+0x800) and MEMREG
         * (+0x1000); the real control registers are elsewhere in the block.
         */
        memory_region_init_io(&s->idprobe_hole2, NULL, &sun3x_busfault_ops, s,
                              "sun3x.idprobe-hole2", 0x400);
        memory_region_add_subregion_overlap(sysmem, 0x61000c00,
                                            &s->idprobe_hole2, 2);

        /* level-7 clock interrupt status / ack registers (overlaid on the
         * monitor's relocation RAM) that the monitor's clock handler services */
        memory_region_init_io(&s->clkstat, NULL, &sun3x_clkstat_ops, s,
                              "sun3x.clkstat", 1);
        memory_region_add_subregion_overlap(sysmem, SUN3X_CLK_STAT,
                                            &s->clkstat, 1);
        memory_region_init_io(&s->clkack, NULL, &sun3x_clkack_ops, s,
                              "sun3x.clkack", 1);
        memory_region_add_subregion_overlap(sysmem, SUN3X_CLK_ACK,
                                            &s->clkack, 1);

        /*
         * "si" SCSI: an NCR5380 (OBIO_EMULEX_SCSI @ 0x66000000) plus a DMA/CSR
         * block in the same 8 KiB window.  The 5380's 8 registers are spaced
         * 4 bytes apart (reg-shift 2) and occupy 0x66000000..0x6600001f; the
         * si DMA/control registers overlay the rest at lower priority.
         * Interrupt is autovector level 2 (ipl 2).
         */
        object_initialize_child(OBJECT(machine), "si", &s->scsi, TYPE_NCR5380);
        qdev_prop_set_uint8(DEVICE(&s->scsi), "reg-shift", 2);
        qdev_prop_set_bit(DEVICE(&s->scsi), "sun-mode", true);
        sysbus_realize(SYS_BUS_DEVICE(&s->scsi), &error_fatal);
        memory_region_init_io(&s->sireg, NULL, &sun3x_sireg_ops, s,
                              "sun3x.si", 0x2000);
        memory_region_add_subregion_overlap(sysmem, 0x66000000, &s->sireg, 1);
        /* NCR5380 reg0 (data) is at 0x66000008; regs 0..7 are 4 bytes apart. */
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&s->scsi), 0, 0x66000008, 2);
        /* reclaim si scratch regs +0x20/+0x24 that fall inside the 5380 win */
        memory_region_init_io(&s->siscratch, NULL, &sun3x_siscratch_ops, s,
                              "sun3x.si.scratch", 0x08);
        memory_region_add_subregion_overlap(sysmem, 0x66000020,
                                            &s->siscratch, 3);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->scsi), 0,
                           qdev_get_gpio_in(s->irqc, 2 - 1));
        /* attach -drive if=scsi disks to the si bus */
        scsi_bus_legacy_handle_cmdline(&s->scsi.bus);

        /* lowest-priority bus-timeout background: any otherwise-unmapped
         * access latches TIMEOUT and faults (the PROM probes 0x40000000). */
        memory_region_init_io(&s->busfault, NULL, &sun3x_busfault_ops, s,
                              "sun3x.busfault", UINT64_MAX);
        memory_region_add_subregion_overlap(sysmem, 0, &s->busfault, -2);

        /*
         * Absent memory banks.  The PROM sizes RAM by probing the four 16 MiB
         * banks (0/16/32/48 MiB, up to 64 MiB) with a write/read-back compare,
         * NOT via bus errors (its vector-2 handler stays the fatal one): the
         * memory controller ACKs the whole low space and absent banks simply
         * read back a value that mismatches the (non-zero) bank base the PROM
         * wrote.  Back the space above installed RAM with a zero-absorb region
         * so those probes mismatch (bank absent) instead of faulting.
         */
        if (machine->ram_size < SUN3X_RAM_MAX) {
            memory_region_init_io(&s->memhole, NULL, &sun3x_memhole_ops, s,
                                  "sun3x.memhole",
                                  SUN3X_RAM_MAX - machine->ram_size);
            memory_region_add_subregion(sysmem, machine->ram_size,
                                        &s->memhole);
        }
    }

    /* periodic clock tick -> autovector level 5 */
    /*
     * Free-running 100 Hz level-5 clock tick.  Delivered only while the
     * interrupt register has both the master-enable (bit 0) and level-5 enable
     * (bit 5) set, so it is inert during the PROM self-test (which never sets
     * bit 5) yet available to the monitor and to SunOS/Linux, both of which
     * enable it and drive their periodic clock / timeout counters from it.
     */
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, sun3x_tick, s);
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / SUN3X_HZ);

    if (s->sunos) {
        /* enter the genuine PROM: reset SSP/PC come from its vector (offset
         * 0/4 of the ROM image): SSP=0xfef60c00, PC=0xfefe0228. */
        const uint8_t *rom = memory_region_get_ram_ptr(&s->prom);
        s->reset_sp = ldl_be_p(rom + 0);
        s->reset_pc = ldl_be_p(rom + 4);
    } else if (machine->kernel_filename) {
        sun3x_load_kernel(s, machine);
    } else {
        error_report("sun3x needs -kernel (Linux) or -bios (SunOS boot PROM)");
        exit(1);
    }

    qemu_register_reset(sun3x_cpu_reset, s);
}

static void sun3x_machine_init(MachineClass *mc)
{
    mc->desc = "Sun 3/80 (68030)";
    mc->init = sun3x_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->max_cpus = 1;
    mc->default_ram_size = SUN3X_RAM_DEFAULT;
    mc->default_ram_id = "sun3x.ram";
    mc->no_floppy = 1;
    mc->no_parallel = 1;
    mc->no_cdrom = 1;
}

DEFINE_MACHINE("sun3x", sun3x_machine_init)
