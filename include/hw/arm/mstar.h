/*
 * MStar/SigmaStar Armv7 SoC family
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_MSTAR_H
#define HW_ARM_MSTAR_H

#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "hw/sd/sd.h"
#include "hw/i2c/i2c.h"
#include "net/net.h"
#include "qemu/audio.h"
#include "qom/object.h"

/*
 * This header is the SoC's device inventory: the per-block QOM types and state
 * structs (implemented in hw/<subsystem>/mstar_*.c) plus the shared physical
 * memory map. It is deliberately free of target/arm includes so the device
 * models can live in the target-independent build; the ARM-specific SoC
 * container (MStarSoCState, which embeds the CPUs and GIC) lives in
 * hw/arm/mstar.c instead.
 */

/*
 * RIU I/O tracer (implemented in hw/arm/mstar.c). Device models that own
 * registers claimed away from the catch-all overlay (display, dphy, i2c) log
 * their accesses through this when MSTAR_IOLOG is set. A no-op otherwise.
 */
void mstar_iolog(hwaddr phys, bool write, uint64_t val, unsigned size);

/*
 * The "mst-intc" (also on MediaTek chips): a hierarchical interrupt
 * controller sitting between the peripherals and the GIC. It adds a per-line
 * mask and forwards each input to a GIC SPI (irq_start + line).
 */
#define TYPE_MST_INTC "mstar-mst-intc"
OBJECT_DECLARE_SIMPLE_TYPE(MstIntcState, MST_INTC)

#define MST_INTC_MAX_IRQS 64

struct MstIntcState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint32_t irq_start;                         /* GIC SPI of input 0 */
    uint32_t num_irqs;
    uint16_t mask[MST_INTC_MAX_IRQS / 16];
    uint16_t polarity[MST_INTC_MAX_IRQS / 16];
    uint64_t level;                             /* input line levels */
    qemu_irq irq_out[MST_INTC_MAX_IRQS];        /* to the GIC */
};

/*
 * The "msc313e-timer": a free-running up-counter used as the clock source
 * and, on the other instances, as a clock event.
 */
#define TYPE_MSC313E_TIMER "mstar-msc313e-timer"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313eTimerState, MSC313E_TIMER)

struct Msc313eTimerState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;           /* counter-reached-MAX interrupt (INT_EN gated) */
    QEMUTimer *hrtimer;     /* fires in host time when the counter hits MAX */
    uint32_t freq;
    uint16_t ctrl;
    uint16_t divide;
    uint32_t max;
    int64_t base_ns;        /* virtual time the counter was (re)based */
    uint64_t base_count;    /* counter value at base_ns */
    uint32_t latch;         /* latched on a COUNTER_LOW read */
    bool int_pending;       /* the counter has reached MAX since the last ack */
};

#define MSTAR_NUM_TIMERS 3

/*
 * The "msc313-rtc": a free-running 1 Hz seconds counter with a match-based
 * alarm interrupt.
 */
#define TYPE_MSC313_RTC "mstar-msc313-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313RtcState, MSC313_RTC)

struct Msc313RtcState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint16_t ctrl;
    uint16_t status;
    uint32_t freq_cw;       /* clock divider written by the guest (unused) */
    uint32_t load_val;      /* seconds value to load into the counter */
    uint32_t match_val;     /* alarm match value */
    uint32_t cnt_latch;     /* counter latched on a READ_EN trigger */
    int64_t base_ns;        /* virtual time the counter was (re)based */
    uint32_t base_count;    /* counter value at base_ns */
};

/*
 * The "msc313-gpio": a bank of single-byte pad control registers. Each pad
 * offset holds IN (bit0, the pin level, read-only), OUT (bit4) and OEN
 * (bit5, output disable) bits. reg = <0x207800 0x200> on the riu bus.
 */
/*
 * The "sar": the SAR ADC (sar@2800). Four voltage channels plus an internal
 * temperature sensor, and four SAR pads that double as a GPIO group. Register
 * layout from the mainline driver drivers/iio/adc/msc313e_sar.c; the model
 * returns a fixed synthesised sample per channel on a one-shot conversion.
 */
#define TYPE_MSC313_SAR "mstar-msc313-sar"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313SarState, MSC313_SAR)

#define MSTAR_SAR_NUM_REGS  (0x200 / 4)
#define MSTAR_SAR_CHANNELS  8

struct Msc313SarState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint16_t regs[MSTAR_SAR_NUM_REGS];
    uint16_t chan_input[MSTAR_SAR_CHANNELS]; /* synthesised per-channel sample */
};

/*
 * The "emac" 10/100 Ethernet MAC (emac@2a2000). It is a classic Atmel MACB/
 * EMAC (AT91RM9200 style, MACB_CAPS_MACB_IS_EMAC in the 6.5 macb driver, with
 * the rm9200 transmit path) sitting behind MStar's RIU bus: each 32-bit MACB
 * register at byte offset G is split into two 16-bit halves - the low half at
 * RIU byte offset 2*G, the high half at 2*G+4 (see include/soc/mstar/riuxiu.h
 * riu_readl/riu_writel). This models enough of it - MDIO PHY, rm9200 TX, the
 * classic RX descriptor ring - to actually move packets to a QEMU netdev.
 */
#define TYPE_MSTAR_EMAC "mstar-emac"
OBJECT_DECLARE_SIMPLE_TYPE(MstarEmacState, MSTAR_EMAC)

struct MstarEmacState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    MemoryRegion phy_iomem;     /* integrated PHY register block @ 0x1f006000 */
    qemu_irq irq;
    NICState *nic;
    NICConf conf;
    uint16_t phy_regs[32];      /* integrated PHY MDIO register table */
    uint16_t raw[0x1000 / 2];   /* RIU 16-bit words, index = RIU addr/2 (MSTAR_EMAC_SIZE) */
    uint32_t isr;           /* pending interrupt status */
    uint32_t ien;           /* enabled interrupt mask (IER sets, IDR clears) */
    uint32_t rsr;           /* receive status (REC/OVR/BNA) */
    uint32_t man;           /* PHY maintenance read-back */
    unsigned int rx_idx;    /* current RX descriptor index in the RBQP ring */
    uint8_t phy_addr;       /* MDIO address the (single) PHY answers at */
};

#define TYPE_MSC313_GPIO "mstar-msc313-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313GpioState, MSC313_GPIO)

#define MSTAR_GPIO_NUM_REGS 0x200

struct Msc313GpioState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint8_t regs[MSTAR_GPIO_NUM_REGS];
    uint32_t buttons;       /* bitmask of pressed board buttons (QOM property) */
    bool gpioi2c;           /* GPIO8=SDA / GPIO9=SCL bit-banged I2C (cameras) */
    void *bbi2c;            /* bitbang_i2c_interface * */
    void *i2c_bus;          /* I2CBus *; exposed so a sensor can be attached */
    int sda_level;          /* current bus SDA (bit-bang readback) */
};

/*
 * The "pm_gpio" bank (gpio_pm@1e00, the power-management GPIO controller). Its
 * pads survive suspend and carry the SD card-detect (SD_CDZ) among others. On
 * the Miyoo Mini the SD card-detect is on this bank (6.5 dts cd-gpios =
 * <&gpio_pm SSD20XD_PM_SD_CDZ GPIO_ACTIVE_LOW>); the vendor sdmmc driver reads
 * it as bank register 0x47 bit 2. We model register storage plus that one
 * input bit so the host detects the "-drive if=sd" card.
 */
#define TYPE_MSTAR_PM_GPIO "mstar-pm-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(MstarPmGpioState, MSTAR_PM_GPIO)

#define MSTAR_PM_GPIO_NUM_REGS (0x200 / 4)

struct MstarPmGpioState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint16_t regs[MSTAR_PM_GPIO_NUM_REGS];
    bool card_present;      /* an SD card is inserted (drives SD_CDZ low) */
    uint32_t buttons;       /* pressed PM-bank buttons: bit0 down, bit1 left */
};

/*
 * The "isp" SPI-NOR controller (mstar,msc313-isp): a byte-at-a-time SPI
 * master plus a memory-mapped XIP read window. It drives an m25p80 SPI-NOR
 * flash over an SSI bus.
 */
#define TYPE_MSC313_ISP "mstar-msc313-isp"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313IspState, MSC313_ISP)

#define MSTAR_ISP_QSPI_NUM_REGS (0x200 / 4)
#define MSTAR_ISP_FSP_NUM_REGS  (0x200 / 4)

struct Msc313IspState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;     /* isp core registers @0x1f001000 */
    MemoryRegion fsp;       /* fsp flash controller  @0x1f002c00 */
    MemoryRegion qspi;      /* qspi config registers @0x1f002e00 */
    MemoryRegion xip;       /* memory-mapped read window @0x14000000 */
    SSIBus *spi;
    qemu_irq cs;            /* chip select to the flash (active low) */
    bool cs_asserted;
    uint16_t rdata;         /* last byte clocked in from the flash */
    uint8_t *flash_cache;   /* in-memory copy of the flash for XIP reads */
    uint16_t password;
    uint16_t clkdiv;
    uint16_t trigger;
    uint16_t rst;
    uint16_t qspi_regs[MSTAR_ISP_QSPI_NUM_REGS];
    uint16_t fsp_regs[MSTAR_ISP_FSP_NUM_REGS];
};

/*
 * The "bdma" bulk DMA engine (mstar,msc313-bdma): a 2-channel copy engine.
 * Each channel (0x40 apart) copies "size" bytes between two slaves (MIU
 * DRAM, the QSPI flash, ...) and raises a completion interrupt.
 */
#define TYPE_MSC313_BDMA "mstar-msc313-bdma"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313BdmaState, MSC313_BDMA)

#define MSTAR_BDMA_NUM_CHANNELS 2
#define MSTAR_BDMA_CHAN_SIZE    0x40
#define MSTAR_BDMA_CHAN_NREGS   (MSTAR_BDMA_CHAN_SIZE / 4)

typedef struct Msc313BdmaChan {
    uint16_t regs[MSTAR_BDMA_CHAN_NREGS];
    qemu_irq irq;
} Msc313BdmaChan;

struct Msc313BdmaState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    Msc313BdmaChan chans[MSTAR_BDMA_NUM_CHANNELS];
};

/*
 * The "clkgen": the main clock mux/gate block. Each mux lives in one 16-bit
 * register (gate bit + parent-select field + deglitch bit). The Linux driver
 * (drivers/clk/mstar/clk-msc313-clkgen.c) was reverse engineered and only
 * knows some registers/bits, so the model logs anything the firmware touches
 * outside what the driver describes (via LOG_UNIMP / -d unimp).
 */
/*
 * A generic "register probe" for RE: a block of 16-bit registers (4-byte
 * stride) that stores/returns writes and logs any access the mainline Linux
 * driver does not describe, so registers/bits the firmware uses that are not
 * in the v6.5 kernel can be found. The set of described registers is SoC- and
 * block-specific, so each (block, SoC) pair is its own concrete type carrying
 * its own table in the class. Used for the clkgen and pinctrl blocks, whose
 * layouts differ between the msc313 (infinity3) and ssd20xd (infinity2m) SoCs.
 */
#define TYPE_MSTAR_REGPROBE "mstar-regprobe"
OBJECT_DECLARE_TYPE(MstarRegProbeState, MstarRegProbeClass, MSTAR_REGPROBE)

#define TYPE_MSC313_CLKGEN  "mstar-msc313-clkgen"
#define TYPE_SSD20XD_CLKGEN "mstar-ssd20xd-clkgen"
#define TYPE_MSC313_PINCTRL "mstar-msc313-pinctrl"
#define TYPE_SSD20XD_PINCTRL "mstar-ssd20xd-pinctrl"

#define MSTAR_REGPROBE_MAX_REGS (0x400 / 4)

/* One described register: its byte offset, a label, and the bits the driver
 * knows about (0xffff = the whole 16-bit register is described). */
typedef struct MstarRegProbeReg {
    uint16_t offset;
    const char *name;
    uint16_t known;
} MstarRegProbeReg;

struct MstarRegProbeState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint16_t regs[MSTAR_REGPROBE_MAX_REGS];
};

struct MstarRegProbeClass {
    /*< private >*/
    SysBusDeviceClass parent_class;
    /*< public >*/
    const char *label;                  /* "clkgen"/"pinctrl" for log messages */
    uint32_t size;                      /* MMIO region size */
    const MstarRegProbeReg *known;      /* registers the v6.5 driver describes */
    unsigned n_known;
};

/*
 * The "sdio": the MStar "FCIE" SD/MMC host controller (mstar,msc313-sdio).
 * Register map from the mainline driver drivers/mmc/host/mstar-fcie.c: a
 * bank of 16-bit registers plus a small command/response FIFO. The guest
 * loads a command into the FIFO, programs SD_CTL and fires the job; we run
 * the request on a QEMU SD card and post the completion interrupt.
 */
#define TYPE_MSC313_SDIO "mstar-msc313-sdio"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313SdioState, MSC313_SDIO)

#define TYPE_MSC313_SDIO_BUS "mstar-msc313-sdio-bus"

#define MSTAR_SDIO_NUM_REGS  (0x100 / 4)
#define MSTAR_SDIO_FIFO_BYTES 64

struct Msc313SdioState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    SDBus sdbus;
    qemu_irq irq;
    uint16_t regs[MSTAR_SDIO_NUM_REGS];
    uint8_t fifo[MSTAR_SDIO_FIFO_BYTES];
    uint8_t last_cmd;       /* opcode of the in-flight command (for auto-stop) */
};

/*
 * The "pwm": the MStar/SSD20xD PWM controller (mstar,ssd20xd-pwm). A bank of
 * per-channel duty/period/divider/control registers; on this board channel 0
 * drives the panel backlight. Register layout is from the mainline driver
 * drivers/pwm/pwm-msc313e.c.
 */
#define TYPE_MSC313_PWM "mstar-msc313-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313PwmState, MSC313_PWM)

#define MSTAR_PWM_NUM_REGS (0x400 / 4)
#define MSTAR_PWM_CHANNELS 4

struct Msc313PwmState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint16_t regs[MSTAR_PWM_NUM_REGS];
};

/* Channel brightness 0..256 (implemented in hw/gpio/mstar_pwm.c); the display
 * scanout reads channel 0 to dim the panel backlight. */
unsigned int msc313_pwm_brightness(Msc313PwmState *s, unsigned int ch);

/*
 * The "disp": enough of the SSD20xD display pipeline to scan out to a QEMU
 * console and drive DRM vblank (gop1 primary plane, display-top vsync, the
 * mopg overlay/video plane and the MIPI DSI controller). Register layout is
 * from the mstar DRM driver (drivers/gpu/drm/mstar/mstar_{gop,top,mop,dsi}.c).
 */
#define TYPE_MSC313_DISP "mstar-msc313-disp"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313DispState, MSC313_DISP)

#define MSTAR_DISP_GOP_NUM_REGS (0x400 / 4)
#define MSTAR_DISP_TOP_NUM_REGS (0x200 / 4)
#define MSTAR_DISP_MOP_NUM_REGS (0x600 / 4)
#define MSTAR_DISP_DSI_NUM_REGS (0x400 / 4)
#define MSTAR_DISP_GE_NUM_REGS  (0x200 / 4)

struct Msc313DispState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion gop;       /* gop1 primary plane registers @0x1f246800 */
    MemoryRegion top;       /* display-top registers        @0x1f225000 */
    MemoryRegion mop;       /* mopg overlay (video) plane   @0x1f280a00 */
    MemoryRegion dsi;       /* MIPI DSI controller          @0x1f345200 */
    MemoryRegion ge;        /* GE 2D graphics engine        @0x1f281200 */
    qemu_irq irq;           /* display-top vsync interrupt (SPI 82) */
    qemu_irq gop_irq;       /* GOP/fbdev vsync interrupt (SPI 52) */
    QemuConsole *con;
    QEMUTimer *vblank;
    QEMUTimer *gop_off;     /* lowers gop_irq shortly after each vblank pulse */
    MemoryRegionSection fbsection;
    Msc313PwmState *backlight;   /* PWM whose channel 0 dims the panel */
    unsigned int brightness;     /* backlight level, 0..256 */
    uint16_t gopregs[MSTAR_DISP_GOP_NUM_REGS];
    uint16_t topregs[MSTAR_DISP_TOP_NUM_REGS];
    uint16_t mopregs[MSTAR_DISP_MOP_NUM_REGS];
    uint32_t dsiregs[MSTAR_DISP_DSI_NUM_REGS];
    uint16_t geregs[MSTAR_DISP_GE_NUM_REGS];
    uint32_t width, height;
    bool invalidate;
    bool flip;              /* panel mounted 180deg (Miyoo Mini): rotate output */
};

/*
 * The "dphy": the SSD20xD MIPI D-PHY between the DSI controller and the panel
 * (dphy@2a5000). A deliberately minimal model - it stores/returns 16-bit
 * registers so a real Linux PHY driver can be written and exercised against it.
 */
#define TYPE_MSTAR_DPHY "mstar-ssd20xd-dphy"
OBJECT_DECLARE_SIMPLE_TYPE(MstarDphyState, MSTAR_DPHY)

#define MSTAR_DPHY_NUM_REGS (0x200 / 4)

struct MstarDphyState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint16_t regs[MSTAR_DPHY_NUM_REGS];
};

/*
 * The "i2c": the mstar/sstar HWI2C master (i2c@223000/223200). A QEMU I2CBus
 * is exposed so slaves can be attached (none by default, so transfers NAK).
 * This lets the vendor kernel's i2c driver complete its transfers (setting the
 * done flag) instead of polling forever.
 */
#define TYPE_MSC313_I2C "mstar-msc313-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313I2cState, MSC313_I2C)

#define MSTAR_I2C_NUM_REGS (0x200 / 4)
#define MSTAR_NUM_I2C 2

struct Msc313I2cState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    I2CBus *bus;
    uint16_t regs[MSTAR_I2C_NUM_REGS];
    bool int_pending;       /* INT_CTL bit0: a byte/command completed */
    bool nak;               /* last byte was not acked (no slave) */
    bool active;            /* a transfer is in progress on the bus */
    bool start_pending;     /* START seen; next WDATA is the address */
    uint8_t rdata;          /* last byte clocked in from the bus */
};

/*
 * Dummy "security element" i2c slave (Miyoo Mini, i2c1 address 0x3d). The
 * vendor kernel has a client driver that talks to a small auth/security chip
 * and dereferences a NULL pointer (killing init) when the chip does not answer.
 * This slave just ACKs every transfer and returns a canned response so the
 * driver's probe succeeds and the board boots. It models no real crypto.
 */
#define TYPE_MSTAR_SECELEM "mstar-secelem"
#define TYPE_MSTAR_CAM_SENSOR "mstar-cam-sensor"
/* Sony IMX323: a preset of mstar-cam-sensor (16-bit regs, chip-id 0x301c=0x50). */
#define TYPE_IMX323 "imx323"
OBJECT_DECLARE_SIMPLE_TYPE(MstarSecElemState, MSTAR_SECELEM)

#define MSTAR_SECELEM_BUFSZ 64

struct MstarSecElemState {
    /*< private >*/
    I2CSlave parent_obj;
    /*< public >*/
    bool reading;                       /* current transfer is master-read */
    unsigned cmd_len;                   /* bytes written by the host */
    uint8_t cmd[MSTAR_SECELEM_BUFSZ];
    unsigned resp_pos;                  /* next response byte to hand back */

    /* Emulated challenge-response crypto state (see mstar_secelem.c). */
    uint8_t ac1;                        /* mirror of the host buf_ac[1] counter */
    bool have_e9;                       /* a challenge was written to reg 0xe9 */
    bool have_87;                       /* a challenge was written to reg 0x87 */
    uint8_t tgt_e9[8];                  /* captured arg0 from the 0xe9 write */
    uint8_t tgt_87[8];                  /* captured arg0 from the 0x87 write */
    uint8_t resp[16];                   /* prepared response for the next read */
    unsigned resp_len;
};

/*
 * The "bach" audio controller (mstar,msc313-bach): the SoC audio block plus
 * its DMA sub-channels and analog codec, alongside the separate "audiotop"
 * syscon it uses for the analog codec registers. This is a scaffold for
 * reverse engineering / developing the Linux sound driver (sound/soc/mstar/
 * msc313-bach.c): it just stores and returns register values and logs every
 * access via mstar_iolog(), so the driver's programming can be captured.
 */
#define TYPE_MSC313_BACH "mstar-msc313-bach"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313BachState, MSC313_BACH)

#define MSTAR_BACH_NUM_REGS      (0x600 / 4)
#define MSTAR_AUDIOTOP_NUM_REGS  (0x200 / 4)

struct Msc313BachState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;     /* bach controller  @0x1f2a0400 */
    MemoryRegion atop;      /* audiotop syscon  @0x1f206800 */
    qemu_irq irq;
    uint16_t regs[MSTAR_BACH_NUM_REGS];
    uint16_t atopregs[MSTAR_AUDIOTOP_NUM_REGS];

    /* QEMU audio output for the DMA-reader (playback) sub-channel. */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    bool play_active;       /* guest has the reader sub-channel enabled */
    bool voice_on;          /* backend voice is currently active */
    uint32_t play_wptr;     /* guest write pointer within the DRAM ring */
    GByteArray *pcm;        /* PCM snapshotted from the ring, pending playout */
    unsigned pcm_rdpos;     /* consume offset into pcm */
    bool irq_pending;       /* reader underrun/empty IRQ asserted, awaiting ack */
    bool irq_armed;         /* level rose above threshold; a new IRQ may fire */
};

/*
 * Physical memory map shared by the MStar/SigmaStar Armv7 SoCs. The on-chip
 * peripherals live inside the "riu" register bus at 0x1f000000; DRAM is
 * mapped at 0x20000000.
 */
#define MSTAR_RIU_BASE          0x1f000000
#define MSTAR_PM_UART_BASE      (MSTAR_RIU_BASE + 0x221000)
#define MSTAR_PM_UART_REGSHIFT  3
#define MSTAR_PM_UART_CLK       172000000
#define MSTAR_PM_UART_HWIRQ     34      /* line on the "irq" mst-intc */

#define MSTAR_DRAM_BASE         0x20000000

/* On-chip "IMI" SRAM (sram@a0000000); the boot ROM loads the IPL here. */
#define MSTAR_IMI_BASE          0xa0000000
#define MSTAR_IMI_SIZE          0x20000

/* The mask boot ROM, mapped at address 0 and loaded via -bios. */
#define MSTAR_BOOTROM_BASE      0x0
#define MSTAR_BOOTROM_SIZE      0x8000

/*
 * The "smpctrl" secondary-CPU boot mailbox (mstar,smpctrl), present on the
 * multi-core SoCs (infinity2m/pioneer3). The kernel writes CPU1's entry
 * address then an unlock magic to release it; see arch/arm/mach-mstar.
 */
#define MSTAR_SMPCTRL_BASE          (MSTAR_RIU_BASE + 0x204000)
#define MSTAR_SMPCTRL_SIZE          0x200

/* CPU PLL register bank (vendor 0x103200 base, at the RIU 2x stride). */
#define MSTAR_CPUPLL_BASE           (MSTAR_RIU_BASE + 0x206400)
#define MSTAR_CPUPLL_SIZE           0x200

/* The "did" chip block (did@7000); holds the boot-media strap the ROM reads. */
#define MSTAR_DID_BASE          (MSTAR_RIU_BASE + 0x7000)

/* The "chipid" register (chip@3c00); the mask-ROM IPL reads it as "D-<id>". */
#define MSTAR_CHIPID_BASE       (MSTAR_RIU_BASE + 0x3c00)
#define MSTAR_CHIPID_SIZE       0x200

/* The MIU DDR controller (miu@202000); the IPL's memory BIST lives here. */
#define MSTAR_MIU_BASE          (MSTAR_RIU_BASE + 0x202000)
#define MSTAR_MIU_SIZE          0x1000

/*
 * The "chiptop" pin-mux / chip-strap block (pinctrl@203c00). The IPL reads the
 * package "BOND" strap at +0x120 (0x1f203d20) to identify the chip variant.
 */
#define MSTAR_CHIPTOP_BASE      (MSTAR_RIU_BASE + 0x203c00)
#define MSTAR_CHIPTOP_SIZE      0x400
#define MSTAR_CHIPTOP_BOND      0x120

/* The "emac" 10/100 Ethernet MAC (emac@2a2000), on the 16-bit RIU bus. */
#define MSTAR_EMAC_BASE         (MSTAR_RIU_BASE + 0x2a2000)
#define MSTAR_EMAC_SIZE         0x1000
#define MSTAR_EMAC_HWIRQ        26      /* "irq" mst-intc line 26 (from the DT) */

/*
 * The integrated Ethernet PHY register block (phys 0x1f006000). The vendor
 * camera kernel does not do real MDIO for the internal PHY: it reads each PHY
 * register directly from a table at offset 0x200 + reg*4 (writing bit2 of
 * 0x204 to refresh), so the PHY has to appear here for the driver's PHY scan
 * (which looks for a non-zero, non-0xffff BMSR) to find it.
 */
#define MSTAR_EMACPHY_BASE      (MSTAR_RIU_BASE + 0x006000)
#define MSTAR_EMACPHY_SIZE      0x1000
#define MSTAR_EMACPHY_TABLE     0x200   /* PHY register table: reg N at +N*4 */

/* The "clkgen" clock mux/gate block (reg = <0x207000 0x200>). */
#define MSTAR_CLKGEN_BASE           (MSTAR_RIU_BASE + 0x207000)
#define MSTAR_CLKGEN_SIZE           0x200
#define MSTAR_PINCTRL_BASE          (MSTAR_RIU_BASE + 0x203c00)
#define MSTAR_PINCTRL_SIZE          0x200

/* The "sdio" FCIE SD/MMC host controller (sdio@282000). */
#define MSTAR_SDIO_BASE             (MSTAR_RIU_BASE + 0x282000)
#define MSTAR_SDIO_SIZE             0x200
#define MSTAR_SDIO_HWIRQ            19      /* line on the "irq" mst-intc */

/* The "pwm" controller (pwm@3400); channel 0 is the panel backlight. */
#define MSTAR_PWM_BASE              (MSTAR_RIU_BASE + 0x3400)
#define MSTAR_PWM_SIZE              0x400

/* The "disp" pipeline: gop1 primary plane + display-top (SSD20xD). */
#define MSTAR_DISP_GOP_BASE         (MSTAR_RIU_BASE + 0x246800)
#define MSTAR_DISP_GOP_SIZE         0x400
#define MSTAR_DISP_TOP_BASE         (MSTAR_RIU_BASE + 0x225000)
#define MSTAR_DISP_TOP_SIZE         0x200
#define MSTAR_DISP_MOP_BASE         (MSTAR_RIU_BASE + 0x280a00)
#define MSTAR_DISP_MOP_SIZE         0x600
#define MSTAR_DISP_DSI_BASE         (MSTAR_RIU_BASE + 0x345200)
#define MSTAR_DISP_GE_BASE          (MSTAR_RIU_BASE + 0x281200)
#define MSTAR_DISP_GE_SIZE          0x200
#define MSTAR_DISP_DSI_SIZE         0x400
#define MSTAR_DISP_HWIRQ            50      /* display-top vsync (SPI 82), "irq" intc */
#define MSTAR_DISP_GOP_HWIRQ       20      /* GOP/fbdev vsync (SPI 52), "irq" intc */

/* The "bach" audio controller + its "audiotop" syscon (mstar,msc313-bach). */
#define MSTAR_BACH_BASE            (MSTAR_RIU_BASE + 0x2a0400)
#define MSTAR_BACH_SIZE            0x600
#define MSTAR_AUDIOTOP_BASE        (MSTAR_RIU_BASE + 0x206800)
#define MSTAR_AUDIOTOP_SIZE        0x200
#define MSTAR_BACH_HWIRQ           42      /* audio DMA, "irq" intc */

/* The MIPI D-PHY for the DSI link (dphy@2a5000, vendor "DPHY_DSI" bank). */
#define MSTAR_DPHY_BASE             (MSTAR_RIU_BASE + 0x2a5000)
#define MSTAR_DPHY_SIZE             0x200

/* The two HWI2C masters (i2c@223000 / i2c@223200); MSTAR_NUM_I2C is above. */
#define MSTAR_I2C0_BASE             (MSTAR_RIU_BASE + 0x223000)
#define MSTAR_I2C1_BASE             (MSTAR_RIU_BASE + 0x223200)
#define MSTAR_I2C_SIZE              0x200

/* GIC (arm,cortex-a7-gic), with 128 SPIs. */
#define MSTAR_GIC_NUM_SPI       128
#define MSTAR_GIC_DIST_BASE     0x16001000
#define MSTAR_GIC_CPU_BASE      0x16002000
#define MSTAR_GIC_HYP_BASE      0x16004000
#define MSTAR_GIC_VCPU_BASE     0x16006000

/* Architected timer frequency, from the arm,armv7-timer clock-frequency. */
#define MSTAR_ARCH_TIMER_FREQ   6000000

/* Architected timer PPIs, from the arm,armv7-timer node. */
#define MSTAR_GIC_PPI_HYPTIMER  10
#define MSTAR_GIC_PPI_VIRTTIMER 11
#define MSTAR_GIC_PPI_SECTIMER  13
#define MSTAR_GIC_PPI_PHYSTIMER 14

/*
 * The two mst-intc instances, from the mstar,irqs-map-range in the DT:
 * "irq" forwards to GIC SPI 32..95, "fiq" to GIC SPI 96..127.
 */
#define MSTAR_INTC_IRQ_BASE     (MSTAR_RIU_BASE + 0x201350)
#define MSTAR_INTC_IRQ_START    32
#define MSTAR_INTC_IRQ_NUM      64
#define MSTAR_INTC_FIQ_BASE     (MSTAR_RIU_BASE + 0x201310)
#define MSTAR_INTC_FIQ_START    96
#define MSTAR_INTC_FIQ_NUM      32

/*
 * The "l3bridge" MIU write-flush block. mstarv7_mb() triggers a flush and
 * spins on the STATUS DONE bit; since QEMU's memory is coherent the flush is
 * a no-op and STATUS always reports done.
 */
#define MSTAR_L3BRIDGE_BASE         (MSTAR_RIU_BASE + 0x204400)
#define MSTAR_L3BRIDGE_SIZE         0x200
#define MSTAR_L3BRIDGE_STATUS       0x40
#define MSTAR_L3BRIDGE_STATUS_DONE  (1 << 12)

/*
 * The msc313e-timer instances at 0x1f206040/80/c0, clocked from xtal_div2
 * (12MHz). Their interrupts are lines 0, 1 and 12 of the "fiq" mst-intc.
 */
#define MSTAR_TIMER_BASE            (MSTAR_RIU_BASE + 0x6040)
#define MSTAR_TIMER_STRIDE          0x40
#define MSTAR_TIMER_FREQ            12000000

/*
 * The "msc313-rtc" at 0x1f002400 (reg = <0x2400 0x40> on the riu bus). Its
 * alarm interrupt is line 44 of the "irq" mst-intc.
 */
#define MSTAR_RTC_BASE              (MSTAR_RIU_BASE + 0x2400)
#define MSTAR_RTC_SIZE              0x40
#define MSTAR_RTC_HWIRQ             44

/*
 * The "sar" SAR ADC (sar@2800, reg = <0x2800 0x200>). Its conversion-done
 * interrupt is line 45 of the "irq" mst-intc.
 */
#define MSTAR_SAR_BASE              (MSTAR_RIU_BASE + 0x2800)
#define MSTAR_SAR_SIZE              0x200
#define MSTAR_SAR_HWIRQ             45

/* The "msc313-gpio" pad register bank (reg = <0x207800 0x200>). */
#define MSTAR_GPIO_BASE             (MSTAR_RIU_BASE + 0x207800)
#define MSTAR_GPIO_SIZE             MSTAR_GPIO_NUM_REGS

/* The "pm_gpio" bank (gpio_pm@1e00): PM-domain pads incl. the SD card-detect. */
#define MSTAR_PM_GPIO_BASE          (MSTAR_RIU_BASE + 0x001e00)
#define MSTAR_PM_GPIO_SIZE          0x200

/* The "fsp" flash controller (the ISP block's second register window). */
#define MSTAR_FSP_BASE          (MSTAR_RIU_BASE + 0x2c00)
#define MSTAR_FSP_SIZE          0x200

/*
 * The "isp" SPI-NOR controller: core regs @0x1f001000, qspi config regs
 * @0x1f002e00, and a 16 MiB memory-mapped XIP read window @0x14000000.
 */
#define MSTAR_ISP_BASE              (MSTAR_RIU_BASE + 0x1000)
#define MSTAR_ISP_SIZE              0x400
#define MSTAR_ISP_QSPI_BASE         (MSTAR_RIU_BASE + 0x2e00)
#define MSTAR_ISP_QSPI_SIZE         0x200
#define MSTAR_ISP_XIP_BASE          0x14000000
#define MSTAR_ISP_XIP_SIZE          0x1000000

/*
 * The "bdma" engine (reg = <0x200400 0x80>). Its two channels interrupt on
 * lines 40 and 41 of the "irq" mst-intc.
 */
#define MSTAR_BDMA_BASE             (MSTAR_RIU_BASE + 0x200400)
#define MSTAR_BDMA_SIZE             0x80
#define MSTAR_BDMA_CH0_HWIRQ        40
#define MSTAR_BDMA_CH1_HWIRQ        41

#endif /* HW_ARM_MSTAR_H */
