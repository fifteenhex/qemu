/*
 * Sony CLIE Memory Stick host controller (Fujitsu MB86189) -- Phase 1
 * "no card present" stub.  See the header for the design note.
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
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sony_mshc_stub.h"

#define MSHC_REGS_SIZE   0x14
#define MSHC_OFF_MSCMD   0x00
#define MSHC_OFF_MSCS    0x02
#define MSHC_OFF_MSDATA  0x04
#define MSHC_OFF_MSICS   0x06
#define MSHC_OFF_MSPPCD  0x08

/*
 * MSCS reset value on real silicon (EmRegsMB86189::ResetHostController):
 * RBE (recv buffer empty) set, no INT/DRQ/RST pending.  Presenting this
 * constantly means "controller idle, nothing to service" to any code
 * that peeks at it before checking the GPIO card-detect bit.
 */
#define MSHC_MSCS_IDLE   0x0a05

static uint64_t sony_mshc_stub_read(void *opaque, hwaddr offset, unsigned size)
{
    uint16_t val;

    switch (offset & ~1) {
    case MSHC_OFF_MSCS:
        val = MSHC_MSCS_IDLE;
        break;
    default:
        /* MSCMD/MSDATA/MSICS/MSPPCD and the unused tail all read 0 */
        val = 0;
        break;
    }

    if (size == 1) {
        return (offset & 1) ? (val & 0xff) : (val >> 8);
    }
    return val;
}

static void sony_mshc_stub_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    /*
     * Accept and drop every write.  This is a "no card" stub, not a
     * functional TPC engine (see MemoryStickStructs.h / EmRegsMB86189
     * for the real protocol) -- Phase 3 work per CLIE-RESEARCH.md.
     */
    qemu_log_mask(LOG_UNIMP,
                 "sony-clie-mshc-stub: write (size %d, offset 0x%"
                 HWADDR_PRIx ", value 0x%" PRIx64 ") ignored\n",
                 size, offset, value);
}

static const MemoryRegionOps sony_mshc_stub_ops = {
    .read = sony_mshc_stub_read,
    .write = sony_mshc_stub_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void sony_mshc_stub_realize(DeviceState *dev, Error **errp)
{
    SonyMshcStubState *s = SONY_MSHC_STUB(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sony_mshc_stub_ops, s,
                          TYPE_SONY_MSHC_STUB, MSHC_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void sony_mshc_stub_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sony_mshc_stub_realize;
    dc->desc = "Sony CLIE Memory Stick host controller (stub, no card)";
}

static const TypeInfo sony_mshc_stub_info = {
    .name          = TYPE_SONY_MSHC_STUB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SonyMshcStubState),
    .class_init    = sony_mshc_stub_class_init,
};

static void sony_mshc_stub_register_types(void)
{
    type_register_static(&sony_mshc_stub_info);
}

type_init(sony_mshc_stub_register_types)
