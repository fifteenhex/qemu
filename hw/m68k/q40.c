/*
 * Q40 — Sinclair-QL-successor 68040 desktop (Linux/m68k `q40`).
 *
 * Boots the Linux/m68k `q40` kernel (CONFIG_Q40, CONFIG_M68040) to a serial
 * shell via initramfs.  Models: 68040 + RAM at 0, the Q40 "master chip"
 * (interrupt status/enable + keyboard regs and the 200 Hz FRAME periodic
 * timer that drives the system tick), the ISA-mapped 16550 UART (COM1/ttyS0)
 * reached through the Q40 ISA longword-spaced address mapping, and the custom
 * Q40 RTC.
 *
 * Hardware contract reverse-engineered from the Linux q40 port; see
 * q40-NOTES.md in this directory for the full memory map and register layout.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
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
#include "target/m68k/cpu.h"
#include "elf.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "bootinfo.h"

/* ------------------------------------------------------------------ */
/* Physical memory map (see q40-NOTES.md) */
#define Q40_IO_BG_BASE          0xfe000000
#define Q40_IO_BG_SIZE          0x02000000      /* screen/ROM + all of 0xff.. */

#define Q40_MASTER_BASE         0xff000000
#define Q40_MASTER_SIZE         0x40

#define Q40_RTC_BASE            0xff021fe0      /* CTRL..YEAR, 4-byte stride  */
#define Q40_RTC_SIZE            0x20

#define Q40_ISA_IO_BASE         0xff400000
/* COM1/ttyS0: Q40_ISA_IO_B(0x3f8) = base + 1 + 4*0x3f8 */
#define Q40_COM1_ADDR           (Q40_ISA_IO_BASE + 1 + 4 * 0x3f8)
#define Q40_COM1_BAUDBASE       115200          /* 1843200 / 16 */

#define Q40_RAM_DEFAULT         (32 * MiB)
#define Q40_RAM_MAX             (32 * MiB)

/* Master-chip register offsets (byte, offset = reg*4) */
#define IIRQ_REG                0x00
#define EIRQ_REG                0x04
#define KEY_IRQ_ENABLE_REG      0x08
#define EXT_ENABLE_REG          0x10
#define SAMPLE_ENABLE_REG       0x14
#define DISPLAY_CONTROL_REG     0x18
#define KEYCODE_REG             0x1c
#define KEYBOARD_UNLOCK_REG     0x20
#define FRAME_CLEAR_REG         0x24
#define SAMPLE_CLEAR_REG        0x28
#define SAMPLE_RATE_REG         0x2c
#define LED_REG                 0x30
#define FRAME_RATE_REG          0x38

/* IIRQ_REG bits */
#define Q40_IRQ_KEYB_MASK       0x02
#define Q40_IRQ_SER_MASK        0x04
#define Q40_IRQ_FRAME_MASK      0x08
#define Q40_IRQ_EXT_MASK        0x10

/* The master chip drives these two 68040 IPL levels. */
#define Q40_FRAME_LEVEL         2       /* FRAME timer, keyboard, ISA (IRQ_AUTO_2) */
#define Q40_SAMPLE_LEVEL        4       /* SAMPLE timer (IRQ_AUTO_4) */

#define Q40_FRAME_HZ            200

/* EIRQ_REG bit for each supported ISA IRQ line. Index by ISA IRQ number. */
static inline uint8_t q40_eirq_bit(int isa_irq)
{
    switch (isa_irq) {
    case 3:  return 1 << 0;
    case 4:  return 1 << 1;
    case 5:  return 1 << 2;
    case 6:  return 1 << 3;
    case 7:  return 1 << 4;
    case 10: return 1 << 5;
    case 14: return 1 << 6;
    case 15: return 1 << 7;
    default: return 0;
    }
}

typedef struct Q40State {
    M68kCPU *cpu;
    DeviceState *irqc;

    MemoryRegion io_bg;
    MemoryRegion master;
    MemoryRegion rtc_mem;

    /* master-chip state */
    uint8_t ext_enable;
    uint8_t key_enable;
    uint8_t sample_enable;
    uint8_t frame_rate;
    bool frame_pending;
    bool sample_pending;
    bool keyb_pending;
    uint8_t keycode;
    uint8_t eirq;               /* asserted ISA IRQ lines (EIRQ_REG bits) */

    QEMUTimer *frame_timer;

    bool linux_boot;
    uint32_t reset_pc;
    uint32_t reset_sp;
} Q40State;

/* ------------------------------------------------------------------ */
/* Interrupt aggregation                                              */

static qemu_irq q40_level_irq(Q40State *s, int level)
{
    return qdev_get_gpio_in(s->irqc, level - 1);
}

static void q40_update_irq(Q40State *s)
{
    bool ext = (s->eirq != 0) && s->ext_enable;
    bool ipl2 = s->frame_pending ||
                (s->keyb_pending && s->key_enable) || ext;
    bool ipl4 = s->sample_pending && s->sample_enable;

    qemu_set_irq(q40_level_irq(s, Q40_FRAME_LEVEL), ipl2);
    qemu_set_irq(q40_level_irq(s, Q40_SAMPLE_LEVEL), ipl4);
}

/* External ISA IRQ line (e.g. the COM1 UART on ISA IRQ 4). */
static void q40_set_isa_irq(void *opaque, int isa_irq, int level)
{
    Q40State *s = opaque;
    uint8_t bit = q40_eirq_bit(isa_irq);

    if (level) {
        s->eirq |= bit;
    } else {
        s->eirq &= ~bit;
    }
    q40_update_irq(s);
}

/* ------------------------------------------------------------------ */
/* FRAME periodic timer (system tick, 200 Hz)                          */

static void q40_frame_arm(Q40State *s)
{
    uint64_t period_ns = NANOSECONDS_PER_SECOND / Q40_FRAME_HZ;

    timer_mod(s->frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period_ns);
}

static void q40_frame_expired(void *opaque)
{
    Q40State *s = opaque;

    s->frame_pending = true;
    q40_update_irq(s);
    if (s->frame_rate) {
        q40_frame_arm(s);
    }
}

/* ------------------------------------------------------------------ */
/* Master chip registers                                              */

static uint64_t q40_master_read(void *opaque, hwaddr addr, unsigned size)
{
    Q40State *s = opaque;

    switch (addr) {
    case IIRQ_REG:
        return (s->frame_pending ? Q40_IRQ_FRAME_MASK : 0) |
               ((s->keyb_pending && s->key_enable) ? Q40_IRQ_KEYB_MASK : 0) |
               (((s->eirq != 0) && s->ext_enable) ? Q40_IRQ_EXT_MASK : 0);
    case EIRQ_REG:
        return s->eirq;
    case KEYCODE_REG:
        s->keyb_pending = false;
        q40_update_irq(s);
        return s->keycode;
    default:
        return 0;
    }
}

static void q40_master_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Q40State *s = opaque;

    switch (addr) {
    case KEY_IRQ_ENABLE_REG:
        s->key_enable = val & 1;
        q40_update_irq(s);
        break;
    case EXT_ENABLE_REG:
        s->ext_enable = val & 1;
        q40_update_irq(s);
        break;
    case SAMPLE_ENABLE_REG:
        s->sample_enable = val & 1;
        q40_update_irq(s);
        break;
    case KEYBOARD_UNLOCK_REG:
        s->keyb_pending = false;
        q40_update_irq(s);
        break;
    case FRAME_CLEAR_REG:
        s->frame_pending = false;
        q40_update_irq(s);
        break;
    case SAMPLE_CLEAR_REG:
        s->sample_pending = false;
        q40_update_irq(s);
        break;
    case FRAME_RATE_REG:
        s->frame_rate = val;
        if (s->frame_rate) {
            q40_frame_arm(s);
        } else {
            timer_del(s->frame_timer);
        }
        break;
    case DISPLAY_CONTROL_REG:
    case SAMPLE_RATE_REG:
    case LED_REG:
    default:
        break;
    }
}

static const MemoryRegionOps q40_master_ops = {
    .read = q40_master_read,
    .write = q40_master_write,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* Custom Q40 RTC.  Byte registers, 4-byte stride, growing DOWN from    */
/* Q40_RTC_BASE (0xff021ffc); modelled from host wall-clock, BCD.      */

#define Q40_RTC_CTRL_OFF        0x00    /* 0xff021fe0 */
#define Q40_RTC_SECS_OFF        0x04
#define Q40_RTC_MINS_OFF        0x08
#define Q40_RTC_HOUR_OFF        0x0c
#define Q40_RTC_DOW_OFF         0x10
#define Q40_RTC_DATE_OFF        0x14
#define Q40_RTC_MNTH_OFF        0x18
#define Q40_RTC_YEAR_OFF        0x1c    /* 0xff021ffc */

static uint8_t q40_bcd(int v)
{
    return ((v / 10) << 4) | (v % 10);
}

static uint64_t q40_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    struct tm tm;

    qemu_get_timedate(&tm, 0);

    switch (addr) {
    case Q40_RTC_SECS_OFF: return q40_bcd(tm.tm_sec);
    case Q40_RTC_MINS_OFF: return q40_bcd(tm.tm_min);
    case Q40_RTC_HOUR_OFF: return q40_bcd(tm.tm_hour);
    case Q40_RTC_DOW_OFF:  return q40_bcd(tm.tm_wday + 1);
    case Q40_RTC_DATE_OFF: return q40_bcd(tm.tm_mday);
    case Q40_RTC_MNTH_OFF: return q40_bcd(tm.tm_mon + 1);
    case Q40_RTC_YEAR_OFF: return q40_bcd(tm.tm_year % 100);
    case Q40_RTC_CTRL_OFF: return 0;    /* PLL trim = 0, latch bits read 0 */
    default:               return 0;
    }
}

static void q40_rtc_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    /* Read/write latch and clock-set writes are accepted and ignored. */
}

static const MemoryRegionOps q40_rtc_ops = {
    .read = q40_rtc_read,
    .write = q40_rtc_write,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* I/O background absorber (screen/ROM/ISA probes): floating bus       */

static uint64_t q40_io_bg_read(void *opaque, hwaddr addr, unsigned size)
{
    return ~0ULL;
}

static void q40_io_bg_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
}

static const MemoryRegionOps q40_io_bg_ops = {
    .read = q40_io_bg_read,
    .write = q40_io_bg_write,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* Machine                                                            */

static void q40_cpu_reset(void *opaque)
{
    Q40State *s = opaque;
    CPUM68KState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->pc = s->reset_pc;
    env->aregs[7] = s->reset_sp;
    env->sp[env->current_sp] = s->reset_sp;
}

static void q40_load_kernel(Q40State *s, MachineState *machine)
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

    BOOTINFO1(param_ptr, BI_MACHTYPE, MACH_Q40);
    BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68040);
    BOOTINFO1(param_ptr, BI_MMUTYPE, MMU_68040);
    BOOTINFO1(param_ptr, BI_FPUTYPE, FPU_68040);
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

static void q40_init(MachineState *machine)
{
    Q40State *s = g_new0(Q40State, 1);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *irqc;
    qemu_irq com1_irq;

    if (machine->ram_size > Q40_RAM_MAX) {
        error_report("q40 supports at most %d MiB of RAM",
                     (int)(Q40_RAM_MAX / MiB));
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

    /* Main RAM at physical 0 */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    /* I/O background absorber over the screen/ROM + I/O window */
    memory_region_init_io(&s->io_bg, NULL, &q40_io_bg_ops, s,
                          "q40.io-bg", Q40_IO_BG_SIZE);
    memory_region_add_subregion_overlap(sysmem, Q40_IO_BG_BASE, &s->io_bg, 0);

    /* Master chip */
    memory_region_init_io(&s->master, NULL, &q40_master_ops, s,
                          "q40.master", Q40_MASTER_SIZE);
    memory_region_add_subregion_overlap(sysmem, Q40_MASTER_BASE,
                                        &s->master, 1);

    /* Custom RTC */
    memory_region_init_io(&s->rtc_mem, NULL, &q40_rtc_ops, s,
                          "q40.rtc", Q40_RTC_SIZE);
    memory_region_add_subregion_overlap(sysmem, Q40_RTC_BASE,
                                        &s->rtc_mem, 1);

    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, q40_frame_expired, s);

    /* ISA 16550 UART: COM1 / ttyS0, IRQ 4 through the master chip */
    com1_irq = qemu_allocate_irq(q40_set_isa_irq, s, 4);
    serial_mm_init(sysmem, Q40_COM1_ADDR, 2, com1_irq, Q40_COM1_BAUDBASE,
                   serial_hd(0), DEVICE_BIG_ENDIAN);

    if (s->linux_boot) {
        q40_load_kernel(s, machine);
    } else {
        error_report("q40 currently only supports -kernel boot");
        exit(1);
    }

    qemu_register_reset(q40_cpu_reset, s);
}

static void q40_machine_init(MachineClass *mc)
{
    mc->desc = "Q40 Sinclair QL successor (68040)";
    mc->init = q40_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->max_cpus = 1;
    mc->default_ram_size = Q40_RAM_DEFAULT;
    mc->default_ram_id = "q40.ram";
    mc->no_floppy = 1;
    mc->no_parallel = 1;
    mc->no_cdrom = 1;
}

DEFINE_MACHINE("q40", q40_machine_init)
