/*
 * Amiga keyboard.
 *
 * The keyboard is a serial device on CIA-A: it clocks one keycode at a
 * time into the CIA's serial data register (raising the SP interrupt),
 * then waits for the host to acknowledge by briefly driving the serial
 * port to output — the handshake pulse this model receives on its "ack"
 * input.  Keycodes are the 7-bit Amiga raw codes with bit 7 set for
 * key-up; on the wire the byte is rotated left one bit and inverted.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/m68k/amiga_kbd.h"
#include "migration/vmstate.h"

/* key-up flag in a raw keycode */
#define AMIGA_KEY_UP        0x80

/*
 * The keyboard normally advances on the host's acknowledge handshake;
 * this timer is only a fallback for a guest that reads the code but
 * never handshakes (or has no keyboard.device), so keep it well clear
 * of the real handshake latency.
 */
#define AMIGA_KBD_FALLBACK_NS (100 * SCALE_MS)

/* QEMU qcode -> Amiga raw keycode */
static const uint8_t amiga_keymap[Q_KEY_CODE__MAX] = {
    [Q_KEY_CODE_GRAVE_ACCENT] = 0x00,
    [Q_KEY_CODE_1] = 0x01, [Q_KEY_CODE_2] = 0x02, [Q_KEY_CODE_3] = 0x03,
    [Q_KEY_CODE_4] = 0x04, [Q_KEY_CODE_5] = 0x05, [Q_KEY_CODE_6] = 0x06,
    [Q_KEY_CODE_7] = 0x07, [Q_KEY_CODE_8] = 0x08, [Q_KEY_CODE_9] = 0x09,
    [Q_KEY_CODE_0] = 0x0a,
    [Q_KEY_CODE_MINUS] = 0x0b, [Q_KEY_CODE_EQUAL] = 0x0c,
    [Q_KEY_CODE_BACKSLASH] = 0x0d,

    [Q_KEY_CODE_Q] = 0x10, [Q_KEY_CODE_W] = 0x11, [Q_KEY_CODE_E] = 0x12,
    [Q_KEY_CODE_R] = 0x13, [Q_KEY_CODE_T] = 0x14, [Q_KEY_CODE_Y] = 0x15,
    [Q_KEY_CODE_U] = 0x16, [Q_KEY_CODE_I] = 0x17, [Q_KEY_CODE_O] = 0x18,
    [Q_KEY_CODE_P] = 0x19,
    [Q_KEY_CODE_BRACKET_LEFT] = 0x1a, [Q_KEY_CODE_BRACKET_RIGHT] = 0x1b,

    [Q_KEY_CODE_A] = 0x20, [Q_KEY_CODE_S] = 0x21, [Q_KEY_CODE_D] = 0x22,
    [Q_KEY_CODE_F] = 0x23, [Q_KEY_CODE_G] = 0x24, [Q_KEY_CODE_H] = 0x25,
    [Q_KEY_CODE_J] = 0x26, [Q_KEY_CODE_K] = 0x27, [Q_KEY_CODE_L] = 0x28,
    [Q_KEY_CODE_SEMICOLON] = 0x29, [Q_KEY_CODE_APOSTROPHE] = 0x2a,

    [Q_KEY_CODE_Z] = 0x31, [Q_KEY_CODE_X] = 0x32, [Q_KEY_CODE_C] = 0x33,
    [Q_KEY_CODE_V] = 0x34, [Q_KEY_CODE_B] = 0x35, [Q_KEY_CODE_N] = 0x36,
    [Q_KEY_CODE_M] = 0x37,
    [Q_KEY_CODE_COMMA] = 0x38, [Q_KEY_CODE_DOT] = 0x39,
    [Q_KEY_CODE_SLASH] = 0x3a,

    [Q_KEY_CODE_KP_0] = 0x0f,
    [Q_KEY_CODE_KP_1] = 0x1d, [Q_KEY_CODE_KP_2] = 0x1e,
    [Q_KEY_CODE_KP_3] = 0x1f, [Q_KEY_CODE_KP_4] = 0x2d,
    [Q_KEY_CODE_KP_5] = 0x2e, [Q_KEY_CODE_KP_6] = 0x2f,
    [Q_KEY_CODE_KP_7] = 0x3d, [Q_KEY_CODE_KP_8] = 0x3e,
    [Q_KEY_CODE_KP_9] = 0x3f,
    [Q_KEY_CODE_KP_DECIMAL] = 0x3c, [Q_KEY_CODE_KP_SUBTRACT] = 0x4a,
    [Q_KEY_CODE_KP_ENTER] = 0x43, [Q_KEY_CODE_KP_DIVIDE] = 0x5c,
    [Q_KEY_CODE_KP_MULTIPLY] = 0x5d, [Q_KEY_CODE_KP_ADD] = 0x5e,

    [Q_KEY_CODE_SPC] = 0x40, [Q_KEY_CODE_BACKSPACE] = 0x41,
    [Q_KEY_CODE_TAB] = 0x42, [Q_KEY_CODE_RET] = 0x44,
    [Q_KEY_CODE_ESC] = 0x45, [Q_KEY_CODE_DELETE] = 0x46,

    [Q_KEY_CODE_UP] = 0x4c, [Q_KEY_CODE_DOWN] = 0x4d,
    [Q_KEY_CODE_RIGHT] = 0x4e, [Q_KEY_CODE_LEFT] = 0x4f,

    [Q_KEY_CODE_F1] = 0x50, [Q_KEY_CODE_F2] = 0x51, [Q_KEY_CODE_F3] = 0x52,
    [Q_KEY_CODE_F4] = 0x53, [Q_KEY_CODE_F5] = 0x54, [Q_KEY_CODE_F6] = 0x55,
    [Q_KEY_CODE_F7] = 0x56, [Q_KEY_CODE_F8] = 0x57, [Q_KEY_CODE_F9] = 0x58,
    [Q_KEY_CODE_F10] = 0x59, [Q_KEY_CODE_HELP] = 0x5f,

    [Q_KEY_CODE_SHIFT] = 0x60, [Q_KEY_CODE_SHIFT_R] = 0x61,
    [Q_KEY_CODE_CAPS_LOCK] = 0x62, [Q_KEY_CODE_CTRL] = 0x63,
    [Q_KEY_CODE_ALT] = 0x64, [Q_KEY_CODE_ALT_R] = 0x65,
    [Q_KEY_CODE_META_L] = 0x66, [Q_KEY_CODE_META_R] = 0x67,
};

static bool amiga_kbd_empty(AmigaKbdState *s)
{
    return s->head == s->tail;
}

/* clock the next queued code into the CIA and wait for the ack */
static void amiga_kbd_send_next(AmigaKbdState *s)
{
    uint8_t code;

    if (s->waiting || amiga_kbd_empty(s) || !s->cia) {
        return;
    }
    code = s->fifo[s->head];
    s->head = (s->head + 1) % AMIGA_KBD_FIFO;
    s->waiting = true;
    /* the wire format: rotate left one bit, then invert */
    mos8520_sdr_input(s->cia, ~((code << 1) | (code >> 7)));
    timer_mod(&s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              AMIGA_KBD_FALLBACK_NS);
}

static void amiga_kbd_queue(AmigaKbdState *s, uint8_t code)
{
    unsigned next = (s->tail + 1) % AMIGA_KBD_FIFO;

    if (next == s->head) {
        return;                 /* full: drop */
    }
    s->fifo[s->tail] = code;
    s->tail = next;
    amiga_kbd_send_next(s);
}

/* the host acknowledged the last code (drove the serial port to output) */
static void amiga_kbd_ack(void *opaque, int n, int level)
{
    AmigaKbdState *s = opaque;

    if (level && s->waiting) {
        s->waiting = false;
        amiga_kbd_send_next(s);
    }
}

/* fallback pacing if the host never handshakes (e.g. no keyboard.device) */
static void amiga_kbd_tick(void *opaque)
{
    AmigaKbdState *s = opaque;

    s->waiting = false;
    amiga_kbd_send_next(s);
}

static void amiga_kbd_event(DeviceState *dev, QemuConsole *src,
                            QemuInputEvent *evt)
{
    AmigaKbdState *s = AMIGA_KBD(dev);
    int qcode;
    uint8_t raw;

    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    /* handlers receive the host (Linux) keycode; map it to a QKeyCode */
    qcode = qemu_input_linux_to_qcode(evt->key.key);
    if (qcode <= 0 || qcode >= Q_KEY_CODE__MAX) {
        return;
    }
    raw = amiga_keymap[qcode];
    /* 0 is both the table's unmapped default and the grave key's code */
    if (raw == 0 && qcode != Q_KEY_CODE_GRAVE_ACCENT) {
        return;
    }
    amiga_kbd_queue(s, raw | (evt->key.down ? 0 : AMIGA_KEY_UP));
}

static const QemuInputHandler amiga_kbd_handler = {
    .name = "Amiga keyboard",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = amiga_kbd_event,
};

static void amiga_kbd_reset(DeviceState *dev)
{
    AmigaKbdState *s = AMIGA_KBD(dev);

    s->head = s->tail = 0;
    s->waiting = false;
    timer_del(&s->timer);
}

static void amiga_kbd_realize(DeviceState *dev, Error **errp)
{
    AmigaKbdState *s = AMIGA_KBD(dev);

    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, amiga_kbd_tick, s);
    s->hs = qemu_input_handler_register(dev, &amiga_kbd_handler);
}

static void amiga_kbd_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);

    qdev_init_gpio_in_named(dev, amiga_kbd_ack, "ack", 1);
}

static const VMStateDescription vmstate_amiga_kbd = {
    .name = "amiga-kbd",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(fifo, AmigaKbdState, AMIGA_KBD_FIFO),
        VMSTATE_UINT32(head, AmigaKbdState),
        VMSTATE_UINT32(tail, AmigaKbdState),
        VMSTATE_BOOL(waiting, AmigaKbdState),
        VMSTATE_TIMER(timer, AmigaKbdState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property amiga_kbd_properties[] = {
    DEFINE_PROP_LINK("cia", AmigaKbdState, cia, TYPE_MOS8520, MOS8520State *),
};

static void amiga_kbd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = amiga_kbd_realize;
    device_class_set_legacy_reset(dc, amiga_kbd_reset);
    dc->vmsd = &vmstate_amiga_kbd;
    device_class_set_props(dc, amiga_kbd_properties);
}

static const TypeInfo amiga_kbd_info = {
    .name          = TYPE_AMIGA_KBD,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AmigaKbdState),
    .instance_init = amiga_kbd_init,
    .class_init    = amiga_kbd_class_init,
};

static void amiga_kbd_register_types(void)
{
    type_register_static(&amiga_kbd_info);
}

type_init(amiga_kbd_register_types)
