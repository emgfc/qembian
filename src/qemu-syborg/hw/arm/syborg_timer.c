/*
 * Syborg timer and RTC.
 *
 * Timer registers (word index): 0 ID, 1 RUNNING, 2 ONESHOT, 3 LIMIT (write reloads the
 * counter), 4 VALUE, 5 INT_ENABLE (bit 0), 6 INT_STATUS (bit 0, write-1-to-clear), 7 FREQ.
 * The Syborg kernel programs LIMIT=1000 at 100 kHz for its 10 ms tick.
 *
 * RTC registers: 0 ID, 1 LATCH, 2 DATA_LOW, 3 DATA_HIGH. LATCH v>=4 sets the clock from
 * DATA; v<4 latches (now_ns + offset) / 1000^v. The offset is initialised exactly like the
 * 0.9.1 plugin (host start time in microseconds minus the virtual clock in nanoseconds); the
 * guest only uses LATCH=1 / DATA_LOW as a free-running 1 MHz counter.
 *
 * qembian extension: register 4 WALL = calendar time in UTC seconds since 2000-01-01, read and
 * written by the baseport's Syborg::(Set)SystemTimeInSecondsFrom2000 (kernel hardware RTC).
 * WALL = QEMU rtc clock (host time, honours -rtc base=/clock=) + wall_offset. A guest write moves
 * wall_offset, so a date/time set on the phone keeps running from the new value. With the
 * property offset-file=PATH the offset (decimal seconds) is loaded at start and rewritten on every
 * guest write, which persists the phone's time across QEMU restarts like a battery-backed RTC.
 * Register 5 COUNT32K = free-running 32768 Hz counter (virtual clock), one read, no latch: the
 * kernel's fast counter (NKern::FastCounter, User::FastCounter). The N95's hal.dll reports
 * EFastCounterFrequency = 32768 (its OMAP 32 kHz timer) without asking the kernel, so the counter
 * must really run at that rate or apps that pace frames with it run unthrottled. LATCH=1 (1 MHz)
 * stays for the original Syborg baseport.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "system/system.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "syborg.h"

OBJECT_DECLARE_SIMPLE_TYPE(SyborgTimerState, SYBORG_TIMER)

struct SyborgTimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    uint32_t freq;
    uint32_t running;
    uint32_t oneshot;
    uint32_t limit;
    uint32_t int_status;
    uint32_t int_enable;
};

static void syborg_timer_update(SyborgTimerState *s)
{
    qemu_set_irq(s->irq, (s->int_status & s->int_enable) != 0);
}

static void syborg_timer_tick(void *opaque)
{
    SyborgTimerState *s = opaque;

    s->int_status |= 1;
    if (s->oneshot) {
        s->running = 0;
    }
    syborg_timer_update(s);
}

static uint64_t syborg_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgTimerState *s = opaque;

    switch (offset >> 2) {
    case 0: return SYBORG_ID_TIMER;
    case 1: return s->running;
    case 2: return s->oneshot;
    case 3: return s->limit;
    case 4: return ptimer_get_count(s->timer);
    case 5: return s->int_enable;
    case 6: return s->int_status;
    case 7: return s->freq;
    default: return 0;
    }
}

static void syborg_timer_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    SyborgTimerState *s = opaque;

    switch (offset >> 2) {
    case 1:
        if (s->running != (value != 0)) {
            s->running = value != 0;
            ptimer_transaction_begin(s->timer);
            if (s->running) {
                ptimer_run(s->timer, s->oneshot);
            } else {
                ptimer_stop(s->timer);
            }
            ptimer_transaction_commit(s->timer);
        }
        break;
    case 2:
        ptimer_transaction_begin(s->timer);
        if (s->running) {
            ptimer_stop(s->timer);
        }
        s->oneshot = value != 0;
        if (s->running) {
            ptimer_run(s->timer, s->oneshot);
        }
        ptimer_transaction_commit(s->timer);
        break;
    case 3:
        s->limit = value;
        ptimer_transaction_begin(s->timer);
        ptimer_set_limit(s->timer, value, 1);
        ptimer_transaction_commit(s->timer);
        break;
    case 4:
        ptimer_transaction_begin(s->timer);
        ptimer_set_count(s->timer, value);
        ptimer_transaction_commit(s->timer);
        break;
    case 5:
        s->int_enable = value & 1;
        syborg_timer_update(s);
        break;
    case 6:
        s->int_status &= ~value;
        syborg_timer_update(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps syborg_timer_ops = {
    .read = syborg_timer_read,
    .write = syborg_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void syborg_timer_realize(DeviceState *dev, Error **errp)
{
    SyborgTimerState *s = SYBORG_TIMER(dev);

    if (!s->freq) {
        error_setg(errp, "syborg-timer: frequency must be set");
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &syborg_timer_ops, s,
                          "syborg-timer", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->timer = ptimer_init(syborg_timer_tick, s, PTIMER_POLICY_LEGACY);
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, s->freq);
    ptimer_transaction_commit(s->timer);
}

static const VMStateDescription vmstate_syborg_timer = {
    .name = "syborg-timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(running, SyborgTimerState),
        VMSTATE_UINT32(oneshot, SyborgTimerState),
        VMSTATE_UINT32(limit, SyborgTimerState),
        VMSTATE_UINT32(int_status, SyborgTimerState),
        VMSTATE_UINT32(int_enable, SyborgTimerState),
        VMSTATE_PTIMER(timer, SyborgTimerState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property syborg_timer_properties[] = {
    DEFINE_PROP_UINT32("frequency", SyborgTimerState, freq, 100000),
};

static void syborg_timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = syborg_timer_realize;
    dc->vmsd = &vmstate_syborg_timer;
    device_class_set_props(dc, syborg_timer_properties);
}

/* ---- RTC ---- */

OBJECT_DECLARE_SIMPLE_TYPE(SyborgRtcState, SYBORG_RTC)

struct SyborgRtcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    int64_t offset;
    uint64_t data;
    int64_t wall_offset;   /* guest UTC seconds minus rtc_clock seconds */
    char *offset_file;
};

#define SECS_1970_TO_2000 946684800LL

static int64_t syborg_rtc_host_secs2000(void)
{
    return qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND - SECS_1970_TO_2000;
}

static void syborg_rtc_save_offset(SyborgRtcState *s)
{
    g_autofree char *txt = NULL;
    g_autoptr(GError) err = NULL;

    if (!s->offset_file || !*s->offset_file) {
        return;
    }
    txt = g_strdup_printf("%" PRId64 "\n", s->wall_offset);
    if (!g_file_set_contents(s->offset_file, txt, -1, &err)) {
        qemu_log_mask(LOG_UNIMP, "syborg-rtc: cannot write %s: %s\n",
                      s->offset_file, err->message);
    }
}

static uint64_t syborg_rtc_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgRtcState *s = opaque;

    switch (offset >> 2) {
    case 0: return SYBORG_ID_RTC;
    case 2: return (uint32_t)s->data;
    case 3: return (uint32_t)(s->data >> 32);
    case 4: return (uint32_t)(int32_t)(syborg_rtc_host_secs2000() + s->wall_offset);
    case 5: return (uint32_t)muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 32768,
                                      NANOSECONDS_PER_SECOND);
    default: return 0;
    }
}

static void syborg_rtc_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    SyborgRtcState *s = opaque;
    int64_t now;

    switch (offset >> 2) {
    case 1:
        now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (value >= 4) {
            s->offset = (int64_t)s->data - now;
        } else {
            s->data = now + s->offset;
            while (value--) {
                s->data /= 1000;
            }
        }
        break;
    case 2:
        s->data = (s->data & ~0xffffffffull) | (uint32_t)value;
        break;
    case 3:
        s->data = (s->data & 0xffffffffull) | ((uint64_t)value << 32);
        break;
    case 4:
        s->wall_offset = (int64_t)(int32_t)value - syborg_rtc_host_secs2000();
        syborg_rtc_save_offset(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps syborg_rtc_ops = {
    .read = syborg_rtc_read,
    .write = syborg_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void syborg_rtc_realize(DeviceState *dev, Error **errp)
{
    SyborgRtcState *s = SYBORG_RTC(dev);
    int64_t start_s = qemu_clock_get_ns(QEMU_CLOCK_HOST) / NANOSECONDS_PER_SECOND;

    memory_region_init_io(&s->iomem, OBJECT(s), &syborg_rtc_ops, s,
                          "syborg-rtc", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    /* same (mixed-unit) initialisation as the 0.9.1 plugin */
    s->offset = start_s * 1000000 - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->wall_offset = 0;
    if (s->offset_file && *s->offset_file) {
        g_autofree char *txt = NULL;
        if (g_file_get_contents(s->offset_file, &txt, NULL, NULL)) {
            s->wall_offset = g_ascii_strtoll(txt, NULL, 10);
        }
    }
}

static const Property syborg_rtc_properties[] = {
    DEFINE_PROP_STRING("offset-file", SyborgRtcState, offset_file),
};

static const VMStateDescription vmstate_syborg_rtc = {
    .name = "syborg-rtc",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(offset, SyborgRtcState),
        VMSTATE_UINT64(data, SyborgRtcState),
        VMSTATE_INT64(wall_offset, SyborgRtcState),
        VMSTATE_END_OF_LIST()
    }
};

static void syborg_rtc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = syborg_rtc_realize;
    dc->vmsd = &vmstate_syborg_rtc;
    device_class_set_props(dc, syborg_rtc_properties);
}

static const TypeInfo syborg_timer_types[] = {
    {
        .name = TYPE_SYBORG_TIMER,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SyborgTimerState),
        .class_init = syborg_timer_class_init,
    }, {
        .name = TYPE_SYBORG_RTC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SyborgRtcState),
        .class_init = syborg_rtc_class_init,
    },
};

DEFINE_TYPES(syborg_timer_types)
