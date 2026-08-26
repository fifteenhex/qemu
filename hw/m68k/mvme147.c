/*
 * QEMU Motorola MVME147 System Emulator
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "exec/target_page.h"
#include "system/reset.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "hw/char/escc.h"
#include "hw/misc/mvme147_pcc.h"
#include "hw/misc/mvme147_vmechip.h"
#include "hw/net/lance.h"
#include "hw/scsi/wd33c93.h"
#include "hw/rtc/m48t59.h"
#include "elf.h"
//#include "exec/memory.h"
#include "qemu/error-report.h"
#include "qemu/units.h"

#include "target/m68k/cpu.h"
#include "hw/intc/m68k_irqc.h"
#include "hw/block/flash.h"
#include "hw/vme/vme.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "bootinfo.h"

#define MVME147_ROM_BANK1    0xff800000
#define MVME147_ROM_BANK1_SZ (2 * MiB)
#define MVME147_ROM_BANK2    0xffa00000
#define MVME147_ROM_BANK2_SZ (2 * MiB)
#define MVME147_BBRAM        0xfffe0000
#define MVME147_BBRAM_SZ     2024
/* second NVRAM, "free for the OS": u-boot keeps its environment here */
#define MVME147_BBRAM2       0xfffe0800
#define MVME147_PCC          0xfffe1000
#define MVME147_LANCE        0xfffe1800
#define MVME147_VMECHIP      0xfffe2000
#define MVME147_SCC          0xfffe3000
#define MVME147_SCC2         0xfffe3800
#define MVME147_SCSI         0xfffe4000

/*
 * Where the board sees the VMEbus.  A VME slave card mapped here answers to
 * the 68030; the AVME-352 serial card is jumpered so its dual ported RAM
 * lands at 0xf0c00000.  Model that as a VME host bridge whose slave space
 * starts at this local address (so a card at VME base 0 appears here).
 */
#define MVME147_VME_WINDOW   0xf0c00000
#define MVME147_VME_WINDOW_SZ (16 * MiB)

void ledma_memory_read(void *opaque, hwaddr addr,
                       uint8_t *buf, int len, int do_bswap);
void ledma_memory_write(void *opaque, hwaddr addr,
                        uint8_t *buf, int len, int do_bswap);

void ledma_memory_write(void *opaque, hwaddr addr,
                        uint8_t *buf, int len, int do_bswap)
{
	//printf("ledma write - 0x%x (%d bytes) - %d\n", (unsigned) addr, len, do_bswap);

	/* data bytes are swapped */
	if (!do_bswap){
		for (int i = 0; i < len; i +=2) {
			uint8_t tmp = buf[i];
			buf[i] = buf[i + 1];
			buf[i + 1] = tmp;
		}
	}
	dma_memory_write(&address_space_memory, addr, buf, len, MEMTXATTRS_UNSPECIFIED);
}

void ledma_memory_read(void *opaque, hwaddr addr,
                       uint8_t *buf, int len, int do_bswap)
{
	//printf("ledma read - 0x%x (%d bytes) - %d\n", (unsigned) addr, len, do_bswap);

    dma_memory_read(&address_space_memory, addr, buf, len, MEMTXATTRS_UNSPECIFIED);
    /* data bytes are swapped */
    if (!do_bswap) {
		for (int i = 0; i < len; i +=2) {
			uint8_t tmp = buf[i];
			buf[i] = buf[i + 1];
			buf[i + 1] = tmp;
		}
    }
}

/*
 * The 68030's seven interrupt levels are shared between the on-board PCC
 * and the VMEbus.  Each source presents its request encoded as
 * (level << 8) | vector (0 = idle); the higher level wins and supplies the
 * vector the CPU acknowledges.
 */
typedef struct {
    M68kCPU *cpu;
    int pcc;        /* (level << 8) | vector, 0 = idle */
    int vme;
} MVME147IRQState;

static void mvme147_update_irq(MVME147IRQState *s)
{
    int enc = (s->vme >> 8) > (s->pcc >> 8) ? s->vme : s->pcc;

    m68k_set_irq_level(s->cpu, enc >> 8, enc & 0xff);
}

static void mvme147_pcc_irq(void *opaque, int n, int value)
{
    MVME147IRQState *s = opaque;

    s->pcc = value;
    mvme147_update_irq(s);
}

/* VME host bridge request, same (level << 8) | vector encoding. */
static void mvme147_vme_irq(void *opaque, int n, int value)
{
    MVME147IRQState *s = opaque;

    s->vme = value;
    mvme147_update_irq(s);
}

/*
 * The 147Bug entry point this board normally resets into (see
 * docs/system/m68k/mvme147.rst: the reset handler jumps here directly
 * rather than reading the reset vector).  -kernel overrides this with
 * the loaded ELF's entry point instead, see mvme147_init() below.
 */
#define MVME147_147BUG_ENTRY 0xff823952

typedef struct {
    M68kCPU *cpu;
    hwaddr reset_pc;
    hwaddr reset_sp;	/* 0 = leave whatever cpu_reset() fetched from
			 * address 0; the 147Bug ROM sets up its own stack
			 * as its first instructions and never relied on
			 * that fetch producing anything valid either. */
} MVME147ResetInfo;

static void main_cpu_reset(void *opaque)
{
    MVME147ResetInfo *info = opaque;
    CPUState *cs = CPU(info->cpu);
    CPUM68KState *env = &info->cpu->env;

    cpu_reset(cs);

    env->pc = info->reset_pc;
    if (info->reset_sp) {
        /*
         * Real VME bootloaders (vmelilo/tftplilo) don't actually reset
         * the CPU at all -- they just jump into the kernel from
         * whatever stack they were already running on.  Since this
         * model does a real cpu_reset() first, give the kernel a valid
         * stack explicitly rather than relying on address 0 holding a
         * vector table (RAM there is just zeroed).  Write both the
         * live register and its shadow slot directly instead of going
         * through m68k_switch_sp(), which saves aregs[7] into the
         * *current* context and loads aregs[7] back from whatever
         * context env->sr says is active -- a no-op swap here, not an
         * assignment.
         */
        env->aregs[7] = info->reset_sp;
        env->sp[env->current_sp] = info->reset_sp;
    }
}

static void mvme147_init(MachineState *machine)
{
    M68kCPU *cpu = NULL;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *rombank1 = g_new(MemoryRegion, 1);
    MemoryRegion *rombank2 = g_new(MemoryRegion, 1);
    MemoryRegion *lance_alias = g_new(MemoryRegion, 1);

    DriveInfo *dinfo;
    DeviceState *bbram_dev;
    DeviceState *pcc_dev;
    DeviceState *lance_dev;
    DeviceState *vmechip_dev;
    DeviceState *vmebridge_dev;
    DeviceState *serial_dev;
    DeviceState *scsi_dev;
    MVME147IRQState *irqs;
    MVME147ResetInfo *reset_info;

    if(!machine)
        printf("machine is null\n");

    /* CPU init */
    cpu = M68K_CPU(cpu_create(machine->cpu_type));
    reset_info = g_new0(MVME147ResetInfo, 1);
    reset_info->cpu = cpu;
    reset_info->reset_pc = MVME147_147BUG_ENTRY;
    qemu_register_reset(main_cpu_reset, reset_info);

    /* the PCC and the VMEbus share the 68030's interrupt levels */
    irqs = g_new0(MVME147IRQState, 1);
    irqs->cpu = cpu;

    /* RAM */
    memory_region_add_subregion(sysmem, 0, machine->ram);

    /* ROM bank 1 */
    memory_region_init_rom(rombank1, NULL, "mvme147.rombank1", MVME147_ROM_BANK1_SZ, &error_fatal);
    memory_region_add_subregion(sysmem, MVME147_ROM_BANK1, rombank1);
    if (machine->firmware)
        load_image_targphys(machine->firmware, MVME147_ROM_BANK1, MVME147_ROM_BANK1_SZ, NULL);

    /* ROM bank 2 */
    memory_region_init_rom(rombank2, NULL, "mvme147.rombank2", MVME147_ROM_BANK2_SZ, &error_fatal);
    memory_region_add_subregion(sysmem, MVME147_ROM_BANK2, rombank2);

    /* BBRAM, optionally persistent via -drive if=mtd */
    bbram_dev = qdev_new("sysbus-m48t02");
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive(bbram_dev, "drive", blk_by_legacy_dinfo(dinfo));
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(bbram_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(bbram_dev), 0, MVME147_BBRAM);

    /*
     * Second NVRAM at 0xfffe0800: the board has a further 2KB of
     * battery-backed RAM that the OS is free to use; u-boot stores its
     * environment here.  Persist it via a second -drive if=mtd,index=1.
     */
    bbram_dev = qdev_new("sysbus-m48t02");
    dinfo = drive_get(IF_MTD, 0, 1);
    if (dinfo) {
        qdev_prop_set_drive(bbram_dev, "drive", blk_by_legacy_dinfo(dinfo));
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(bbram_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(bbram_dev), 0, MVME147_BBRAM2);

    /* SCSI */
    scsi_dev = qdev_new(TYPE_WD33C93);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(scsi_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(scsi_dev), 0, MVME147_SCSI);

    /* PCC */
    pcc_dev = qdev_new(TYPE_MVME147_PCC);
    object_property_set_link(OBJECT(pcc_dev), "sbic", OBJECT(scsi_dev),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(pcc_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(pcc_dev), 0, MVME147_PCC);
    sysbus_connect_irq(SYS_BUS_DEVICE(pcc_dev), 0,
                       qemu_allocate_irq(mvme147_pcc_irq, irqs, 0));
    qdev_connect_gpio_out_named(scsi_dev, "drq", 0,
                                qdev_get_gpio_in_named(pcc_dev, "dma-drq", 0));

    /* LANCE */
    lance_dev = qdev_new(TYPE_LANCE);
    qemu_configure_nic_device(lance_dev, true, NULL);
    //object_property_set_link(OBJECT(lance_dev), "dma", OBJECT(sysmem), &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(lance_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(lance_dev), 0, MVME147_LANCE);
    /* 147bug uses a mirror */
    memory_region_init_alias(lance_alias, NULL, "lance.alias",
    		SYS_BUS_DEVICE(lance_dev)->mmio[0].memory, 0, 0x4);
    memory_region_add_subregion(sysmem, MVME147_LANCE + 0x4, lance_alias);

    /* VMEChip: the control/status registers the firmware probes */
    vmechip_dev = qdev_new(TYPE_MVME147_VMECHIP);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(vmechip_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(vmechip_dev), 0, MVME147_VMECHIP);

    /*
     * The VMEbus itself: a host bridge whose slave address space appears at
     * MVME147_VME_WINDOW.  Slave cards are added with -device on this bus,
     * e.g. -device avme352,rom=avme352.bin,serial-base=4 for the six channel
     * serial card, which then answers at 0xf0c00000.  Its interrupt joins
     * the PCC's on the shared 68030 levels.
     */
    vmebridge_dev = qdev_new(TYPE_VME_BRIDGE);
    qdev_prop_set_uint64(vmebridge_dev, "size", MVME147_VME_WINDOW_SZ);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(vmebridge_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(vmebridge_dev), 0, MVME147_VME_WINDOW);
    sysbus_connect_irq(SYS_BUS_DEVICE(vmebridge_dev), 0,
                       qemu_allocate_irq(mvme147_vme_irq, irqs, 0));

    /* SCC, serial ports 1 and 2 */
    serial_dev = qdev_new(TYPE_ESCC);
    qdev_prop_set_chr(serial_dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(serial_dev, "chrB", serial_hd(1));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(serial_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(serial_dev), 0, MVME147_SCC);

    /* second SCC, serial ports 3 and 4 */
    serial_dev = qdev_new(TYPE_ESCC);
    qdev_prop_set_chr(serial_dev, "chrA", serial_hd(2));
    qdev_prop_set_chr(serial_dev, "chrB", serial_hd(3));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(serial_dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(serial_dev), 0, MVME147_SCC2);

    /*
     * -kernel: load a raw vmlinux ELF and hand it a classic m68k bootinfo
     * block (BI_* records right after the kernel image, per
     * arch/m68k/kernel/setup_mm.c) instead of resetting into 147Bug.
     * Modelled on the linux_boot path in q800_init() in this same file.
     */
    if (machine->kernel_filename) {
        CPUState *cs = CPU(cpu);
        uint64_t elf_entry, high;
        int64_t kernel_size;
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
        reset_info->reset_pc = elf_entry;

        parameters_base = (high + 1) & ~1;
        param_blob = g_malloc(machine->kernel_cmdline ?
                              strlen(machine->kernel_cmdline) + 1024 : 1024);
        param_ptr = param_blob;

        /*
         * Tag order matches virt_init()'s linux_boot path in this same
         * file: BI_MMUTYPE before the (repeated) BI_CPUTYPE, and no
         * BI_FPUTYPE record at all for a board with no on-chip/coprocessor
         * FPU.  head.S parses machtype/cputype/fputype/mmutype itself
         * before the generic C bootinfo scanner ever runs, so match a
         * proven-working sequence rather than the (also spec-legal, but
         * untested) ordering used for q800's MAC_* records.
         */
        BOOTINFO1(param_ptr, BI_MACHTYPE, MACH_MVME147);
        BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68030);
        BOOTINFO1(param_ptr, BI_MMUTYPE, MMU_68030);
        BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68030);
        BOOTINFO2(param_ptr, BI_MEMCHUNK, 0, machine->ram_size);

        if (machine->kernel_cmdline)
            BOOTINFOSTR(param_ptr, BI_COMMAND_LINE, machine->kernel_cmdline);

        if (machine->initrd_filename) {
            int64_t initrd_size = get_image_size(machine->initrd_filename, NULL);
            hwaddr initrd_base;

            if (initrd_size < 0) {
                error_report("could not load initial ram disk '%s'",
                             machine->initrd_filename);
                exit(1);
            }
            initrd_base = (machine->ram_size - initrd_size) & TARGET_PAGE_MASK;
            load_image_targphys(machine->initrd_filename, initrd_base,
                                machine->ram_size - initrd_base, &error_fatal);
            BOOTINFO2(param_ptr, BI_RAMDISK, initrd_base, initrd_size);
            top_of_ram = initrd_base;
        }

        BOOTINFO0(param_ptr, BI_LAST);
        rom_add_blob_fixed_as("bootinfo", param_blob, param_ptr - param_blob,
                              parameters_base, cs->as);
        g_free(param_blob);

        /* A temporary stack is all this needs -- head.S sets up the
         * kernel's real one almost immediately. */
        reset_info->reset_sp = (top_of_ram - 0x1000) & ~3;
    }
}

#define MVME147_DEFAULT_SDRAM_SIZE (16 * MiB)

static void mvme147_machine_init(MachineClass *mc)
{
    mc->desc = "mvme147";
    mc->init = mvme147_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->default_ram_size = MVME147_DEFAULT_SDRAM_SIZE;
    mc->default_ram_id = "dram";
}

DEFINE_MACHINE("mvme147", mvme147_machine_init)
