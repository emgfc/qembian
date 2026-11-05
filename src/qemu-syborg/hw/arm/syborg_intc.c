/*
 * Syborg interrupt controller.
 *
 * Level-sensitive inputs, one output to the CPU IRQ line. Registers (word index):
 *   0 ID, 1 STATUS (number of asserted & enabled lines), 2 CURRENT (lowest such line or
 *   0xffffffff), 3 DISABLE_ALL, 4 DISABLE n, 5 ENABLE n, 6 TOTAL.
 * The guest dispatcher indexes its handler table with CURRENT without checking for
 * 0xffffffff, so the output line is recomputed on every change and only raised while
 * CURRENT is valid.
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
#include "qom/object.h"
#include "syborg.h"

OBJECT_DECLARE_SIMPLE_TYPE(SyborgIntcState, SYBORG_INTC)

struct SyborgIntcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq out;
    uint32_t num;
    uint64_t level;
    uint64_t enabled;
};

static uint64_t pending(SyborgIntcState *s)
{
    return s->level & s->enabled;
}

static void syborg_intc_update(SyborgIntcState *s)
{
    qemu_set_irq(s->out, pending(s) != 0);
}

static void syborg_intc_set(void *opaque, int n, int level)
{
    SyborgIntcState *s = opaque;

    if (level) {
        s->level |= 1ull << n;
    } else {
        s->level &= ~(1ull << n);
    }
    syborg_intc_update(s);
}

static uint64_t syborg_intc_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgIntcState *s = opaque;
    uint64_t p = pending(s);

    switch (offset >> 2) {
    case 0:
        return SYBORG_ID_INTC;
    case 1:
        return ctpop64(p);
    case 2:
        return p ? ctz64(p) : 0xffffffff;
    case 6:
        return s->num;
    default:
        return 0;
    }
}

static void syborg_intc_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    SyborgIntcState *s = opaque;

    switch (offset >> 2) {
    case 3:
        s->enabled = 0;
        break;
    case 4:
        if (value < s->num) {
            s->enabled &= ~(1ull << value);
        }
        break;
    case 5:
        if (value < s->num) {
            s->enabled |= 1ull << value;
        }
        break;
    default:
        break;
    }
    syborg_intc_update(s);
}

static const MemoryRegionOps syborg_intc_ops = {
    .read = syborg_intc_read,
    .write = syborg_intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void syborg_intc_realize(DeviceState *dev, Error **errp)
{
    SyborgIntcState *s = SYBORG_INTC(dev);

    if (s->num == 0 || s->num > 64) {
        error_setg(errp, "syborg-intc: num-interrupts must be 1..64");
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &syborg_intc_ops, s,
                          "syborg-intc", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->out);
    qdev_init_gpio_in(dev, syborg_intc_set, s->num);
}

static const VMStateDescription vmstate_syborg_intc = {
    .name = "syborg-intc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(level, SyborgIntcState),
        VMSTATE_UINT64(enabled, SyborgIntcState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property syborg_intc_properties[] = {
    DEFINE_PROP_UINT32("num-interrupts", SyborgIntcState, num, 32),
};

static void syborg_intc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = syborg_intc_realize;
    dc->vmsd = &vmstate_syborg_intc;
    device_class_set_props(dc, syborg_intc_properties);
}

static const TypeInfo syborg_intc_info = {
    .name = TYPE_SYBORG_INTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SyborgIntcState),
    .class_init = syborg_intc_class_init,
};

static void syborg_intc_register_types(void)
{
    type_register_static(&syborg_intc_info);
}

type_init(syborg_intc_register_types)
