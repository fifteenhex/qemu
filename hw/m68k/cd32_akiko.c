/*
 * Akiko, the CD32 system chip.
 *
 * Akiko sits at 0xb80000 and bundles the CD32's console glue: the CD
 * controller's DMA and interrupt registers, the chunky-to-planar
 * conversion port, and the I2C lines of the battery-backed NVRAM (a
 * 24C08-style 1KB EEPROM).  Register map, as established by the CD32
 * developer documentation and hardware reverse engineering:
 *
 *   0x00.l  identification, reads 0xc0cacafe (Kickstart checks the
 *           0xcafe word at +2 before it uses the C2P port)
 *   0x04.l  interrupt request (subcode, drive tx/rx, DMA done, PBX,
 *           overflow bits in the top byte)
 *   0x08.l  interrupt enable
 *   0x10.l  CD sector-data DMA base address
 *   0x14.l  CD misc DMA base (the subcode area and the command tx/rx
 *           circular buffers live in this 1KB-aligned block)
 *   0x18.b  subcode buffer offset
 *   0x19.b  command tx buffer index      0x1a.b  command rx index
 *   0x1d.b  command rx compare           0x1f.b  command tx compare
 *   0x20.w  PBX: pending-sector bits for the sector DMA ring
 *   0x24.l  CD configuration/enable flags (DMA enables in the top
 *           byte)
 *   0x30.b  NVRAM I2C lines: bit 7 SCL, bit 6 SDA (reads return the
 *           line state)
 *   0x32.b  NVRAM I2C direction: bit 7/6 drive-enable for SCL/SDA;
 *           lines not driven read pulled up (or as the EEPROM drives
 *           SDA)
 *   0x38.l  chunky-to-planar port: eight longword writes fill the
 *           FIFO with 32 8-bit chunky pixels, eight reads return the
 *           corresponding 32-pixel span of each of the 8 bitplanes
 *
 * The CD drive itself is not modelled yet: the CD registers store and
 * read back (so cd.device's probe finds the chip) but no command is
 * executed and no interrupt is raised, which a CD32 reads as "no disc
 * in the drive".  The C2P port and the NVRAM are functional.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/nvram/eeprom_at24c.h"
#include "migration/vmstate.h"
#include "hw/m68k/cd32_akiko.h"

#define REG_ID          0x00
#define REG_INTREQ      0x04
#define REG_INTENA      0x08
#define REG_CDDMADATA   0x10
#define REG_CDDMAMISC   0x14
#define REG_SUBCODEOFF  0x18
#define REG_CDTXINX     0x19
#define REG_CDRXINX     0x1a
#define REG_CDRXCMP     0x1d
#define REG_CDTXCMP     0x1f
#define REG_PBX         0x20    /* word */
#define REG_CDFLAGS     0x24
#define REG_NVRAM_IO    0x30
#define REG_NVRAM_DIR   0x32
#define REG_C2P         0x38    /* ..0x3b, longword port */

#define AKIKO_ID        0xc0cacafe

#define NVRAM_SCL       0x80
#define NVRAM_SDA       0x40

/* the 24C08-style NVRAM: 1KB as four 256-byte banks on 0x50..0x53 */
#define NVRAM_BANKS     4
#define NVRAM_BANK_SIZE 256
#define NVRAM_I2C_ADDR  0x50

static void akiko_c2p_write(CD32AkikoState *s, uint32_t val)
{
    s->c2p_buf[s->c2p_wpos++ & 7] = val;
    /* writing invalidates any half-read result set */
    s->c2p_valid = false;
}

static uint32_t akiko_c2p_read(CD32AkikoState *s)
{
    if (!s->c2p_valid) {
        int i;

        /*
         * Convert the FIFO: 256 input bits, one output bit each.  Bit
         * (i & 31) of input longword 7 - (i >> 5) is pixel i / 8's
         * plane-(i & 7) bit, and lands at bit i >> 3 of output
         * longword i & 7 (so reads return bitplane 0's 32 pixels
         * first, each output long one full bitplane span).
         */
        memset(s->c2p_result, 0, sizeof(s->c2p_result));
        for (i = 0; i < 256; i++) {
            if (s->c2p_buf[7 - (i >> 5)] & (1u << (i & 31))) {
                s->c2p_result[i & 7] |= 1u << (i >> 3);
            }
        }
        s->c2p_valid = true;
        s->c2p_rpos = 0;
        s->c2p_wpos = 0;
    }
    return s->c2p_result[s->c2p_rpos++ & 7];
}

/*
 * Drive the NVRAM I2C lines from the IO/direction register pair.  The
 * bus lines are open collector: a line whose direction bit says
 * "input" floats high (SDA additionally as the EEPROM drives it).
 */
static void akiko_nvram_update(CD32AkikoState *s)
{
    int scl = (s->nvram_dir & NVRAM_SCL) ? !!(s->nvram_io & NVRAM_SCL) : 1;
    int sda = (s->nvram_dir & NVRAM_SDA) ? !!(s->nvram_io & NVRAM_SDA) : 1;

    bitbang_i2c_set(&s->bitbang, BITBANG_I2C_SDA, sda);
    s->nvram_sda_in = bitbang_i2c_set(&s->bitbang, BITBANG_I2C_SCL, scl);
    s->nvram_scl_in = scl;
}

static uint8_t akiko_reg_read(CD32AkikoState *s, unsigned addr)
{
    switch (addr) {
    case REG_ID ... REG_ID + 3:
        return AKIKO_ID >> (8 * (3 - addr));
    case REG_INTREQ ... REG_INTREQ + 3:
        return s->intreq >> (8 * (3 - (addr - REG_INTREQ)));
    case REG_INTENA ... REG_INTENA + 3:
        return s->intena >> (8 * (3 - (addr - REG_INTENA)));
    case REG_CDDMADATA ... REG_CDDMADATA + 3:
        return s->cd_dma_data >> (8 * (3 - (addr - REG_CDDMADATA)));
    case REG_CDDMAMISC ... REG_CDDMAMISC + 3:
        return s->cd_dma_misc >> (8 * (3 - (addr - REG_CDDMAMISC)));
    case REG_SUBCODEOFF:
        return s->cd_subcode_off;
    case REG_CDTXINX:
        return s->cd_tx_inx;
    case REG_CDRXINX:
        return s->cd_rx_inx;
    case REG_CDRXCMP:
        return s->cd_rx_cmp;
    case REG_CDTXCMP:
        return s->cd_tx_cmp;
    case REG_PBX:
    case REG_PBX + 1:
        return s->cd_pbx >> (8 * (1 - (addr - REG_PBX)));
    case REG_CDFLAGS ... REG_CDFLAGS + 3:
        return s->cd_flags >> (8 * (3 - (addr - REG_CDFLAGS)));
    case REG_NVRAM_IO:
        return (s->nvram_scl_in ? NVRAM_SCL : 0) |
               (s->nvram_sda_in ? NVRAM_SDA : 0);
    case REG_NVRAM_DIR:
        return s->nvram_dir;
    default:
        qemu_log_mask(LOG_UNIMP, "akiko: unimplemented read 0x%02x\n", addr);
        return 0;
    }
}

static void akiko_reg_write(CD32AkikoState *s, unsigned addr, uint8_t val)
{
    unsigned shift;

    switch (addr) {
    case REG_INTENA ... REG_INTENA + 3:
        shift = 8 * (3 - (addr - REG_INTENA));
        s->intena = (s->intena & ~(0xff << shift)) | (val << shift);
        break;
    case REG_CDDMADATA ... REG_CDDMADATA + 3:
        shift = 8 * (3 - (addr - REG_CDDMADATA));
        s->cd_dma_data = (s->cd_dma_data & ~(0xff << shift)) | (val << shift);
        break;
    case REG_CDDMAMISC ... REG_CDDMAMISC + 3:
        shift = 8 * (3 - (addr - REG_CDDMAMISC));
        s->cd_dma_misc = (s->cd_dma_misc & ~(0xff << shift)) | (val << shift);
        break;
    case REG_SUBCODEOFF:
        s->cd_subcode_off = val;
        break;
    case REG_CDTXINX:
        s->cd_tx_inx = val;
        break;
    case REG_CDRXINX:
        s->cd_rx_inx = val;
        break;
    case REG_CDRXCMP:
        s->cd_rx_cmp = val;
        break;
    case REG_CDTXCMP:
        /*
         * On real Akiko this arms the command transmit: bytes between
         * the tx index and the compare value DMA to the drive.  With
         * no drive modelled the command is ignored and no completion
         * interrupt ever arrives, which cd.device treats as a dead or
         * empty drive.
         */
        s->cd_tx_cmp = val;
        break;
    case REG_PBX:
    case REG_PBX + 1:
        shift = 8 * (1 - (addr - REG_PBX));
        s->cd_pbx = (s->cd_pbx & ~(0xff << shift)) | (val << shift);
        break;
    case REG_CDFLAGS ... REG_CDFLAGS + 3:
        shift = 8 * (3 - (addr - REG_CDFLAGS));
        s->cd_flags = (s->cd_flags & ~(0xff << shift)) | (val << shift);
        break;
    case REG_NVRAM_IO:
        s->nvram_io = val;
        akiko_nvram_update(s);
        break;
    case REG_NVRAM_DIR:
        s->nvram_dir = val;
        akiko_nvram_update(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "akiko: unimplemented write 0x%02x = 0x%02x\n",
                      addr, val);
        break;
    }
}

static uint64_t akiko_read(void *opaque, hwaddr addr, unsigned size)
{
    CD32AkikoState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    if (size == 4 && addr >= REG_C2P && addr < REG_C2P + 4) {
        return akiko_c2p_read(s);
    }
    for (i = 0; i < size; i++) {
        val = (val << 8) | akiko_reg_read(s, addr + i);
    }
    return val;
}

static void akiko_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    CD32AkikoState *s = opaque;
    unsigned i;

    if (addr >= REG_C2P && addr < REG_C2P + 4) {
        if (size == 4) {
            akiko_c2p_write(s, val);
        } else {
            qemu_log_mask(LOG_UNIMP,
                          "akiko: non-longword C2P write (size %u)\n", size);
        }
        return;
    }
    for (i = 0; i < size; i++) {
        akiko_reg_write(s, addr + i, val >> (8 * (size - 1 - i)));
    }
}

static const MemoryRegionOps akiko_ops = {
    .read = akiko_read,
    .write = akiko_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void akiko_reset(DeviceState *dev)
{
    CD32AkikoState *s = CD32_AKIKO(dev);

    s->intreq = 0;
    s->intena = 0;
    s->cd_dma_data = 0;
    s->cd_dma_misc = 0;
    s->cd_subcode_off = 0;
    s->cd_tx_inx = 0;
    s->cd_rx_inx = 0;
    s->cd_rx_cmp = 0;
    s->cd_tx_cmp = 0;
    s->cd_pbx = 0;
    s->cd_flags = 0;
    s->nvram_io = 0;
    s->nvram_dir = 0;
    s->c2p_wpos = 0;
    s->c2p_rpos = 0;
    s->c2p_valid = false;
    akiko_nvram_update(s);
    qemu_set_irq(s->irq, 0);
}

static void akiko_realize(DeviceState *dev, Error **errp)
{
    CD32AkikoState *s = CD32_AKIKO(dev);
    int i;

    s->i2c_bus = i2c_init_bus(dev, "akiko-nvram-i2c");
    bitbang_i2c_init(&s->bitbang, s->i2c_bus);
    /*
     * The 24C08's four 256-byte banks select on the low two device
     * address bits, so four separate 256-byte EEPROMs on 0x50..0x53
     * decode identically.
     */
    for (i = 0; i < NVRAM_BANKS; i++) {
        at24c_eeprom_init(s->i2c_bus, NVRAM_I2C_ADDR + i, NVRAM_BANK_SIZE);
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &akiko_ops, s,
                          "akiko", CD32_AKIKO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void akiko_initfn(Object *obj)
{
    CD32AkikoState *s = CD32_AKIKO(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_akiko = {
    .name = "cd32-akiko",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(intreq, CD32AkikoState),
        VMSTATE_UINT32(intena, CD32AkikoState),
        VMSTATE_UINT32(cd_dma_data, CD32AkikoState),
        VMSTATE_UINT32(cd_dma_misc, CD32AkikoState),
        VMSTATE_UINT8(cd_subcode_off, CD32AkikoState),
        VMSTATE_UINT8(cd_tx_inx, CD32AkikoState),
        VMSTATE_UINT8(cd_rx_inx, CD32AkikoState),
        VMSTATE_UINT8(cd_rx_cmp, CD32AkikoState),
        VMSTATE_UINT8(cd_tx_cmp, CD32AkikoState),
        VMSTATE_UINT16(cd_pbx, CD32AkikoState),
        VMSTATE_UINT32(cd_flags, CD32AkikoState),
        VMSTATE_UINT8(nvram_io, CD32AkikoState),
        VMSTATE_UINT8(nvram_dir, CD32AkikoState),
        VMSTATE_BOOL(nvram_scl_in, CD32AkikoState),
        VMSTATE_BOOL(nvram_sda_in, CD32AkikoState),
        VMSTATE_UINT32_ARRAY(c2p_buf, CD32AkikoState, 8),
        VMSTATE_UINT32_ARRAY(c2p_result, CD32AkikoState, 8),
        VMSTATE_UINT8(c2p_wpos, CD32AkikoState),
        VMSTATE_UINT8(c2p_rpos, CD32AkikoState),
        VMSTATE_BOOL(c2p_valid, CD32AkikoState),
        VMSTATE_END_OF_LIST()
    }
};

static void akiko_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = akiko_realize;
    device_class_set_legacy_reset(dc, akiko_reset);
    dc->vmsd = &vmstate_akiko;
}

static const TypeInfo cd32_akiko_types[] = {
    {
        .name = TYPE_CD32_AKIKO,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(CD32AkikoState),
        .instance_init = akiko_initfn,
        .class_init = akiko_class_init,
    },
};

DEFINE_TYPES(cd32_akiko_types)
