/*
 * Motorola / BVM Ltd. BVME4000/6000 VME single-board computer
 * (Linux/m68k `bvme6000` platform).
 *
 * Models a BVME4000-class board: 68040 + FPU, a Zilog Z8530 SCC serial
 * controller (the console), a DP8570A RTC whose timer 1 provides the
 * periodic tick, and an MC68230 PIT.  Boots the Linux/m68k `bvme6000`
 * kernel to an interactive serial console.
 *
 * See bvme6000-NOTES.md for the full hardware contract.  Bootinfo + kernel
 * load + reset-hook plumbing mirror hw/m68k/mvme16x.c; the console driver
 * (drivers/tty/serial/serial_mvme147_scc.c) is shared with mvme147.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "system/reset.h"
#include "system/system.h"
#include "exec/target_page.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "target/m68k/cpu.h"
#include "elf.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "bootinfo.h"

/* VME-specific bootinfo tags (not in QEMU standard-headers) */
#define BI_VME_TYPE             0x8000  /* __be32 VME sub-architecture */
#define VME_TYPE_BVME4000       0x4000
#define VME_TYPE_BVME6000       0x6000

/* Physical memory map (see bvme6000-NOTES.md) */
#define BVME_IO_BASE            0xff000000
#define BVME_IO_SIZE            0x01000000      /* whole on-board I/O window */
#define BVME_RTC_BASE           0xff900000
#define BVME_RTC_SIZE           0x00000080      /* RtcRegs_t: 32 regs * 4 */
#define BVME_PIT_BASE           0xffa00000
#define BVME_PIT_SIZE           0x00000100
#define BVME_SCC_BASE           0xffb00000
#define BVME_SCC_SIZE           0x00000010

#define BVME_DEFAULT_RAM        (32 * MiB)

/* Interrupt levels / vectors */
#define BVME_TICK_LEVEL         6
#define BVME_TICK_VECTOR        (24 + 6)        /* VEC_SPUR+6, autovector 6 */
#define BVME_SCC_LEVEL          4
#define BVME_SCC_VEC_BASE       0x40            /* VEC_USER; A-Rx -> 0x4C */

#define BVME_TICK_HZ            100             /* CONFIG_HZ */

/* Z8530 WR0 command field (bits 5..3) */
#define SCC_CMD_POINT_HIGH      1
/* Z8530 RR0 status bits */
#define SCC_RR0_RX_AVAIL        0x01
#define SCC_RR0_TX_EMPTY        0x04
#define SCC_RR0_DCD             0x08
#define SCC_RR0_CTS             0x20
/* WR1 receive-interrupt-mode bits, WR9 master-int-enable */
#define SCC_WR1_RXINT_MASK      0x18
#define SCC_WR9_MIE             0x08

/* ------------------------------------------------------------------ */

typedef struct BVMEState BVMEState;

typedef struct {
    BVMEState *board;
    CharFrontend chr;
    bool is_chan_a;
    uint8_t ptr;                /* register pointer */
    uint8_t wr[16];             /* write-register shadow */
    uint8_t rx;                 /* receive holding byte */
    bool rx_full;
} SCCChannel;

struct BVMEState {
    M68kCPU *cpu;

    MemoryRegion io_bg;
    MemoryRegion rtc;
    MemoryRegion pit;
    MemoryRegion scc;

    /* interrupt aggregation, (level<<8)|vector per source, 0 = idle */
    int tick_irq;
    int scc_irq;

    /* Z8530 SCC */
    SCCChannel scc_a;           /* channel A: console */
    SCCChannel scc_b;
    uint8_t scc_wr9;            /* master interrupt control (shared) */

    /* DP8570A RTC timer 1 */
    QEMUTimer *tick;
    bool tick_enabled;
    bool tick_pending;
    uint8_t rtc_msr;
    uint8_t rtc_reg[32];        /* shadow for programmed registers */

    /* reset state */
    uint32_t reset_pc;
    uint32_t reset_sp;
};

static void bvme_update_irq(BVMEState *s)
{
    int enc = (s->tick_irq >> 8) > (s->scc_irq >> 8) ? s->tick_irq : s->scc_irq;

    m68k_set_irq_level(s->cpu, enc >> 8, enc & 0xff);
}

/* ------------------------------------------------------------------ */
/* Zilog Z8530 SCC (console on channel A)                              */

static void scc_update_irq(BVMEState *s)
{
    int enc = 0;

    /* Only the receive interrupt is modelled (TX is polled, DCD static). */
    if (s->scc_wr9 & SCC_WR9_MIE) {
        if (s->scc_a.rx_full && (s->scc_a.wr[1] & SCC_WR1_RXINT_MASK)) {
            enc = (BVME_SCC_LEVEL << 8) | (BVME_SCC_VEC_BASE | 0x0c); /* A Rx */
        } else if (s->scc_b.rx_full && (s->scc_b.wr[1] & SCC_WR1_RXINT_MASK)) {
            enc = (BVME_SCC_LEVEL << 8) | (BVME_SCC_VEC_BASE | 0x04); /* B Rx */
        }
    }
    s->scc_irq = enc;
    bvme_update_irq(s);
}

static void scc_tx(SCCChannel *c, uint8_t val)
{
    /* Transmitter is instantaneous: buffer always reports empty. */
    qemu_chr_fe_write_all(&c->chr, &val, 1);
}

static uint8_t scc_ctrl_read(SCCChannel *c)
{
    BVMEState *s = c->board;
    uint8_t reg = c->ptr;
    uint8_t ret;

    switch (reg) {
    case 0:                     /* RR0 status */
        ret = SCC_RR0_TX_EMPTY | SCC_RR0_CTS | SCC_RR0_DCD;
        if (c->rx_full) {
            ret |= SCC_RR0_RX_AVAIL;
        }
        break;
    case 3:                     /* RR3 interrupt-pending (channel A reads it) */
        ret = 0;
        if (s->scc_a.rx_full && (s->scc_a.wr[1] & SCC_WR1_RXINT_MASK)) {
            ret |= 0x20;        /* IPR_A_RX */
        }
        if (s->scc_b.rx_full && (s->scc_b.wr[1] & SCC_WR1_RXINT_MASK)) {
            ret |= 0x04;        /* IPR_B_RX */
        }
        break;
    case 8:                     /* RR8 receive data (via pointer) */
        ret = c->rx;
        c->rx_full = false;
        scc_update_irq(s);
        qemu_chr_fe_accept_input(&c->chr);
        break;
    default:
        ret = c->wr[reg & 0x0f];
        break;
    }
    c->ptr = 0;
    return ret;
}

static void scc_ctrl_write(SCCChannel *c, uint8_t val)
{
    BVMEState *s = c->board;
    uint8_t reg = c->ptr;

    if (reg == 0) {
        uint8_t cmd = (val >> 3) & 7;
        uint8_t ptrlow = val & 7;

        if (cmd == SCC_CMD_POINT_HIGH) {
            c->ptr = 8 | ptrlow;
        } else {
            c->ptr = ptrlow;
            /* cmd 2..7 are reset/ack operations; just recompute the IRQ. */
            scc_update_irq(s);
        }
        return;
    }

    switch (reg) {
    case 8:                     /* WR8 transmit data (via pointer) */
        scc_tx(c, val);
        break;
    case 9:                     /* WR9 master interrupt control (shared) */
        s->scc_wr9 = val;
        c->wr[9] = val;
        scc_update_irq(s);
        break;
    default:
        c->wr[reg & 0x0f] = val;
        scc_update_irq(s);
        break;
    }
    c->ptr = 0;
}

static uint64_t bvme_scc_read(void *opaque, hwaddr addr, unsigned size)
{
    BVMEState *s = opaque;
    SCCChannel *c = (addr >> 3) & 1 ? &s->scc_a : &s->scc_b;

    if ((addr >> 2) & 1) {      /* data port == RR8 */
        uint8_t ret = c->rx;
        c->rx_full = false;
        scc_update_irq(s);
        qemu_chr_fe_accept_input(&c->chr);
        return ret;
    }
    return scc_ctrl_read(c);
}

static void bvme_scc_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    BVMEState *s = opaque;
    SCCChannel *c = (addr >> 3) & 1 ? &s->scc_a : &s->scc_b;

    if ((addr >> 2) & 1) {      /* data port == WR8 transmit */
        scc_tx(c, val);
        return;
    }
    scc_ctrl_write(c, val);
}

static const MemoryRegionOps bvme_scc_ops = {
    .read = bvme_scc_read,
    .write = bvme_scc_write,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

static int scc_can_receive(void *opaque)
{
    SCCChannel *c = opaque;

    return c->rx_full ? 0 : 1;
}

static void scc_receive(void *opaque, const uint8_t *buf, int size)
{
    SCCChannel *c = opaque;

    if (size < 1) {
        return;
    }
    c->rx = buf[0];
    c->rx_full = true;
    scc_update_irq(c->board);
}

/* ------------------------------------------------------------------ */
/* DP8570A RTC + timer 1 (periodic tick)                              */

static void bvme_tick_arm(BVMEState *s)
{
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
              + NANOSECONDS_PER_SECOND / BVME_TICK_HZ);
}

static void bvme_tick_expired(void *opaque)
{
    BVMEState *s = opaque;

    s->tick_pending = true;
    s->rtc_msr |= 0x20;                 /* T1 interrupt flag */
    s->tick_irq = (BVME_TICK_LEVEL << 8) | BVME_TICK_VECTOR;
    bvme_update_irq(s);
    if (s->tick_enabled) {
        bvme_tick_arm(s);
    }
}

static uint64_t bvme_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    BVMEState *s = opaque;
    unsigned reg = (addr >> 2) & 0x1f;

    switch (reg) {
    case 0:                             /* msr */
        return s->rtc_msr;
    case 6:  return 0x00;               /* bcd_sec */
    case 7:  return 0x00;               /* bcd_min */
    case 8:  return 0x12;              /* bcd_hr  */
    case 9:  return 0x01;              /* bcd_dom */
    case 10: return 0x01;              /* bcd_mth */
    case 11: return 0x25;              /* bcd_year (2025) */
    case 14: return 0x04;              /* bcd_dow (Wed) */
    case 17: return 0x10;             /* t1lsb: count 0x2710 = 10000 */
    case 18: return 0x27;             /* t1msb */
    default:
        return s->rtc_reg[reg];
    }
}

static void bvme_rtc_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    BVMEState *s = opaque;
    unsigned reg = (addr >> 2) & 0x1f;
    uint8_t b = val;

    switch (reg) {
    case 0:                             /* msr */
        if (b & 0x20) {                 /* ack / clear T1 interrupt */
            s->rtc_msr &= ~0x20;
            if (s->tick_pending) {
                s->tick_pending = false;
                s->tick_irq = 0;
                bvme_update_irq(s);
            }
        }
        /* keep the mode bits the driver wrote (bank select is ignored) */
        s->rtc_msr = (s->rtc_msr & 0x20) | (b & ~0x20);
        break;
    case 3:                             /* pfr_icr0: bit7 = timer1 int enable */
        s->rtc_reg[reg] = b;
        if (b & 0x80) {
            if (!s->tick_enabled) {
                s->tick_enabled = true;
                bvme_tick_arm(s);
            }
        } else {
            s->tick_enabled = false;
            timer_del(s->tick);
        }
        break;
    default:
        s->rtc_reg[reg] = b;
        break;
    }
}

static const MemoryRegionOps bvme_rtc_ops = {
    .read = bvme_rtc_read,
    .write = bvme_rtc_write,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* MC68230 PIT stub: only port C data (pcdr) is meaningful             */

static uint64_t bvme_pit_read(void *opaque, hwaddr addr, unsigned size)
{
    unsigned reg = (addr >> 2) & 0x1f;

    if (reg == 11) {                    /* pcdr: bit2 = timer-1 output high */
        return 0x04;
    }
    return 0;
}

static void bvme_pit_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
}

static const MemoryRegionOps bvme_pit_ops = {
    .read = bvme_pit_read,
    .write = bvme_pit_write,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* On-board I/O background: absorb probes to unmodelled devices        */

static uint64_t bvme_io_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void bvme_io_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
}

static const MemoryRegionOps bvme_io_ops = {
    .read = bvme_io_read,
    .write = bvme_io_write,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */

static void bvme_cpu_reset(void *opaque)
{
    BVMEState *s = opaque;
    CPUM68KState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->pc = s->reset_pc;
    env->aregs[7] = s->reset_sp;
    env->sp[env->current_sp] = s->reset_sp;
}

static void bvme_load_kernel(BVMEState *s, MachineState *machine)
{
    CPUState *cs = CPU(s->cpu);
    uint64_t elf_entry, high;
    ssize_t kernel_size;
    hwaddr parameters_base;
    hwaddr top_of_ram = machine->ram_size;
    void *param_blob, *param_ptr;

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

    BOOTINFO1(param_ptr, BI_MACHTYPE, MACH_BVME6000);
    BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68040);
    BOOTINFO1(param_ptr, BI_MMUTYPE, MMU_68040);
    BOOTINFO1(param_ptr, BI_FPUTYPE, FPU_68040);
    BOOTINFO1(param_ptr, BI_VME_TYPE, VME_TYPE_BVME4000);
    BOOTINFO2(param_ptr, BI_MEMCHUNK, 0, machine->ram_size);

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

static void bvme_init(MachineState *machine)
{
    BVMEState *s = g_new0(BVMEState, 1);
    MemoryRegion *sysmem = get_system_memory();

    s->cpu = M68K_CPU(cpu_create(machine->cpu_type));

    /* RAM at physical 0 */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    /* On-board I/O background absorber (lowest priority in the I/O window) */
    memory_region_init_io(&s->io_bg, NULL, &bvme_io_ops, s,
                          "bvme.io-bg", BVME_IO_SIZE);
    memory_region_add_subregion_overlap(sysmem, BVME_IO_BASE, &s->io_bg, 0);

    /* DP8570A RTC + timer 1 */
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, bvme_tick_expired, s);
    memory_region_init_io(&s->rtc, NULL, &bvme_rtc_ops, s,
                          "bvme.rtc", BVME_RTC_SIZE);
    memory_region_add_subregion_overlap(sysmem, BVME_RTC_BASE, &s->rtc, 1);

    /* MC68230 PIT */
    memory_region_init_io(&s->pit, NULL, &bvme_pit_ops, s,
                          "bvme.pit", BVME_PIT_SIZE);
    memory_region_add_subregion_overlap(sysmem, BVME_PIT_BASE, &s->pit, 1);

    /* Zilog Z8530 SCC: channel A is the console */
    s->scc_a.board = s;
    s->scc_a.is_chan_a = true;
    s->scc_b.board = s;
    qemu_chr_fe_init(&s->scc_a.chr, serial_hd(0), &error_abort);
    qemu_chr_fe_set_handlers(&s->scc_a.chr, scc_can_receive, scc_receive,
                             NULL, NULL, &s->scc_a, NULL, true);
    if (serial_hd(1)) {
        qemu_chr_fe_init(&s->scc_b.chr, serial_hd(1), &error_abort);
        qemu_chr_fe_set_handlers(&s->scc_b.chr, scc_can_receive, scc_receive,
                                 NULL, NULL, &s->scc_b, NULL, true);
    }
    memory_region_init_io(&s->scc, NULL, &bvme_scc_ops, s,
                          "bvme.scc", BVME_SCC_SIZE);
    memory_region_add_subregion_overlap(sysmem, BVME_SCC_BASE, &s->scc, 1);

    if (machine->kernel_filename) {
        bvme_load_kernel(s, machine);
    } else {
        error_report("bvme4000 currently only supports -kernel boot");
        exit(1);
    }

    qemu_register_reset(bvme_cpu_reset, s);
}

static void bvme4000_machine_init(MachineClass *mc)
{
    mc->desc = "BVM Ltd. BVME4000 (68040)";
    mc->init = bvme_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->max_cpus = 1;
    mc->default_ram_size = BVME_DEFAULT_RAM;
    mc->default_ram_id = "bvme.ram";
    mc->no_floppy = 1;
    mc->no_parallel = 1;
    mc->no_cdrom = 1;
}

DEFINE_MACHINE("bvme4000", bvme4000_machine_init)
