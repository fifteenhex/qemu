/*
 * QEMU Macintosh Quadra 700 hardware system emulator
 *
 * A close sibling of the Quadra 800 (q800.c): 68040, VIA1 (ADB-II) +
 * VIA2, ESP 53C96 SCSI, ESCC serial, SWIM floppy, SONIC Ethernet,
 * EASC sound, NuBus with the built-in DAFB video in pseudo-slot 9.
 *
 * Deltas from the Quadra 800:
 *  - the ESP lives at 0x50F0F000 (not 0x50F10000);
 *  - SCSI pseudo-DMA is the MAC_SCSI_QUADRA2 flavour: the DRQ line is
 *    bit 9 of the DAFB "TurboSCSI" handshake register at 0xf9800024
 *    (DAFB register space + 0x24), not the VIA2 IFR;
 *  - no djMEMC memory controller and no IOSB;
 *  - the VIA1 port A machine-ID straps differ.
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
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/datadir.h"
#include "qemu/guest-random.h"
#include "exec/target_page.h"
#include "system/system.h"
#include "target/m68k/cpu.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "elf.h"
#include "hw/core/loader.h"
#include "ui/console.h"
#include "hw/char/escc.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/scsi/esp.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "standard-headers/asm-m68k/bootinfo-mac.h"
#include "bootinfo.h"
#include "hw/m68k/q800-glue.h"
#include "hw/misc/mac_via.h"
#include "hw/input/adb.h"
#include "hw/audio/asc.h"
#include "hw/nubus/mac-nubus-bridge.h"
#include "hw/display/macfb.h"
#include "hw/block/swim.h"
#include "hw/net/dp8393x.h"
#include "net/net.h"
#include "net/util.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/qtest.h"
#include "system/runstate.h"
#include "system/reset.h"
#include "migration/vmstate.h"

#define MACROM_ADDR     0x40800000
#define MACROM_SIZE     0x00100000

#define MACROM_FILENAME "quadra700.rom"

#define IO_BASE               0x50000000
#define IO_SLICE              0x00040000
#define IO_SLICE_MASK         (IO_SLICE - 1)
#define IO_SIZE               0x04000000

#define VIA_BASE              (IO_BASE + 0x00000)
#define SONIC_PROM_BASE       (IO_BASE + 0x08000)
#define SONIC_BASE            (IO_BASE + 0x0a000)
#define SCC_BASE              (IO_BASE + 0x0c020)
#define ESP_BASE              (IO_BASE + 0x0f000)
#define ESP_PDMA              (IO_BASE + 0x0f100)
#define ASC_BASE              (IO_BASE + 0x14000)
#define SWIM_BASE             (IO_BASE + 0x1E000)

#define SONIC_PROM_SIZE       0x1000

/*
 * The DAFB (slot 9) register space contains the "TurboSCSI" pseudo-DMA
 * handshake register: 32-bit reg at +0x24, bit 9 = live SCSI DRQ
 * (MAC_SCSI_QUADRA2; see Linux drivers/scsi/mac_esp.c and the U-Boot
 * oldmac port).  We overlay a small window on top of the macfb regs.
 */
#define DAFB_REGS_BASE        0xf9800000
#define TURBOSCSI_BASE        (DAFB_REGS_BASE + 0x20)
#define TURBOSCSI_SIZE        0x10
#define TURBOSCSI_DRQ         0x200

/*
 * the video base, whereas it a Nubus address,
 * is needed by the kernel to have early display and
 * thus provided by the bootloader
 */
#define VIDEO_BASE            0xf9000000

#define MAC_CLOCK  3686418

/* Size of whole RAM area */
#define RAM_SIZE              0x40000000

/*
 * Slot 0x9 is reserved for use by the in-built framebuffer whilst only
 * slots 0xd and 0xe physically exist on the Quadra 700
 */
#define Q700_NUBUS_SLOTS_AVAILABLE    (BIT(0x9) | BIT(0xd) | BIT(0xe))

/* Same ID register value as the other Quadra-class machines */
#define Q700_MACHINE_ID    0xa55a2bad

/*
 * VIA1 port A machine-ID straps (mask 0x56: PA1, PA2, PA4, PA6).
 * The ROM's universal table entry for the Quadra 700 (box 0x10, ROM
 * offset 0x390c) requires decoder kind 8 (a successful long read of the
 * MCU at 0x5000e000) and (VIA1 PA & 0x56) == 0x40 (MAME returns 0xc1
 * from via_in_a).  NOTE: 0x50 selects box 0x0e = the QUADRA 900, whose
 * entry flags demand an SCC IOP and an Egret MCU instead of the
 * classic RTC/ADB pair.
 */
#define Q700_VIA1_CPUID    0x40

struct Q700MachineState {
    MachineState parent_obj;

    bool easc;
    M68kCPU cpu;
    MemoryRegion rom;
    MemoryRegion rom_alias;
    GLUEState glue;
    MOS6522Q800VIA1State via1;
    MOS6522Q800VIA2State via2;
    dp8393xState dp8393x;
    MemoryRegion dp8393x_prom;
    ESCCState escc;
    OrIRQState escc_orgate;
    SysBusESPState esp;
    Swim swim;
    MacNubusBridge mac_nubus_bridge;
    MacfbNubusState macfb;
    ASCState asc;
    MemoryRegion ramio;
    MemoryRegion macio;
    MemoryRegion macio_alias;
    MemoryRegion machine_id;
    MemoryRegion escc_alias;
    MemoryRegion turboscsi_mem;
    MemoryRegion mcu_mem;
    uint8_t mcu_regs[0x2000];
    MemoryRegion iop_mem;
    uint8_t iop_ram[0x8000];
    uint16_t iop_addr;
    uint8_t iop_ctrl;
    MemoryRegion iotrace;
    MemoryRegion bgtrace;

    /* TurboSCSI pseudo-DMA handshake */
    uint32_t turboscsi_ctrl;
    int esp_drq;
    qemu_irq esp_drq_via2;
};

#define TYPE_Q700_MACHINE MACHINE_TYPE_NAME("quadra700")
OBJECT_DECLARE_SIMPLE_TYPE(Q700MachineState, Q700_MACHINE)

static void main_cpu_reset(void *opaque)
{
    M68kCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    cpu->env.aregs[7] = ldl_phys(cs->as, 0);
    cpu->env.pc = ldl_phys(cs->as, 4);
}

static uint32_t q700_trace_pc(void)
{
    return current_cpu ? M68K_CPU(current_cpu)->env.pc : 0;
}

static void rerandomize_rng_seed(void *opaque)
{
    struct bi_record *rng_seed = opaque;
    qemu_guest_getrandom_nofail((void *)rng_seed->data + 2,
                                be16_to_cpu(*(uint16_t *)rng_seed->data));
}

static MemTxResult macio_alias_read(void *opaque, hwaddr addr, uint64_t *data,
                                    unsigned size, MemTxAttrs attrs)
{
    MemTxResult r;
    uint32_t val;

    addr &= IO_SLICE_MASK;
    addr |= IO_BASE;

    switch (size) {
    case 4:
        val = address_space_ldl_be(&address_space_memory, addr, attrs, &r);
        break;
    case 2:
        val = address_space_lduw_be(&address_space_memory, addr, attrs, &r);
        break;
    case 1:
        val = address_space_ldub(&address_space_memory, addr, attrs, &r);
        break;
    default:
        g_assert_not_reached();
    }

    *data = val;
    return r;
}

static MemTxResult macio_alias_write(void *opaque, hwaddr addr, uint64_t value,
                                     unsigned size, MemTxAttrs attrs)
{
    MemTxResult r;

    addr &= IO_SLICE_MASK;
    addr |= IO_BASE;

    switch (size) {
    case 4:
        address_space_stl_be(&address_space_memory, addr, value, attrs, &r);
        break;
    case 2:
        address_space_stw_be(&address_space_memory, addr, value, attrs, &r);
        break;
    case 1:
        address_space_stb(&address_space_memory, addr, value, attrs, &r);
        break;
    default:
        g_assert_not_reached();
    }

    return r;
}

static const MemoryRegionOps macio_alias_ops = {
    .read_with_attrs = macio_alias_read,
    .write_with_attrs = macio_alias_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t machine_id_read(void *opaque, hwaddr addr, unsigned size)
{
    /* long reads want 0xA55Axxxx; word reads compare the low word */
    return Q700_MACHINE_ID & ((size == 4) ? 0xffffffff :
                              (size == 2) ? 0xffff : 0xff);
}

static void machine_id_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
}

static const MemoryRegionOps machine_id_ops = {
    .read = machine_id_read,
    .write = machine_id_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t ramio_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0x0;
}

static void ramio_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
}

static const MemoryRegionOps ramio_ops = {
    .read = ramio_read,
    .write = ramio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * DAFB TurboSCSI pseudo-DMA handshake: 32-bit register at DAFB + 0x24.
 * Bit 9 reads back the live ESP DRQ; the other bits are handshake
 * timing control written by drivers (Linux/U-Boot write 0x1d1).
 */

static uint64_t q700_turboscsi_read(void *opaque, hwaddr addr, unsigned size)
{
    Q700MachineState *m = opaque;
    uint32_t reg = 0;
    uint64_t val;
    int i;

    if ((addr & ~3) == 4) {     /* 0xf9800024 */
        static int count;

        reg = (m->turboscsi_ctrl & ~TURBOSCSI_DRQ) |
              (m->esp_drq ? TURBOSCSI_DRQ : 0);
        if (count < 400) {
            count++;
            qemu_log_mask(LOG_UNIMP,
                          "q700 turboscsi: read +0x%02x -> 0x%08x pc=0x%08x\n",
                          (unsigned)addr, reg, q700_trace_pc());
        }
    }

    /* slice the 32-bit register by byte lane */
    val = 0;
    for (i = 0; i < size; i++) {
        val = (val << 8) | ((reg >> (8 * (3 - ((addr + i) & 3)))) & 0xff);
    }
    return val;
}

static void q700_turboscsi_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    Q700MachineState *m = opaque;
    static int count;

    if (count < 200) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q700 turboscsi: write +0x%02x (%d) <- 0x%08" PRIx64
                      " pc=0x%08x\n", (unsigned)addr, size, val,
                      q700_trace_pc());
    }
    if ((addr & ~3) == 4) {
        m->turboscsi_ctrl = val;
    }
}

static const MemoryRegionOps q700_turboscsi_ops = {
    .read = q700_turboscsi_read,
    .write = q700_turboscsi_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * SCC IOP at 0x50F0C000 (SCC_IOP_BASE_QUADRA): the Quadra 700 fronts
 * its SCC with an I/O Processor.  The ROM's POST RAM-tests the IOP's
 * shared RAM through the host interface and then talks to the SCC via
 * the bypass window at +0x20 (where our ESCC is mapped); we model only
 * the host registers and 32K of shared RAM, not the IOP's own core.
 * Register layout (Linux asm/mac_iop.h; A0 is not decoded):
 *   +0/+1 ram_addr_hi, +2/+3 ram_addr_lo, +4..+7 status_ctrl,
 *   +8..+0x1f ram_data (autoincrementing when IOP_AUTOINC is set).
 */

#define IOP_BYPASS       0x01
#define IOP_AUTOINC      0x02
#define IOP_RUN          0x04
#define IOP_IRQ          0x08
#define IOP_DMAINACTIVE  0x80

static uint64_t q700_iop_read(void *opaque, hwaddr addr, unsigned size)
{
    Q700MachineState *m = opaque;
    uint8_t val;

    switch (addr >> 1) {
    case 0:
        val = m->iop_addr >> 8;
        break;
    case 1:
        val = m->iop_addr & 0xff;
        break;
    case 2:
    case 3:
        /* no DMA request pending in bypass mode */
        val = m->iop_ctrl | IOP_DMAINACTIVE;
        break;
    default:
        val = m->iop_ram[m->iop_addr & 0x7fff];
        if (m->iop_ctrl & IOP_AUTOINC) {
            m->iop_addr++;
        }
        break;
    }
    return val;
}

static void q700_iop_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    Q700MachineState *m = opaque;

    switch (addr >> 1) {
    case 0:
        m->iop_addr = (m->iop_addr & 0x00ff) | ((val & 0xff) << 8);
        break;
    case 1:
        m->iop_addr = (m->iop_addr & 0xff00) | (val & 0xff);
        break;
    case 2:
    case 3:
        m->iop_ctrl = val & 0x7f;
        break;
    default:
        m->iop_ram[m->iop_addr & 0x7fff] = val;
        if (m->iop_ctrl & IOP_AUTOINC) {
            m->iop_addr++;
        }
        break;
    }
}

static const MemoryRegionOps q700_iop_ops = {
    .read = q700_iop_read,
    .write = q700_iop_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        /* multi-byte accesses hit the byte registers lane by lane */
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * MCU (Quadra 700/900 memory controller) at 0x50F0E000: the ROM's
 * machine identification requires a successful long read here (decoder
 * kind 8) and the memory sizing writes bank registers.  RAM-backed
 * logging regbank, grown empirically.
 */

static uint64_t q700_mcu_read(void *opaque, hwaddr addr, unsigned size)
{
    Q700MachineState *m = opaque;
    uint64_t val = 0;
    static int count;
    int i;

    for (i = 0; i < size; i++) {
        val = (val << 8) | m->mcu_regs[(addr + i) & (sizeof(m->mcu_regs) - 1)];
    }
    if (count < 2000) {
        count++;
        qemu_log_mask(LOG_UNIMP, "q700 mcu: read  +0x%04x (%d) -> 0x%08"
                      PRIx64 " pc=0x%08x\n", (unsigned)addr, size, val,
                      q700_trace_pc());
    }
    return val;
}

static void q700_mcu_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    Q700MachineState *m = opaque;
    static int count;
    int i;

    if (count < 2000) {
        count++;
        qemu_log_mask(LOG_UNIMP, "q700 mcu: write +0x%04x (%d) <- 0x%08"
                      PRIx64 " pc=0x%08x\n", (unsigned)addr, size, val,
                      q700_trace_pc());
    }
    for (i = size - 1; i >= 0; i--) {
        m->mcu_regs[(addr + i) & (sizeof(m->mcu_regs) - 1)] = val & 0xff;
        val >>= 8;
    }
}

static const MemoryRegionOps q700_mcu_ops = {
    .read = q700_mcu_read,
    .write = q700_mcu_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* ESP DRQ: latch for the TurboSCSI register and forward to VIA2 */
static void q700_esp_drq(void *opaque, int n, int level)
{
    Q700MachineState *m = opaque;

    m->esp_drq = level;
    /* negative edge triggered, as on q800 */
    qemu_set_irq(m->esp_drq_via2, !level);
}

/* unmapped I/O space bus-errors on the real machine; log the probes */

static MemTxResult q700_iotrace_read(void *opaque, hwaddr addr,
                                     uint64_t *data, unsigned size,
                                     MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q700 io: read  +0x%05x (%d) -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, q700_trace_pc());
    }
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult q700_iotrace_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned size,
                                      MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q700 io: write +0x%05x (%d) <- 0x%08" PRIx64
                      " -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, val, q700_trace_pc());
    }
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps q700_iotrace_ops = {
    .read_with_attrs = q700_iotrace_read,
    .write_with_attrs = q700_iotrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* background catch-all outside the I/O slice: log + BERR */

static MemTxResult q700_bgtrace_read(void *opaque, hwaddr addr,
                                     uint64_t *data, unsigned size,
                                     MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q700 bus: read  0x%08x (%d) -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, q700_trace_pc());
    }
    *data = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult q700_bgtrace_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned size,
                                      MemTxAttrs attrs)
{
    static int count;

    if (count < 20000) {
        count++;
        qemu_log_mask(LOG_UNIMP,
                      "q700 bus: write 0x%08x (%d) <- 0x%08" PRIx64
                      " -> BERR pc=0x%08x\n",
                      (unsigned)addr, size, val, q700_trace_pc());
    }
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps q700_bgtrace_ops = {
    .read_with_attrs = q700_bgtrace_read,
    .write_with_attrs = q700_bgtrace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void q700_machine_init(MachineState *machine)
{
    Q700MachineState *m = Q700_MACHINE(machine);
    int linux_boot;
    int32_t kernel_size;
    uint64_t elf_entry;
    char *filename;
    int bios_size;
    ram_addr_t initrd_base;
    int32_t initrd_size;
    uint8_t *prom;
    int i, checksum;
    const MacFbMode *macfb_mode;
    ram_addr_t ram_size = machine->ram_size;
    const char *kernel_filename = machine->kernel_filename;
    const char *initrd_filename = machine->initrd_filename;
    const char *kernel_cmdline = machine->kernel_cmdline;
    const char *bios_name = machine->firmware ?: MACROM_FILENAME;
    hwaddr parameters_base;
    CPUState *cs;
    DeviceState *dev;
    SysBusESPState *sysbus_esp;
    ESPState *esp;
    SysBusDevice *sysbus;
    BusState *adb_bus;
    NubusBus *nubus;
    DriveInfo *dinfo;
    NICInfo *nd;
    MACAddr mac;
    uint8_t rng_seed[32];

    linux_boot = (kernel_filename != NULL);

    if (ram_size > 1 * GiB) {
        error_report("Too much memory for this machine: %" PRId64 " MiB, "
                     "maximum 1024 MiB", ram_size / MiB);
        exit(1);
    }

    /* init CPUs */
    object_initialize_child(OBJECT(machine), "cpu", &m->cpu, machine->cpu_type);
    qdev_realize(DEVICE(&m->cpu), NULL, &error_fatal);
    qemu_register_reset(main_cpu_reset, &m->cpu);

    /* RAM */
    memory_region_init_io(&m->ramio, OBJECT(machine), &ramio_ops, &m->ramio,
                          "ram", RAM_SIZE);
    memory_region_add_subregion(get_system_memory(), 0x0, &m->ramio);

    memory_region_add_subregion(&m->ramio, 0, machine->ram);

    /* background catch-all: everything unclaimed bus-errors, logged */
    memory_region_init_io(&m->bgtrace, OBJECT(machine), &q700_bgtrace_ops,
                          m, "q700.bus-trace", 0xffffffffull + 1);
    memory_region_add_subregion_overlap(get_system_memory(), 0,
                                        &m->bgtrace, -3);

    /*
     * Create container for all IO devices
     */
    memory_region_init(&m->macio, OBJECT(machine), "mac-io", IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE, &m->macio);

    /*
     * Memory from IO_BASE to IO_BASE + IO_SLICE is repeated
     * from IO_BASE + IO_SLICE to IO_BASE + IO_SIZE
     */
    memory_region_init_io(&m->macio_alias, OBJECT(machine), &macio_alias_ops,
                          &m->macio, "mac-io.alias", IO_SIZE - IO_SLICE);
    memory_region_add_subregion(get_system_memory(), IO_BASE + IO_SLICE,
                                &m->macio_alias);

    /* catch-all trace region behind the devices in the I/O slice */
    memory_region_init_io(&m->iotrace, OBJECT(machine), &q700_iotrace_ops,
                          m, "mac-io.trace", IO_SLICE);
    memory_region_add_subregion_overlap(&m->macio, 0, &m->iotrace, -1);

    /* MCU memory controller regbank at 0x50F0E000 */
    memory_region_init_io(&m->mcu_mem, OBJECT(machine), &q700_mcu_ops, m,
                          "mcu", sizeof(m->mcu_regs));
    memory_region_add_subregion(&m->macio, 0xe000, &m->mcu_mem);

    memory_region_init_io(&m->machine_id, NULL, &machine_id_ops, NULL,
                          "Machine ID", 4);
    memory_region_add_subregion(get_system_memory(), 0x5ffffffc,
                                &m->machine_id);

    /* IRQ Glue */
    object_initialize_child(OBJECT(machine), "glue", &m->glue, TYPE_GLUE);
    object_property_set_link(OBJECT(&m->glue), "cpu", OBJECT(&m->cpu),
                             &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&m->glue), &error_fatal);

    /* VIA 1 */
    object_initialize_child(OBJECT(machine), "via1", &m->via1,
                            TYPE_MOS6522_Q800_VIA1);
    qdev_prop_set_uint8(DEVICE(&m->via1), "cpuid", Q700_VIA1_CPUID);
    /*
     * Port A input pin levels: PA0 high (low selects the ROM's burn-in
     * mode, POST then loops forever), PA6/PA7 high as on real hardware
     * (MAME's macquadra700 via_in_a returns 0xc1)
     */
    qdev_prop_set_uint16(DEVICE(&m->via1), "pins-a", 0xc1);
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive(DEVICE(&m->via1), "drive",
                            blk_by_legacy_dinfo(dinfo));
    }
    sysbus = SYS_BUS_DEVICE(&m->via1);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, VIA_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 1));
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), GLUE_IRQ_IN_VIA1));
    /* A/UX mode */
    qdev_connect_gpio_out(DEVICE(&m->via1), 0,
                          qdev_get_gpio_in_named(DEVICE(&m->glue),
                                                 "auxmode", 0));

    adb_bus = qdev_get_child_bus(DEVICE(&m->via1), "adb.0");
    dev = qdev_new(TYPE_ADB_KEYBOARD);
    qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    dev = qdev_new(TYPE_ADB_MOUSE);
    qdev_realize_and_unref(dev, adb_bus, &error_fatal);

    /* VIA 2 */
    object_initialize_child(OBJECT(machine), "via2", &m->via2,
                            TYPE_MOS6522_Q800_VIA2);
    sysbus = SYS_BUS_DEVICE(&m->via2);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, VIA_BASE - IO_BASE + VIA_SIZE,
                                sysbus_mmio_get_region(sysbus, 1));
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), GLUE_IRQ_IN_VIA2));

    /* MACSONIC */

    /*
     * MacSonic driver needs an Apple MAC address
     * Valid prefix are:
     * 00:05:02 Apple
     * 00:80:19 Dayna Communications, Inc.
     * 00:A0:40 Apple
     * 08:00:07 Apple
     * (Q800 use the last one)
     */
    object_initialize_child(OBJECT(machine), "dp8393x", &m->dp8393x,
                            TYPE_DP8393X);
    dev = DEVICE(&m->dp8393x);
    nd = qemu_find_nic_info(TYPE_DP8393X, true, "dp83932");
    if (nd) {
        qdev_set_nic_properties(dev, nd);
        memcpy(mac.a, nd->macaddr.a, sizeof(mac.a));
    } else {
        qemu_macaddr_default_if_unset(&mac);
    }
    mac.a[0] = 0x08;
    mac.a[1] = 0x00;
    mac.a[2] = 0x07;
    qdev_prop_set_macaddr(dev, "mac", mac.a);

    qdev_prop_set_uint8(dev, "it_shift", 2);
    qdev_prop_set_bit(dev, "big_endian", true);
    object_property_set_link(OBJECT(dev), "dma_mr",
                             OBJECT(get_system_memory()), &error_abort);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, SONIC_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));
    sysbus_connect_irq(sysbus, 0,
                       qdev_get_gpio_in(DEVICE(&m->glue), GLUE_IRQ_IN_SONIC));

    memory_region_init_rom(&m->dp8393x_prom, NULL, "dp8393x-q700.prom",
                           SONIC_PROM_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), SONIC_PROM_BASE,
                                &m->dp8393x_prom);

    /* Add MAC address with valid checksum to PROM */
    prom = memory_region_get_ram_ptr(&m->dp8393x_prom);
    checksum = 0;
    for (i = 0; i < 6; i++) {
        prom[i] = revbit8(mac.a[i]);
        checksum ^= prom[i];
    }
    prom[7] = 0xff - checksum;

    /* SCC */

    object_initialize_child(OBJECT(machine), "escc", &m->escc,
                            TYPE_ESCC);
    dev = DEVICE(&m->escc);
    qdev_prop_set_uint32(dev, "disabled", 0);
    qdev_prop_set_uint32(dev, "frequency", MAC_CLOCK);
    qdev_prop_set_uint32(dev, "it_shift", 1);
    qdev_prop_set_bit(dev, "bit_swap", true);
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));
    qdev_prop_set_uint32(dev, "chnBtype", 0);
    qdev_prop_set_uint32(dev, "chnAtype", 0);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize(sysbus, &error_fatal);

    /* Logically OR both its IRQs together */
    object_initialize_child(OBJECT(machine), "escc_orgate", &m->escc_orgate,
                            TYPE_OR_IRQ);
    object_property_set_int(OBJECT(&m->escc_orgate), "num-lines", 2,
                            &error_fatal);
    dev = DEVICE(&m->escc_orgate);
    qdev_realize(dev, NULL, &error_fatal);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(dev, 0));
    sysbus_connect_irq(sysbus, 1, qdev_get_gpio_in(dev, 1));
    qdev_connect_gpio_out(dev, 0,
                          qdev_get_gpio_in(DEVICE(&m->glue),
                                           GLUE_IRQ_IN_ESCC));
    memory_region_add_subregion(&m->macio, SCC_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));

    /* SCC IOP host registers below the SCC bypass window */
    memory_region_init_io(&m->iop_mem, OBJECT(machine), &q700_iop_ops, m,
                          "scc-iop", 0x20);
    memory_region_add_subregion(&m->macio, SCC_BASE - IO_BASE - 0x20,
                                &m->iop_mem);

    /* SCSI */

    object_initialize_child(OBJECT(machine), "esp", &m->esp,
                            TYPE_SYSBUS_ESP);
    sysbus_esp = SYSBUS_ESP(&m->esp);
    esp = &sysbus_esp->esp;
    esp->dma_memory_read = NULL;
    esp->dma_memory_write = NULL;
    esp->dma_opaque = NULL;
    sysbus_esp->it_shift = 4;
    esp->dma_enabled = 1;

    sysbus = SYS_BUS_DEVICE(&m->esp);
    sysbus_realize(sysbus, &error_fatal);
    /* SCSI IRQ is negative edge triggered */
    sysbus_connect_irq(sysbus, 0,
                       qemu_irq_invert(
                           qdev_get_gpio_in(DEVICE(&m->via2),
                                                   VIA2_IRQ_SCSI_BIT)));
    /*
     * SCSI DRQ: latched into the DAFB TurboSCSI handshake register
     * (MAC_SCSI_QUADRA2) and also forwarded to the VIA2 SCSI DATA input
     * as on the Quadra 800
     */
    m->esp_drq_via2 = qdev_get_gpio_in(DEVICE(&m->via2),
                                       VIA2_IRQ_SCSI_DATA_BIT);
    sysbus_connect_irq(sysbus, 1, qemu_allocate_irq(q700_esp_drq, m, 0));
    memory_region_add_subregion(&m->macio, ESP_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_add_subregion(&m->macio, ESP_PDMA - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 1));

    scsi_bus_legacy_handle_cmdline(&esp->bus);

    /* Apple Sound Chip */

    object_initialize_child(OBJECT(machine), "asc", &m->asc, TYPE_ASC);
    qdev_prop_set_uint8(DEVICE(&m->asc), "asctype", m->easc ? ASC_TYPE_EASC
                                                            : ASC_TYPE_ASC);
    if (machine->audiodev) {
        qdev_prop_set_string(DEVICE(&m->asc), "audiodev", machine->audiodev);
    }
    sysbus = SYS_BUS_DEVICE(&m->asc);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, ASC_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(DEVICE(&m->glue),
                                                   GLUE_IRQ_IN_ASC));

    /* Wire ASC IRQ via GLUE for use in classic mode */
    qdev_connect_gpio_out(DEVICE(&m->glue), GLUE_IRQ_ASC,
                          qdev_get_gpio_in(DEVICE(&m->via2),
                                           VIA2_IRQ_ASC_BIT));

    /* SWIM floppy controller */

    object_initialize_child(OBJECT(machine), "swim", &m->swim,
                            TYPE_SWIM);
    sysbus = SYS_BUS_DEVICE(&m->swim);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(&m->macio, SWIM_BASE - IO_BASE,
                                sysbus_mmio_get_region(sysbus, 0));

    /* NuBus */

    object_initialize_child(OBJECT(machine), "mac-nubus-bridge",
                            &m->mac_nubus_bridge,
                            TYPE_MAC_NUBUS_BRIDGE);
    sysbus = SYS_BUS_DEVICE(&m->mac_nubus_bridge);
    dev = DEVICE(&m->mac_nubus_bridge);
    qdev_prop_set_uint32(DEVICE(&m->mac_nubus_bridge), "slot-available-mask",
                         Q700_NUBUS_SLOTS_AVAILABLE);
    sysbus_realize(sysbus, &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                MAC_NUBUS_FIRST_SLOT * NUBUS_SUPER_SLOT_SIZE,
                                sysbus_mmio_get_region(sysbus, 0));
    memory_region_add_subregion(get_system_memory(),
                                NUBUS_SLOT_BASE +
                                MAC_NUBUS_FIRST_SLOT * NUBUS_SLOT_SIZE,
                                sysbus_mmio_get_region(sysbus, 1));
    qdev_connect_gpio_out(dev, 9,
                          qdev_get_gpio_in_named(DEVICE(&m->via2), "nubus-irq",
                          VIA2_NUBUS_IRQ_INTVIDEO));
    for (i = 1; i < VIA2_NUBUS_IRQ_NB; i++) {
        qdev_connect_gpio_out(dev, 9 + i,
                              qdev_get_gpio_in_named(DEVICE(&m->via2),
                                                     "nubus-irq",
                                                     VIA2_NUBUS_IRQ_9 + i));
    }

    /*
     * Since the framebuffer in slot 0x9 uses a separate IRQ, wire the unused
     * IRQ via GLUE for use by SONIC Ethernet in classic mode
     */
    qdev_connect_gpio_out(DEVICE(&m->glue), GLUE_IRQ_NUBUS_9,
                          qdev_get_gpio_in_named(DEVICE(&m->via2), "nubus-irq",
                                                 VIA2_NUBUS_IRQ_9));

    nubus = NUBUS_BUS(qdev_get_child_bus(dev, "nubus-bus.0"));

    /* framebuffer in nubus slot #9 */

    object_initialize_child(OBJECT(machine), "macfb", &m->macfb,
                            TYPE_NUBUS_MACFB);
    dev = DEVICE(&m->macfb);
    qdev_prop_set_uint32(dev, "slot", 9);
    /*
     * Default to the 21" 1152x870 display: the Q700 ROM and MacOS both
     * program the DAFB 1152x870 timings for it with the framebuffer
     * base taken from the VADDR register (VADDR1 * 0x200), which this
     * DAFB revision uses (the Quadra 800 mode table offsets don't
     * apply)
     */
    qdev_prop_set_bit(dev, "vaddr-base", true);
    qdev_prop_set_uint32(dev, "width", graphic_width ?: 1152);
    qdev_prop_set_uint32(dev, "height", graphic_height ?: 870);
    qdev_prop_set_uint8(dev, "depth", graphic_depth ?: 8);
    qdev_realize(dev, BUS(nubus), &error_fatal);

    macfb_mode = (NUBUS_MACFB(dev)->macfb).mode;

    /* TurboSCSI handshake register shadowing the DAFB register space */
    memory_region_init_io(&m->turboscsi_mem, OBJECT(machine),
                          &q700_turboscsi_ops, m, "turboscsi",
                          TURBOSCSI_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(), TURBOSCSI_BASE,
                                        &m->turboscsi_mem, 1);

    cs = CPU(&m->cpu);
    if (linux_boot) {
        uint64_t high;
        void *param_blob, *param_ptr, *param_rng_seed;

        if (kernel_cmdline) {
            param_blob = g_malloc(strlen(kernel_cmdline) + 1024);
        } else {
            param_blob = g_malloc(1024);
        }

        kernel_size = load_elf(kernel_filename, NULL, NULL, NULL,
                               &elf_entry, NULL, &high, NULL, ELFDATA2MSB,
                               EM_68K, 0, 0);
        if (kernel_size < 0) {
            error_report("could not load kernel '%s'", kernel_filename);
            exit(1);
        }
        stl_phys(cs->as, 4, elf_entry); /* reset initial PC */
        parameters_base = (high + 1) & ~1;
        param_ptr = param_blob;

        BOOTINFO1(param_ptr, BI_MACHTYPE, MACH_MAC);
        BOOTINFO1(param_ptr, BI_FPUTYPE, FPU_68040);
        BOOTINFO1(param_ptr, BI_MMUTYPE, MMU_68040);
        BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68040);
        BOOTINFO1(param_ptr, BI_MAC_CPUID, CPUB_68040);
        BOOTINFO1(param_ptr, BI_MAC_MODEL, MAC_MODEL_Q700);
        BOOTINFO1(param_ptr,
                  BI_MAC_MEMSIZE, ram_size >> 20); /* in MB */
        BOOTINFO2(param_ptr, BI_MEMCHUNK, 0, ram_size);
        BOOTINFO1(param_ptr, BI_MAC_VADDR,
                  VIDEO_BASE + macfb_mode->offset);
        BOOTINFO1(param_ptr, BI_MAC_VDEPTH, macfb_mode->depth);
        BOOTINFO1(param_ptr, BI_MAC_VDIM,
                  (macfb_mode->height << 16) | macfb_mode->width);
        BOOTINFO1(param_ptr, BI_MAC_VROW, macfb_mode->stride);
        BOOTINFO1(param_ptr, BI_MAC_SCCBASE, SCC_BASE);

        if (kernel_cmdline) {
            BOOTINFOSTR(param_ptr, BI_COMMAND_LINE,
                        kernel_cmdline);
        }

        /* Pass seed to RNG. */
        param_rng_seed = param_ptr;
        qemu_guest_getrandom_nofail(rng_seed, sizeof(rng_seed));
        BOOTINFODATA(param_ptr, BI_RNG_SEED,
                     rng_seed, sizeof(rng_seed));

        /* load initrd */
        if (initrd_filename) {
            initrd_size = get_image_size(initrd_filename, NULL);
            if (initrd_size < 0) {
                error_report("could not load initial ram disk '%s'",
                             initrd_filename);
                exit(1);
            }

            initrd_base = (ram_size - initrd_size) & TARGET_PAGE_MASK;
            load_image_targphys(initrd_filename, initrd_base,
                                ram_size - initrd_base, &error_fatal);
            BOOTINFO2(param_ptr, BI_RAMDISK, initrd_base,
                      initrd_size);
        } else {
            initrd_base = 0;
            initrd_size = 0;
        }
        BOOTINFO0(param_ptr, BI_LAST);
        rom_add_blob_fixed_as("bootinfo", param_blob, param_ptr - param_blob,
                              parameters_base, cs->as);
        qemu_register_reset_nosnapshotload(rerandomize_rng_seed,
                            rom_ptr_for_as(cs->as, parameters_base,
                                           param_ptr - param_blob) +
                            (param_rng_seed - param_blob));
        g_free(param_blob);
    } else {
        uint8_t *ptr;
        /* allocate and load BIOS */
        memory_region_init_rom(&m->rom, NULL, "m68k_mac.rom", MACROM_SIZE,
                               &error_abort);
        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
        memory_region_add_subregion(get_system_memory(), MACROM_ADDR, &m->rom);

        memory_region_init_alias(&m->rom_alias, NULL, "m68k_mac.rom-alias",
                                 &m->rom, 0, MACROM_SIZE);
        memory_region_add_subregion(get_system_memory(), 0x40000000,
                                    &m->rom_alias);

        /* Load MacROM binary */
        if (filename) {
            bios_size = load_image_targphys(filename, MACROM_ADDR, MACROM_SIZE,
                                            NULL);
            g_free(filename);
        } else {
            bios_size = -1;
        }

        /* Remove qtest_enabled() check once firmware files are in the tree */
        if (!qtest_enabled()) {
            if (bios_size <= 0 || bios_size > MACROM_SIZE) {
                error_report("could not load MacROM '%s'", bios_name);
                exit(1);
            }

            ptr = rom_ptr(MACROM_ADDR, bios_size);
            assert(ptr != NULL);
            stl_phys(cs->as, 0, ldl_be_p(ptr));    /* reset initial SP */
            stl_phys(cs->as, 4,
                     MACROM_ADDR + ldl_be_p(ptr + 4)); /* reset initial PC */

            /*
             * Make POST failures non-fatal: the ROM's timing tests
             * measure VIA-timer periods against CPU busy loops (e.g.
             * test 0x87 requires 10 VIA1 SR interrupts and 128..207 T1
             * interrupts within a 983040-iteration wait), ratios that
             * TCG's fast CPU cannot honour against wall-clock timers.
             * The dispatcher has a single failure branch at ROM offset
             * 0x46f7c "tstl %d6; beq next" - turn the beq into a bra
             * so a failed test is skipped like a passed one (the ROM
             * already ignores failures on warm boots).
             */
            if (bios_size >= 0x46f80 && ldl_be_p(ptr + 0x46f7c) == 0x4a86671a) {
                uint8_t *p = rom_ptr(MACROM_ADDR + 0x46f7e, 1);
                if (p) {
                    *p = 0x60;      /* beq.s -> bra.s */
                    /*
                     * Keep the ROM checksum test (POST test 1) happy:
                     * the header long is the 32-bit sum of all 16-bit
                     * words after it, and the word at +0x46f7e just
                     * dropped from 0x671a to 0x601a
                     */
                    stl_be_p(ptr, ldl_be_p(ptr) - 0x700);
                }
            }
        }
    }
}

static bool q700_get_easc(Object *obj, Error **errp)
{
    Q700MachineState *ms = Q700_MACHINE(obj);

    return ms->easc;
}

static void q700_set_easc(Object *obj, bool value, Error **errp)
{
    Q700MachineState *ms = Q700_MACHINE(obj);

    ms->easc = value;
}

static void q700_init(Object *obj)
{
    Q700MachineState *ms = Q700_MACHINE(obj);

    /* Default to EASC */
    ms->easc = true;
}

static GlobalProperty hw_compat_q700[] = {
    { "scsi-hd", "quirk_mode_page_vendor_specific_apple", "on" },
    { "scsi-hd", "vendor", " SEAGATE" },
    { "scsi-hd", "product", "          ST225N" },
    { "scsi-hd", "ver", "1.0 " },
    { "scsi-cd", "quirk_mode_page_apple_vendor", "on" },
    { "scsi-cd", "quirk_mode_sense_rom_use_dbd", "on" },
    { "scsi-cd", "quirk_mode_page_vendor_specific_apple", "on" },
    { "scsi-cd", "quirk_mode_page_truncated", "on" },
    { "scsi-cd", "vendor", "MATSHITA" },
    { "scsi-cd", "product", "CD-ROM CR-8005" },
    { "scsi-cd", "ver", "1.0k" },
};
static const size_t hw_compat_q700_len = G_N_ELEMENTS(hw_compat_q700);

static void q700_machine_class_init(ObjectClass *oc, const void *data)
{
    static const char * const valid_cpu_types[] = {
        M68K_CPU_TYPE_NAME("m68040"),
        NULL
    };
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Macintosh Quadra 700";
    mc->init = q700_machine_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68040");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
    mc->block_default_type = IF_SCSI;
    /*
     * Without media, the auto-created scsi-cd at ID 2 makes MacOS loop
     * a "Disk initialization failed because the disk is locked!" alert
     */
    mc->no_cdrom = true;
    mc->default_ram_id = "m68k_mac.ram";
    machine_add_audiodev_property(mc);
    compat_props_add(mc->compat_props, hw_compat_q700, hw_compat_q700_len);

    object_class_property_add_bool(oc, "easc", q700_get_easc, q700_set_easc);
    object_class_property_set_description(oc, "easc",
        "Set to off to use ASC rather than EASC");
}

static const TypeInfo q700_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("quadra700"),
    .parent     = TYPE_MACHINE,
    .instance_init = q700_init,
    .instance_size = sizeof(Q700MachineState),
    .class_init = q700_machine_class_init,
};

static void q700_machine_register_types(void)
{
    type_register_static(&q700_machine_typeinfo);
}

type_init(q700_machine_register_types)
