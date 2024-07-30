/*
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/misc/mvme147_pcc.h"
#include "migration/vmstate.h"

static uint16_t mvme147_pcc_timer_get_count(MVME147PCCState *s, unsigned int which)
{
    ptimer_state *timer = s->timers[which];
    uint64_t count = ptimer_get_count(timer);
    uint16_t ticks = 0xffff - count;

    return ticks;
}

static uint64_t mvme147_pcc_read(void *opaque, hwaddr addr, unsigned size)
{
    MVME147PCCState *s = opaque;

#if 0
    printf("pcc read 0x%08x - %d\n", (unsigned int) addr, size);
#endif

    switch(addr) {
    case MVME147_PCC_TABLE_ADDRESS:
    	return s->table_address;
    case MVME147_PCC_DATA_ADDRESS:
    	return s->data_address;
    case MVME147_PCC_LINK:
    	return s->link;
    case MVME147_PCC_TIMER1_PRELOAD:
    	return s->timerN_preload[0];
    case MVME147_PCC_TIMER1_COUNT:
    	return mvme147_pcc_timer_get_count(s, 0);
    case MVME147_PCC_TIMER2_PRELOAD:
		return s->timerN_preload[1];
    case MVME147_PCC_TIMER2_COUNT:
		return mvme147_pcc_timer_get_count(s, 1);
    case MVME147_PCC_TIMER1_INT_CTRL:
    	return s->timerN_int_ctrl[0];
    case MVME147_PCC_TIMER1_CTRL:
    	return s->timerN_ctrl[0];
    case MVME147_PCC_TIMER2_INT_CTRL:
    	return s->timerN_int_ctrl[1];
    case MVME147_PCC_TIMER2_CTRL:
    	return s->timerN_ctrl[1];
    case MVME147_PCC_WDOG_TIMER_CTRL:
    	return 0;
    case MVME147_PCC_PRINTER_INT_CTRL:
    	return 0;
    case MVME147_PCC_PRINTER_CTRL:
    	return 0;
    case MVME147_PCC_DMA_INT_CTRL:
    	return 0;
    case MVME147_PCC_BUS_ERROR_INT_CTRL:
    	return 0;
    case MVME147_PCC_AC_FAIL_INT_CTRL:
    	return 0;
    case MVME147_PCC_ABORT_INT_CTRL:
    	// zero is fine for now
    	return 0;
    case MVME147_PCC_GEN_PURPOSE_CTRL:
    	return s->gen_purpose_control;
    case MVME147_PCC_GEN_PURPOSE_STAT:
    	return s->gen_purpose_stat;
    case MVME147_PCC_SLAVE_BASE_ADDR:
		return 0;
    default:
    	break;
    }

    return 0;
}

static void mvme147_pcc_timer_update(MVME147PCCState *s, unsigned int which)
{
	ptimer_state *timer = s->timers[which];

	if (s->timerN_ctrl[which] & MVME147_PCC_TIMERN_CTRL_CLROVF) {
		s->timerN_ctrl[which] &= ~((MVME147_PCC_TIMERN_CTRL_OVF_MASK << MVME147_PCC_TIMERN_CTRL_OVF_SHIFT) |
                                    MVME147_PCC_TIMERN_CTRL_CLROVF);
		printf("cleared timer overflow\n");
	}

    ptimer_transaction_begin(timer);
    /* Configure the frequency and compare value */
    ptimer_set_freq(timer, 160000);
    ptimer_set_limit(timer, 0xffff, 1);

    /* Start or stop the timer */
    if (s->timerN_ctrl[which] & (MVME147_PCC_TIMERN_CTRL_ENABLE | MVME147_PCC_TIMERN_CTRL_ENACNT)) {
    	printf("starting timer %d\n", which);
        ptimer_run(timer, 0);
    }
    else
        ptimer_stop(timer);

    ptimer_transaction_commit(timer);
}

static void mvme147_pcc_write(void *opaque, hwaddr addr, uint64_t value,
        unsigned size)
{
    MVME147PCCState *s = opaque;

#if 0
    printf("pcc write 0x%08x - %d - 0x%08x\n",
    		(unsigned int) addr, size, (unsigned int) value);
#endif

    switch(addr) {
    case MVME147_PCC_TABLE_ADDRESS:
    	s->table_address = value;
    	break;
    case MVME147_PCC_DATA_ADDRESS:
    	s->data_address = value;
    	break;
    case MVME147_PCC_LINK:
    	s->link = value;
    	break;
    case MVME147_PCC_TIMER1_PRELOAD:
    	s->timerN_preload[0] = value;
    	break;
    case MVME147_PCC_TIMER2_PRELOAD:
    	s->timerN_preload[1] = value;
    	break;
    case MVME147_PCC_TIMER1_INT_CTRL:
    	s->timerN_int_ctrl[0] = value;
    	mvme147_pcc_timer_update(s, 0);
    	break;
    case MVME147_PCC_TIMER1_CTRL:
    	s->timerN_ctrl[0] = value;
    	mvme147_pcc_timer_update(s, 0);
    	break;
    case MVME147_PCC_TIMER2_INT_CTRL:
    	s->timerN_int_ctrl[1] = value;
    	mvme147_pcc_timer_update(s, 1);
    	break;
    case MVME147_PCC_TIMER2_CTRL:
    	s->timerN_ctrl[1] = value;
    	mvme147_pcc_timer_update(s, 1);
    	break;
    case MVME147_PCC_WDOG_TIMER_CTRL:
    	break;
    case MVME147_PCC_PRINTER_INT_CTRL:
    	break;
    case MVME147_PCC_PRINTER_CTRL:
    	break;
    case MVME147_PCC_DMA_INT_CTRL:
    	break;
    case MVME147_PCC_AC_FAIL_INT_CTRL:
    	break;
    case MVME147_PCC_DMA_CTRL_STAT:
    	if (value & MVME147_PCC_DMA_CTRL_STAT_ENABLE) {
    		printf("PCC: DMA enabled\n");
    	}
    	else
    		printf("PCC: DMA disabled\n");
    	break;
    case MVME147_PCC_BUS_ERROR_INT_CTRL:
    	break;
    case MVME147_PCC_ABORT_INT_CTRL:
    	if (value & MVME147_PCC_ABORT_INT_CTRL_INT_STAT)
    		printf("clear abort int\n");
    	break;
    case MVME147_PCC_TBL_AD_FUNC_CTRL:
    	break;
    case MVME147_PCC_SERIAL_PRT_INT_CTRL:
    	break;
    case MVME147_PCC_GEN_PURPOSE_CTRL:
    	s->gen_purpose_control = value;
    	break;
    case MVME147_PCC_LAN_INT_CTRL:
    	break;
    case MVME147_PCC_SCSI_PRT_INT_CTRL:
    	break;
    case MVME147_PCC_GEN_PURPOSE_STAT:
    	if (value & MVME147_PCC_GEN_PURPOSE_STAT_PARERR) {

    	}
    	if (value & MVME147_PCC_GEN_PURPOSE_STAT_PURESET) {
    		s->gen_purpose_stat &= ~MVME147_PCC_GEN_PURPOSE_STAT_PURESET;
    	}
    	break;
    case MVME147_PCC_SLAVE_BASE_ADDR:
		break;
    case MVME147_PCC_SW_INT1_CTRL:
    	break;
    case MVME147_PCC_INT_VECTOR_BASE:
    	break;
    case MVME147_PCC_SW_INT2_CTRL:
    	break;
    default:
        break;
    }
}

static const MemoryRegionOps mvme147_pcc_ops = {
    .read = mvme147_pcc_read,
    .write = mvme147_pcc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void mvme147_pcc_reset(DeviceState *dev)
{
    MVME147PCCState *s = MVME147_PCC(dev);

    s->gen_purpose_control = 0;
    s->gen_purpose_stat = MVME147_PCC_GEN_PURPOSE_STAT_PURESET;
}

static void mvme147_pcc_realize(DeviceState *dev, Error **errp)
{
    MVME147PCCState *s = MVME147_PCC(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &mvme147_pcc_ops, s, TYPE_MVME147_PCC, 0x30);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static const VMStateDescription vmstate_mvme147_pcc = {
    .name = "mvme147_pcc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
            VMSTATE_END_OF_LIST()
        }
};

static Property mvme147_pcc_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void mvme147_pcc_timer_overflow(MVME147PCCState *s, unsigned int which)
{
	unsigned int overflows = (s->timerN_ctrl[which] >> MVME147_PCC_TIMERN_CTRL_OVF_SHIFT) &
                              MVME147_PCC_TIMERN_CTRL_OVF_MASK;
	s->timerN_ctrl[which] &= ~(MVME147_PCC_TIMERN_CTRL_OVF_MASK << MVME147_PCC_TIMERN_CTRL_OVF_SHIFT);
	s->timerN_ctrl[which] |= ((overflows + 1) & MVME147_PCC_TIMERN_CTRL_OVF_MASK) << MVME147_PCC_TIMERN_CTRL_OVF_SHIFT;
}


static void mvme147_pcc_timer_cb_0(void *opaque)
{
	mvme147_pcc_timer_overflow(opaque, 0);
}

static void mvme147_pcc_timer_cb_1(void *opaque)
{
	mvme147_pcc_timer_overflow(opaque, 1);
}

static void mvme147_pcc_init(Object *obj)
{
	MVME147PCCState *s = MVME147_PCC(obj);

    s->timers[0] = ptimer_init(mvme147_pcc_timer_cb_0, s, 0);
    s->timers[1] = ptimer_init(mvme147_pcc_timer_cb_1, s, 0);
}

static void mvme147_pcc_finalize(Object *obj)
{
	MVME147PCCState *s = MVME147_PCC(obj);

    ptimer_free(s->timers[0]);
    ptimer_free(s->timers[1]);
}

static void mvme147_pcc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->legacy_reset = mvme147_pcc_reset;
    device_class_set_props(dc, mvme147_pcc_properties);
    dc->realize = mvme147_pcc_realize;
    dc->vmsd = &vmstate_mvme147_pcc;
}

static const TypeInfo mvme147_pcc_info = {
    .name              = TYPE_MVME147_PCC,
    .parent            = TYPE_SYS_BUS_DEVICE,
    .instance_size     = sizeof(MVME147PCCState),
    .instance_init     = mvme147_pcc_init,
    .instance_finalize = mvme147_pcc_finalize,
    .class_init        = mvme147_pcc_class_init,
};

static void mvme147_pcc_register_types(void)
{
    type_register_static(&mvme147_pcc_info);
}

type_init(mvme147_pcc_register_types)
