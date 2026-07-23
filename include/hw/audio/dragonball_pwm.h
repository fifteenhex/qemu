/*
 * DragonBall PWM unit 1 (0xfffff500) — the Palm speaker
 */

#ifndef HW_AUDIO_DRAGONBALL_PWM_H
#define HW_AUDIO_DRAGONBALL_PWM_H

#include "hw/core/sysbus.h"
#include "qemu/audio.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_PWM "dragonball.pwm"

typedef struct DragonBallPWMState DragonBallPWMState;
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallPWMState, DRAGONBALL_PWM)

struct DragonBallPWMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    uint16_t pwmc;
    uint8_t pwms_hi;
    uint8_t pwms_lo;
    uint8_t pwmp;

    uint32_t sysclk;

    /* synthesis state */
    bool active;
    uint64_t phase;      /* 32.32 fixed point wave phase */
    uint64_t phase_inc;
    uint32_t duty;       /* high time as a 0..0x10000 fraction */

    AudioBackend *audio_be;
    SWVoiceOut *voice;
};

#define DRAGONBALL_PWM_PWMC_HI  0x0
#define DRAGONBALL_PWM_PWMC_LO  0x1
#define DRAGONBALL_PWM_PWMS_HI  0x2
#define DRAGONBALL_PWM_PWMS_LO  0x3
#define DRAGONBALL_PWM_PWMP     0x4
#define DRAGONBALL_PWM_PWMCNT   0x5

#define DRAGONBALL_PWM_PWMC_CLKSRC_32K (1 << 15)
#define DRAGONBALL_PWM_PWMC_EN         (1 << 4)

#endif
