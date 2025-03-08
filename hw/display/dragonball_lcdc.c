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
#include "hw/display/dragonball_lcdc.h"
#include "migration/vmstate.h"
#include "hw/irq.h"

#include "ui/console.h"
#include "framebuffer.h"
#include "ui/pixel_ops.h"

#define DRAGBONBALL_LCDC_WIDTH(_s) (_s->lxmax)
#define DRAGBONBALL_LCDC_HEIGHT(_s) (_s->lymax + 1)

/*
 * 1-bit colour
 */
static void draw_line1_32(void *opaque, uint8_t *d, const uint8_t *s,
                          int width, int deststep)
{
    int i, j;
    assert(deststep == sizeof(uint32_t));


    for (i = 0; i < width; i += 8) {
        uint8_t pixels = *s++;
        for (j = 0; j < 8; j++) {
            //printf("%s:%d %d\n", __func__, __LINE__, i + j);
            bool pixel = (pixels & (1 << (7 - j))) ? true : false;
            uint32_t *rgbpixel = ((uint32_t *) d) + (i + j);

            if (pixel)
                *rgbpixel = rgb_to_pixel32(0x9c, 0x9c, 0x9c);
            else
                *rgbpixel = rgb_to_pixel32(0, 0x22, 0x66);
        }
    }
}

/*
 * 2-bit colour
 */
static void draw_line2_32(void *opaque, uint8_t *d, const uint8_t *s,
                          int width, int deststep)
{
   //printf("%s:%d\n", __func__, __LINE__);
}

/*
 * 4-bit
 */
static void draw_line4_32(void *opaque, uint8_t *d, const uint8_t *s,
                          int width, int deststep)
{
   //printf("%s:%d\n", __func__, __LINE__);
}


static inline int dragonball_lcdc_bpp(DragonBallLCDCState *s, drawfn *fn)
{
    switch(s->lpicf & DRAGONBALL_LCDC_LPICF_GS_MASK)
    {
        case 0:
	    if (fn)
                *fn = draw_line1_32;
            return 1;
        case 1:
	    if (fn)
                *fn = draw_line2_32;
	    return 2;
	case 2:
	    if (fn)
                *fn = draw_line4_32;
	    return 4;
	default:
	    return -EINVAL;
    }
}

static void dragonball_lcdc_updatefb_params(DragonBallLCDCState *s)
{
    unsigned int width = DRAGBONBALL_LCDC_WIDTH(s);
    unsigned int height = DRAGBONBALL_LCDC_HEIGHT(s);
    DisplaySurface *surface;

    surface = qemu_console_surface(s->con);
    if (width != surface_width(surface) ||
            height != surface_height(surface))
        qemu_console_resize(s->con, width, height);
}


static uint64_t dragonball_lcdc_read(void *opaque, hwaddr addr, unsigned size)
{
    DragonBallLCDCState *s = opaque;

    printf("%s:%d\n", __func__, __LINE__);
    switch(addr){
        case DRAGONBALL_LCDC_LSSA:
            return s->lssa;
        case DRAGONBALL_LCDC_LVPW:
            return s->lvpw;
        case DRAGONBALL_LCDC_LXMAX:
            return s->lxmax;
        case DRAGONBALL_LCDC_LYMAX:
            return s->lymax;
        case DRAGONBALL_LCDC_LCXP:
            return s->lcxp;
        case DRAGONBALL_LCDC_LCYP:
            return s->lcyp;
        case DRAGONBALL_LCDC_LCWCH:
            return s->lcwch;
        case DRAGONBALL_LCDC_LBLKC:
            return s->lblkc;
        case DRAGONBALL_LCDC_LPICF:
            return s->lpicf;
    }

    return 0;
}

static void dragonball_lcdc_write(void *opaque, hwaddr addr, uint64_t value,
        unsigned size)
{
    DragonBallLCDCState *s = opaque;

    printf("%s:%d 0x%08lx 0x%08lx\n", __func__, __LINE__, addr, value);
    switch(addr){
        case DRAGONBALL_LCDC_LSSA:
            s->lssa = value;
            break;
        case DRAGONBALL_LCDC_LVPW:
            s->lvpw = value;
            break;
        case DRAGONBALL_LCDC_LXMAX:
            s->lxmax = value;
            break;
        case DRAGONBALL_LCDC_LYMAX:
            s->lymax = value;
            break;
    }
}

static const MemoryRegionOps dragonball_lcdc_ops = {
    .read = dragonball_lcdc_read,
    .write = dragonball_lcdc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void dragonball_lcdc_reset(DeviceState *dev)
{
    DragonBallLCDCState *s = DRAGONBALL_LCDC(dev);

    s->lssa = 0;
    s->lvpw = ~0;
    s->lxmax = 0x3f0;
    s->lymax = 0x1ff;
    s->lcxp = 0;
    s->lcyp = 0;
    s->lcwch = 0x101;
    s->lblkc = 0x7f;
    s->lpicf = 0;
    s->lpolcf = 0;
    s->lacdrc = 0;
    s->lpxcd = 0;
    s->lckcon = 0;
    s->lrra = 0;
    s->lposr = 0;
    s->lfrcm = 0xb9;
    s->lgpmr = 0x84;
    s->pwmr = 0;
}

static void dragonball_update_display(void *opaque)
{
    DragonBallLCDCState *s = opaque;
    DisplaySurface *surface;
    drawfn fn = NULL;
    unsigned int width = DRAGBONBALL_LCDC_WIDTH(s);
    unsigned int height = DRAGBONBALL_LCDC_HEIGHT(s);
    unsigned int linewidth;
    unsigned int surfacebpp;
    int first = 0, last;
    int ret;

    ret = dragonball_lcdc_bpp(s, &fn);
    if (ret < 0)
	return;
    linewidth = (ret * width) / 8;

    dragonball_lcdc_updatefb_params(s);

    surface = qemu_console_surface(s->con);
    if (!(surfacebpp = surface_bits_per_pixel(surface)))
        return;


    framebuffer_update_memory_section(&s->fbsection,
                                      s->fbmem,
                                      s->lssa,
                                      height,
                                      linewidth);

    framebuffer_update_display(surface,
                               &s->fbsection,
                               width,
                               height,
                               linewidth,
                               width * 4,
                               surfacebpp / 8,
                               1,
                               fn,
                               NULL,
                               &first,
                               &last);

    dpy_gfx_update_full(s->con);
}

static void dragonball_invalidate_display(void * opaque)
{

}

static const GraphicHwOps dragonball_gfx_ops = {
    .invalidate  = dragonball_invalidate_display,
    .gfx_update  = dragonball_update_display,
};

static void dragonball_lcdc_realize(DeviceState *dev, Error **errp)
{
    DragonBallLCDCState *s = DRAGONBALL_LCDC(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &dragonball_lcdc_ops, s,
                          TYPE_DRAGONBALL_LCDC, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    s->con = graphic_console_init(dev, 0, &dragonball_gfx_ops, s);
}

static const VMStateDescription vmstate_dragonball_lcdc = {
    .name = "dragonball_lcdc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
            VMSTATE_END_OF_LIST()
        }
};

static const Property dragonball_lcdc_properties[] = {
    DEFINE_PROP_LINK("framebuffer-memory", DragonBallLCDCState, fbmem,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void dragonball_lcdc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->legacy_reset = dragonball_lcdc_reset;
    device_class_set_props(dc, dragonball_lcdc_properties);
    dc->realize = dragonball_lcdc_realize;
    dc->vmsd = &vmstate_dragonball_lcdc;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo dragonball_lcdc_info = {
    .name          = TYPE_DRAGONBALL_LCDC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DragonBallLCDCState),
    .class_init    = dragonball_lcdc_class_init,
};

static void dragonball_lcdc_register_types(void)
{
    type_register_static(&dragonball_lcdc_info);
}

type_init(dragonball_lcdc_register_types)
