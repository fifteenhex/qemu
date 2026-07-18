/*
 * MStar/SigmaStar HWI2C master controller
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/loader.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"
#include "trace.h"

/* ---------------------------------------------------------------- i2c */

/*
 * HWI2C master (i2c@223000/223200). The register file is 16-bit registers at
 * the 4-byte RIU stride, but the vendor HAL (drivers/sstar/i2c) reads them as
 * 32-bit words and writes them a *byte* at a time - and it addresses the two
 * byte lanes of a word separately, so several commands live on odd offsets:
 *
 *   0x04  CMD_START  write byte, bit0 = generate START
 *   0x05  CMD_STOP   write byte, bit0 = generate STOP   (high lane of word 0x04)
 *   0x08  WDATA      write byte = data to transmit;
 *                    read: bit8 (high-lane bit0, "WDATA_GET") = 1 => slave NAKed
 *   0x0c  RDATA      read: last received byte
 *   0x0d  RDATA_CFG  write byte, bit0 = trigger a read, bit1 = send ACK after
 *   0x10  INT_CTL    read: bit0 = a byte/command completed; write bit0 = clear
 *   0x14  CUR_STATE  read: state machine (0 = idle)
 *
 * Because the writes are single bytes, this region must accept 1-byte accesses
 * (an earlier min_access_size of 2 silently dropped every CMD/WDATA write, so
 * the driver's START never took effect and it busy-polled INT_CTL forever).
 *
 * A transfer is: write CMD START, WDATA the address byte (then read WDATA_GET
 * for the ack), WDATA data or trigger RDATA, CMD STOP. The driver polls INT_CTL
 * after each step. With no slave attached the address NAKs, exactly as a real
 * bus with the device absent does; the vendor driver then logs the target
 * address ("Slave dev NAK, Addr: 0x..") instead of hanging. Attach an i2c
 * slave to s->bus to actually respond.
 */
#define I2C_CMD_START       0x04    /* byte: bit0 = START */
#define I2C_CMD_STOP        0x05    /* byte: bit0 = STOP  */
#define I2C_CMD_BIT         0x01
#define I2C_STOP_HIBIT      0x0100  /* STOP as bit8 of a 32-bit write to 0x04 */
#define I2C_WDATA           0x08    /* write: byte to send; read: bit8 = NAK */
#define I2C_WDATA_NAK       0x0100
#define I2C_RDATA           0x0c    /* read: received byte */
#define I2C_RDATA_CFG       0x0d    /* byte: bit0 = trigger read, bit1 = ACK */
#define I2C_RDATA_CFG_TRIG  0x01
#define I2C_INT_CTL         0x10    /* read bit0 = int pending; write = clear */
#define I2C_CUR_STATE       0x14    /* read: 0 = idle */

/*
 * DMA mode (drivers/sstar/i2c/infinity mhal_iic.c). The vendor camera kernel
 * does NOT bit-bang START/WDATA - it programs a descriptor and lets the MIIC
 * DMA engine run the whole transfer, streaming read data to/from a DRAM buffer.
 * Register byte offsets below are the HAL's logical register * 4 (16-bit RIU
 * registers at the 4-byte stride); 32-bit fields (MIU_ADR, DATLEN) span two.
 * Two CMDDAT bytes pack into each 16-bit register (low/high lane).
 */
#define I2C_DMA_CFG         0x80    /* bit1 EN_DMA, bit1(within) RESET, ... */
#define I2C_DMA_MIU_ADR     0x84    /* +0x84 low16, +0x88 high16 (MIU offset) */
#define I2C_DMA_MIU_ADR_HI  0x88
#define I2C_DMA_CTL         0x8c    /* bit5 TXNOSTOP, bit6 RDWTCMD(1=read) */
#define I2C_DMA_CTL_RDWTCMD 0x40
#define I2C_DMA_TXR         0x90    /* bit0 DONE (W1C to clear, HW sets on done) */
#define DMA_TXR_DONE        0x01
#define I2C_DMA_CMDDAT0     0x94    /* CMDDAT0..7: two bytes per 16-bit reg */
#define I2C_DMA_CMDLEN      0xa4    /* number of command bytes (& 7) */
#define I2C_DMA_DATLEN      0xa8    /* +0xa8 low16, +0xac high16 */
#define I2C_DMA_SLVADR      0xb8    /* bit0..6 = 7-bit slave address */
#define I2C_DMA_CTL_TRIG    0xbc    /* bit0 = trigger the descriptor */
#define I2C_DMA_TRIG_BIT    0x01

/*
 * Run one DMA descriptor against the attached slave. For a read the received
 * bytes are written to the guest DRAM buffer the driver programmed in MIU_ADR
 * (a MIU offset; DRAM is at MSTAR_DRAM_BASE); for a write the payload comes from
 * that same buffer. The command bytes (register address) always go out first.
 */
static void msc313_i2c_dma_run(Msc313I2cState *s)
{
    uint8_t slave = s->regs[I2C_DMA_SLVADR / 4] & 0x7f;
    bool read = s->regs[I2C_DMA_CTL / 4] & I2C_DMA_CTL_RDWTCMD;
    unsigned cmdlen = s->regs[I2C_DMA_CMDLEN / 4] & 0x7;
    uint32_t datlen = (uint32_t)s->regs[I2C_DMA_DATLEN / 4] |
                      ((uint32_t)s->regs[(I2C_DMA_DATLEN + 4) / 4] << 16);
    hwaddr miu = ((uint32_t)s->regs[I2C_DMA_MIU_ADR / 4] |
                  ((uint32_t)s->regs[I2C_DMA_MIU_ADR_HI / 4] << 16)) +
                 MSTAR_DRAM_BASE;
    uint8_t cmd[8];
    unsigned i;

    for (i = 0; i < cmdlen && i < sizeof(cmd); i++) {
        uint16_t word = s->regs[(I2C_DMA_CMDDAT0 + (i / 2) * 4) / 4];
        cmd[i] = (i & 1) ? (word >> 8) : (word & 0xff);
    }

    s->dma_done = true;                 /* the engine always reports completion */

    /* Address + command phase (write). NAK => no slave: leave the buffer be. */
    if (i2c_start_transfer(s->bus, slave, 0)) {
        i2c_end_transfer(s->bus);
        return;
    }
    for (i = 0; i < cmdlen; i++) {
        i2c_send(s->bus, cmd[i]);
    }

    if (read) {
        i2c_start_transfer(s->bus, slave, 1);   /* repeated START, read */
        for (i = 0; i < datlen; i++) {
            uint8_t b = i2c_recv(s->bus);
            address_space_write(&address_space_memory, miu + i,
                                MEMTXATTRS_UNSPECIFIED, &b, 1);
        }
    } else {
        for (i = 0; i < datlen; i++) {
            uint8_t b = 0;
            address_space_read(&address_space_memory, miu + i,
                               MEMTXATTRS_UNSPECIFIED, &b, 1);
            i2c_send(s->bus, b);
        }
    }
    i2c_end_transfer(s->bus);
}

static uint64_t msc313_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313I2cState *s = opaque;

    switch (addr) {
    case I2C_DMA_TXR:
        return s->dma_done ? DMA_TXR_DONE : 0;
    case I2C_INT_CTL:
        return s->int_pending ? 1 : 0;
    case I2C_WDATA:
        /* "WDATA_GET" ack lives in the high byte: bit8 set => slave NAKed. */
        return s->nak ? I2C_WDATA_NAK : 0;
    case I2C_WDATA + 1:
        /* The mercury5 RTOS HAL reads the ack flag as a byte (strb/ldrb HAL);
         * serve the NAK bit for a byte read of the high half too. */
        return s->nak ? 1 : 0;
    case I2C_RDATA:
        return s->rdata;
    case I2C_CUR_STATE:
        return 0;                           /* idle */
    default:
        return s->regs[addr / 4];
    }
}

static void msc313_i2c_start(Msc313I2cState *s)
{
    if (s->active) {
        i2c_end_transfer(s->bus);           /* repeated start */
        s->active = false;
    }
    s->start_pending = true;
    s->int_pending = true;
}

static void msc313_i2c_stop(Msc313I2cState *s)
{
    if (s->active) {
        i2c_end_transfer(s->bus);
        s->active = false;
    }
    s->start_pending = false;
    s->int_pending = true;
}

static void msc313_i2c_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Msc313I2cState *s = opaque;

    s->regs[addr / 4] = val;
    if (addr >= 0x20) {
        switch (addr) {
        case I2C_DMA_TXR:
            if (val & DMA_TXR_DONE) {
                s->dma_done = false;    /* W1C: driver clears before triggering */
            }
            break;
        case I2C_DMA_CTL_TRIG:
            if (val & I2C_DMA_TRIG_BIT) {
                msc313_i2c_dma_run(s);
            }
            break;
        }
        return;                             /* clock/DMA config: store only */
    }

    switch (addr) {
    case I2C_CMD_START:
        if (val & I2C_CMD_BIT) {
            msc313_i2c_start(s);
        }
        if (val & I2C_STOP_HIBIT) {         /* combined 32-bit write */
            msc313_i2c_stop(s);
        }
        break;
    case I2C_CMD_STOP:
        if (val & I2C_CMD_BIT) {
            msc313_i2c_stop(s);
        }
        break;
    case I2C_WDATA:
        if (s->start_pending) {
            /* First byte after START is the 7-bit address + R/W bit. */
            s->nak = i2c_start_transfer(s->bus, (val >> 1) & 0x7f,
                                        val & 1) != 0;
            s->active = !s->nak;
            s->start_pending = false;
            trace_msc313_i2c_addr((val >> 1) & 0x7f, val & 1, s->nak);
        } else if (s->active) {
            s->nak = i2c_send(s->bus, val & 0xff) != 0;
        } else {
            s->nak = true;
        }
        s->int_pending = true;
        break;
    case I2C_RDATA_CFG:
        if (val & I2C_RDATA_CFG_TRIG) {
            s->rdata = s->active ? i2c_recv(s->bus) : 0xff;
            s->int_pending = true;
        }
        break;
    case I2C_INT_CTL:
        s->int_pending = false;             /* write to clear */
        break;
    }
}

static const MemoryRegionOps msc313_i2c_ops = {
    .read = msc313_i2c_read,
    .write = msc313_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,     /* vendor HAL writes registers a byte at a time */
    .valid.max_access_size = 4,
};

static void msc313_i2c_reset_hold(Object *obj, ResetType type)
{
    Msc313I2cState *s = MSC313_I2C(obj);

    if (s->active) {
        i2c_end_transfer(s->bus);
    }
    memset(s->regs, 0, sizeof(s->regs));
    s->int_pending = false;
    s->nak = false;
    s->active = false;
    s->start_pending = false;
    s->rdata = 0xff;
    s->dma_done = false;
}

static void msc313_i2c_realize(DeviceState *dev, Error **errp)
{
    Msc313I2cState *s = MSC313_I2C(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313_i2c_ops, s,
                          "mstar.i2c", MSTAR_I2C_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    s->bus = i2c_init_bus(dev, "i2c");
}

static const VMStateDescription vmstate_mstar_msc313_i2c = {
    .name = "mstar-msc313-i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, Msc313I2cState, MSTAR_I2C_NUM_REGS),
        VMSTATE_BOOL(int_pending, Msc313I2cState),
        VMSTATE_BOOL(nak, Msc313I2cState),
        VMSTATE_BOOL(active, Msc313I2cState),
        VMSTATE_BOOL(start_pending, Msc313I2cState),
        VMSTATE_UINT8(rdata, Msc313I2cState),
        VMSTATE_BOOL(dma_done, Msc313I2cState),
        VMSTATE_END_OF_LIST()
    },
};

static void msc313_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313_i2c_realize;
    rc->phases.hold = msc313_i2c_reset_hold;
    dc->vmsd = &vmstate_mstar_msc313_i2c;
}

static const TypeInfo mstar_i2c_types[] = {
    {
        .name           = TYPE_MSC313_I2C,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(Msc313I2cState),
        .class_init     = msc313_i2c_class_init,
    },
};

DEFINE_TYPES(mstar_i2c_types)
