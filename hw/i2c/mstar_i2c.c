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
#include "hw/arm/mstar.h"

/* ---------------------------------------------------------------- i2c */

/*
 * HWI2C master (i2c@223000/223200). 16-bit registers at the 4-byte RIU stride;
 * register index N (vendor "HWI2C_REG_BASE + N") is at byte offset N*4. Layout
 * from the vendor HAL and the mainline i2c-msc313e.c, which agree here:
 *
 *   0x04  CMD       bit0 = generate START, bit8 = generate STOP
 *   0x08  WDATA     write: byte to transmit; read: bit8 set => slave NAKed
 *   0x0c  RDATA     read: last received byte; write bit8 = trigger a read
 *   0x10  INT_CTL   read: bit0 = a byte/command completed; write: clear it
 *   0x14  INT_STAT  bits 8-13 = START/STOP/RX/TX-done etc. (mainline path)
 *
 * A transfer is: write CMD START, WDATA the address byte (check the ack), WDATA
 * data (or trigger RDATA), CMD STOP. The driver polls INT_CTL after each step.
 * With no slave attached every address NAKs, which is what a real bus with the
 * device absent does - the point is that transfers *complete* (INT flag set) so
 * the driver stops polling forever and reports "no device" instead of a
 * timeout. Attach an i2c slave to s->bus to actually respond.
 */
#define I2C_CMD             0x04
#define I2C_CMD_START       0x0001
#define I2C_CMD_STOP        0x0100
#define I2C_WDATA           0x08
#define I2C_WDATA_NAK       0x0100
#define I2C_RDATA           0x0c
#define I2C_RDATA_TRIG      0x0100
#define I2C_INT_CTL         0x10
#define I2C_INT_STAT        0x14
#define I2C_INT_STARTDET    (1 << 8)
#define I2C_INT_STOPDET     (1 << 9)
#define I2C_INT_RXDONE      (1 << 10)
#define I2C_INT_TXDONE      (1 << 11)

static uint64_t msc313_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    Msc313I2cState *s = opaque;

    switch (addr) {
    case I2C_INT_CTL:
        return s->int_pending ? 1 : 0;
    case I2C_WDATA:
        return s->nak ? I2C_WDATA_NAK : 0;
    case I2C_RDATA:
        return s->rdata;
    default:
        return s->regs[addr / 4];
    }
}

static void msc313_i2c_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Msc313I2cState *s = opaque;

    s->regs[addr / 4] = val;

    switch (addr) {
    case I2C_CMD:
        if (val & I2C_CMD_START) {
            if (s->active) {
                i2c_end_transfer(s->bus);   /* repeated start */
                s->active = false;
            }
            s->start_pending = true;
            s->int_pending = true;
            s->regs[I2C_INT_STAT / 4] |= I2C_INT_STARTDET;
        }
        if (val & I2C_CMD_STOP) {
            if (s->active) {
                i2c_end_transfer(s->bus);
                s->active = false;
            }
            s->int_pending = true;
            s->regs[I2C_INT_STAT / 4] |= I2C_INT_STOPDET;
        }
        break;
    case I2C_WDATA:
        if (s->start_pending) {
            /* First byte after START is the 7-bit address + R/W bit. */
            s->nak = i2c_start_transfer(s->bus, (val >> 1) & 0x7f,
                                        val & 1) != 0;
            s->active = !s->nak;
            s->start_pending = false;
        } else if (s->active) {
            s->nak = i2c_send(s->bus, val & 0xff) != 0;
        } else {
            s->nak = true;
        }
        s->int_pending = true;
        s->regs[I2C_INT_STAT / 4] |= I2C_INT_TXDONE;
        break;
    case I2C_RDATA:
        if (val & I2C_RDATA_TRIG) {
            s->rdata = s->active ? i2c_recv(s->bus) : 0xff;
            s->int_pending = true;
            s->regs[I2C_INT_STAT / 4] |= I2C_INT_RXDONE;
        }
        break;
    case I2C_INT_CTL:
        s->int_pending = false;             /* write to clear */
        break;
    case I2C_INT_STAT:
        s->regs[I2C_INT_STAT / 4] &= ~(uint16_t)val;    /* write-1-to-clear */
        break;
    }
}

static const MemoryRegionOps msc313_i2c_ops = {
    .read = msc313_i2c_read,
    .write = msc313_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
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
}

static void msc313_i2c_realize(DeviceState *dev, Error **errp)
{
    Msc313I2cState *s = MSC313_I2C(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &msc313_i2c_ops, s,
                          "mstar.i2c", MSTAR_I2C_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    s->bus = i2c_init_bus(dev, "i2c");
}

static void msc313_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = msc313_i2c_realize;
    rc->phases.hold = msc313_i2c_reset_hold;
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
