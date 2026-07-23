/* SPDX-License-Identifier: MIT */
/* QEMU Sega MegaDrive I/O area: version register + controller ports */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"
#include "migration/vmstate.h"
#include "system/reset.h"
#include "ui/input.h"

#include "hw/input/md_io.h"

static void md_io_reset(void *opaque);

/*
 * Register map (8-bit registers at odd byte addresses within 0xA10000):
 *
 *   0x01  version            0x03  data 1     0x05  data 2     0x07  data 3
 *   0x09  ctrl 1             0x0B  ctrl 2     0x0D  ctrl 3
 *   0x0F  txdata 1  0x11  rxdata 1  0x13  s-ctrl 1
 *   0x15  txdata 2  0x17  rxdata 2  0x19  s-ctrl 2
 *   0x1B  txdata 3  0x1D  rxdata 3  0x1F  s-ctrl 3
 */
#define MD_IO_VERSION   0x01
#define MD_IO_DATA(n)   (0x03 + (n) * 2)
#define MD_IO_CTRL(n)   (0x09 + (n) * 2)
#define MD_IO_TXDATA(n) (0x0F + (n) * 6)
#define MD_IO_RXDATA(n) (0x11 + (n) * 6)
#define MD_IO_SCTRL(n)  (0x13 + (n) * 6)

#define MD_IO_TH        0x40    /* TH select line (bit 6) */

/*
 * Read a controller data port.  A 3-button pad on port 1 is fed from the
 * host keyboard; the other ports report an idle pad (all buttons released
 * = 1, active-low).  The value the CPU drives on output pins is read back;
 * input pins return the pad state, which is multiplexed by the TH line
 * (bit 6):
 *
 *   TH = 1:  -  -  C  B  R  L  D  U
 *   TH = 0:  -  -  St A  0  0  D  U
 */
static uint8_t md_io_pad_read(MDIOState *s, int port)
{
    uint8_t ctrl = s->ctrl[port];
    uint8_t out  = s->data_out[port];
    uint8_t pad  = (port == 0) ? s->pad : 0;
    uint8_t in;

    if (out & MD_IO_TH) {
        in = 0x3F & ~(((pad & MD_PAD_C)     ? 0x20 : 0) |
                      ((pad & MD_PAD_B)     ? 0x10 : 0) |
                      ((pad & MD_PAD_RIGHT) ? 0x08 : 0) |
                      ((pad & MD_PAD_LEFT)  ? 0x04 : 0) |
                      ((pad & MD_PAD_DOWN)  ? 0x02 : 0) |
                      ((pad & MD_PAD_UP)    ? 0x01 : 0));
    } else {
        in = 0x33 & ~(((pad & MD_PAD_START) ? 0x20 : 0) |
                      ((pad & MD_PAD_A)     ? 0x10 : 0) |
                      ((pad & MD_PAD_DOWN)  ? 0x02 : 0) |
                      ((pad & MD_PAD_UP)    ? 0x01 : 0));
    }

    return (out & ctrl) | (in & ~ctrl);
}

/* Host keyboard -> pad: arrows, A/S/D = A/B/C, Enter = Start */
static void md_io_kbd_event(DeviceState *dev, QemuConsole *src,
                            QemuInputEvent *evt)
{
    MDIOState *s = MD_IO(dev);
    int qcode;
    uint8_t bit;

    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    qcode = qemu_input_linux_to_qcode(evt->key.key);

    switch (qcode) {
    case Q_KEY_CODE_UP:    bit = MD_PAD_UP;    break;
    case Q_KEY_CODE_DOWN:  bit = MD_PAD_DOWN;  break;
    case Q_KEY_CODE_LEFT:  bit = MD_PAD_LEFT;  break;
    case Q_KEY_CODE_RIGHT: bit = MD_PAD_RIGHT; break;
    case Q_KEY_CODE_A:     bit = MD_PAD_A;     break;
    case Q_KEY_CODE_S:     bit = MD_PAD_B;     break;
    case Q_KEY_CODE_D:     bit = MD_PAD_C;     break;
    case Q_KEY_CODE_RET:
    case Q_KEY_CODE_KP_ENTER:
        bit = MD_PAD_START;
        break;
    default:
        return;
    }

    if (evt->key.down) {
        s->pad |= bit;
    } else {
        s->pad &= ~bit;
    }
}

static const QemuInputHandler md_io_kbd_handler = {
    .name = "MegaDrive pad",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = md_io_kbd_event,
};

static uint8_t md_io_reg_read(MDIOState *s, unsigned off)
{
    switch (off) {
    case MD_IO_VERSION:   return s->version;
    case MD_IO_DATA(0):   return md_io_pad_read(s, 0);
    case MD_IO_DATA(1):   return md_io_pad_read(s, 1);
    case MD_IO_DATA(2):   return md_io_pad_read(s, 2);
    case MD_IO_CTRL(0):   return s->ctrl[0];
    case MD_IO_CTRL(1):   return s->ctrl[1];
    case MD_IO_CTRL(2):   return s->ctrl[2];
    case MD_IO_TXDATA(0): return s->txdata[0];
    case MD_IO_TXDATA(1): return s->txdata[1];
    case MD_IO_TXDATA(2): return s->txdata[2];
    case MD_IO_SCTRL(0):  return s->sctrl[0];
    case MD_IO_SCTRL(1):  return s->sctrl[1];
    case MD_IO_SCTRL(2):  return s->sctrl[2];
    case MD_IO_RXDATA(0):
    case MD_IO_RXDATA(1):
    case MD_IO_RXDATA(2):
        return 0x00;      /* serial RX not modelled */
    default:
        return 0xFF;
    }
}

static void md_io_reg_write(MDIOState *s, unsigned off, uint8_t val)
{
    switch (off) {
    case MD_IO_VERSION:   break;      /* read-only */
    case MD_IO_DATA(0):   s->data_out[0] = val; break;
    case MD_IO_DATA(1):   s->data_out[1] = val; break;
    case MD_IO_DATA(2):   s->data_out[2] = val; break;
    case MD_IO_CTRL(0):   s->ctrl[0] = val; break;
    case MD_IO_CTRL(1):   s->ctrl[1] = val; break;
    case MD_IO_CTRL(2):   s->ctrl[2] = val; break;
    case MD_IO_TXDATA(0): s->txdata[0] = val; break;
    case MD_IO_TXDATA(1): s->txdata[1] = val; break;
    case MD_IO_TXDATA(2): s->txdata[2] = val; break;
    case MD_IO_SCTRL(0):  s->sctrl[0] = val; break;
    case MD_IO_SCTRL(1):  s->sctrl[1] = val; break;
    case MD_IO_SCTRL(2):  s->sctrl[2] = val; break;
    default:
        qemu_log_mask(LOG_UNIMP,
            "md_io: write 0x%02x to unhandled reg 0x%02x\n", val, off);
        break;
    }
}

static uint64_t md_io_read(void *opaque, hwaddr offset, unsigned size)
{
    MDIOState *s = MD_IO(opaque);

    /* registers are 8-bit and live at odd byte offsets */
    return md_io_reg_read(s, (offset & 0x1F) | 1);
}

static void md_io_write(void *opaque, hwaddr offset, uint64_t val,
                        unsigned size)
{
    MDIOState *s = MD_IO(opaque);

    md_io_reg_write(s, (offset & 0x1F) | 1, val & 0xFF);
}

static const MemoryRegionOps md_io_ops = {
    .read       = md_io_read,
    .write      = md_io_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

static const VMStateDescription vmstate_md_io = {
    .name    = "md-io",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(version,         MDIOState),
        VMSTATE_UINT8_ARRAY(ctrl,      MDIOState, MD_IO_NUM_PORTS),
        VMSTATE_UINT8_ARRAY(data_out,  MDIOState, MD_IO_NUM_PORTS),
        VMSTATE_UINT8_ARRAY(txdata,    MDIOState, MD_IO_NUM_PORTS),
        VMSTATE_UINT8_ARRAY(sctrl,     MDIOState, MD_IO_NUM_PORTS),
        VMSTATE_UINT8(pad,             MDIOState),
        VMSTATE_END_OF_LIST()
    },
};

static void md_io_realize(DeviceState *dev, Error **errp)
{
    MDIOState    *s   = MD_IO(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &md_io_ops, s,
                          "md-io", 0x20);
    sysbus_init_mmio(sbd, &s->iomem);

    if (!qemu_input_handler_register(dev, &md_io_kbd_handler)) {
        error_setg(errp, "md_io: failed to register input handler");
        return;
    }

    qemu_register_reset(md_io_reset, s);
}

static void md_io_reset(void *opaque)
{
    MDIOState *s = opaque;

    memset(s->ctrl,     0, sizeof(s->ctrl));
    memset(s->data_out, 0, sizeof(s->data_out));
    memset(s->txdata,   0, sizeof(s->txdata));
    memset(s->sctrl,    0, sizeof(s->sctrl));
}

static const Property md_io_properties[] = {
    /* default: overseas (export) NTSC machine, no expansion, version 0 */
    DEFINE_PROP_UINT8("version", MDIOState, version, 0xA0),
};

static void md_io_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = md_io_realize;
    dc->vmsd    = &vmstate_md_io;
    dc->desc    = "Sega MegaDrive I/O (version + controller ports)";
    device_class_set_props(dc, md_io_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo md_io_info = {
    .name          = TYPE_MD_IO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MDIOState),
    .class_init    = md_io_class_init,
};

static void md_io_register_types(void)
{
    type_register_static(&md_io_info);
}

type_init(md_io_register_types)
