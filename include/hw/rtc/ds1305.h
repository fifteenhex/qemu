/*
 *
 */

#ifndef HW_MISC_DS1305_H
#define HW_MISC_DS1305_H

#include "hw/ssi/ssi.h"
#include "qom/object.h"

struct DS1305State {
    SSIPeripheral parent_obj;

    qemu_irq interrupt;

    struct tm now;
    bool write;
    unsigned int counter;
    uint8_t address;
};

#define TYPE_DS1305 "ds1305"

OBJECT_DECLARE_SIMPLE_TYPE(DS1305State, DS1305)

#define DS1305_REG_SECONDS   0x00
#define DS1305_REG_MINUTES   0x01
#define DS1305_REG_HOURS     0x02
#define DS1305_REG_DAY       0x03
#define DS1305_REG_DATE      0x04
#define DS1305_REG_MONTH     0x05
#define DS1305_REG_YEAR      0x06
#define DS1305_REG_CONTROL   0x0f
#define DS1305_REG_STATUS    0x10
#define DS1305_REG_TRICKLE   0x11
#define DS1305_REG_NVRAM     0x20
#define DS1305_NVRAM_SZ      0x60
#endif
