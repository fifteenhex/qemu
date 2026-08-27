/*
 * Motorola MVME162/167 VME single-board computer (Linux/m68k `mvme16x`).
 *
 * Models the MVME167: 25 MHz 68040 + FPU, VMEchip2, PCCchip2 (tick timer +
 * CD2401 interrupt glue), CD2401 serial controller, M48T59 NVRAM/RTC and a
 * VME A24 host bridge.  Boots the Linux/m68k `mvme16x` kernel; the first
 * 68040 machine in this tree to do so.
 *
 * See mvme16x-NOTES.md for the full hardware contract.  Bootinfo + kernel
 * load plumbing mirrors hw/m68k/mvme147.c; the CD2401 model is shared with
 * hw/m68k/e17.c.
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
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/device_tree.h"
#include "exec/target_page.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/char/cd2401.h"
#include "hw/vme/vme.h"
#include "target/m68k/cpu.h"
#include "elf.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "bootinfo.h"

/* VME-specific bootinfo tags (not in QEMU standard-headers) */
#define BI_VME_TYPE             0x8000  /* __be32 VME sub-architecture */
#define BI_VME_BRDINFO          0x8001  /* 32-byte t_bdid board-info struct */
#define VME_TYPE_MVME167        0x0167

/* Physical memory map (see mvme16x-NOTES.md) */
#define MVME16X_IO_BASE         0xfff00000
#define MVME16X_IO_SIZE         0x00100000      /* absorbs on-board I/O + NVRAM */
#define MVME16X_VMECHIP2_BASE   0xfff40000
#define MVME16X_VMECHIP2_SIZE   0x00010000
#define MVME16X_PCCCHIP2_BASE   0xfff42000
#define MVME16X_PCCCHIP2_SIZE   0x00000100
#define MVME16X_CD2401_BASE     0xfff45000
#define MVME16X_CD2401_IACK     0xfff4d000      /* unused on this board */
#define MVME16X_RTC_BASE        0xfffc0000
#define MVME16X_RTC_SIZE        0x00002000
#define MVME16X_VME_WINDOW      0xf0c00000
#define MVME16X_VME_WINDOW_SIZE (16 * MiB)

#define MVME16X_DEFAULT_RAM     (32 * MiB)

/* PCCchip2 tick timer 1 delivers CPU vector 0x59 at IPL 6 (see NOTES). */
#define PCC_TIMER_VECTOR        0x59
#define PCC_TIMER_LEVEL         6
#define PCC_TIMER_HZ            1000000         /* 1 MHz counter */

/* PCCchip2 register offsets */
#define PCCTCMP1                0x04
#define PCCTCNT1                0x08
#define PCCTOVR1                0x17
#define PCCTIC1                 0x1b
#define PCSCCMICR               0x1d
#define PCSCCTICR               0x1e
#define PCSCCRICR               0x1f
#define PCTPIACKR               0x25

#define PCCTOVR1_TIC_EN         0x01
#define PCCTOVR1_COC_EN         0x02
#define PCCTOVR1_OVR_CLR        0x04
#define PCCTIC1_INT_CLR         0x08
#define PCCTIC1_INT_EN          0x10

/* ------------------------------------------------------------------ */

typedef struct MVME16xState {
    M68kCPU *cpu;

    MemoryRegion io_bg;
    MemoryRegion vmechip2;
    MemoryRegion pccchip2;

    /* interrupt aggregation, (level<<8)|vector per source, 0 = idle */
    int pcc_irq;
    int vme_irq;

    /* PCCchip2 tick timer 1 */
    QEMUTimer *tick;
    uint32_t tcmp1;
    uint8_t tovr1;
    uint8_t tic1;
    bool tick_pending;
    uint8_t ovf;                        /* overflow nibble */
    int64_t base_ns;                    /* counter epoch */

    /* CD2401 SCC interrupt-control shadow */
    uint8_t sccmicr, sccticr, sccricr;

    /* reset state */
    uint32_t reset_pc;
    uint32_t reset_sp;
} MVME16xState;

static void mvme16x_update_irq(MVME16xState *s)
{
    int enc = (s->vme_irq >> 8) > (s->pcc_irq >> 8) ? s->vme_irq : s->pcc_irq;

    m68k_set_irq_level(s->cpu, enc >> 8, enc & 0xff);
}

static void mvme16x_vme_irq(void *opaque, int n, int value)
{
    MVME16xState *s = opaque;

    s->vme_irq = value;
    mvme16x_update_irq(s);
}

/* ------------------------------------------------------------------ */
/* PCCchip2 tick timer 1                                               */

static void mvme16x_tick_arm(MVME16xState *s)
{
    uint64_t period_ns = (uint64_t)s->tcmp1 * NANOSECONDS_PER_SECOND
        / PCC_TIMER_HZ;

    period_ns = MAX(period_ns, 1000);
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period_ns);
}

static bool mvme16x_tick_enabled(MVME16xState *s)
{
    return (s->tovr1 & PCCTOVR1_TIC_EN) && (s->tic1 & PCCTIC1_INT_EN);
}

static void mvme16x_tick_expired(void *opaque)
{
    MVME16xState *s = opaque;

    s->tick_pending = true;
    if (s->ovf < 15) {
        s->ovf++;
    }
    s->pcc_irq = (PCC_TIMER_LEVEL << 8) | PCC_TIMER_VECTOR;
    mvme16x_update_irq(s);

    if (mvme16x_tick_enabled(s)) {
        mvme16x_tick_arm(s);
    }
}

static void mvme16x_tick_deassert(MVME16xState *s)
{
    s->tick_pending = false;
    s->pcc_irq = 0;
    mvme16x_update_irq(s);
}

static uint32_t mvme16x_tcnt(MVME16xState *s)
{
    int64_t elapsed;
    uint64_t us;

    if (!(s->tovr1 & PCCTOVR1_TIC_EN) || s->tcmp1 == 0) {
        return 0;
    }
    elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->base_ns;
    us = (uint64_t)elapsed * PCC_TIMER_HZ / NANOSECONDS_PER_SECOND;
    return us % s->tcmp1;
}

static uint64_t mvme16x_pcc_read(void *opaque, hwaddr addr, unsigned size)
{
    MVME16xState *s = opaque;

    switch (addr) {
    case PCCTCMP1:
        return s->tcmp1;
    case PCCTCNT1:
        return mvme16x_tcnt(s);
    case PCCTOVR1:
        return (s->ovf << 4) | (s->tovr1 & 0x0f);
    case PCCTIC1:
        return s->tic1 | (s->tick_pending ? 0x80 : 0);
    case PCSCCMICR:
        return s->sccmicr;
    case PCSCCTICR:
        /* bit5 (0x20) = CD2401 tx interrupt active; TX is instantaneous */
        return s->sccticr | 0x20;
    case PCSCCRICR:
        return s->sccricr;
    case PCTPIACKR:
        return 0;
    default:
        return 0;
    }
}

static void mvme16x_pcc_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    MVME16xState *s = opaque;
    uint8_t b = val;

    switch (addr) {
    case PCCTCMP1:
        s->tcmp1 = val;
        break;
    case PCCTCNT1:
        s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        break;
    case PCCTOVR1:
        if (b & PCCTOVR1_OVR_CLR) {
            s->ovf = 0;
        }
        s->tovr1 = b & (PCCTOVR1_TIC_EN | PCCTOVR1_COC_EN);
        if (mvme16x_tick_enabled(s)) {
            mvme16x_tick_arm(s);
        } else {
            timer_del(s->tick);
        }
        break;
    case PCCTIC1:
        if (b & PCCTIC1_INT_CLR) {
            mvme16x_tick_deassert(s);
        }
        s->tic1 = b & (PCCTIC1_INT_EN | 0x07);
        if (mvme16x_tick_enabled(s)) {
            mvme16x_tick_arm(s);
        } else {
            timer_del(s->tick);
        }
        break;
    case PCSCCMICR:
        s->sccmicr = b;
        break;
    case PCSCCTICR:
        s->sccticr = b;
        break;
    case PCSCCRICR:
        s->sccricr = b;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps mvme16x_pcc_ops = {
    .read = mvme16x_pcc_read,
    .write = mvme16x_pcc_write,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* VMEchip2 GCSR / on-board I/O background: absorb reads/writes         */

static uint64_t mvme16x_io_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void mvme16x_io_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
}

static const MemoryRegionOps mvme16x_io_ops = {
    .read = mvme16x_io_read,
    .write = mvme16x_io_write,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* ------------------------------------------------------------------ */

static void mvme16x_cpu_reset(void *opaque)
{
    MVME16xState *s = opaque;
    CPUM68KState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->pc = s->reset_pc;
    env->aregs[7] = s->reset_sp;
    env->sp[env->current_sp] = s->reset_sp;
}

/* Emit the 32-byte t_bdid board-ID struct as a raw BI_VME_BRDINFO record. */
static void mvme16x_bootinfo_brdinfo(uint8_t **pp)
{
    uint8_t *p = *pp;
    uint8_t brdinfo[32];

    memset(brdinfo, 0, sizeof(brdinfo));
    memcpy(&brdinfo[0], "BDID", 4);
    brdinfo[4] = 0x10;                  /* rev  -> BUG 1.0 */
    brdinfo[5] = 0x01;                  /* mth */
    brdinfo[6] = 0x01;                  /* day */
    brdinfo[7] = 0x26;                  /* yr  */
    stw_be_p(&brdinfo[12], VME_TYPE_MVME167);   /* brdno */

    stw_be_p(p, BI_VME_BRDINFO);
    p += 2;
    stw_be_p(p, sizeof(struct bi_record) + sizeof(brdinfo));
    p += 2;
    memcpy(p, brdinfo, sizeof(brdinfo));
    p += sizeof(brdinfo);

    *pp = p;
}

static void mvme16x_load_kernel(MVME16xState *s, MachineState *machine)
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

    BOOTINFO1(param_ptr, BI_MACHTYPE, MACH_MVME16x);
    BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68040);
    BOOTINFO1(param_ptr, BI_MMUTYPE, MMU_68040);
    BOOTINFO1(param_ptr, BI_FPUTYPE, FPU_68040);
    BOOTINFO1(param_ptr, BI_VME_TYPE, VME_TYPE_MVME167);
    mvme16x_bootinfo_brdinfo((uint8_t **)&param_ptr);
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

    if (machine->dtb) {
        int dtb_size;
        void *dtb_blob = load_device_tree(machine->dtb, &dtb_size);
        hwaddr dtb_base;

        if (!dtb_blob) {
            error_report("could not load DTB '%s'", machine->dtb);
            exit(1);
        }
        /*
         * Place the DTB just above the kernel image (and its bootinfo
         * block).  head.S on mvme16x only maps low RAM early, so the DTB
         * that m68k_parse_bootinfo() copies during setup_arch() must live
         * near the kernel, not up at the top of a large RAM (which would
         * be unmapped when the early BI_FDT scan reads it).
         */
        dtb_base = (parameters_base + 0x100000) & TARGET_PAGE_MASK;
        rom_add_blob_fixed_as("dtb", dtb_blob, dtb_size, dtb_base, cs->as);
        g_free(dtb_blob);
        BOOTINFO1(param_ptr, BI_FDT, dtb_base);
    }

    BOOTINFO0(param_ptr, BI_LAST);
    rom_add_blob_fixed_as("bootinfo", param_blob, param_ptr - param_blob,
                          parameters_base, cs->as);
    g_free(param_blob);

    s->reset_sp = (top_of_ram - 0x1000) & ~3;
}

static void mvme16x_init(MachineState *machine)
{
    MVME16xState *s = g_new0(MVME16xState, 1);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *cd2401_dev;
    DeviceState *vmebridge_dev;
    DeviceState *rtc_dev;
    DriveInfo *dinfo;

    s->cpu = M68K_CPU(cpu_create(machine->cpu_type));

    /* RAM at physical 0 */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    /* On-board I/O background absorber (lowest priority in the I/O area) */
    memory_region_init_io(&s->io_bg, NULL, &mvme16x_io_ops, s,
                          "mvme16x.io-bg", MVME16X_IO_SIZE);
    memory_region_add_subregion_overlap(sysmem, MVME16X_IO_BASE, &s->io_bg, 0);

    /* VMEchip2 GCSR region (absorbed for now) */
    memory_region_init_io(&s->vmechip2, NULL, &mvme16x_io_ops, s,
                          "mvme16x.vmechip2", MVME16X_VMECHIP2_SIZE);
    memory_region_add_subregion_overlap(sysmem, MVME16X_VMECHIP2_BASE,
                                        &s->vmechip2, 1);

    /* PCCchip2: tick timer 1 + CD2401 interrupt glue */
    s->tcmp1 = PCC_TIMER_HZ / 100;      /* HZ = 100 default */
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, mvme16x_tick_expired, s);
    memory_region_init_io(&s->pccchip2, NULL, &mvme16x_pcc_ops, s,
                          "mvme16x.pccchip2", MVME16X_PCCCHIP2_SIZE);
    memory_region_add_subregion_overlap(sysmem, MVME16X_PCCCHIP2_BASE,
                                        &s->pccchip2, 1);

    /* CD2401 serial controller (shared model); console polled via PCCchip2 */
    cd2401_dev = qdev_new(TYPE_CD2401);
    qdev_prop_set_chr(cd2401_dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(cd2401_dev, "chrB", serial_hd(1));
    qdev_prop_set_chr(cd2401_dev, "chrC", serial_hd(2));
    qdev_prop_set_chr(cd2401_dev, "chrD", serial_hd(3));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(cd2401_dev), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(cd2401_dev), 0,
                            MVME16X_CD2401_BASE, 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(cd2401_dev), 1,
                            MVME16X_CD2401_IACK, 1);

    /* M48T59/M48T08 NVRAM + RTC */
    rtc_dev = qdev_new("sysbus-m48t08");
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive(rtc_dev, "drive", blk_by_legacy_dinfo(dinfo));
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(rtc_dev), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(rtc_dev), 0, MVME16X_RTC_BASE, 1);

    /*
     * VME A24 host bridge.  Slave cards attach with -device on this bus,
     * e.g. -device avme352,... for the interactive serial console, which
     * then answers at 0xf0c00000; its interrupt joins the PCC's on the
     * shared 68040 levels via the (level<<8)|vector encoding.
     */
    vmebridge_dev = qdev_new(TYPE_VME_BRIDGE);
    qdev_prop_set_uint64(vmebridge_dev, "size", MVME16X_VME_WINDOW_SIZE);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(vmebridge_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(vmebridge_dev), 0, MVME16X_VME_WINDOW);
    sysbus_connect_irq(SYS_BUS_DEVICE(vmebridge_dev), 0,
                       qemu_allocate_irq(mvme16x_vme_irq, s, 0));

    if (machine->kernel_filename) {
        mvme16x_load_kernel(s, machine);
    } else {
        error_report("mvme167 currently only supports -kernel boot");
        exit(1);
    }

    qemu_register_reset(mvme16x_cpu_reset, s);
}

static void mvme167_machine_init(MachineClass *mc)
{
    mc->desc = "Motorola MVME167 (68040)";
    mc->init = mvme16x_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->max_cpus = 1;
    mc->default_ram_size = MVME16X_DEFAULT_RAM;
    mc->default_ram_id = "mvme16x.ram";
    mc->no_floppy = 1;
    mc->no_parallel = 1;
    mc->no_cdrom = 1;
}

DEFINE_MACHINE("mvme167", mvme167_machine_init)
