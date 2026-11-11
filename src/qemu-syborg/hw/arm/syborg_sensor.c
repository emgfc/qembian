/*
 * Syborg accelerometer (ours): the phone's gravity vector, set by the host (the emulator window's Turn
 * view, sydb accel), read by the accelerometer stand-in (src/standins/accelerometer), which serves it to
 * the phone's own sensor stack (accssy.dll -> orientationssy -> automatic UI rotation).
 * docs/devices/nokia-n8-rm596/accelerometer.md.
 *
 * Registers (base + index * 4), read-only to the guest:
 *   0 ID (SYBORG_ID_SENSOR)   1 X   2 Y   3 Z (signed, device axes: x to the display's right edge, y to
 *   its top, z out of the display; milli-g, the reaction to gravity: upright portrait = 0, +1000, 0)
 *   4 SEQ (incremented on every change: the guest reads SEQ, X, Y, Z, SEQ and retries when it moved)
 *   5 INFO_LEN (248)   0x100..0x1f7 INFO: the driver information block the phone's sensor plug-in reads at
 *   open (Nokia's accelerometer channel control 0; accssy.dll needs bit 0 of byte 0x2a, "XYZ channel present").
 *   The block is the device profile's (`[board.accelerometer] info`, -global syborg-sensor.info=
 *   "OFFSET=HEXBYTES,..."), zeros elsewhere.
 * Host side: the QOM property "accel" = "x,y,z" (qom-set /machine/sensor accel "1000,0,0"), atomic.
 * The vector survives a machine reset (it is the phone's position, not device state).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "syborg.h"

enum { R_ID = 0, R_X = 1, R_Y = 2, R_Z = 3, R_SEQ = 4, R_INFO_LEN = 5 };
#define INFO_BASE 0x100
#define INFO_LEN  248

OBJECT_DECLARE_SIMPLE_TYPE(SyborgSensorState, SYBORG_SENSOR)

struct SyborgSensorState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    int32_t v[3];
    uint32_t seq;
    uint8_t info[INFO_LEN];
    char *info_spec;
};

static uint64_t syborg_sensor_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgSensorState *s = opaque;
    if (offset >= INFO_BASE && offset + 4 <= INFO_BASE + INFO_LEN) {
        const uint8_t *b = s->info + (offset - INFO_BASE);
        return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    }
    switch (offset >> 2) {
    case R_ID:  return SYBORG_ID_SENSOR;
    case R_X:   return (uint32_t)s->v[0];
    case R_Y:   return (uint32_t)s->v[1];
    case R_Z:   return (uint32_t)s->v[2];
    case R_SEQ: return s->seq;
    case R_INFO_LEN: return INFO_LEN;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-sensor: read of register 0x%x\n", (int)offset);
        return 0;
    }
}

static void syborg_sensor_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "syborg-sensor: write to register 0x%x\n", (int)offset);
}

static const MemoryRegionOps syborg_sensor_ops = {
    .read = syborg_sensor_read,
    .write = syborg_sensor_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static char *syborg_sensor_get_accel(Object *obj, Error **errp)
{
    SyborgSensorState *s = SYBORG_SENSOR(obj);
    return g_strdup_printf("%d,%d,%d", s->v[0], s->v[1], s->v[2]);
}

static void syborg_sensor_set_accel(Object *obj, const char *value, Error **errp)
{
    SyborgSensorState *s = SYBORG_SENSOR(obj);
    int x, y, z;
    char tail;
    if (sscanf(value, "%d,%d,%d%c", &x, &y, &z, &tail) != 3 ||
        abs(x) > 100000 || abs(y) > 100000 || abs(z) > 100000) {
        error_setg(errp, "syborg-sensor: accel wants \"x,y,z\" in milli-g (|v| <= 100000), not \"%s\"", value);
        return;
    }
    if (x != s->v[0] || y != s->v[1] || z != s->v[2]) {
        s->v[0] = x;
        s->v[1] = y;
        s->v[2] = z;
        s->seq++;
    }
}

static char *syborg_sensor_get_info(Object *obj, Error **errp)
{
    SyborgSensorState *s = SYBORG_SENSOR(obj);
    return g_strdup(s->info_spec ? s->info_spec : "");
}

/* "OFFSET=HEXBYTES,..." (hex offset, e.g. "2a=01"): bytes of the info block; the rest stays zero */
static void syborg_sensor_set_info(Object *obj, const char *value, Error **errp)
{
    SyborgSensorState *s = SYBORG_SENSOR(obj);
    uint8_t info[INFO_LEN] = { 0 };
    g_auto(GStrv) parts = g_strsplit(value, ",", -1);
    for (int i = 0; parts[i]; i++) {
        char *p = g_strstrip(parts[i]), *eq, *end;
        if (!*p) {
            continue;
        }
        eq = strchr(p, '=');
        unsigned long off = strtoul(p, &end, 16);
        if (!eq || end != eq || off >= INFO_LEN) {
            error_setg(errp, "syborg-sensor: info wants \"OFFSET=HEXBYTES,...\" (offset < 0x%x), not \"%s\"", INFO_LEN, p);
            return;
        }
        for (const char *h = eq + 1; *h; ) {
            if (*h == ' ') { h++; continue; }
            if (!g_ascii_isxdigit(h[0]) || !g_ascii_isxdigit(h[1]) || off >= INFO_LEN) {
                error_setg(errp, "syborg-sensor: info \"%s\": bad hex bytes or past the 0x%x-byte block", p, INFO_LEN);
                return;
            }
            info[off++] = g_ascii_xdigit_value(h[0]) << 4 | g_ascii_xdigit_value(h[1]);
            h += 2;
        }
    }
    memcpy(s->info, info, INFO_LEN);
    g_free(s->info_spec);
    s->info_spec = g_strdup(value);
}

static void syborg_sensor_init(Object *obj)
{
    SyborgSensorState *s = SYBORG_SENSOR(obj);
    s->v[0] = 0;
    s->v[1] = 1000;          /* held upright, portrait */
    s->v[2] = 0;
    memory_region_init_io(&s->iomem, obj, &syborg_sensor_ops, s, "syborg-sensor", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const VMStateDescription vmstate_syborg_sensor = {
    .name = "syborg-sensor",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32_ARRAY(v, SyborgSensorState, 3),
        VMSTATE_UINT32(seq, SyborgSensorState),
        VMSTATE_END_OF_LIST()
    }
};

static void syborg_sensor_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->vmsd = &vmstate_syborg_sensor;
    object_class_property_add_str(oc, "accel", syborg_sensor_get_accel, syborg_sensor_set_accel);
    object_class_property_set_description(oc, "accel", "gravity reaction \"x,y,z\" in milli-g (device axes)");
    object_class_property_add_str(oc, "info", syborg_sensor_get_info, syborg_sensor_set_info);
    object_class_property_set_description(oc, "info", "driver information block: \"OFFSET=HEXBYTES,...\" (device profile)");
}

static const TypeInfo syborg_sensor_info = {
    .name = TYPE_SYBORG_SENSOR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SyborgSensorState),
    .instance_init = syborg_sensor_init,
    .class_init = syborg_sensor_class_init,
};

static void syborg_sensor_register_types(void)
{
    type_register_static(&syborg_sensor_info);
}

type_init(syborg_sensor_register_types)
