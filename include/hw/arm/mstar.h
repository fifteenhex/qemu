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
#include "chardev/char-fe.h"
#include "qemu/audio.h"
#include "qom/object.h"
#include "hw/display/mstar_gop.h"

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

/* Returns true the first time (addr, write) is seen, for one-shot tracing. */
bool mstar_iolog_first(hwaddr addr, bool write);

/*
 * SNAPSHOT / MIGRATION SUPPORT
 *
 * Every stateful device in this inventory carries a VMStateDescription so a
 * running machine can be snapshotted and fully restored - the firmware boots
 * take 45-120s of wall clock, so experiments restart from a snapshot instead:
 *
 *   qemu-img create -f qcow2 -b flash.bin -F raw overlay.qcow2
 *   qemu-system-arm -M miyoomini -drive if=mtd,format=qcow2,file=overlay.qcow2 ...
 *   (monitor) savevm booted          # after the firmware reaches the state
 *   ... later runs:  -loadvm booted  # or (monitor) loadvm booted
 *
 * The qcow2 overlay is required: savevm stores the VM state *and* the block
 * state as an internal snapshot, so every loadvm restores DRAM, devices and
 * flash to the same instant (raw + snapshot=on cannot do this). RAM regions
 * (DRAM, IMI, boot ROM) migrate automatically; only device structs need
 * descriptions.
 *
 * Rules when touching device state:
 *  - New state field => add it to the device's vmsd and bump version_id
 *    (old snapshots become invalid; that is expected and fine here).
 *  - Derived/transient state (consoles, framebuffer sections, audio voices,
 *    the ISP flash_cache) is NOT migrated: re-derive it in post_load (see
 *    mstar_gop.c / mstar_bach.c for the pattern).
 *  - Helper blocks that are not QOM devices (the mercury5 uart1/shim overlays
 *    in mstar_mercury5.c) use vmstate_register() with a fixed instance id.
 */

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

/*
 * The watchdog (watchdog@1f006000), common to the MStar Armv7 SoCs (declared
 * in mstar-v7.dtsi). See hw/watchdog/mstar_wdt.c.
 */
#define TYPE_MSTAR_WDT "mstar-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(MstarWdtState, MSTAR_WDT)

struct MstarWdtState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;               /* pre-timeout interrupt */
    QEMUTimer *reset_timer;     /* counter reached MAX_PRD -> reset the SoC */
    QEMUTimer *int_timer;       /* counter reached INT     -> pre-timeout irq */
    uint32_t freq;              /* input clock (xtal_div2, 12MHz) */
    uint16_t intr;              /* WDT_INT: pre-timeout threshold (top 16 bits) */
    uint16_t prd_l;             /* WDT_MAX_PRD_L */
    uint16_t prd_h;             /* WDT_MAX_PRD_H (period 0 = stopped) */
};

/*
 * URDMA (urdma@220600): the FUART's UART RX/TX ring-buffer DMA engine
 * (mstar,msc313-urdma). See hw/dma/mstar_urdma.c. Present family-wide but the
 * dashcam RTOS drives the FUART in plain-16550 PIO mode and never enables it;
 * this models the register/reset/interrupt semantics of the 6.5 kernel driver
 * plus a functional TX/RX ring data path (against its own optional chardev).
 */
#define TYPE_MSTAR_URDMA "mstar-urdma"
OBJECT_DECLARE_SIMPLE_TYPE(MstarUrdmaState, MSTAR_URDMA)

struct MstarUrdmaState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    CharFrontend chr;           /* optional UART backend for the DMA data path */
    uint16_t ctrl;             /* REG_CTRL   (0x00) */
    uint16_t threshold;        /* REG_INTR_THRESHOLD (0x04) */
    uint16_t tx_base_h, tx_base_l, tx_size, tx_rptr, tx_wptr, tx_timeout;
    uint16_t rx_base_h, rx_base_l, rx_size, rx_wptr, rx_timeout;
    uint16_t status;           /* REG_STATUS (0x34) */
    uint8_t rx_pending;        /* a byte latched from the backend, not yet DMAd */
    uint8_t rx_byte;
};

/*
 * Generic passive register bank (RAM-backed, read-after-write consistent), used
 * for RIU sub-blocks that only store/return configuration. See
 * hw/misc/mstar_regbank.c.
 */
#define TYPE_MSTAR_REGBANK "mstar-regbank"
OBJECT_DECLARE_SIMPLE_TYPE(MstarRegbankState, MSTAR_REGBANK)

struct MstarRegbankState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint8_t *store;             /* backing bytes, "size" long */
    uint32_t size;              /* region size (property) */
    bool readonly;             /* writes ignored (e.g. efuse) (property) */
    char *name;                 /* region/log name (property) */
};

/*
 * CMDQ - the command-queue engine at 0x1f224000 (DT "msc313-cmdq"), on
 * infinity/infinity2m/infinity3. Named store/read-back region while its
 * register layout is being mapped (MSTAR_CMDQ_DBG=1 logs accesses+PC). See
 * hw/misc/mstar_cmdq.c.
 */
#define TYPE_MSTAR_CMDQ "mstar-cmdq"
OBJECT_DECLARE_SIMPLE_TYPE(MstarCmdqState, MSTAR_CMDQ)

#define MSTAR_CMDQ_BASE (MSTAR_RIU_BASE + 0x224000)
#define MSTAR_CMDQ_SIZE 0x1000

struct MstarCmdqState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint8_t *store;
    uint32_t size;
};

/*
 * VIF - the sensor video-input interface (infinity3 csi@1f240800): the receiver
 * that clocks pixel data in from a MIPI or parallel image sensor and feeds the
 * ISP. See hw/misc/mstar_vif.c. Register file is store/read-back plus a 7-bit
 * interrupt block; mstar_vif_frame_irq() lets a future frame source raise a
 * per-frame VIF interrupt.
 */
#define TYPE_MSTAR_VIF "mstar-vif"
OBJECT_DECLARE_SIMPLE_TYPE(MstarVifState, MSTAR_VIF)

#define MSTAR_VIF_SIZE 0x200

struct MstarVifState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint8_t store[MSTAR_VIF_SIZE];  /* config registers (store/read-back) */
    uint8_t irq_raw;                /* 7-bit raw interrupt status */
    uint8_t irq_mask;               /* 7-bit interrupt mask (1 = masked off) */
};

void mstar_vif_frame_irq(MstarVifState *s, unsigned bits);

/*
 * On-die camera capture pipeline (scaler-DMA + ISP + HVSP scaler) with a timer
 * that fakes ~25fps frame delivery. Shared by infinity3 (MSC313E) and mercury5;
 * the three block base offsets (for the RIU tracer) and the ISP frame-counter
 * offset are properties since they differ between families. See
 * hw/misc/mstar_camcap.c. MMIO regions: 0 = scldma, 1 = isppoll, 2 = hvsp.
 * IRQs: 0 = scaler-DMA frame-done, 1 = image-ISP frame-done.
 */
#define TYPE_MSTAR_CAMCAP "mstar-camcap"
OBJECT_DECLARE_SIMPLE_TYPE(MstarCamCapState, MSTAR_CAMCAP)

struct MstarCamCapState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion scldma;        /* scaler-DMA capture      (0x200) */
    MemoryRegion isppoll;       /* ISP frame-counter poll  (0x4000) */
    MemoryRegion hvsp;          /* HVSP/SCL scaler         (0x2000) */
    qemu_irq scldma_irq;        /* SCLINTR / scaler-DMA frame-done */
    qemu_irq isp_img_irq;       /* image-ISP frame-done */
    QEMUTimer *timer;
    uint32_t scldma_base;       /* RIU offsets for the tracer (properties) */
    uint32_t isppoll_base;
    uint32_t hvsp_base;
    uint32_t framecnt_off;      /* isppoll offset of the frame counter */
    uint32_t scldma_status_off; /* scldma double-buffer status reg offset */
    uint32_t hvsp_istat1_off;   /* hvsp ISP frame-done status1 reg offset */
    uint32_t hvsp_clkhb_off;    /* hvsp scaler clock heartbeat reg offset */
    uint16_t scldma_regs[0x100];
    uint16_t isppoll_regs[0x2000];
    uint16_t hvsp_regs[0x1000];
    uint16_t scldma_status;
    uint16_t hvsp_hb;
    bool isp_frame_pending;
    uint32_t frame_count;       /* advanced once per fake captured frame */
    int frame_phase;            /* 0 = raise IRQs, 1 = lower IRQs */
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
    bool gpioi2c;           /* enable the bit-banged SCCB bus (camera SoCs) */
    /*
     * The camera firmware bit-bangs one Linux i2c-gpio bus on this pad bank
     * (cam.dtb "mstar,infinity-gpioi2c", SDA/SCL at +0x58/+0x5c) carrying the
     * module-ID EEPROM. A slave is attached to i2c_bus[] by the board. (The
     * sensor is on the hardware MIIC master, not a gpio bus.)
     */
#define MSTAR_GPIO_NUM_I2C 1
    void *bbi2c[MSTAR_GPIO_NUM_I2C];    /* bitbang_i2c_interface * per bus */
    void *i2c_bus[MSTAR_GPIO_NUM_I2C];  /* I2CBus * per bus (board attaches slaves) */
    int sda_level[MSTAR_GPIO_NUM_I2C];  /* current bus SDA (bit-bang readback) */
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
 * rtcpwc: RTC power/wake controller (DT sstar,infinity-rtcpwc). The RTC counter
 * is in an always-on domain read across an ISO handshake; see hw/rtc/mstar_rtcpwc.c.
 */
#define TYPE_MSTAR_RTCPWC "mstar-rtcpwc"
OBJECT_DECLARE_SIMPLE_TYPE(MstarRtcpwcState, MSTAR_RTCPWC)

#define MSTAR_RTCPWC_BASE (MSTAR_RIU_BASE + 0x006800)
#define MSTAR_RTCPWC_SIZE 0x200

struct MstarRtcpwcState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint16_t regs[MSTAR_RTCPWC_SIZE / 4];
    bool iso_ack;           /* current DIG2RTC/RTC2DIG ISO handshake ack */
    uint32_t base_seconds;  /* wall-clock seconds at reset */
    int64_t reset_ns;       /* virtual-clock ns at reset (counter epoch) */
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
    char *flash_model;      /* m25p80 chip model (property; def "n25q128a13") */
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
 * The "disp": the rest of the SSD20xD display pipeline around the GOP plane
 * (which is now the standalone TYPE_MSTAR_GOP device): the display-top vsync,
 * the mopg overlay/video plane, the MIPI DSI controller and the GE 2D engine.
 * Register layout is from the mstar DRM driver (drivers/gpu/drm/mstar/
 * mstar_{top,mop,dsi}.c).
 */
#define TYPE_MSC313_DISP "mstar-msc313-disp"
OBJECT_DECLARE_SIMPLE_TYPE(Msc313DispState, MSC313_DISP)

#define MSTAR_DISP_TOP_NUM_REGS (0x200 / 4)
#define MSTAR_DISP_MOP_NUM_REGS (0x600 / 4)
#define MSTAR_DISP_DSI_NUM_REGS (0x400 / 4)
#define MSTAR_DISP_GE_NUM_REGS  (0x200 / 4)

struct Msc313DispState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion top;       /* display-top registers        @0x1f225000 */
    MemoryRegion mop;       /* mopg overlay (video) plane   @0x1f280a00 */
    MemoryRegion dsi;       /* MIPI DSI controller          @0x1f345200 */
    MemoryRegion ge;        /* GE 2D graphics engine        @0x1f281200 */
    qemu_irq irq;           /* display-top vsync interrupt (SPI 82) */
    QemuConsole *con;
    QEMUTimer *vblank;
    uint16_t topregs[MSTAR_DISP_TOP_NUM_REGS];
    uint16_t mopregs[MSTAR_DISP_MOP_NUM_REGS];
    uint32_t dsiregs[MSTAR_DISP_DSI_NUM_REGS];
    uint16_t geregs[MSTAR_DISP_GE_NUM_REGS];
    uint32_t width, height;
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
/*
 * Array size / maximum number of HWI2C masters. msc313/ssd202 populate 2
 * (i2c@223000/223200); mercury5 has 4 (adds i2c@222a00/222c00). The actual
 * count per SoC is MStarSoCInfo.num_i2c (0 => the default 2).
 */
#define MSTAR_NUM_I2C 4

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
    bool dma_done;          /* DMA_TXR bit0: the DMA transfer completed */
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
 * Injoinic IP6303 PMIC (70mai dashcam, i2c0 address 0x30). A byte register
 * file with the handful of status bits the firmware's APK_IP6303_* HAL polls
 * (power key level, battery OK, charge state, VBAT ADC); see hw/i2c/ip6303.c.
 */
#define TYPE_SC7A30E "sc7a30e"
OBJECT_DECLARE_SIMPLE_TYPE(Sc7a30eState, SC7A30E)

struct Sc7a30eState {
    /*< private >*/
    I2CSlave parent_obj;
    /*< public >*/
    uint8_t regs[128];
    uint8_t ptr;                        /* register pointer (auto-increments) */
    bool have_ptr;
    bool motion;                        /* a motion/impact interrupt is pending */
    bool move_boot;                     /* property: arm motion at reset */
};

#define TYPE_IP6303 "ip6303"
OBJECT_DECLARE_SIMPLE_TYPE(Ip6303State, IP6303)

struct Ip6303State {
    /*< private >*/
    I2CSlave parent_obj;
    /*< public >*/
    uint8_t regs[256];
    uint8_t ptr;                        /* register pointer (auto-increments) */
    bool have_ptr;                      /* pointer byte seen this transfer */
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
    bool mercury5_reader;   /* use the mercury5 reader control-bit layout */
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

/* uart1 (serial@221200): second dw-apb-uart, the mercury5 kernel console. */
#define MSTAR_UART1_BASE        (MSTAR_RIU_BASE + 0x221200)
#define MSTAR_UART1_HWIRQ       35      /* line on the "irq" mst-intc */

/*
 * FUART (serial@220400): the "fast UART", a third dw-apb-uart (snps,dw-apb-uart,
 * reg-shift 3) present on every MStar variant. The mercury5 dashcam RTOS drives
 * it as a plain 16550 (it programmes LCR/DLL/FCR/IER and never touches the URDMA
 * below), so it is modelled with serial_mm on the third serial backend. Its
 * companion RX/TX ring-DMA engine is the URDMA at +0x200 (urdma@220600).
 */
#define MSTAR_FUART_BASE        (MSTAR_RIU_BASE + 0x220400)
#define MSTAR_FUART_REGSHIFT    3
#define MSTAR_FUART_CLK         172000000
#define MSTAR_FUART_HWIRQ       47      /* "irq" mst-intc line (DT: GIC_SPI 47) */

/* URDMA (urdma@220600): the FUART's RX/TX ring-buffer DMA engine. */
#define MSTAR_URDMA_BASE        (MSTAR_RIU_BASE + 0x220600)
#define MSTAR_URDMA_SIZE        0x100
#define MSTAR_URDMA_HWIRQ       48      /* "irq" mst-intc line (DT: GIC_SPI 48) */

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
 * The mainline DT puts the MAC at 0x2a2000, but the vendor camera driver
 * accesses the very same registers through a second RIU aperture at
 * 0x1f343c00 (its MHal_EMAC_* accessors read e.g. TSR at 0x1f343c28 = base +
 * 2*0x14). Alias the MAC there too so the vendor driver's register reads land.
 */
#define MSTAR_EMAC_ALT_BASE     (MSTAR_RIU_BASE + 0x343c00)

/*
 * The integrated Ethernet PHY register block (phys 0x1f006200). The vendor
 * camera kernel does not do real MDIO for the internal PHY: it reads each PHY
 * register directly from a table at the block base + reg*4 (writing bit2 of
 * +0x04 to refresh), so the PHY has to appear here for the driver's PHY scan
 * (which looks for a non-zero, non-0xffff BMSR) to find it.
 *
 * This block sits above the watchdog (+0x000) and the PIT timers (+0x040) in
 * the 0x1f006000 bank; each is its own non-overlapping region.
 */
#define MSTAR_EMACPHY_BASE      (MSTAR_RIU_BASE + 0x006200)
#define MSTAR_EMACPHY_SIZE      0x600
#define MSTAR_EMACPHY_TABLE     0x0     /* PHY register table: reg N at +N*4 */

/*
 * The watchdog (watchdog@1f006000, "mstar,msc313e-wdt"): its own region at the
 * bottom of the 0x1f006000 bank (0x1f006000..0x1f00603f, below the timers at
 * +0x040 and the emac-phy at +0x200). Pre-timeout interrupt on the "fiq"
 * mst-intc, line 2 (per mstar-v7.dtsi).
 */
#define MSTAR_WDT_BASE          (MSTAR_RIU_BASE + 0x006000)
#define MSTAR_WDT_SIZE          0x40
#define MSTAR_WDT_HWIRQ         2

/*
 * Read-only fuse array (efuse@4000, "mstar,msc313-efuse"): the guest only reads
 * it (chip straps/calibration); modelled as a read-only regbank (0 = no fuses
 * blown), ready to hold a real fuse dump.
 */
#define MSTAR_EFUSE_BASE        (MSTAR_RIU_BASE + 0x004000)
#define MSTAR_EFUSE_SIZE        0x100

/* Passive syscon/simple-mfd register bank (syscon@226600). */
#define MSTAR_SYSCON_BASE       (MSTAR_RIU_BASE + 0x226600)
#define MSTAR_SYSCON_SIZE       0x200

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
#define MSTAR_ISP_IMG_HWIRQ        25      /* image-ISP frame-done (GIC 89), "irq" intc */
/* VIF/CSI sensor video-input front-end (DT csi@1f240800; SPI 0x41 = GIC 97). */
#define MSTAR_VIF_BASE             (MSTAR_RIU_BASE + 0x240800)

/* The "bach" audio controller + its "audiotop" syscon (mstar,msc313-bach). */
#define MSTAR_BACH_BASE            (MSTAR_RIU_BASE + 0x2a0400)
#define MSTAR_BACH_SIZE            0x600
#define MSTAR_AUDIOTOP_BASE        (MSTAR_RIU_BASE + 0x206800)
#define MSTAR_AUDIOTOP_SIZE        0x200
#define MSTAR_BACH_HWIRQ           42      /* audio DMA, "irq" intc */

/* The MIPI D-PHY for the DSI link (dphy@2a5000, vendor "DPHY_DSI" bank). */
#define MSTAR_DPHY_BASE             (MSTAR_RIU_BASE + 0x2a5000)
#define MSTAR_DPHY_SIZE             0x200

/*
 * HWI2C masters. i2c@223000/223200 exist on all SoCs; mercury5 adds
 * i2c@222a00/222c00 (the DrvI2c "channels" 2/3 the camera sensor uses).
 */
#define MSTAR_I2C0_BASE             (MSTAR_RIU_BASE + 0x223000)
#define MSTAR_I2C1_BASE             (MSTAR_RIU_BASE + 0x223200)
#define MSTAR_I2C2_BASE             (MSTAR_RIU_BASE + 0x222a00)
#define MSTAR_I2C3_BASE             (MSTAR_RIU_BASE + 0x222c00)
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
