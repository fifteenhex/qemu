/*
 * HP 9000/300 series (hp300) — Motorola 68030 workstation
 *
 * Boots the Linux/m68k `hp300` kernel (CONFIG_HP300) to a serial shell.
 * Model emulated: HP 9000/340 (16 MHz 68030), the model the Linux hp300
 * README says was actually tested.
 *
 * Hardware contract reverse-engineered from the Linux hp300 port
 * (arch/m68k/hp300/{config.c,time.c}, arch/m68k/kernel/head.S,
 * drivers/tty/serial/8250/8250_hp300.c, include/linux/dio.h).  See
 * hp300-NOTES.md in this directory for the full memory map.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
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
#include "hw/char/serial-mm.h"
#include "hw/intc/m68k_irqc.h"
#include "chardev/char.h"
#include "qemu/timer.h"
#include "target/m68k/cpu.h"
#include "elf.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "bootinfo.h"

/* hp300-specific bootinfo tags (not in QEMU standard-headers) */
#define BI_HP300_MODEL          0x8000  /* __be32 model */
#define BI_HP300_UART_SCODE     0x8001  /* __be32 UART select code */
#define BI_HP300_UART_ADDR      0x8002  /* __be32 phys addr of UART card */

#define HP_340                  2       /* 16 MHz 68030 */

/* Physical memory map (see hp300-NOTES.md) */
#define HP300_LEDROM_BASE       0x00000000
#define HP300_LEDROM_SIZE       0x00020000
#define HP300_RTC_BASE          0x00420000
#define HP300_RTC_SIZE          0x00000100
#define HP300_TIMER_BASE        0x005f8000
#define HP300_TIMER_SIZE        0x00000100
/* 030/040 FPU + cache "energise" control register block (head.S writes it
 * right after mmu_engage: movel #0x60,0xf05f400c). Absorb it. */
#define HP300_CACHECTL_BASE     0x005f4000
#define HP300_CACHECTL_SIZE     0x00001000

/* Built-in DCA (98644) serial, DIO select code 9, ipl 5 */
#define HP300_DCA_SCODE         9
#define HP300_DCA_IPL           5
#define HP300_DCA_BASE          0x00690000              /* DIO_BASE + 9*0x10000 */
#define HP300_DCA_CTRL_SIZE     0x11                    /* card regs before UART */
#define HP300_DCA_UART_OFFSET   0x11                    /* UART_OFFSET (17) */
#define HP300_DCA_BAUDBASE      153600                  /* HPDCA_BAUD_BASE */

/* DIO card-control register offsets / values */
#define DCA_REG_ID              0x01    /* DIO_IDOFF: read = ID, write = reset */
#define DCA_REG_IPL             0x03    /* DIO_IPLOFF read / DCA_IC write */
#define DIO_ID_DCA0             0x02
/* DIO_IPL = ((in_8(base+3) >> 4) & 3) + 3; ipl 5 => (2<<4) = 0x20 */
#define DCA_IPL_ENC             ((HP300_DCA_IPL - 3) << 4)

/*
 * Main RAM must sit above the entire DIO/DIO-II probe range so the kernel's
 * fault-protected select-code scan (scode 0..255, DIO-II reaching phys
 * 0x1fc00000) never reads RAM as a device.  Place it at 0x20000000 (512 MB),
 * exactly one DIO-II frame past the last select code.
 */
#define HP300_RAM_BASE          0x20000000
#define HP300_RAM_DEFAULT       (16 * MiB)
#define HP300_RAM_MAX           (256 * MiB)

/* ------------------------------------------------------------------ */
/* MC6840-style clock timer (98630).  Registers at odd byte offsets    */
/* (reg = offset >> 1).  T1 is the 250 kHz system tick that drives      */
/* IRQ_AUTO_6.  Model adapted from hw/m68k/apollo.c's PTM.              */

#define PTM_CR_INTERNAL_RESET   0x01    /* CR1 only */
#define PTM_CR_CR1_SELECT       0x01    /* CR2 only */
#define PTM_CR_T3_PRESCALE      0x01    /* CR3 only */
#define PTM_CR_IRQ_ENABLE       0x40

typedef struct HP300State HP300State;

typedef struct HP300Timer {
    HP300State *s;
    int idx;
    uint16_t latch;
    uint8_t cr;
    bool flag;                  /* interrupt flag in status register */
    bool status_read;           /* status read with flag set (clear protocol) */
    int64_t start_ns;
    uint8_t msb_buf;
    uint8_t lsb_buf;
    QEMUTimer *timer;
} HP300Timer;

struct HP300State {
    M68kCPU *cpu;
    DeviceState *irqc;

    MemoryRegion buserr;
    MemoryRegion ledrom;
    MemoryRegion rtc_mem;
    MemoryRegion timer_mem;
    MemoryRegion cachectl;
    MemoryRegion dca_ctrl;

    HP300Timer ptm[3];

    /* HP RTC command-protocol state */
    uint8_t rtc_mode;           /* last command: 0=idle, 0xe0=setreg */
    uint8_t rtc_reg;            /* selected register */

    bool linux_boot;
    uint32_t reset_pc;
    uint32_t reset_sp;
};

static qemu_irq hp300_level_irq(HP300State *s, int level)
{
    /* level 1..7 -> IRQC gpio index M68K_IRQC_LEVEL_1..7 (0-based) */
    return qdev_get_gpio_in(s->irqc, level - 1);
}

static uint64_t hp300_timer_rate(HP300State *s, int idx)
{
    static const uint64_t rates[3] = { 250000, 125000, 62500 };
    uint64_t rate = rates[idx];

    if (idx == 2 && (s->ptm[2].cr & PTM_CR_T3_PRESCALE)) {
        rate /= 8;
    }
    return rate;
}

static void hp300_timer_update_irq(HP300State *s)
{
    bool level = false;
    int i;

    for (i = 0; i < 3; i++) {
        if (s->ptm[i].flag && (s->ptm[i].cr & PTM_CR_IRQ_ENABLE)) {
            level = true;
        }
    }
    /* The 98630 clock IRQ is wired to CPU IPL 6 (IRQ_AUTO_6). */
    qemu_set_irq(hp300_level_irq(s, 6), level);
}

static bool hp300_timer_running(HP300State *s)
{
    return !(s->ptm[0].cr & PTM_CR_INTERNAL_RESET);
}

static uint16_t hp300_timer_count(HP300State *s, int idx)
{
    HP300Timer *t = &s->ptm[idx];
    uint32_t period = (uint32_t)t->latch + 1;
    uint64_t ticks;

    if (!hp300_timer_running(s)) {
        return t->latch;
    }
    ticks = (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - t->start_ns)
        * hp300_timer_rate(s, idx) / NANOSECONDS_PER_SECOND;
    return t->latch - (ticks % period);
}

static void hp300_timer_arm(HP300State *s, int idx)
{
    HP300Timer *t = &s->ptm[idx];
    uint64_t period_ns = ((uint64_t)t->latch + 1) * NANOSECONDS_PER_SECOND
        / hp300_timer_rate(s, idx);

    period_ns = MAX(period_ns, 1000);
    timer_mod(t->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period_ns);
}

static void hp300_timer_reload(HP300State *s, int idx)
{
    HP300Timer *t = &s->ptm[idx];

    t->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (hp300_timer_running(s)) {
        hp300_timer_arm(s, idx);
    } else {
        timer_del(t->timer);
    }
}

static void hp300_timer_expired(void *opaque)
{
    HP300Timer *t = opaque;
    HP300State *s = t->s;

    t->flag = true;
    hp300_timer_update_irq(s);
    if (hp300_timer_running(s)) {
        hp300_timer_arm(s, t->idx);
    }
}

static void hp300_timer_set_reset(HP300State *s, bool reset)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (reset) {
            s->ptm[i].flag = false;
            s->ptm[i].status_read = false;
            timer_del(s->ptm[i].timer);
        } else {
            hp300_timer_reload(s, i);
        }
    }
    hp300_timer_update_irq(s);
}

static uint64_t hp300_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    HP300State *s = opaque;
    unsigned reg = (addr >> 1) & 7;
    HP300Timer *t;
    uint64_t data = 0;
    uint16_t count;
    int i;

    switch (reg) {
    case 0:                     /* no read register */
        break;
    case 1:                     /* status register */
        for (i = 0; i < 3; i++) {
            if (s->ptm[i].flag) {
                data |= 1 << i;
                if (s->ptm[i].cr & PTM_CR_IRQ_ENABLE) {
                    data |= 0x80;
                }
                s->ptm[i].status_read = true;
            }
        }
        break;
    case 2:                     /* T1 MSB */
    case 4:                     /* T2 MSB */
    case 6:                     /* T3 MSB */
        t = &s->ptm[(reg - 2) >> 1];
        count = hp300_timer_count(s, t->idx);
        t->lsb_buf = count & 0xff;
        data = count >> 8;
        if (t->status_read) {   /* status-then-counter clears the flag */
            t->flag = false;
            t->status_read = false;
            hp300_timer_update_irq(s);
        }
        break;
    case 3:                     /* T1 LSB */
    case 5:                     /* T2 LSB */
    case 7:                     /* T3 LSB */
        t = &s->ptm[(reg - 3) >> 1];
        data = t->lsb_buf;
        break;
    }
    return data;
}

static void hp300_timer_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    HP300State *s = opaque;
    unsigned reg = (addr >> 1) & 7;
    HP300Timer *t;
    uint8_t data = val;
    bool was_reset;

    switch (reg) {
    case 0:                     /* CR1 or CR3, selected by CR2 bit 0 */
        if (s->ptm[1].cr & PTM_CR_CR1_SELECT) {
            was_reset = !hp300_timer_running(s);
            s->ptm[0].cr = data;
            if (was_reset != !hp300_timer_running(s)) {
                hp300_timer_set_reset(s, !hp300_timer_running(s));
            }
        } else {
            s->ptm[2].cr = data;
        }
        hp300_timer_update_irq(s);
        break;
    case 1:                     /* CR2 */
        s->ptm[1].cr = data;
        hp300_timer_update_irq(s);
        break;
    case 2:                     /* T1 MSB buffer */
    case 4:
    case 6:
        s->ptm[(reg - 2) >> 1].msb_buf = data;
        break;
    case 3:                     /* T1 LSB: transfers latch, reinitialises */
    case 5:
    case 7:
        t = &s->ptm[(reg - 3) >> 1];
        t->latch = (t->msb_buf << 8) | data;
        t->flag = false;
        t->status_read = false;
        hp300_timer_reload(s, t->idx);
        hp300_timer_update_irq(s);
        break;
    }
}

static const MemoryRegionOps hp300_timer_ops = {
    .read = hp300_timer_read,
    .write = hp300_timer_write,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* HP custom RTC (0x420000).  config.c busy-waits on the status bits,  */
/* so we always report ready and answer the command protocol with BCD  */
/* digits taken from host time.                                        */

#define RTC_CMD_SETREG          0xe0
#define RTC_CMD_WRITEREG        0xc2
#define RTC_CMD_READREG         0xc3
#define RTC_STATUS_READY        0x41    /* RTC_STAT_RDY(0x40) | DATA_RDY(0x01) */

/* register index -> BCD digit of current time */
static uint8_t hp300_rtc_digit(uint8_t reg)
{
    struct tm tm;

    qemu_get_timedate(&tm, 0);

    switch (reg) {
    case 0:  return tm.tm_sec % 10;                 /* SEC2 */
    case 1:  return tm.tm_sec / 10;                 /* SEC1 */
    case 2:  return tm.tm_min % 10;                 /* MIN2 */
    case 3:  return tm.tm_min / 10;                 /* MIN1 */
    case 4:  return tm.tm_hour % 10;                /* HOUR2 */
    case 5:  return tm.tm_hour / 10;                /* HOUR1 (24h, top bits ignored) */
    case 6:  return tm.tm_wday;                     /* WDAY */
    case 7:  return tm.tm_mday % 10;                /* DAY2 */
    case 8:  return tm.tm_mday / 10;                /* DAY1 */
    case 9:  return (tm.tm_mon + 1) % 10;           /* MON2 */
    case 10: return (tm.tm_mon + 1) / 10;           /* MON1 */
    case 11: return (tm.tm_year % 100) % 10;        /* YEAR2 */
    case 12: return (tm.tm_year % 100) / 10;        /* YEAR1 */
    default: return 0;
    }
}

static uint64_t hp300_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    HP300State *s = opaque;

    switch (addr & 3) {
    case 3:                     /* status/command register */
        return RTC_STATUS_READY;
    case 1:                     /* data register */
        return hp300_rtc_digit(s->rtc_reg);
    default:
        return 0;
    }
}

static void hp300_rtc_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    HP300State *s = opaque;
    uint8_t v = val;

    switch (addr & 3) {
    case 3:                     /* command */
        s->rtc_mode = v;
        break;
    case 1:                     /* data */
        if (s->rtc_mode == RTC_CMD_SETREG) {
            s->rtc_reg = v & 0x0f;
            s->rtc_mode = 0;
        }
        /* WRITEREG payload (val<<4)|reg is accepted and ignored */
        break;
    }
}

static const MemoryRegionOps hp300_rtc_ops = {
    .read = hp300_rtc_read,
    .write = hp300_rtc_write,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* DCA DIO card-control registers (ID / IPL / interrupt-control).      */
/* The 8250 UART itself sits just above this region and is served by    */
/* the standard serial-mm model.                                       */

static uint64_t hp300_dca_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case DCA_REG_ID:            /* primary DIO ID */
        return DIO_ID_DCA0;
    case DCA_REG_IPL:           /* interrupt priority level encoding */
        return DCA_IPL_ENC;
    default:
        return 0;
    }
}

static void hp300_dca_ctrl_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    /* Reset (offset 1) and interrupt-control (offset 3) writes are the
     * board-level enable; the 8250 IER already gates delivery, so these
     * are accepted and ignored. */
}

static const MemoryRegionOps hp300_dca_ctrl_ops = {
    .read = hp300_dca_ctrl_read,
    .write = hp300_dca_ctrl_write,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* boot ROM / LED latch stub: head.S writes the debug LEDs at phys      */
/* 0x1ffff before and after the MMU comes up; absorb those writes.      */

static uint64_t hp300_ledrom_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void hp300_ledrom_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
}

static const MemoryRegionOps hp300_ledrom_ops = {
    .read = hp300_ledrom_read,
    .write = hp300_ledrom_write,
    .impl = { .min_access_size = 1, .max_access_size = 2 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* Bus-error background: everything not otherwise mapped raises a real  */
/* 68k bus error, so the kernel's fault-protected DIO probe sees "no    */
/* card" at unpopulated select codes.                                  */

static MemTxResult hp300_buserr_read(void *opaque, hwaddr addr, uint64_t *val,
                                     unsigned size, MemTxAttrs attrs)
{
    *val = ~0ULL;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult hp300_buserr_write(void *opaque, hwaddr addr, uint64_t val,
                                      unsigned size, MemTxAttrs attrs)
{
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps hp300_buserr_ops = {
    .read_with_attrs = hp300_buserr_read,
    .write_with_attrs = hp300_buserr_write,
    .impl = { .min_access_size = 1, .max_access_size = 8 },
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* Machine                                                            */

/* vmlinux is linked ~0x1000; bias every segment (and the entry) into   */
/* RAM at HP300_RAM_BASE.  head.S is PC-relative and recomputes its own  */
/* physical base, so a fixed offset works.                              */
static uint64_t hp300_kernel_translate(void *opaque, uint64_t addr)
{
    return addr + HP300_RAM_BASE;
}

static void hp300_cpu_reset(void *opaque)
{
    HP300State *s = opaque;
    CPUM68KState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->pc = s->reset_pc;
    env->aregs[7] = s->reset_sp;
    env->sp[env->current_sp] = s->reset_sp;
}

static void hp300_load_kernel(HP300State *s, MachineState *machine)
{
    CPUState *cs = CPU(s->cpu);
    uint64_t elf_entry, high;
    ssize_t kernel_size;
    hwaddr parameters_base;
    hwaddr top_of_ram = HP300_RAM_BASE + machine->ram_size;
    void *param_blob, *param_ptr;

    kernel_size = load_elf(machine->kernel_filename, NULL,
                           hp300_kernel_translate, NULL,
                           &elf_entry, NULL, &high, NULL, ELFDATA2MSB,
                           EM_68K, 0, 0);
    if (kernel_size < 0) {
        error_report("could not load kernel '%s'", machine->kernel_filename);
        exit(1);
    }
    s->reset_pc = elf_entry + HP300_RAM_BASE;

    parameters_base = (high + 1) & ~1;
    param_blob = g_malloc(machine->kernel_cmdline ?
                          strlen(machine->kernel_cmdline) + 1024 : 1024);
    param_ptr = param_blob;

    BOOTINFO1(param_ptr, BI_MACHTYPE, MACH_HP300);
    /* Exactly one BI_CPUTYPE record; emit the MMU/CPU pair the core is. */
    if (m68k_feature(&s->cpu->env, M68K_FEATURE_M68040)) {
        BOOTINFO1(param_ptr, BI_FPUTYPE, FPU_68040);
        BOOTINFO1(param_ptr, BI_MMUTYPE, MMU_68040);
        BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68040);
    } else {
        BOOTINFO1(param_ptr, BI_MMUTYPE, MMU_68030);
        BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68030);
    }

    BOOTINFO1(param_ptr, BI_HP300_MODEL, HP_340);
    BOOTINFO1(param_ptr, BI_HP300_UART_SCODE, HP300_DCA_SCODE);
    BOOTINFO1(param_ptr, BI_HP300_UART_ADDR, HP300_DCA_BASE);
    BOOTINFO2(param_ptr, BI_MEMCHUNK, HP300_RAM_BASE, machine->ram_size);

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

static void hp300_init(MachineState *machine)
{
    HP300State *s = g_new0(HP300State, 1);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *irqc;
    int i;

    if (machine->ram_size > HP300_RAM_MAX) {
        error_report("hp9000-340 supports at most %d MiB of RAM",
                     (int)(HP300_RAM_MAX / MiB));
        exit(1);
    }

    s->cpu = M68K_CPU(cpu_create(machine->cpu_type));
    s->linux_boot = machine->kernel_filename != NULL;

    /* Autovector interrupt controller */
    irqc = qdev_new(TYPE_M68K_IRQC);
    object_property_set_link(OBJECT(irqc), "m68k-cpu", OBJECT(s->cpu),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(irqc), &error_fatal);
    s->irqc = irqc;

    /* Bus-error background over the whole address space (lowest priority) */
    memory_region_init_io(&s->buserr, NULL, &hp300_buserr_ops, s,
                          "hp300.buserr", UINT64_MAX);
    memory_region_add_subregion_overlap(sysmem, 0, &s->buserr, -2);

    /* Main RAM, above the I/O window (overlaps the background) */
    memory_region_add_subregion_overlap(sysmem, HP300_RAM_BASE,
                                        machine->ram, 1);

    /* boot ROM / LED latch stub */
    memory_region_init_io(&s->ledrom, NULL, &hp300_ledrom_ops, s,
                          "hp300.ledrom", HP300_LEDROM_SIZE);
    memory_region_add_subregion(sysmem, HP300_LEDROM_BASE, &s->ledrom);

    /* RTC */
    memory_region_init_io(&s->rtc_mem, NULL, &hp300_rtc_ops, s,
                          "hp300.rtc", HP300_RTC_SIZE);
    memory_region_add_subregion(sysmem, HP300_RTC_BASE, &s->rtc_mem);

    /* 98630 clock timer (IRQ_AUTO_6) */
    for (i = 0; i < 3; i++) {
        s->ptm[i].s = s;
        s->ptm[i].idx = i;
        s->ptm[i].cr = (i == 0) ? PTM_CR_INTERNAL_RESET : 0;
        s->ptm[i].latch = 0xffff;
        s->ptm[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       hp300_timer_expired, &s->ptm[i]);
    }
    memory_region_init_io(&s->timer_mem, NULL, &hp300_timer_ops, s,
                          "hp300.timer", HP300_TIMER_SIZE);
    memory_region_add_subregion(sysmem, HP300_TIMER_BASE, &s->timer_mem);

    /* FPU/cache energise control register (writes absorbed) */
    memory_region_init_io(&s->cachectl, NULL, &hp300_ledrom_ops, s,
                          "hp300.cachectl", HP300_CACHECTL_SIZE);
    memory_region_add_subregion(sysmem, HP300_CACHECTL_BASE, &s->cachectl);

    /* DCA (98644) serial console: card-control regs then the 8250 UART */
    memory_region_init_io(&s->dca_ctrl, NULL, &hp300_dca_ctrl_ops, s,
                          "hp300.dca-ctrl", HP300_DCA_CTRL_SIZE);
    memory_region_add_subregion(sysmem, HP300_DCA_BASE, &s->dca_ctrl);

    serial_mm_init(sysmem, HP300_DCA_BASE + HP300_DCA_UART_OFFSET, 1,
                   hp300_level_irq(s, HP300_DCA_IPL), HP300_DCA_BAUDBASE,
                   serial_hd(0), DEVICE_BIG_ENDIAN);

    if (s->linux_boot) {
        hp300_load_kernel(s, machine);
    } else {
        error_report("hp9000-340 currently only supports -kernel boot");
        exit(1);
    }

    qemu_register_reset(hp300_cpu_reset, s);
}

static void hp300_340_machine_init(MachineClass *mc)
{
    mc->desc = "HP 9000/340 (68030)";
    mc->init = hp300_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->max_cpus = 1;
    mc->default_ram_size = HP300_RAM_DEFAULT;
    mc->default_ram_id = "hp300.ram";
    mc->no_floppy = 1;
    mc->no_parallel = 1;
    mc->no_cdrom = 1;
}

DEFINE_MACHINE("hp9000-340", hp300_340_machine_init)
