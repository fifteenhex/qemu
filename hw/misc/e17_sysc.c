/*
 * ELTEC Eurocom E17 onboard I/O ("system controller" region)
 *
 * One device covering the 0xfec00000 window of the Eurocom 17: the
 * chip select unit, two Z8536 CIOs (POST display, DIP switches,
 * console select), the RTC/NVRAM, the AT keyboard controller and
 * stubs for the not-yet-modelled peripherals.  All knowledge here
 * comes from reverse engineering the RMON 3.1.3 ROM; see E17-NOTES.md
 * for the evidence.  Register behaviour that RMON does not exercise
 * is unknown and modelled as plain storage.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/e17_sysc.h"
#include "migration/vmstate.h"
#include "hw/input/ps2.h"
#include "trace.h"

/* Z8536 register numbers used by RMON */
#define Z8536_MICR          0x00
#define Z8536_MICR_RESET    0x01
#define Z8536_MCCR          0x01
#define Z8536_PCDDR         0x06
#define Z8536_PADDR         0x23
#define Z8536_PBDDR         0x2b

/*
 * AT keyboard interface status bits, from the RMON driver
 * (fe81ac7c reset probe, fe81ad1c LED update, fe81af2a reader):
 * bit 1 = a scancode/reply byte is waiting in the data register,
 * bit 0 = keyboard interface ready/present (polled high).  The
 * keyboard talks raw AT scan code set 2 (the RMON translation
 * table at fe81b01c is laid out in set 2 order) and the usual
 * commands: 0xFF reset (0xFA+0xAA), 0xED LEDs, 0xF3 typematic.
 */
#define E17_KBC_STAT_RDY    0x01
#define E17_KBC_STAT_OBF    0x02

/*
 * Chip select controller: reg 0 reads back the region base address,
 * reg 0xa8 is a status/config register the boot code requires to have
 * 0x2 in bits 8-11 before it programs the chip select banks.
 */
#define E17_CSCTL_BASE_REG  (0x00 / 4)
#define E17_CSCTL_STATUS    (0xa8 / 4)
#define E17_CSCTL_STATUS_READY  0x200

#define E17_IO_BASE         0xfec00000

/*
 * Z8536 CIO.  Control port: first write sets the register pointer,
 * second writes the register; a read returns the pointed-to register
 * and resets the pointer state.  Ports A/B/C are data latches; reads
 * mix the output latch with the input pins according to the data
 * direction registers (DDR bit set = input).
 *
 * Reset protocol: the chip powers up with the MICR RESET bit set,
 * and while it is set every control port WRITE addresses the MICR
 * directly (only the reset bit is writable), bypassing the pointer
 * state machine.  RMON's init depends on this: it writes 0x00, 0x01
 * (pointer MICR, data RESET), then a bare 0x00 that must clear the
 * reset — getting this wrong desyncs every subsequent pointer/data
 * pair, the DDRs never load, and the DIP switch read returns the
 * port B output latch (0xff) instead of the switches.
 */
static uint8_t e17_cio_ddr_reg(int port)
{
    switch (port) {
    case E17_CIO_PORTA:
        return Z8536_PADDR;
    case E17_CIO_PORTB:
        return Z8536_PBDDR;
    default:
        return Z8536_PCDDR;
    }
}

static uint64_t e17_cio_read(E17CIOState *c, hwaddr addr)
{
    uint8_t ddr;

    switch (addr) {
    case E17_CIO_CTRL:
        c->ctrl_expect_data = false;
        return c->regs[c->ctrl_ptr];
    case E17_CIO_PORTA:
    case E17_CIO_PORTB:
    case E17_CIO_PORTC:
        ddr = c->regs[e17_cio_ddr_reg(addr)];
        return (c->in[addr] & ddr) | (c->out[addr] & ~ddr);
    }
    return 0;
}

static void e17_cio_write(E17CIOState *c, hwaddr addr, uint8_t val)
{
    switch (addr) {
    case E17_CIO_CTRL:
        if (c->regs[Z8536_MICR] & Z8536_MICR_RESET) {
            /* in reset state every control write hits the MICR */
            c->regs[Z8536_MICR] = val & Z8536_MICR_RESET;
            c->ctrl_expect_data = false;
        } else if (c->ctrl_expect_data) {
            c->regs[c->ctrl_ptr] = val;
            c->ctrl_expect_data = false;
        } else {
            c->ctrl_ptr = val & 0x3f;
            c->ctrl_expect_data = true;
        }
        break;
    case E17_CIO_PORTA:
    case E17_CIO_PORTB:
    case E17_CIO_PORTC:
        c->out[addr] = val;
        break;
    }
}

static uint64_t e17_sysc_read_impl(void *opaque, hwaddr addr, unsigned size)
{
    E17SysCState *s = opaque;
    hwaddr block = addr & 0x7f000;
    hwaddr off = addr & 0xfff;

    switch (block) {
    case E17_SYSC_VIC:
    case E17_SYSC_VIC_MIRR:
        /* one byte-wide chip on byte lane 3: register N at N*4+3 */
        if ((addr & 3) == 3 && off < ARRAY_SIZE(s->vic_regs) * 4) {
            uint8_t v = s->vic_regs[off >> 2];

            /*
             * LICR6 monitors the (active low) CD2401 interrupt line:
             * the STATE bit reads the raw pin level.
             */
            if (off == E17_VIC_LICR6) {
                v &= ~E17_VIC_LICR_STATE;
                if (!s->cd2401_irq) {
                    v |= E17_VIC_LICR_STATE;
                }
            }
            return v;
        }
        break;
    case E17_SYSC_DRAMC:
        if (off == 0xf0 || off == 0xf4) {
            return s->dramc[(off - 0xf0) / 4];
        }
        break;
    case E17_SYSC_CIO2:
        return e17_cio_read(&s->cio[1], addr & 3);
    case E17_SYSC_CIO1:
        return e17_cio_read(&s->cio[0], addr & 3);
    case E17_SYSC_VID_DAC:
    case E17_SYSC_VID_CRTC:
        /*
         * Open bus: the e17-vid device overlays these blocks when the
         * board has video fitted; without it the RMON probe fails
         * cleanly and the monitor uses the serial console.
         */
        return (1ULL << (size * 8)) - 1;
    case E17_SYSC_ACK:
        return 0;
    case E17_SYSC_I2C:
        /* no IPIN EEPROM: bus reads back released/high */
        return (1ULL << (size * 8)) - 1;
    case E17_SYSC_SLAVE:
        return s->slave_ctl;
    case E17_SYSC_MISC:
        return s->misc_5c;
    case E17_SYSC_CPUTYPE:
        return s->cputype;
    case E17_SYSC_KBC:
        if ((addr & 7) == 0) {
            return s->kbd_obf ? ps2_read_data(PS2_DEVICE(&s->ps2kbd)) : 0;
        } else if ((addr & 7) == 1) {
            return E17_KBC_STAT_RDY | (s->kbd_obf ? E17_KBC_STAT_OBF : 0);
        }
        break;
    case E17_SYSC_CSCTL:
        if (off < 0xb0) {
            if (off / 4 == E17_CSCTL_BASE_REG) {
                return E17_IO_BASE;
            }
            return s->csctl[off / 4];
        }
        break;
    }
    qemu_log_mask(LOG_UNIMP, "e17-sysc: unimplemented read %u @0x%05"
                  HWADDR_PRIx "\n", size, addr);
    return 0;
}

static void e17_sysc_write_impl(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    E17SysCState *s = opaque;
    hwaddr block = addr & 0x7f000;
    hwaddr off = addr & 0xfff;

    switch (block) {
    case E17_SYSC_VIC:
    case E17_SYSC_VIC_MIRR:
        if ((addr & 3) == 3 && off < ARRAY_SIZE(s->vic_regs) * 4) {
            s->vic_regs[off >> 2] = val;
            return;
        }
        break;
    case E17_SYSC_DRAMC:
        if (off == 0xf0 || off == 0xf4) {
            s->dramc[(off - 0xf0) / 4] = val;
            return;
        }
        break;
    case E17_SYSC_CIO2:
        e17_cio_write(&s->cio[1], addr & 3, val);
        return;
    case E17_SYSC_CIO1:
        if ((addr & 3) == E17_CIO_PORTC && val != s->post_code) {
            /*
             * Port C drives the POST code display.  The register is
             * only 4 bits wide on the real chip but RMON writes full
             * checkpoint bytes; keep them whole for diagnostics.
             */
            s->post_code = val;
            trace_e17_post_code(val);
        }
        e17_cio_write(&s->cio[0], addr & 3, val);
        return;
    case E17_SYSC_VID_DAC:
    case E17_SYSC_VID_CRTC:
        /* open bus unless the e17-vid device overlays these blocks */
        return;
    case E17_SYSC_I2C:
        /* no EEPROM fitted */
        return;
    case E17_SYSC_SLAVE:
        /* 0x20 releases the secondary CPU from halt */
        s->slave_ctl = val;
        qemu_set_irq(s->slave_run, (val & E17_SYSC_SLAVE_RUN) != 0);
        return;
    case E17_SYSC_MISC:
        s->misc_5c = val;
        return;
    case E17_SYSC_CPUTYPE:
        s->cputype = val;
        return;
    case E17_SYSC_KBC:
        if ((addr & 7) == 0) {
            ps2_write_keyboard(&s->ps2kbd, val);
            return;
        }
        break;
    case E17_SYSC_CSCTL:
        if (off < 0xb0) {
            s->csctl[off / 4] = val;
            return;
        }
        break;
    }
    qemu_log_mask(LOG_UNIMP, "e17-sysc: unimplemented write %u @0x%05"
                  HWADDR_PRIx " = 0x%" PRIx64 "\n", size, addr, val);
}

/* every access can be traced: handy while reverse engineering RMON */
static uint64_t e17_sysc_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t val = e17_sysc_read_impl(opaque, addr, size);

    trace_e17_sysc_read(addr, size, val);
    return val;
}

static void e17_sysc_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    trace_e17_sysc_write(addr, size, val);
    e17_sysc_write_impl(opaque, addr, val, size);
}

static const MemoryRegionOps e17_sysc_ops = {
    .read = e17_sysc_read,
    .write = e17_sysc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void e17_sysc_reset(DeviceState *dev)
{
    E17SysCState *s = E17_SYSC(dev);
    int i;

    for (i = 0; i < 2; i++) {
        memset(s->cio[i].regs, 0, sizeof(s->cio[i].regs));
        /* the Z8536 powers up in reset state */
        s->cio[i].regs[Z8536_MICR] = Z8536_MICR_RESET;
        s->cio[i].ctrl_ptr = 0;
        s->cio[i].ctrl_expect_data = false;
        memset(s->cio[i].out, 0, sizeof(s->cio[i].out));
    }
    /*
     * CIO1 port B reads the configuration DIP switches, port A bit 7
     * a console select input (reads high on the bench).
     */
    s->cio[0].in[E17_CIO_PORTB] = s->dip_switches;
    s->cio[0].in[E17_CIO_PORTA] = 0x80;
    s->cio[0].in[E17_CIO_PORTC] = 0x00;

    memset(s->csctl, 0, sizeof(s->csctl));
    s->csctl[E17_CSCTL_STATUS] = E17_CSCTL_STATUS_READY;

    s->slave_ctl = 0;
    s->misc_5c = 0;
    s->cputype = 0;
    s->post_code = 0;
    memset(s->vic_regs, 0, sizeof(s->vic_regs));
}

static void e17_sysc_cd2401_irq(void *opaque, int n, int level)
{
    E17SysCState *s = E17_SYSC(opaque);

    s->cd2401_irq = level;
}

/* the PS2 keyboard core raises its "irq" while a byte is waiting */
static void e17_sysc_kbd_irq(void *opaque, int n, int level)
{
    E17SysCState *s = E17_SYSC(opaque);

    s->kbd_obf = level;
}

static void e17_sysc_realize(DeviceState *dev, Error **errp)
{
    E17SysCState *s = E17_SYSC(dev);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ps2kbd), errp)) {
        return;
    }
    qdev_connect_gpio_out(DEVICE(&s->ps2kbd), PS2_DEVICE_IRQ,
                          qdev_get_gpio_in_named(dev, "ps2-kbd-irq", 0));
}

static void e17_sysc_init(Object *obj)
{
    E17SysCState *s = E17_SYSC(obj);

    memory_region_init_io(&s->iomem, obj, &e17_sysc_ops, s, "e17-sysc",
                          E17_SYSC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out_named(DEVICE(obj), &s->slave_run, "slave-run", 1);
    qdev_init_gpio_in_named(DEVICE(obj), e17_sysc_cd2401_irq, "cd2401-irq", 1);
    object_initialize_child(obj, "ps2kbd", &s->ps2kbd, TYPE_PS2_KBD_DEVICE);
    qdev_init_gpio_in_named(DEVICE(obj), e17_sysc_kbd_irq, "ps2-kbd-irq", 1);
}

static const VMStateDescription vmstate_e17_cio = {
    .name = "e17-sysc/cio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, E17CIOState, 64),
        VMSTATE_UINT8(ctrl_ptr, E17CIOState),
        VMSTATE_BOOL(ctrl_expect_data, E17CIOState),
        VMSTATE_UINT8_ARRAY(out, E17CIOState, 3),
        VMSTATE_UINT8_ARRAY(in, E17CIOState, 3),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_e17_sysc = {
    .name = "e17-sysc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(cio, E17SysCState, 2, 1, vmstate_e17_cio,
                             E17CIOState),
        VMSTATE_UINT8_ARRAY(vic_regs, E17SysCState, 256),
        VMSTATE_BOOL(cd2401_irq, E17SysCState),
        VMSTATE_BOOL(kbd_obf, E17SysCState),
        VMSTATE_UINT32_ARRAY(csctl, E17SysCState, 0x2c),
        VMSTATE_UINT32_ARRAY(dramc, E17SysCState, 2),
        VMSTATE_UINT8(slave_ctl, E17SysCState),
        VMSTATE_UINT8(misc_5c, E17SysCState),
        VMSTATE_UINT8(cputype, E17SysCState),
        VMSTATE_UINT8(post_code, E17SysCState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property e17_sysc_properties[] = {
    DEFINE_PROP_UINT8("dip-switches", E17SysCState, dip_switches, 0x00),
};

static void e17_sysc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "ELTEC Eurocom E17 onboard I/O";
    dc->realize = e17_sysc_realize;
    device_class_set_legacy_reset(dc, e17_sysc_reset);
    dc->vmsd = &vmstate_e17_sysc;
    device_class_set_props(dc, e17_sysc_properties);
}

static const TypeInfo e17_sysc_info = {
    .name = TYPE_E17_SYSC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(E17SysCState),
    .instance_init = e17_sysc_init,
    .class_init = e17_sysc_class_init,
};

static void e17_sysc_register_types(void)
{
    type_register_static(&e17_sysc_info);
}

type_init(e17_sysc_register_types)
