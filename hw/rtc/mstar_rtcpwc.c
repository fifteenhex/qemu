/*
 * MStar/SigmaStar "rtcpwc" - RTC power/wake controller.
 *
 * DT: sstar,infinity-rtcpwc, reg = <0x1f006800 0x200>. Present on infinity2m
 * (SSD20xD) and mercury5. The RTC counter lives in an always-on power domain
 * isolated from the digital (CPU) domain; the driver reads it across the
 * boundary with a DIG2RTC/RTC2DIG "ISO" handshake:
 *
 *   - it walks the ISO state machine S0..S5 by writing DIG2RTC_ISO_CTRL (0x0c,
 *     3 bits: 000,001,011,111,101,001) and after each step polls
 *     RTC2DIG_ISO_CTRL_ACK (0x20 bit3) for the RTC domain to acknowledge, with
 *     the expected ack ALTERNATING clear/set/clear/set/clear/set - i.e. the ack
 *     is the parity of the ISO_CTRL bits (see 6.5 drivers/sstar/rtc/ms_rtcpwc.c
 *     ms_rtc_ISOCTL()).
 *   - then it pulses CNT_RD (0x04 bit0) / CNT_RD_TRIG (0x38 bit0), waits for
 *     CNT2DIG_UPDATING (0x2c bit0) to clear, and reads the 32-bit seconds count
 *     from RDDATA_CNT_L/H (0x30/0x34): run_sec = (H << 16) | L.
 *
 * Modelled just enough for the firmware to read a running wall clock: answer the
 * ISO ack (parity), report the count as never-updating/valid, and return a live
 * seconds counter derived from the virtual clock. Register offset 0x48 in the
 * same bank is a power-source status the mercury5 firmware polls (bit2 = external
 * DC/ACC power present); report it present.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "hw/arm/mstar.h"

/* DIG2RTC (digital -> RTC) request registers. */
#define RTCPWC_DIG2RTC_CNT_RD           0x04    /* bit0: count-read enable */
#define RTCPWC_DIG2RTC_ISO_CTRL         0x0c    /* bits[2:0]: ISO state */
#define RTCPWC_ISO_CTRL_MASK            0x7
/* RTC2DIG (RTC -> digital) status registers. */
#define RTCPWC_RTC2DIG_VALID            0x1c    /* bit0: RTC time valid */
#define RTCPWC_RTC2DIG_VALID_BIT        (1 << 0)
#define RTCPWC_RTC2DIG_ISO_CTRL_ACK     0x20    /* bit3: ISO handshake ack */
#define RTCPWC_ISO_CTRL_ACK_BIT         (1 << 3)
#define RTCPWC_RTC2DIG_CNT_UPDATING     0x2c    /* bit0: count read in progress */
#define RTCPWC_RTC2DIG_CNT_UPDATING_BIT (1 << 0)
#define RTCPWC_RTC2DIG_RDDATA_CNT_L     0x30    /* seconds count [15:0] */
#define RTCPWC_RTC2DIG_RDDATA_CNT_H     0x34    /* seconds count [31:16] */
#define RTCPWC_DIG2RTC_CNT_RD_TRIG      0x38    /* bit0: trigger a count read */

/* Power-source status in the same bank (mercury5). bit2 = external power. */
#define RTCPWC_PWRSRC                   0x48
#define RTCPWC_PWRSRC_EXTPWR_BIT        (1 << 2)

static uint32_t mstar_rtcpwc_seconds(MstarRtcpwcState *s)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->reset_ns;

    return s->base_seconds + (uint32_t)(elapsed / NANOSECONDS_PER_SECOND);
}

static uint64_t mstar_rtcpwc_read(void *opaque, hwaddr addr, unsigned size)
{
    MstarRtcpwcState *s = opaque;

    switch (addr) {
    case RTCPWC_RTC2DIG_ISO_CTRL_ACK:
        return s->iso_ack ? RTCPWC_ISO_CTRL_ACK_BIT : 0;
    case RTCPWC_RTC2DIG_VALID:
        return RTCPWC_RTC2DIG_VALID_BIT;
    case RTCPWC_RTC2DIG_CNT_UPDATING:
        return 0;                       /* count is always immediately ready */
    case RTCPWC_RTC2DIG_RDDATA_CNT_L:
        return mstar_rtcpwc_seconds(s) & 0xffff;
    case RTCPWC_RTC2DIG_RDDATA_CNT_H:
        return (mstar_rtcpwc_seconds(s) >> 16) & 0xffff;
    case RTCPWC_PWRSRC:
        return RTCPWC_PWRSRC_EXTPWR_BIT;
    default:
        return s->regs[addr / 4];
    }
}

static void mstar_rtcpwc_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    MstarRtcpwcState *s = opaque;

    s->regs[addr / 4] = val;

    if (addr == RTCPWC_DIG2RTC_ISO_CTRL) {
        /*
         * The RTC domain acknowledges each ISO state transition; across the
         * S0..S5 sequence the ack alternates clear/set, which is exactly the
         * parity of the 3 ISO_CTRL bits the driver polls for.
         */
        s->iso_ack = ctpop32(val & RTCPWC_ISO_CTRL_MASK) & 1;
    }
}

static const MemoryRegionOps mstar_rtcpwc_ops = {
    .read = mstar_rtcpwc_read,
    .write = mstar_rtcpwc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_rtcpwc_reset_hold(Object *obj, ResetType type)
{
    MstarRtcpwcState *s = MSTAR_RTCPWC(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->iso_ack = false;
    /*
     * Seed the counter from host wall-clock time so the firmware reads a
     * plausible date, then advance it with the (deterministic) virtual clock.
     */
    s->base_seconds = qemu_clock_get_ns(QEMU_CLOCK_HOST) / NANOSECONDS_PER_SECOND;
    s->reset_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void mstar_rtcpwc_init(Object *obj)
{
    MstarRtcpwcState *s = MSTAR_RTCPWC(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_rtcpwc_ops, s,
                          "mstar.rtcpwc", MSTAR_RTCPWC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_mstar_rtcpwc = {
    .name = "mstar.rtcpwc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, MstarRtcpwcState, MSTAR_RTCPWC_SIZE / 4),
        VMSTATE_BOOL(iso_ack, MstarRtcpwcState),
        VMSTATE_UINT32(base_seconds, MstarRtcpwcState),
        VMSTATE_INT64(reset_ns, MstarRtcpwcState),
        VMSTATE_END_OF_LIST()
    },
};

static void mstar_rtcpwc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->vmsd = &vmstate_mstar_rtcpwc;
    rc->phases.hold = mstar_rtcpwc_reset_hold;
}

static const TypeInfo mstar_rtcpwc_info = {
    .name          = TYPE_MSTAR_RTCPWC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MstarRtcpwcState),
    .instance_init = mstar_rtcpwc_init,
    .class_init    = mstar_rtcpwc_class_init,
};

static void mstar_rtcpwc_register_types(void)
{
    type_register_static(&mstar_rtcpwc_info);
}

type_init(mstar_rtcpwc_register_types)
