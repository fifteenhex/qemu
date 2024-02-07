/*
 *
 */

#ifndef DRAGONBALL_GPIO_H
#define DRAGONBALL_GPIO_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_DRAGONBALL_GPIO "dragonball.gpio"
OBJECT_DECLARE_SIMPLE_TYPE(DragonBallGPIOState, DRAGONBALL_GPIO)

#define DRAGONBALL_GPIO_NGPIOPERPORT 8
#define DRAGONBALL_GPIO_PORTS        7
#define DRAGONBALL_GPIO_NGPIO        (DRAGONBALL_GPIO_NGPIOPERPORT * DRAGONBALL_GPIO_PORTS)
#define DRAGONBALL_GPIO_PORTD        3

struct DragonBallGPIOPort {
	uint8_t dir;
	uint8_t data;
	uint8_t puden;
	uint8_t sel;
};

struct DragonBallGPIOState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    qemu_irq output[DRAGONBALL_GPIO_NGPIO];

    struct DragonBallGPIOPort ports[DRAGONBALL_GPIO_PORTS];
};

#define DRAGONBALL_GPIO_ADDR2PORT(_addr) (_addr >> 3)
#define DRAGONBALL_GPIO_ADDR2REG(_addr) (_addr & 0x7)

#define DRAGONBALL_GPIO_REG_DIR           0x0
#define DRAGONBALL_GPIO_REG_DATA          0x1
#define DRAGONBALL_GPIO_REG_PUDEN         0x2
#define DRAGONBALL_GPIO_REG_SEL           0x3

/* PORT A */
#define DRAGONBALL_GPIO_PADIR           0x0
#define DRAGONBALL_GPIO_PADATA          0x1
#define DRAGONBALL_GPIO_PAPUEN          0x2
#define DRAGONBALL_GPIO_PAPUEN_RESET    0xff

/* PORT B */
#define DRAGONBALL_GPIO_PBDIR           0x8
#define DRAGONBALL_GPIO_PBDATA          0x9
#define DRAGONBALL_GPIO_PBPUEN          0xa
#define DRAGONBALL_GPIO_PBPUEN_RESET    0xff
#define DRAGONBALL_GPIO_PBSEL           0xb
#define DRAGONBALL_GPIO_PBSEL_RESET     0xff

/* PORT C */
#define DRAGONBALL_GPIO_PCDIR           0x10
#define DRAGONBALL_GPIO_PCDATA          0x11
#define DRAGONBALL_GPIO_PCPDEN          0x12
#define DRAGONBALL_GPIO_PCPDEN_RESET    0xff
#define DRAGONBALL_GPIO_PCSEL           0x13
#define DRAGONBALL_GPIO_PCSEL_RESET     0xff

/* PORT D */
#define DRAGONBALL_GPIO_PDDIR           0x18
#define DRAGONBALL_GPIO_PDPUEN_RESET	0xff
#define DRAGONBALL_GPIO_PDSEL_RESET     0xf0
#if 0
0xFFFFF418 PDDIR 8 Port D Direction Register 0x00 -7
0xFFFFF419 PDDATA 8 Port D Data Register 0x00 -7
0xFFFFF41C PDPOL 8 Port D Polarity Register 0x00 -7
0xFFFFF41D PDIRQEN 8 Port D Interrupt Request Enable Register 0x00 -7
0xFFFFF41E PDKBEN 8 Port D Keyboard Enable Register 0x00 -7
0xFFFFF41F PDIRQEG 8 Port D Interrupt Request Edge Register 0x00 -7
#endif

/* PORT E */
#define DRAGONBALL_GPIO_PEDIR           0x20
#define DRAGONBALL_GPIO_PEDATA          0x21
#define DRAGONBALL_GPIO_PEPUEN          0x22
#define DRAGONBALL_GPIO_PEPUEN_RESET    0xff
#define DRAGONBALL_GPIO_PESEL           0x23
#define DRAGONBALL_GPIO_PESEL_RESET     0xff

/* PORT F */
#define DRAGONBALL_GPIO_PFDIR           0x28
#define DRAGONBALL_GPIO_PFDATA          0x29
#define DRAGONBALL_GPIO_PFPUEN          0x2a
#define DRAGONBALL_GPIO_PFPUEN_RESET    0xff
#define DRAGONBALL_GPIO_PFSEL           0x2b

/* PORT G */
#define DRAGONBALL_GPIO_PGDIR           0x30
#define DRAGONBALL_GPIO_PGDATA          0x31
#define DRAGONBALL_GPIO_PGPUEN          0x32
#define DRAGONBALL_GPIO_PGPUEN_RESET    0x3d
#define DRAGONBALL_GPIO_PGSEL           0x33
#define DRAGONBALL_GPIO_PGSEL_RESET     0x08

#endif /* DRAGONBALL_GPIO_H */
