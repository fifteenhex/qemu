/*
 * DragonBall PWM unit 1: the Palm speaker.
 *
 * PalmOS plays tones by programming the carrier straight to the tone
 * frequency: f = clk / (prescaler+1) / (2 << clksel) / (period+2),
 * with the sample register setting the duty cycle.  We synthesize
 * the resulting square wave; the 5-byte sample FIFO and its
 * interrupt (which would allow PCM playback) are not modelled, which
 * matches what POSE does.
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
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/audio/dragonball_pwm.h"
#include "migration/vmstate.h"

#define PWM_SAMPLE_RATE 32000
#define PWM_MAX_FREQ    20000
#define PWM_AMPLITUDE   48

static void dragonball_pwm_callback(void *opaque, int free)
{
    DragonBallPWMState *s = opaque;
    uint8_t buf[2048];

    while (free > 0) {
        int n = MIN(free, (int)sizeof(buf));
        int i, written;

        for (i = 0; i < n; i++) {
            if (s->active) {
                uint32_t frac = (s->phase >> 16) & 0xffff;

                buf[i] = 128 + ((frac < s->duty) ? PWM_AMPLITUDE
                                                 : -PWM_AMPLITUDE);
                s->phase += s->phase_inc;
            } else {
                buf[i] = 128;
            }
        }

        written = audio_be_write(s->audio_be, s->voice, buf, n);
        if (!written)
            break;
        free -= written;
    }
}

static void dragonball_pwm_update(DragonBallPWMState *s, bool started)
{
    uint32_t prescaler = (s->pwmc >> 8) & 0x7f;
    uint32_t clksel = s->pwmc & 0x03;
    uint32_t base = (s->pwmc & DRAGONBALL_PWM_PWMC_CLKSRC_32K) ? 32768
                                                               : s->sysclk;
    uint32_t divider = (prescaler + 1) * (2 << clksel) *
                       MIN(256u, (uint32_t)s->pwmp + 2);
    uint32_t freq = base / divider;

    if (started && (s->pwmc & DRAGONBALL_PWM_PWMC_EN))
        s->active = true;
    if (!(s->pwmc & DRAGONBALL_PWM_PWMC_EN))
        s->active = false;

    /* carriers beyond hearing are used to mute the speaker */
    if (freq == 0 || freq >= PWM_MAX_FREQ)
        s->active = false;

    if (s->active) {
        s->phase_inc = ((uint64_t)freq << 32) / PWM_SAMPLE_RATE;
        s->duty = s->pwmp ? MIN(0x10000u, ((uint32_t)s->pwms_lo << 16) /
                                          s->pwmp)
                          : 0;
    }

    if (s->voice)
        audio_be_set_active_out(s->audio_be, s->voice, s->active);
}

static uint64_t dragonball_pwm_read(void *opaque, hwaddr addr, unsigned size)
{
    DragonBallPWMState *s = opaque;

    switch (addr) {
    case DRAGONBALL_PWM_PWMC_HI:
        return s->pwmc >> 8;
    case DRAGONBALL_PWM_PWMC_LO:
        return s->pwmc & 0xff;
    case DRAGONBALL_PWM_PWMS_HI:
        return s->pwms_hi;
    case DRAGONBALL_PWM_PWMS_LO:
        return s->pwms_lo;
    case DRAGONBALL_PWM_PWMP:
        return s->pwmp;
    case DRAGONBALL_PWM_PWMCNT:
        return 0;
    default:
        return 0;
    }
}

static void dragonball_pwm_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    DragonBallPWMState *s = opaque;

    switch (addr) {
    case DRAGONBALL_PWM_PWMC_HI:
        s->pwmc = (s->pwmc & 0x00ff) | (value << 8);
        dragonball_pwm_update(s, false);
        break;
    case DRAGONBALL_PWM_PWMC_LO:
        s->pwmc = (s->pwmc & 0xff00) | (value & 0xff);
        dragonball_pwm_update(s, false);
        break;
    case DRAGONBALL_PWM_PWMS_HI:
        s->pwms_hi = value;
        break;
    case DRAGONBALL_PWM_PWMS_LO:
        /* writing a sample is what starts the output */
        s->pwms_lo = value;
        dragonball_pwm_update(s, true);
        break;
    case DRAGONBALL_PWM_PWMP:
        s->pwmp = value;
        dragonball_pwm_update(s, false);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps dragonball_pwm_ops = {
    .read = dragonball_pwm_read,
    .write = dragonball_pwm_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_pwm_reset(DeviceState *dev)
{
    DragonBallPWMState *s = DRAGONBALL_PWM(dev);

    s->pwmc = 0x0020;
    s->pwms_hi = 0;
    s->pwms_lo = 0;
    s->pwmp = 0xfe;
    s->active = false;
    s->phase = 0;

    if (s->voice)
        audio_be_set_active_out(s->audio_be, s->voice, false);
}

static void dragonball_pwm_realize(DeviceState *dev, Error **errp)
{
    DragonBallPWMState *s = DRAGONBALL_PWM(dev);
    struct audsettings as = { PWM_SAMPLE_RATE, 1, AUDIO_FORMAT_U8, 0 };

    memory_region_init_io(&s->mmio, OBJECT(dev), &dragonball_pwm_ops, s,
                          TYPE_DRAGONBALL_PWM, 0x10);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    /* audio_be_check returns true once a backend is available */
    if (!audio_be_check(&s->audio_be, errp)) {
        /* no backend configured: run silent */
        return;
    }

    s->voice = audio_be_open_out(s->audio_be, s->voice, "dragonball.pwm",
                                 s, dragonball_pwm_callback, &as);
    if (!s->voice) {
        error_report("dragonball_pwm: could not open voice");
    }
}

static const VMStateDescription vmstate_dragonball_pwm = {
    .name = "dragonball_pwm",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(pwmc, DragonBallPWMState),
        VMSTATE_UINT8(pwms_hi, DragonBallPWMState),
        VMSTATE_UINT8(pwms_lo, DragonBallPWMState),
        VMSTATE_UINT8(pwmp, DragonBallPWMState),
        VMSTATE_BOOL(active, DragonBallPWMState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property dragonball_pwm_properties[] = {
    DEFINE_AUDIO_PROPERTIES(DragonBallPWMState, audio_be),
    DEFINE_PROP_UINT32("sysclk", DragonBallPWMState, sysclk, 16580608),
};

static void dragonball_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, dragonball_pwm_properties);
    device_class_set_legacy_reset(dc, dragonball_pwm_reset);
    dc->realize = dragonball_pwm_realize;
    dc->vmsd = &vmstate_dragonball_pwm;
}

static const TypeInfo dragonball_pwm_info = {
    .name          = TYPE_DRAGONBALL_PWM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DragonBallPWMState),
    .class_init    = dragonball_pwm_class_init,
};

static void dragonball_pwm_register_types(void)
{
    type_register_static(&dragonball_pwm_info);
}

type_init(dragonball_pwm_register_types)
