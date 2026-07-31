/*
 * Commodore Amiga machine family, common core.
 *
 * This models the hardware every big-box/classic Amiga shares: chip
 * RAM, the Kickstart ROM (with the reset-time overlay at address 0
 * controlled by CIA-A PA0), the two 8520 CIAs and the custom chip
 * register block.  Board variants subclass this to add their CPU,
 * memory controller and I/O specifics.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/datadir.h"
#include "qemu/guest-random.h"
#include "exec/target_page.h"
#include "elf.h"
#include "system/reset.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "standard-headers/asm-m68k/bootinfo-amiga.h"
#include "bootinfo.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "hw/core/split-irq.h"
#include "hw/m68k/amiga.h"
#include "hw/m68k/amiga_custom.h"
#include "hw/m68k/zorro3.h"
#include "hw/m68k/amiga_fdc.h"
#include "hw/m68k/amiga_kbd.h"
#include "hw/m68k/mos8520.h"
#include "hw/ide/gayle.h"
#include "system/blockdev.h"
#include "target/m68k/cpu.h"

#define AMIGA_CIAB_BASE     0xbfd000
#define AMIGA_CIAA_BASE     0xbfe000
#define AMIGA_CUSTOM_BASE   0xdff000

/* CIA-A port A: the overlay and power LED lines Kickstart drives */
#define CIAA_PA_OVL         0x01
#define CIAA_PA_LED         0x02

/* PAL machines: display vertical blank and mains supply frequency */
#define AMIGA_PAL_VBLANK_HZ 50
#define AMIGA_PSFREQ_HZ     50
/*
 * The serial period passed in BI_AMIGA_SERPER: Paula divides the
 * 3.55MHz colour clock (5 * EClock) by SERPER + 1.
 */
#define AMIGA_SERIAL_BAUD   9600

static const char *const floppy_line_name[FLOPPY_LINE_COUNT] = {
    [FLOPPY_LINE_CHNG] = "chng",
    [FLOPPY_LINE_WPRO] = "wpro",
    [FLOPPY_LINE_TK0] = "tk0",
    [FLOPPY_LINE_RDY] = "rdy",
    [FLOPPY_LINE_INDEX] = "index",
};

static void amiga_floppy_line_set(void *opaque, int n, int level)
{
    struct AmigaFloppyLine *l = opaque;

    l->released = deposit32(l->released, n, 1, level);
    qemu_set_irq(l->out, l->released == (1 << AMIGA_FLOPPY_DRIVES) - 1);
}

/* fan one CIA output out to every drive */
static qemu_irq amiga_floppy_split_in(AmigaMachineState *ams,
                                      const char *name)
{
    DeviceState *split = qdev_new(TYPE_SPLIT_IRQ);
    int d;

    qdev_prop_set_uint16(split, "num-lines", AMIGA_FLOPPY_DRIVES);
    qdev_realize_and_unref(split, NULL, &error_fatal);
    for (d = 0; d < AMIGA_FLOPPY_DRIVES; d++) {
        qdev_connect_gpio_out(split, d,
                              qdev_get_gpio_in_named(ams->fdc[d], name, 0));
    }
    return qdev_get_gpio_in(split, 0);
}

/* Paula presents interrupts on the 68k autovectors */
static void amiga_set_ipl(void *opaque, int n, int level)
{
    M68kCPU *cpu = opaque;

    if (level) {
        m68k_set_irq_level(cpu, level, EXCP_INT_LEVEL_1 + level - 1);
    } else {
        m68k_set_irq_level(cpu, 0, 0);
    }
}

/* CIA-A PA0: ROM overlay at address 0 (pulled up, so on at reset) */
static void amiga_overlay_set(void *opaque, int n, int level)
{
    AmigaMachineState *ams = opaque;

    memory_region_set_enabled(&ams->rom_overlay, level != 0);
}

static void amiga_overlay_reset(void *opaque)
{
    AmigaMachineState *ams = opaque;

    /*
     * The overlay follows CIA-A PA0: pulled up out of reset, so the
     * ROM answers at address 0 - except when direct-booting a kernel,
     * where the machine models the post-Kickstart state with PA0
     * driven low and chip RAM at 0 (see amiga_machine_init).
     */
    memory_region_set_enabled(&ams->rom_overlay, !ams->linux_boot);
}

/*
 * The RESET instruction: /RSTO resets every chip on the board except
 * the CPU, which carries on with the next instruction.  Crucially the
 * CIA reset floats PA0 back high, so the Kickstart overlay returns:
 * the ROM reboot sequence is "reset" followed by a jump through the
 * initial-PC vector at address 4, which must read the ROM again.
 */
static void amiga_reset_out(void *opaque, int n, int level)
{
    AmigaMachineState *ams = opaque;
    int i;

    if (!level) {
        return;
    }
    device_cold_reset(ams->ciaa);
    device_cold_reset(ams->ciab);
    device_cold_reset(ams->custom);
    device_cold_reset(ams->kbd);
    for (i = 0; i < AMIGA_FLOPPY_DRIVES; i++) {
        device_cold_reset(ams->fdc[i]);
    }
    for (i = 0; i < AMIGA_RSTO_DEVS; i++) {
        if (ams->rsto_dev[i]) {
            device_cold_reset(ams->rsto_dev[i]);
        }
    }
    memory_region_set_enabled(&ams->rom_overlay, true);
}

static void amiga_cpu_reset(void *opaque)
{
    AmigaMachineState *ams = opaque;
    M68kCPU *cpu = ams->cpu;
    uint8_t *rom = memory_region_get_ram_ptr(&ams->rom);

    cpu_reset(CPU(cpu));
    if (ams->linux_boot) {
        /*
         * Direct kernel boot: enter head.S like an Amiga bootstrap
         * would - supervisor mode, interrupts masked, MMU off.
         * head.S switches to its own stack immediately, so the
         * initial SP only has to point at valid RAM.
         */
        cpu->env.aregs[7] = ams->fastram_base;
        cpu->env.pc = ams->kernel_entry;
        return;
    }
    /* initial SP/PC come from the ROM via the reset-time overlay */
    cpu->env.aregs[7] = ldl_be_p(rom);
    cpu->env.pc = ldl_be_p(rom + 4);
}

/*
 * Chip-bus filler: the glue logic terminates every cycle in this range,
 * so probing unpopulated addresses reads open bus rather than faulting.
 */
static uint64_t amiga_open_bus_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "amiga: open-bus read 0x%08" HWADDR_PRIx "\n",
                  addr);
    return (uint64_t)-1;
}

static void amiga_open_bus_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
}

const MemoryRegionOps amiga_open_bus_ops = {
    .read = amiga_open_bus_read,
    .write = amiga_open_bus_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

void amiga_gayle_init(AmigaMachineState *ams)
{
    DeviceState *gayle = qdev_new(TYPE_GAYLE_IDE);
    SysBusDevice *sbd = SYS_BUS_DEVICE(gayle);
    int i;

    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, GAYLE_IDE_ATA_BASE);
    sysbus_mmio_map(sbd, 1, GAYLE_IDE_CTRL_BASE);
    sysbus_mmio_map(sbd, 2, GAYLE_IDE_ID_BASE);
    /* the IDE interrupt is on INT2 */
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in_named(ams->custom, "ports-irq", 0));
    gayle_ide_init_drives(gayle, drive_get(IF_IDE, 0, 0),
                          drive_get(IF_IDE, 0, 1));

    for (i = 0; i < AMIGA_RSTO_DEVS; i++) {
        if (!ams->rsto_dev[i]) {
            ams->rsto_dev[i] = gayle;
            break;
        }
    }
}

static void rerandomize_rng_seed(void *opaque)
{
    struct bi_record *rng_seed = opaque;

    qemu_guest_getrandom_nofail((void *)rng_seed->data + 2,
                                be16_to_cpu(*(uint16_t *)rng_seed->data));
}

/*
 * A BI_AMIGA_AUTOCON record: an AmigaOS struct ConfigDev describing
 * one AutoConfig board expansion.library (played by QEMU for direct
 * kernel boot) configured; Linux's Zorro bus enumerates from these.
 */
#define CONFIGDEV_SIZE          68
#define CONFIGDEV_ROM           16      /* struct ExpansionRom cd_Rom */
#define CONFIGDEV_BOARDADDR     32
#define CONFIGDEV_BOARDSIZE     36
#define CONFIGDEV_SLOTADDR      40
#define CONFIGDEV_SLOTSIZE      42

static void *amiga_autocon_record(void *base, const Zorro3Board *board)
{
    uint8_t cd[CONFIGDEV_SIZE] = { 0 };

    cd[CONFIGDEV_ROM + 0] = board->er_type;             /* er_Type */
    cd[CONFIGDEV_ROM + 1] = board->er_product;          /* er_Product */
    cd[CONFIGDEV_ROM + 2] = board->er_flags;            /* er_Flags */
    stw_be_p(&cd[CONFIGDEV_ROM + 4], board->er_manufacturer);
    stl_be_p(&cd[CONFIGDEV_ROM + 6], board->er_serial);
    stl_be_p(&cd[CONFIGDEV_BOARDADDR], board->base);
    stl_be_p(&cd[CONFIGDEV_BOARDSIZE], board->size);
    stw_be_p(&cd[CONFIGDEV_SLOTADDR], board->base >> 16);
    stw_be_p(&cd[CONFIGDEV_SLOTSIZE], board->size >> 16);

    stw_be_p(base, BI_AMIGA_AUTOCON);
    base += 2;
    stw_be_p(base, sizeof(struct bi_record) + CONFIGDEV_SIZE);
    base += 2;
    memcpy(base, cd, CONFIGDEV_SIZE);
    return base + CONFIGDEV_SIZE;
}

/*
 * Linux/m68k is linked at virtual address 0 and head.S expects to be
 * loaded at the same offset from the base of the first BI_MEMCHUNK, so
 * relocate the whole image by the fast RAM base.
 */
static uint64_t amiga_kernel_translate(void *opaque, uint64_t addr)
{
    AmigaMachineState *ams = opaque;

    return ams->fastram_base + addr;
}

static void amiga_load_kernel(AmigaMachineState *ams)
{
    MachineState *machine = MACHINE(ams);
    AmigaMachineClass *amc = AMIGA_MACHINE_GET_CLASS(ams);
    CPUState *cs = CPU(ams->cpu);
    CPUM68KState *env = &ams->cpu->env;
    const char *kernel_cmdline = machine->kernel_cmdline;
    hwaddr fastram_end = ams->fastram_base + machine->ram_size;
    uint64_t elf_entry, high;
    ssize_t kernel_size;
    hwaddr parameters_base;
    ram_addr_t initrd_base;
    int64_t initrd_size;
    void *param_blob, *param_ptr, *param_rng_seed;
    uint8_t rng_seed[32];

    if (amc->amiga_model == AMI_UNKNOWN) {
        error_report("this Amiga model does not support direct kernel boot");
        exit(1);
    }
    if (!machine->ram_size) {
        error_report("direct kernel boot needs fast RAM for the kernel, "
                     "set -m");
        exit(1);
    }

    param_blob = g_malloc((kernel_cmdline ? strlen(kernel_cmdline) : 0) +
                          1024 + ZORRO_NUM_AUTO * (sizeof(struct bi_record) +
                                                   CONFIGDEV_SIZE));

    /*
     * The kernel runs from fast RAM; chip RAM is deliberately not a
     * BI_MEMCHUNK - the kernel takes it from BI_AMIGA_CHIP_SIZE and
     * hands it to its chipmem allocator.
     */
    kernel_size = load_elf(machine->kernel_filename, NULL,
                           amiga_kernel_translate, ams, &elf_entry,
                           NULL, &high, NULL, ELFDATA2MSB, EM_68K, 0, 0);
    if (kernel_size < 0) {
        error_report("could not load kernel '%s'", machine->kernel_filename);
        exit(1);
    }
    if (high + 1 * KiB > fastram_end) {
        error_report("kernel does not fit in %" PRId64 " MiB of fast RAM",
                     machine->ram_size / MiB);
        exit(1);
    }
    ams->kernel_entry = ams->fastram_base + elf_entry;

    /*
     * head.S locates the bootinfo records at the load address of _end,
     * which is the highest address the ELF loader touched.
     */
    parameters_base = QEMU_ALIGN_UP(high, 2);
    param_ptr = param_blob;

    BOOTINFO1(param_ptr, BI_MACHTYPE, MACH_AMIGA);
    if (m68k_feature(env, M68K_FEATURE_M68040)) {
        BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68040);
        BOOTINFO1(param_ptr, BI_FPUTYPE, FPU_68040);
        BOOTINFO1(param_ptr, BI_MMUTYPE, MMU_68040);
    } else if (m68k_feature(env, M68K_FEATURE_M68030)) {
        /* the A3000's 68030 pairs with a 68882 */
        BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68030);
        BOOTINFO1(param_ptr, BI_FPUTYPE, FPU_68882);
        BOOTINFO1(param_ptr, BI_MMUTYPE, MMU_68030);
    } else {
        error_report("direct kernel boot needs a CPU with an MMU");
        exit(1);
    }
    /* the chunk holding the kernel must come first */
    BOOTINFO2(param_ptr, BI_MEMCHUNK, ams->fastram_base, machine->ram_size);
    BOOTINFO1(param_ptr, BI_AMIGA_MODEL, amc->amiga_model);
    BOOTINFO1(param_ptr, BI_AMIGA_CHIPSET, amc->chipset);
    BOOTINFO1(param_ptr, BI_AMIGA_CHIP_SIZE, amc->chipram_size);
    BOOTINFOBYTE(param_ptr, BI_AMIGA_VBLANK, AMIGA_PAL_VBLANK_HZ);
    BOOTINFOBYTE(param_ptr, BI_AMIGA_PSFREQ, AMIGA_PSFREQ_HZ);
    BOOTINFO1(param_ptr, BI_AMIGA_ECLOCK, amc->cia_clock_hz);
    BOOTINFOWORD(param_ptr, BI_AMIGA_SERPER,
                 5 * amc->cia_clock_hz / AMIGA_SERIAL_BAUD - 1);

    /* AutoConfig boards, configured by the machine-done autoconfig pass */
    if (ams->zorro) {
        Zorro3Bus *zbus = &ZORRO3_BRIDGE(ams->zorro)->bus;
        Zorro3Board *board;
        int nboards = 0;

        QTAILQ_FOREACH(board, &zbus->boards, chain) {
            if (!board->configured) {
                continue;
            }
            if (++nboards > ZORRO_NUM_AUTO) {
                error_report("too many Zorro boards for the bootinfo");
                exit(1);
            }
            param_ptr = amiga_autocon_record(param_ptr, board);
        }

        /*
         * amiboot turns MEMLIST boards into extra memory chunks; play
         * that part too so Zorro III RAM expansions become kernel RAM.
         */
        QTAILQ_FOREACH(board, &zbus->boards, chain) {
            if (board->configured &&
                (board->er_type & ZORRO3_ERTF_MEMLIST)) {
                BOOTINFO2(param_ptr, BI_MEMCHUNK, board->base, board->size);
            }
        }
    }

    if (kernel_cmdline) {
        BOOTINFOSTR(param_ptr, BI_COMMAND_LINE, kernel_cmdline);
    }

    /* Pass seed to RNG. */
    param_rng_seed = param_ptr;
    qemu_guest_getrandom_nofail(rng_seed, sizeof(rng_seed));
    BOOTINFODATA(param_ptr, BI_RNG_SEED, rng_seed, sizeof(rng_seed));

    /* load initrd at the top of fast RAM */
    if (machine->initrd_filename) {
        initrd_size = get_image_size(machine->initrd_filename, NULL);
        if (initrd_size < 0) {
            error_report("could not load initial ram disk '%s'",
                         machine->initrd_filename);
            exit(1);
        }
        initrd_base = (fastram_end - initrd_size) & TARGET_PAGE_MASK;
        if (initrd_base < parameters_base + 4 * KiB) {
            error_report("initial ram disk does not fit in fast RAM "
                         "above the kernel");
            exit(1);
        }
        load_image_targphys(machine->initrd_filename, initrd_base,
                            fastram_end - initrd_base, &error_fatal);
        BOOTINFO2(param_ptr, BI_RAMDISK, initrd_base, initrd_size);
    }
    BOOTINFO0(param_ptr, BI_LAST);
    rom_add_blob_fixed_as("bootinfo", param_blob, param_ptr - param_blob,
                          parameters_base, cs->as);
    qemu_register_reset_nosnapshotload(rerandomize_rng_seed,
                        rom_ptr_for_as(cs->as, parameters_base,
                                       param_ptr - param_blob) +
                        (param_rng_seed - param_blob));
    g_free(param_blob);
}

static void amiga_machine_done(Notifier *notifier, void *data)
{
    AmigaMachineState *ams = container_of(notifier, AmigaMachineState,
                                          machine_done);

    /* no ROM ran AutoConfig, so QEMU plays expansion.library */
    if (ams->zorro) {
        zorro3_bridge_autoconfig(ZORRO3_BRIDGE(ams->zorro), &error_fatal);
    }
    amiga_load_kernel(ams);
}

static void amiga_machine_init(MachineState *machine)
{
    AmigaMachineState *ams = AMIGA_MACHINE(machine);
    AmigaMachineClass *amc = AMIGA_MACHINE_GET_CLASS(machine);
    MemoryRegion *sysmem = get_system_memory();
    ssize_t rom_loaded;
    DriveInfo *dinfo;
    char *filename;
    int i;

    ams->linux_boot = machine->kernel_filename != NULL;

    /*
     * Register the overlay reset before creating the CPU so the ROM is
     * mapped at 0 by the time the CPU reset fetches its initial SP/PC.
     */
    qemu_register_reset(amiga_overlay_reset, ams);

    ams->cpu = M68K_CPU(cpu_create(machine->cpu_type));
    qemu_register_reset(amiga_cpu_reset, ams);
    qdev_connect_gpio_out_named(DEVICE(ams->cpu), "reset-out", 0,
                                qemu_allocate_irq(amiga_reset_out, ams, 0));

    if (amc->open_bus_size) {
        memory_region_init_io(&ams->open_bus, OBJECT(machine),
                              &amiga_open_bus_ops, NULL, "amiga.open-bus",
                              amc->open_bus_size);
        memory_region_add_subregion_overlap(sysmem, 0, &ams->open_bus, -1);
    }

    memory_region_init_ram(&ams->chipram, NULL, "amiga.chipram",
                           amc->chipram_size, &error_fatal);
    memory_region_add_subregion(sysmem, 0, &ams->chipram);

    /* Kickstart ROM, and its reset-time overlay at address 0 */
    memory_region_init_rom(&ams->rom, NULL, "amiga.kickstart",
                           amc->rom_size, &error_fatal);
    memory_region_add_subregion(sysmem, amc->rom_base, &ams->rom);
    if (!machine->firmware && !machine->kernel_filename) {
        error_report("A Kickstart ROM image is required, use -bios");
        exit(1);
    }
    /*
     * When direct-booting a kernel with -kernel, Kickstart is never
     * executed and -bios becomes optional; the ROM region stays zeroed
     * if no image is given.
     */
    if (machine->firmware) {
        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        if (!filename) {
            error_report("Could not find Kickstart ROM '%s'",
                         machine->firmware);
            exit(1);
        }
        /*
         * Load the image directly rather than through the rom loader: the
         * contents must be in place before the first CPU reset fetches the
         * initial SP/PC through the overlay.
         */
        rom_loaded = load_image_size(filename,
                                     memory_region_get_ram_ptr(&ams->rom),
                                     amc->rom_size);
        g_free(filename);
        if (rom_loaded < 0) {
            error_report("Could not load Kickstart ROM '%s'",
                         machine->firmware);
            exit(1);
        }
        /*
         * A 256KB Kickstart (the 1.x images) in a 512KB ROM window sits in
         * a socket whose top address line the chip ignores, so the image
         * appears twice.  Replicate it so the 0xfc0000-based vectors of
         * those images land on code in the upper half of the window.
         */
        if (rom_loaded > 0 && rom_loaded * 2 == amc->rom_size) {
            uint8_t *rom = memory_region_get_ram_ptr(&ams->rom);

            memcpy(rom + rom_loaded, rom, rom_loaded);
        }
    }

    memory_region_init_alias(&ams->rom_overlay, OBJECT(machine),
                             "amiga.kickstart-overlay", &ams->rom, 0,
                             amc->rom_size);
    memory_region_add_subregion_overlap(sysmem, 0, &ams->rom_overlay, 1);

    /* the floppy drives, DF0 and DF1 */
    for (i = 0; i < AMIGA_FLOPPY_DRIVES; i++) {
        ams->fdc[i] = qdev_new(TYPE_AMIGA_FDC);
        dinfo = drive_get(IF_FLOPPY, 0, i);
        if (dinfo) {
            qdev_prop_set_drive(ams->fdc[i], "drive",
                                blk_by_legacy_dinfo(dinfo));
        }
        sysbus_realize_and_unref(SYS_BUS_DEVICE(ams->fdc[i]), &error_fatal);
    }

    /* custom chips */
    ams->custom = qdev_new(TYPE_AMIGA_CUSTOM);
    qdev_prop_set_chr(ams->custom, "chardev", serial_hd(0));
    if (machine->audiodev) {
        qdev_prop_set_string(ams->custom, "audiodev", machine->audiodev);
    }
    qdev_prop_set_uint32(ams->custom, "agnus-id", amc->agnus_id);
    qdev_prop_set_uint32(ams->custom, "denise-id", amc->denise_id);
    object_property_set_link(OBJECT(ams->custom), "fdc0",
                             OBJECT(ams->fdc[0]), &error_abort);
    object_property_set_link(OBJECT(ams->custom), "fdc1",
                             OBJECT(ams->fdc[1]), &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ams->custom), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ams->custom), 0, AMIGA_CUSTOM_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(ams->custom), 0,
                       qemu_allocate_irq(amiga_set_ipl, ams->cpu, 0));

    /* CIA-A: TOD counts vertical sync */
    ams->ciaa = qdev_new(TYPE_MOS8520);
    qdev_prop_set_uint32(ams->ciaa, "clock-frequency", amc->cia_clock_hz);
    qdev_prop_set_uint32(ams->ciaa, "tod-frequency", 50);
    if (ams->linux_boot) {
        /*
         * A direct-booted kernel enters the machine the way amiboot
         * leaves it, with Kickstart's CIA-A setup in place: OVL and
         * the power LED driven as outputs, OVL negated so chip RAM
         * answers at address 0.  Without this, the kernel's
         * read-modify-write LED accesses would see the pulled-up
         * input pins and latch the overlay back on.
         */
        qdev_prop_set_uint8(ams->ciaa, "reset-ddra",
                            CIAA_PA_OVL | CIAA_PA_LED);
        qdev_prop_set_uint8(ams->ciaa, "reset-pra", CIAA_PA_LED);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ams->ciaa), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ams->ciaa), 0, AMIGA_CIAA_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(ams->ciaa), 0,
                       qdev_get_gpio_in_named(ams->custom, "cia-irq", 0));
    qdev_connect_gpio_out_named(ams->ciaa, "port-a-out", 0,
                                qemu_allocate_irq(amiga_overlay_set, ams, 0));
    qdev_connect_gpio_out_named(ams->custom, "mouse-btn", 0,
                                qdev_get_gpio_in_named(ams->ciaa, "port-in",
                                                       AMIGA_CIAA_PA_FIR0));
    qdev_connect_gpio_out_named(ams->custom, "joy2-btn", 0,
                                qdev_get_gpio_in_named(ams->ciaa, "port-in",
                                                       AMIGA_CIAA_PA_FIR1));

    /* keyboard: clocks codes into CIA-A's SDR, acked by its SP output */
    ams->kbd = qdev_new(TYPE_AMIGA_KBD);
    object_property_set_link(OBJECT(ams->kbd), "cia", OBJECT(ams->ciaa),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ams->kbd), &error_fatal);
    qdev_connect_gpio_out_named(ams->ciaa, "sp-out", 0,
                                qdev_get_gpio_in_named(ams->kbd, "ack", 0));

    /* CIA-B: TOD counts horizontal sync */
    ams->ciab = qdev_new(TYPE_MOS8520);
    qdev_prop_set_uint32(ams->ciab, "clock-frequency", amc->cia_clock_hz);
    qdev_prop_set_uint32(ams->ciab, "tod-frequency", 15625);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ams->ciab), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ams->ciab), 0, AMIGA_CIAB_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(ams->ciab), 0,
                       qdev_get_gpio_in_named(ams->custom, "cia-irq", 1));

    /*
     * The floppy drives: control lines on CIA-B port B (the four
     * shared lines fanned out to every drive, one select line each),
     * the wired-AND status lines on CIA-A port A, and the index
     * pulse on CIA-B FLAG.
     */
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_MTR,
                                amiga_floppy_split_in(ams, "mtr"));
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_STEP,
                                amiga_floppy_split_in(ams, "step"));
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_DIR,
                                amiga_floppy_split_in(ams, "dir"));
    qdev_connect_gpio_out_named(ams->ciab, "port-b-out", AMIGA_CIAB_PB_SIDE,
                                amiga_floppy_split_in(ams, "side"));
    for (i = 0; i < AMIGA_FLOPPY_DRIVES; i++) {
        qdev_connect_gpio_out_named(ams->ciab, "port-b-out",
                                    AMIGA_CIAB_PB_SEL0 + i,
                                    qdev_get_gpio_in_named(ams->fdc[i],
                                                           "sel", 0));
    }

    ams->floppy_line[FLOPPY_LINE_CHNG].out =
        qdev_get_gpio_in_named(ams->ciaa, "port-in", AMIGA_CIAA_PA_CHNG);
    ams->floppy_line[FLOPPY_LINE_WPRO].out =
        qdev_get_gpio_in_named(ams->ciaa, "port-in", AMIGA_CIAA_PA_WPRO);
    ams->floppy_line[FLOPPY_LINE_TK0].out =
        qdev_get_gpio_in_named(ams->ciaa, "port-in", AMIGA_CIAA_PA_TK0);
    ams->floppy_line[FLOPPY_LINE_RDY].out =
        qdev_get_gpio_in_named(ams->ciaa, "port-in", AMIGA_CIAA_PA_RDY);
    ams->floppy_line[FLOPPY_LINE_INDEX].out =
        qdev_get_gpio_in_named(ams->ciab, "flag", 0);
    for (i = 0; i < FLOPPY_LINE_COUNT; i++) {
        struct AmigaFloppyLine *l = &ams->floppy_line[i];
        int d;

        l->released = (1 << AMIGA_FLOPPY_DRIVES) - 1;
        for (d = 0; d < AMIGA_FLOPPY_DRIVES; d++) {
            qdev_connect_gpio_out_named(ams->fdc[d], floppy_line_name[i], 0,
                                        qemu_allocate_irq(
                                            amiga_floppy_line_set, l, d));
        }
    }

    if (amc->board_init) {
        amc->board_init(ams);
    }

    /*
     * Direct Linux boot, from a machine-done notifier so that Zorro
     * boards added with -device exist (and can be autoconfigured) by
     * the time the bootinfo records are written.
     */
    if (machine->kernel_filename) {
        ams->machine_done.notify = amiga_machine_done;
        qemu_add_machine_init_done_notifier(&ams->machine_done);
    }
}

static void amiga_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AmigaMachineClass *amc = AMIGA_MACHINE_CLASS(oc);

    mc->init = amiga_machine_init;
    mc->block_default_type = IF_NONE;
    mc->no_parallel = 1;
    mc->no_cdrom = 1;
    /*
     * Behind a Mediator PCI bridge the I/O window's bus addresses
     * start at 0 and Linux really assigns the first I/O BAR there, so
     * address 0 must count as a mapped BAR, not "unconfigured".
     */
    mc->pci_allow_0_address = true;
    machine_add_audiodev_property(mc);

    /* PAL defaults */
    amc->cia_clock_hz = 709379;     /* E clock: 7.09MHz / 10 */
    amc->agnus_id = 0x22;           /* ECS 2MB Agnus */
    amc->denise_id = 0xfc;          /* ECS Denise */
}

static const TypeInfo amiga_machine_types[] = {
    {
        .name          = TYPE_AMIGA_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(AmigaMachineState),
        .class_size    = sizeof(AmigaMachineClass),
        .class_init    = amiga_machine_class_init,
        .abstract      = true,
    },
};

DEFINE_TYPES(amiga_machine_types)
