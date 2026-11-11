/*
 * Syborg keyboard and pointer.
 *
 * Keyboard registers (word index): 0 ID, 1 DATA (pop or 0xffffffff), 2 FIFO_COUNT,
 * 3 INT_ENABLE, 4 FIFO_SIZE. Each FIFO word is a PC set-1 scancode; an 0xE0-prefixed key
 * has bit 7 set, a key release has bit 31 set.
 * With -global syborg-keyboard.keymap=FILE (lines "qcode-name value", generated from the device
 * profile by tools/device.py gen qemu-keymap) a mapped host key is delivered already translated:
 * bit 30 set and the value (a Symbian scan code) in bits 0..7; the guest EKEYB passes it through.
 * Unmapped keys keep the set-1 path.
 *
 * Pointer registers: 0 ID (absolute: 0xc51d0006), 1 LATCH (write pops the next event into
 * X/Y/Z/BUTTONS), 2 FIFO_COUNT, 3 X, 4 Y, 5 Z, 6 BUTTONS, 7 INT_ENABLE, 8 FIFO_SIZE.
 * Host mouse input only with -global syborg-pointer.input=on (default off: S60 3rd has no touch).
 * Absolute coordinates are 0..0x7fff (the QEMU input layer range, as in 0.9.1); buttons
 * 1 left, 2 right, 4 middle.
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
#include "migration/vmstate.h"
#include "ui/input.h"
#include "qapi/util.h"
#include "qapi/qapi-types-ui.h"
#include "qemu/error-report.h"
#include "qom/object.h"
#include "syborg.h"

#define INPUT_FIFO_MAX 64

/* ---- keyboard ---- */

OBJECT_DECLARE_SIMPLE_TYPE(SyborgKeyboardState, SYBORG_KEYBOARD)

struct SyborgKeyboardState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t fifo_size;
    uint32_t int_enabled;
    uint32_t count, head;
    uint32_t fifo[INPUT_FIFO_MAX];
    QemuInputHandlerState *hs;
    uint32_t held[8];         /* keys currently down (by 8-bit code): a keypad has no autorepeat */
    uint32_t held_x[8];       /* the same for translated keys */
    char *keymap;             /* device keymap file (qcode -> Symbian scan code), or NULL */
    uint16_t xlat[1024];      /* by linux keycode: 0x8000 | scan code */
};

#define KBD_TRANSLATED 0x40000000u

static void kbd_load_keymap(SyborgKeyboardState *s, Error **errp)
{
    g_autofree char *text = NULL;
    if (!g_file_get_contents(s->keymap, &text, NULL, NULL)) {
        error_setg(errp, "syborg-keyboard: cannot read keymap %s", s->keymap);
        return;
    }
    int n = 0;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char name[64];
        unsigned value;
        if (line[0] == '#' || sscanf(line, "%63s %i", name, (int *)&value) != 2) {
            continue;
        }
        int q = qapi_enum_parse(&QKeyCode_lookup, name, -1, NULL);
        if (q < 0 || q >= qemu_input_map_qcode_to_linux_len) {
            warn_report("syborg-keyboard: keymap: unknown qcode %s", name);
            continue;
        }
        unsigned lin = qemu_input_map_qcode_to_linux[q];
        if (lin && lin < ARRAY_SIZE(s->xlat)) {
            s->xlat[lin] = 0x8000 | (value & 0xff);
            n++;
        }
    }
    info_report("syborg-keyboard: %d keys translated from %s", n, s->keymap);
}

static void kbd_update(SyborgKeyboardState *s)
{
    qemu_set_irq(s->irq, s->count && s->int_enabled);
}

static void kbd_push(SyborgKeyboardState *s, uint32_t v)
{
    if (s->count < s->fifo_size) {
        s->fifo[(s->head + s->count) % s->fifo_size] = v;
        s->count++;
    }
    kbd_update(s);
}

static void kbd_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    SyborgKeyboardState *s = SYBORG_KEYBOARD(dev);
    unsigned kc = 0;
    uint32_t v;

    if (evt->key.key < ARRAY_SIZE(s->xlat) && (s->xlat[evt->key.key] & 0x8000)) {
        /* the device keymap: a Symbian scan code, one down and one up per press (as below) */
        v = s->xlat[evt->key.key] & 0xff;
        if (evt->key.down) {
            if (s->held_x[v >> 5] & (1u << (v & 31))) {
                return;
            }
            s->held_x[v >> 5] |= 1u << (v & 31);
            kbd_push(s, v | KBD_TRANSLATED);
        } else {
            s->held_x[v >> 5] &= ~(1u << (v & 31));
            kbd_push(s, v | KBD_TRANSLATED | 0x80000000);
        }
        return;
    }

    if (evt->key.key < qemu_input_map_linux_to_atset1_len) {
        kc = qemu_input_map_linux_to_atset1[evt->key.key];
    }
    if (!kc) {
        qemu_log_mask(LOG_UNIMP, "syborg-keyboard: no scancode for key %u\n",
                      evt->key.key);
        return;
    }
    v = (kc & 0x7f) | ((kc & 0xff00) ? 0x80 : 0);
    /* Host autorepeat (GTK/SDL send repeated key-downs while a key is held) would reach the guest
       as fresh presses and keep restarting WSERV's own repeat/long-press timing, so holding Menu
       never opened the task switcher. Like the phone's keypad, report one down and one up. */
    if (evt->key.down) {
        if (s->held[v >> 5] & (1u << (v & 31))) {
            return;
        }
        s->held[v >> 5] |= 1u << (v & 31);
    } else {
        s->held[v >> 5] &= ~(1u << (v & 31));
        v |= 0x80000000;
    }
    kbd_push(s, v);
}

static const QemuInputHandler kbd_handler = {
    .name = "syborg-keyboard",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = kbd_event,
};

static uint64_t kbd_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgKeyboardState *s = opaque;
    uint32_t v;

    switch (offset >> 2) {
    case 0: return SYBORG_ID_KEYBOARD;
    case 1:
        if (!s->count) {
            return 0xffffffff;
        }
        v = s->fifo[s->head];
        s->head = (s->head + 1) % s->fifo_size;
        s->count--;
        kbd_update(s);
        return v;
    case 2: return s->count;
    case 3: return s->int_enabled;
    case 4: return s->fifo_size;
    default: return 0;
    }
}

static void kbd_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgKeyboardState *s = opaque;

    if ((offset >> 2) == 3) {
        s->int_enabled = value & 1;
        kbd_update(s);
    }
}

static const MemoryRegionOps kbd_ops = {
    .read = kbd_read,
    .write = kbd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void kbd_realize(DeviceState *dev, Error **errp)
{
    SyborgKeyboardState *s = SYBORG_KEYBOARD(dev);

    if (s->fifo_size == 0 || s->fifo_size > INPUT_FIFO_MAX) {
        error_setg(errp, "syborg-keyboard: bad fifo-size");
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &kbd_ops, s, "syborg-keyboard", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    if (s->keymap && s->keymap[0]) {
        Error *err = NULL;
        kbd_load_keymap(s, &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }
    }
    s->hs = qemu_input_handler_register(dev, &kbd_handler);
}

static const VMStateDescription vmstate_kbd = {
    .name = "syborg-keyboard",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(int_enabled, SyborgKeyboardState),
        VMSTATE_UINT32(count, SyborgKeyboardState),
        VMSTATE_UINT32(head, SyborgKeyboardState),
        VMSTATE_UINT32_ARRAY(fifo, SyborgKeyboardState, INPUT_FIFO_MAX),
        VMSTATE_END_OF_LIST()
    }
};

static const Property kbd_properties[] = {
    DEFINE_PROP_UINT32("fifo-size", SyborgKeyboardState, fifo_size, 16),
    DEFINE_PROP_STRING("keymap", SyborgKeyboardState, keymap),
};

/* a machine reset: no queued or held keys, no interrupt (the keymap stays) */
static void kbd_reset(DeviceState *dev)
{
    SyborgKeyboardState *s = SYBORG_KEYBOARD(dev);
    s->int_enabled = 0;
    s->count = s->head = 0;
    memset(s->held, 0, sizeof(s->held));
    memset(s->held_x, 0, sizeof(s->held_x));
    kbd_update(s);
}

static void kbd_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = kbd_realize;
    device_class_set_legacy_reset(dc, kbd_reset);
    dc->vmsd = &vmstate_kbd;
    device_class_set_props(dc, kbd_properties);
}

/* ---- pointer ---- */

typedef struct SyborgPointerEvent {
    uint32_t x, y, z, buttons;
} SyborgPointerEvent;

OBJECT_DECLARE_SIMPLE_TYPE(SyborgPointerState, SYBORG_POINTER)

struct SyborgPointerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t fifo_size;
    bool absolute;
    bool input;                    /* receive host mouse events (off: S60 3rd has no touch) */
    uint32_t int_enabled;
    uint32_t count, head;
    SyborgPointerEvent cur;        /* latched into the registers */
    SyborgPointerEvent pending;    /* being assembled from input events */
    int32_t dz;
    SyborgPointerEvent fifo[INPUT_FIFO_MAX];
    QemuInputHandlerState *hs;
};

static void ptr_update(SyborgPointerState *s)
{
    qemu_set_irq(s->irq, s->count && s->int_enabled);
}

static void ptr_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    SyborgPointerState *s = SYBORG_POINTER(dev);
    static const uint32_t bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT] = 1,
        [INPUT_BUTTON_RIGHT] = 2,
        [INPUT_BUTTON_MIDDLE] = 4,
    };

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        if (evt->abs.axis == INPUT_AXIS_X) {
            s->pending.x = evt->abs.value;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            s->pending.y = evt->abs.value;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_WHEEL_UP && evt->btn.down) {
            s->dz--;
        } else if (evt->btn.button == INPUT_BUTTON_WHEEL_DOWN && evt->btn.down) {
            s->dz++;
        } else if (evt->btn.down) {
            s->pending.buttons |= bmap[evt->btn.button];
        } else {
            s->pending.buttons &= ~bmap[evt->btn.button];
        }
        break;
    default:
        break;
    }
}

static void ptr_sync(DeviceState *dev)
{
    SyborgPointerState *s = SYBORG_POINTER(dev);

    if (s->count < s->fifo_size) {
        SyborgPointerEvent e = s->pending;
        e.z = s->dz;
        s->fifo[(s->head + s->count) % s->fifo_size] = e;
        s->count++;
    }
    s->dz = 0;
    ptr_update(s);
}

static const QemuInputHandler ptr_handler = {
    .name = "syborg-pointer",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = ptr_event,
    .sync = ptr_sync,
};

static uint64_t ptr_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgPointerState *s = opaque;

    switch (offset >> 2) {
    case 0: return s->absolute ? SYBORG_ID_TOUCHSCREEN : SYBORG_ID_MOUSE;
    case 2: return s->count;
    case 3: return s->cur.x;
    case 4: return s->cur.y;
    case 5: return s->cur.z;
    case 6: return s->cur.buttons;
    case 7: return s->int_enabled;
    case 8: return s->fifo_size;
    default: return 0;
    }
}

static void ptr_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgPointerState *s = opaque;

    switch (offset >> 2) {
    case 1:
        if (s->count) {
            s->cur = s->fifo[s->head];
            s->head = (s->head + 1) % s->fifo_size;
            s->count--;
            ptr_update(s);
        }
        break;
    case 7:
        s->int_enabled = value & 1;
        ptr_update(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ptr_ops = {
    .read = ptr_read,
    .write = ptr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void ptr_realize(DeviceState *dev, Error **errp)
{
    SyborgPointerState *s = SYBORG_POINTER(dev);

    if (s->fifo_size == 0 || s->fifo_size > INPUT_FIFO_MAX) {
        error_setg(errp, "syborg-pointer: bad fifo-size");
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &ptr_ops, s, "syborg-pointer", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    if (s->input) {
        s->hs = qemu_input_handler_register(dev, &ptr_handler);
    }
}

static const VMStateDescription vmstate_ptr_event = {
    .name = "syborg-pointer-event",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(x, SyborgPointerEvent),
        VMSTATE_UINT32(y, SyborgPointerEvent),
        VMSTATE_UINT32(z, SyborgPointerEvent),
        VMSTATE_UINT32(buttons, SyborgPointerEvent),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_ptr = {
    .name = "syborg-pointer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(int_enabled, SyborgPointerState),
        VMSTATE_UINT32(count, SyborgPointerState),
        VMSTATE_UINT32(head, SyborgPointerState),
        VMSTATE_STRUCT(cur, SyborgPointerState, 1, vmstate_ptr_event, SyborgPointerEvent),
        VMSTATE_STRUCT_ARRAY(fifo, SyborgPointerState, INPUT_FIFO_MAX, 1,
                             vmstate_ptr_event, SyborgPointerEvent),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ptr_properties[] = {
    DEFINE_PROP_UINT32("fifo-size", SyborgPointerState, fifo_size, 16),
    DEFINE_PROP_BOOL("absolute", SyborgPointerState, absolute, true),
    /* the N95 (S60 3rd edition) has no touch screen: keep the device for the guest driver but
       do not take host mouse input unless asked (-global syborg-pointer.input=on) */
    DEFINE_PROP_BOOL("input", SyborgPointerState, input, false),
};

/* a machine reset: no queued events, nothing pressed, no interrupt */
static void ptr_reset(DeviceState *dev)
{
    SyborgPointerState *s = SYBORG_POINTER(dev);
    s->int_enabled = 0;
    s->count = s->head = 0;
    memset(&s->cur, 0, sizeof(s->cur));
    memset(&s->pending, 0, sizeof(s->pending));
    s->dz = 0;
    ptr_update(s);
}

static void ptr_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = ptr_realize;
    device_class_set_legacy_reset(dc, ptr_reset);
    dc->vmsd = &vmstate_ptr;
    device_class_set_props(dc, ptr_properties);
}

static const TypeInfo syborg_input_types[] = {
    {
        .name = TYPE_SYBORG_KEYBOARD,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SyborgKeyboardState),
        .class_init = kbd_class_init,
    }, {
        .name = TYPE_SYBORG_POINTER,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SyborgPointerState),
        .class_init = ptr_class_init,
    },
};

DEFINE_TYPES(syborg_input_types)
