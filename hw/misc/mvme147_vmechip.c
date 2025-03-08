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
#include "hw/misc/mvme147_vmechip.h"
#include "migration/vmstate.h"

static uint64_t mvme147_vmechip_read(void *opaque, hwaddr addr, unsigned size)
{
    MVME147VMEChipState *s = opaque;
    uint8_t val;

#if 0
    printf("vmechip read 0x%08x - %d\n", (unsigned int) addr, size);
#endif

    switch(addr) {
    case MVME147_VMECHIP_SYSCONTROLLER_CNFG:
    	return s->syscontroller_cnfg;
    case MVME147_VMECHIP_VMEBUS_REQ_CNFG:
    	return MVME147_VMECHIP_VMEBUS_REQ_CNFG_MASTER;
    case MVME147_VMECHIP_TIMER_CNFG:
    	return s->timer_cnfg;
    case MVME147_VMECHIP_MASTER_ADDR_MOD:
    	break;
    case MVME147_VMECHIP_BUS_ERR_STATUS:
    	val = s->bus_err_status;
    	/* Reading clears any set status bits */
    	s->bus_err_status = 0;
    	return val;
    default:
    	break;
    }

    return 0;
}

static void mvme147_vmechip_write(void *opaque, hwaddr addr, uint64_t value,
        unsigned size)
{
    MVME147VMEChipState *s = opaque;
#if 0
    printf("vmechip write 0x%08x - %d - 0x%08x\n",
    		(unsigned int) addr, size, (unsigned int) value);
#endif

    switch(addr) {
    case MVME147_VMECHIP_SYSCONTROLLER_CNFG:
    	break;
    case MVME147_VMECHIP_VMEBUS_REQ_CNFG:
    	break;
    case MVME147_VMECHIP_TIMER_CNFG:
    	s->timer_cnfg = value;
    	break;
    case MVME147_VMECHIP_MASTER_ADDR_MOD:
		break;
    default:
    	break;
    }
}

static const MemoryRegionOps mvme147_vmechip_ops = {
    .read = mvme147_vmechip_read,
    .write = mvme147_vmechip_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void mvme147_vmechip_reset(DeviceState *dev)
{
	MVME147VMEChipState *s = MVME147_VMECHIP(dev);

	s->syscontroller_cnfg = MVME147_VMECHIP_SYSCONTROLLER_CNFG_SCON;
}

static void mvme147_vmechip_realize(DeviceState *dev, Error **errp)
{
    MVME147VMEChipState *s = MVME147_VMECHIP(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &mvme147_vmechip_ops, s, TYPE_MVME147_VMECHIP, 0x30);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static const VMStateDescription vmstate_mvme147_vmechip = {
    .name = "mvme147_vmechip",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
            VMSTATE_END_OF_LIST()
    }
};

//static Property mvme147_vmechip_properties[] = {
//    DEFINE_PROP_END_OF_LIST(),
//};

static void mvme147_vmechip_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->legacy_reset = mvme147_vmechip_reset;
//    device_class_set_props(dc, mvme147_vmechip_properties);
    dc->realize = mvme147_vmechip_realize;
    dc->vmsd = &vmstate_mvme147_vmechip;
}

static const TypeInfo mvme147_vmechip_info = {
    .name          = TYPE_MVME147_VMECHIP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MVME147VMEChipState),
    .class_init    = mvme147_vmechip_class_init,
};

static void mvme147_vmechip_register_types(void)
{
    type_register_static(&mvme147_vmechip_info);
}

type_init(mvme147_vmechip_register_types)
