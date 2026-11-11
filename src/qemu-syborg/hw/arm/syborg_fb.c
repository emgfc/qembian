/*
 * Syborg framebuffer.
 *
 * Registers (word index): 0 ID, 1 BASE (guest physical; write raises BASE_UPDATE), 2 HEIGHT,
 * 3 WIDTH, 4 ORIENTATION (quarter turns clockwise from memory to screen, 0..3), 5 BLANK, 6 INT_MASK (1 VSYNC, 2 BASE_UPDATE), 7 INT_CAUSE (W1C),
 * 8 BPP, 9 COLOR_ORDER, 10 BYTE_ORDER, 11 PIXEL_ORDER, 12 ROW_PITCH, 13 ENABLED,
 * 14 PARTIAL_START, 15 PARTIAL_END (inclusive), 16 PARTIAL_MODE (bit 0 on, bit 1 full colour,
 *   bit 2 the band is a range of rows; clear = a range of columns),
 * 17 FRAME_COUNT (read: vsyncs since reset), 18 REFRESH_RATE (read: Hz, 0 = no vsync timer),
 * 0x100.. palette (256 entries).
 * WIDTH x HEIGHT is the screen (what the QEMU console shows). The picture in memory has that size
 * at ORIENTATION 0/2 and HEIGHT x WIDTH at 1/3; it is rotated onto the screen. This models a
 * display controller that rotates (the N95's natively landscape panel shows the portrait UI, and
 * in WSERV's native rotation, used by the screensaver, a landscape picture).
 * Partial mode models the N95 panel's partial-display power saving (screensaver): only the band
 * START..END (rows or columns of the picture in memory, as the display driver maps the panel rows
 * for the current rotation) is shown, everything else is black, and without the full-colour bit the band
 * has 8 colours (one bit per channel), like the panel's idle mode.
 * The Symbian driver programs 32 bpp, colour order 0 (little-endian 0x00RRGGBB words),
 * which is exactly the host x8r8g8b8 surface format, so rows are copied as they are.
 * VSYNC (INT_CAUSE bit 0) comes from a fixed-rate timer on the virtual clock (property refresh-rate,
 * Hz, default 60), like the panel's tearing-effect/refresh signal, not from the host display
 * refresh (which may be slower, faster or absent when headless); the N95 display stand-in paces
 * CDirectScreenBitmap::EndUpdate on it. BASE_UPDATE is still raised at the next host refresh.
 * Other depths are logged as unimplemented. A disabled or blanked panel keeps its picture, dimmed (property blank-dim, percent).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "syborg.h"

OBJECT_DECLARE_SIMPLE_TYPE(SyborgFbState, SYBORG_FB)

struct SyborgFbState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QemuConsole *con;
    uint32_t base, width, height, orientation, blank, bpp;
    uint32_t color_order, byte_order, pixel_order, row_pitch, enabled;
    uint32_t int_enable, int_status, need_int;
    uint32_t palette[256];
    bool warned;
    uint32_t blank_dim;       /* brightness (percent) of a blanked/disabled panel */
    uint32_t partial_start, partial_end, partial_mode;
    uint32_t refresh_rate;    /* vsync rate in Hz (0 = none) */
    uint32_t frame_count;
    int64_t next_vsync;       /* QEMU_CLOCK_VIRTUAL ns */
    QEMUTimer *vsync_timer;
    uint32_t *pic;            /* the picture in memory, processed before it is rotated (w*h words) */
};

static void syborg_fb_update_irq(SyborgFbState *s)
{
    qemu_set_irq(s->irq, (s->int_status & s->int_enable) != 0);
}

static void syborg_fb_vsync(void *opaque)
{
    SyborgFbState *s = opaque;
    int64_t period = NANOSECONDS_PER_SECOND / s->refresh_rate;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->frame_count++;
    s->int_status |= 1;
    syborg_fb_update_irq(s);
    /* fixed grid; if the VM was paused or starved, skip the missed frames instead of bursting */
    s->next_vsync += period;
    if (s->next_vsync <= now) {
        s->next_vsync = now + period - (now - s->next_vsync) % period;
    }
    timer_mod(s->vsync_timer, s->next_vsync);
}

static bool syborg_fb_update_display(void *opaque)
{
    SyborgFbState *s = opaque;
    DisplaySurface *surf;
    uint32_t old = s->int_status;

    if (s->need_int) {
        s->int_status |= 2;
        s->need_int = 0;
    }
    if (s->int_status != old) {
        syborg_fb_update_irq(s);
    }
    surf = qemu_console_surface(s->con);
    if (surface_width(surf) != (int)s->width || surface_height(surf) != (int)s->height) {
        qemu_console_resize(s->con, s->width, s->height);
        surf = qemu_console_surface(s->con);
    }
    if (!s->base || s->bpp != 32) {
        if (s->bpp != 32 && s->base && !s->warned) {
            qemu_log_mask(LOG_UNIMP, "syborg-fb: %u bpp not implemented\n", s->bpp);
            s->warned = true;
        }
        return true;
    }
    {
        /* picture size in memory: the screen size, swapped at a quarter-turn orientation */
        uint32_t rot = s->orientation & 3;
        uint32_t pw = (rot & 1) ? s->height : s->width;
        uint32_t ph = (rot & 1) ? s->width : s->height;
        uint32_t pitch = s->row_pitch ? s->row_pitch : pw * 4;
        uint8_t *dst = surface_data(surf);
        int dstride = surface_stride(surf);
        /* a blanked or disabled panel keeps its picture, just without backlight: dimmed */
        bool dim = !s->enabled || s->blank;
        bool partial = s->partial_mode & 1;
        bool eight = partial && !(s->partial_mode & 2);
        bool rows = s->partial_mode & 4;

        /* the host-composed screen when the guest composes on the host (syborg-gl), else guest memory */
        uint32_t hw = 0, hh = 0;
        const uint8_t *hpx = NULL;
        bool host = syborg_gl_frame(&hw, &hh, &hpx) && hw == pw && hh == ph;
        for (uint32_t y = 0; y < ph; y++) {
            uint32_t *row = rot ? s->pic + y * pw : (uint32_t *)(dst + y * dstride);
            if (host) {
                memcpy(row, hpx + (size_t)y * pw * 4, pw * 4);
            } else {
                address_space_read(&address_space_memory, s->base + y * pitch,
                                   MEMTXATTRS_UNSPECIFIED, row, pw * 4);
            }
            if (partial) {
                bool row_out = rows && (y < s->partial_start || y > s->partial_end);
                for (uint32_t x = 0; x < pw; x++) {
                    if (row_out || (!rows && (x < s->partial_start || x > s->partial_end))) {
                        row[x] = 0;
                    } else if (eight) {
                        uint32_t p = row[x];
                        row[x] = (p & 0x800000 ? 0xff0000 : 0) | (p & 0x8000 ? 0xff00 : 0) |
                                 (p & 0x80 ? 0xff : 0);
                    }
                }
            }
            if (dim) {
                for (uint32_t x = 0; x < pw; x++) {
                    uint32_t p = row[x];
                    row[x] = (((p >> 16) & 0xff) * s->blank_dim / 100) << 16 |
                             (((p >> 8) & 0xff) * s->blank_dim / 100) << 8 |
                             ((p & 0xff) * s->blank_dim / 100);
                }
            }
        }
        /* rotate onto the screen: screen pixel (x, y) shows picture pixel (px, py) */
        for (uint32_t y = 0; rot && y < s->height; y++) {
            uint32_t *out = (uint32_t *)(dst + y * dstride);
            for (uint32_t x = 0; x < s->width; x++) {
                uint32_t px, py;
                switch (rot) {
                case 1:  px = y;                py = ph - 1 - x;      break;  /* picture top -> right */
                case 2:  px = pw - 1 - x;       py = ph - 1 - y;      break;
                default: px = pw - 1 - y;       py = x;               break;  /* picture top -> left */
                }
                out[x] = s->pic[py * pw + px];
            }
        }
    }
    qemu_console_update(s->con, 0, 0, s->width, s->height);
    return true;
}

static void syborg_fb_invalidate(void *opaque)
{
}

static const GraphicHwOps syborg_fb_gfx_ops = {
    .invalidate = syborg_fb_invalidate,
    .gfx_update = syborg_fb_update_display,
};

static uint64_t syborg_fb_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgFbState *s = opaque;
    unsigned r = offset >> 2;

    if (r >= 0x100 && r < 0x200) {
        return s->palette[r - 0x100];
    }
    switch (r) {
    case 0: return SYBORG_ID_FB;
    case 1: return s->base;
    case 2: return s->height;
    case 3: return s->width;
    case 4: return s->orientation;
    case 5: return s->blank;
    case 6: return s->int_enable;
    case 7: return s->int_status;
    case 8: return s->bpp;
    case 9: return s->color_order;
    case 10: return s->byte_order;
    case 11: return s->pixel_order;
    case 12: return s->row_pitch;
    case 13: return s->enabled;
    case 14: return s->partial_start;
    case 15: return s->partial_end;
    case 16: return s->partial_mode;
    case 17: return s->frame_count;
    case 18: return s->refresh_rate;
    default: return 0;
    }
}

static void syborg_fb_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    SyborgFbState *s = opaque;
    unsigned r = offset >> 2;

    if (r >= 0x100 && r < 0x200) {
        s->palette[r - 0x100] = value;
        return;
    }
    switch (r) {
    case 1: s->base = value; s->need_int = 1; break;
    case 2: s->height = value; break;
    case 3: s->width = value; break;
    case 4: s->orientation = value & 3; break;
    case 5: s->blank = value; break;
    case 6: s->int_enable = value & 3; syborg_fb_update_irq(s); break;
    case 7: s->int_status &= ~value; syborg_fb_update_irq(s); break;
    case 8: s->bpp = value; break;
    case 9: s->color_order = value; break;
    case 10: s->byte_order = value; break;
    case 11: s->pixel_order = value; break;
    case 12: s->row_pitch = value; break;
    case 13: s->enabled = value != 0; break;
    case 14: s->partial_start = value; break;
    case 15: s->partial_end = value; break;
    case 16: s->partial_mode = value & 7; break;
    default: break;
    }
}

static const MemoryRegionOps syborg_fb_ops = {
    .read = syborg_fb_read,
    .write = syborg_fb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void syborg_fb_realize(DeviceState *dev, Error **errp)
{
    SyborgFbState *s = SYBORG_FB(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &syborg_fb_ops, s,
                          "syborg-fb", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->need_int = 1;
    s->pic = g_new0(uint32_t, (size_t)s->width * s->height);
    s->con = qemu_graphic_console_create(dev, 0, &syborg_fb_gfx_ops, s);
    qemu_console_resize(s->con, s->width, s->height);
    if (s->refresh_rate) {
        s->vsync_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, syborg_fb_vsync, s);
        s->next_vsync = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / s->refresh_rate;
        timer_mod(s->vsync_timer, s->next_vsync);
    }
}

static const VMStateDescription vmstate_syborg_fb = {
    .name = "syborg-fb",
    .version_id = 3,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(base, SyborgFbState),
        VMSTATE_UINT32(width, SyborgFbState),
        VMSTATE_UINT32(height, SyborgFbState),
        VMSTATE_UINT32(orientation, SyborgFbState),
        VMSTATE_UINT32(blank, SyborgFbState),
        VMSTATE_UINT32(bpp, SyborgFbState),
        VMSTATE_UINT32(color_order, SyborgFbState),
        VMSTATE_UINT32(byte_order, SyborgFbState),
        VMSTATE_UINT32(pixel_order, SyborgFbState),
        VMSTATE_UINT32(row_pitch, SyborgFbState),
        VMSTATE_UINT32(enabled, SyborgFbState),
        VMSTATE_UINT32(int_enable, SyborgFbState),
        VMSTATE_UINT32(int_status, SyborgFbState),
        VMSTATE_UINT32(need_int, SyborgFbState),
        VMSTATE_UINT32_ARRAY(palette, SyborgFbState, 256),
        VMSTATE_UINT32_V(partial_start, SyborgFbState, 2),
        VMSTATE_UINT32_V(partial_end, SyborgFbState, 2),
        VMSTATE_UINT32_V(partial_mode, SyborgFbState, 2),
        VMSTATE_UINT32_V(frame_count, SyborgFbState, 3),
        VMSTATE_INT64_V(next_vsync, SyborgFbState, 3),
        VMSTATE_END_OF_LIST()
    }
};

static const Property syborg_fb_properties[] = {
    DEFINE_PROP_UINT32("width", SyborgFbState, width, 240),
    DEFINE_PROP_UINT32("height", SyborgFbState, height, 320),
    /* like a panel with its backlight off: the picture stays, dimmed (0 = black) */
    DEFINE_PROP_UINT32("blank-dim", SyborgFbState, blank_dim, 25),
    /* panel refresh / vsync rate (N95: assumed 60 Hz, not measured) */
    DEFINE_PROP_UINT32("refresh-rate", SyborgFbState, refresh_rate, 60),
};

/* a machine reset: the panel as at power on (no frame buffer, no interrupts), the old picture gone; the
   size, rotation support and vsync (properties) stay */
static void syborg_fb_reset(DeviceState *dev)
{
    SyborgFbState *s = SYBORG_FB(dev);
    s->base = s->orientation = s->blank = s->bpp = 0;
    s->color_order = s->byte_order = s->pixel_order = s->row_pitch = s->enabled = 0;
    s->int_enable = s->int_status = 0;
    s->need_int = 1;
    memset(s->palette, 0, sizeof(s->palette));
    s->partial_start = s->partial_end = s->partial_mode = 0;
    s->frame_count = 0;
    if (s->pic) {
        memset(s->pic, 0, (size_t)s->width * s->height * 4);
    }
    syborg_fb_update_irq(s);
    syborg_fb_invalidate(s);
}

static void syborg_fb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = syborg_fb_realize;
    device_class_set_legacy_reset(dc, syborg_fb_reset);
    dc->vmsd = &vmstate_syborg_fb;
    device_class_set_props(dc, syborg_fb_properties);
}

static const TypeInfo syborg_fb_info = {
    .name = TYPE_SYBORG_FB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SyborgFbState),
    .class_init = syborg_fb_class_init,
};

static void syborg_fb_register_types(void)
{
    type_register_static(&syborg_fb_info);
}

type_init(syborg_fb_register_types)
