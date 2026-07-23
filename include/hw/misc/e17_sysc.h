/*
 * ELTEC Eurocom E17 onboard I/O ("system controller" region)
 *
 * Models the board logic and simple peripherals living in the
 * 0xfec00000 I/O window of the ELTEC Eurocom 17 VMEbus 68040 board.
 * Everything here was reverse engineered from the RMON 3.1.3 monitor
 * ROM — see E17-NOTES.md in the tree root for the full map and the
 * evidence behind each device.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_E17_SYSC_H
#define HW_MISC_E17_SYSC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_E17_SYSC "e17-sysc"
OBJECT_DECLARE_SIMPLE_TYPE(E17SysCState, E17_SYSC)

/* Offsets of the subdevices inside the 0xfec00000 window */
#define E17_SYSC_RTC       0x01000 /* RTC/NVRAM/watchdog, byte lane 3 */
#define E17_SYSC_DRAMC     0x08000 /* DRAM controller regs (0xf0/0xf4) */
#define E17_SYSC_CIO2      0x10000 /* Z8536 CIO #2 */
#define E17_SYSC_SRAM2K    0x20000 /* small shared RAM, purpose unknown */
#define E17_SYSC_CIO1      0x30000 /* Z8536 CIO #1 (POST/DIP/console sel) */
#define E17_SYSC_VID_DAC   0x40000 /* video RAMDAC + pixel clock PLL */
#define E17_SYSC_VID_CRTC  0x48000 /* video CRTC */
#define E17_SYSC_ACK       0x50000 /* read-to-ack status register */
#define E17_SYSC_I2C       0x54000 /* I2C master (IPIN serial EEPROM) */
#define E17_SYSC_SLAVE     0x58000 /* secondary CPU control */
#define E17_SYSC_MISC      0x5c000 /* unknown byte register */
#define E17_SYSC_CPUTYPE   0x5e000 /* CPU type latch (1=68040, 4=68060) */
#define E17_SYSC_KBC       0x60000 /* AT keyboard controller */
/* 0x64000 CD2401 serial and 0x66000 its IACK port: separate device */
#define E17_SYSC_LANCE     0x68000 /* Am7990 LANCE (not yet modelled) */
#define E17_SYSC_SCSI      0x6c000 /* NCR 53C710 (stub, byteswapped) */
#define E17_SYSC_CSCTL     0x70000 /* chip select / memory controller */

#define E17_SYSC_SIZE      0x80000

/* Z8536 CIO: A1/A0 wiring is portC/portB/portA/control */
#define E17_CIO_PORTC      0
#define E17_CIO_PORTB      1
#define E17_CIO_PORTA      2
#define E17_CIO_CTRL       3

/*
 * Minimal Zilog Z8536 CIO state: the indexed register file behind the
 * control port plus the three port data latches.  Register numbers
 * follow the datasheet (0x22/0x2a data direction, etc).
 */
typedef struct {
    uint8_t regs[64];
    uint8_t ctrl_ptr;       /* register pointer for the control port */
    bool ctrl_expect_data;  /* next control write is data, not pointer */
    uint8_t out[3];         /* port C/B/A output latches */
    uint8_t in[3];          /* port C/B/A input pin state */
} E17CIOState;

struct E17SysCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    E17CIOState cio[2];     /* [0] = CIO1 @+0x30000, [1] = CIO2 @+0x10000 */

    uint8_t rtc_nvram[256]; /* RTC/NVRAM chip, one byte per 32bit word */
    uint8_t sram2k[0x800];

    uint8_t kbd_data;       /* AT keyboard controller data register */
    uint8_t kbd_status;

    uint8_t scsi_regs[0x60];/* 53C710 register stub */

    uint32_t csctl[0x2c];   /* chip select controller, 0xb0 bytes */
    uint32_t dramc[2];      /* 0x080f0/0x080f4 */

    uint8_t slave_ctl;
    uint8_t misc_5c;
    uint8_t cputype;
    uint8_t post_code;      /* last value written to the POST display */

    /*
     * Configuration DIP switches, read through CIO1 port B.  The low
     * nibble selects the boot configuration profile (only 0-7 are
     * valid); the high nibble's meaning is not yet understood.
     */
    uint8_t dip_switches;
};

#endif
