/*
 * Syborg serial port.
 *
 * Registers (word index): 0 ID, 1 DATA (read pops the RX FIFO or returns 0xffffffff; write
 * sends a byte), 2 FIFO_COUNT, 3 INT_ENABLE (bit 0 RX FIFO not empty, bit 1 TX DMA complete
 * (always), bit 2 RX DMA complete), 4 DMA_TX_ADDR, 5 DMA_TX_COUNT (write starts TX DMA),
 * 6 DMA_RX_ADDR, 7 DMA_RX_COUNT (write starts RX DMA). The Symbian driver only uses DATA,
 * FIFO_COUNT and INT_ENABLE; DMA is implemented for completeness.
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
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "syborg.h"

#define SERIAL_FIFO_MAX 64

OBJECT_DECLARE_SIMPLE_TYPE(SyborgSerialState, SYBORG_SERIAL)

struct SyborgSerialState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    CharFrontend chr;
    uint32_t fifo_size;
    uint32_t int_enable;
    uint32_t dma_tx_addr;
    uint32_t dma_rx_addr;
    uint32_t dma_rx_count;
    uint32_t fifo_count;
    uint32_t fifo_head;
    uint32_t fifo[SERIAL_FIFO_MAX];
};

static void syborg_serial_update(SyborgSerialState *s)
{
    uint32_t level = 2;     /* TX DMA complete */

    if (s->fifo_count) {
        level |= 1;
    }
    if (s->dma_rx_count == 0) {
        level |= 4;
    }
    qemu_set_irq(s->irq, (level & s->int_enable) != 0);
}

static uint32_t fifo_pop(SyborgSerialState *s)
{
    uint32_t v = s->fifo[s->fifo_head];

    s->fifo_head = (s->fifo_head + 1) % s->fifo_size;
    s->fifo_count--;
    return v;
}

static void fifo_push(SyborgSerialState *s, uint32_t v)
{
    s->fifo[(s->fifo_head + s->fifo_count) % s->fifo_size] = v;
    s->fifo_count++;
}

static int syborg_serial_can_receive(void *opaque)
{
    SyborgSerialState *s = opaque;

    return s->dma_rx_count ? s->dma_rx_count : s->fifo_size - s->fifo_count;
}

static void syborg_serial_receive(void *opaque, const uint8_t *buf, int size)
{
    SyborgSerialState *s = opaque;

    for (int i = 0; i < size; i++) {
        if (s->dma_rx_count) {
            address_space_write(&address_space_memory, s->dma_rx_addr,
                                MEMTXATTRS_UNSPECIFIED, &buf[i], 1);
            s->dma_rx_addr++;
            s->dma_rx_count--;
        } else if (s->fifo_count < s->fifo_size) {
            fifo_push(s, buf[i]);
        }
    }
    syborg_serial_update(s);
}

static void syborg_serial_dma_tx(SyborgSerialState *s, uint32_t count)
{
    uint8_t ch;

    while (count--) {
        address_space_read(&address_space_memory, s->dma_tx_addr,
                           MEMTXATTRS_UNSPECIFIED, &ch, 1);
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        s->dma_tx_addr++;
    }
    syborg_serial_update(s);
}

static void syborg_serial_dma_rx(SyborgSerialState *s, uint32_t count)
{
    while (count && s->fifo_count) {
        uint8_t ch = fifo_pop(s);
        address_space_write(&address_space_memory, s->dma_rx_addr,
                            MEMTXATTRS_UNSPECIFIED, &ch, 1);
        s->dma_rx_addr++;
        count--;
    }
    s->dma_rx_count = count;
    syborg_serial_update(s);
    qemu_chr_fe_accept_input(&s->chr);
}

static uint64_t syborg_serial_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgSerialState *s = opaque;
    uint32_t v;

    switch (offset >> 2) {
    case 0:
        return SYBORG_ID_SERIAL;
    case 1:
        if (!s->fifo_count) {
            return 0xffffffff;
        }
        v = fifo_pop(s);
        syborg_serial_update(s);
        qemu_chr_fe_accept_input(&s->chr);
        return v;
    case 2: return s->fifo_count;
    case 3: return s->int_enable;
    case 4: return s->dma_tx_addr;
    case 5: return 0;
    case 6: return s->dma_rx_addr;
    case 7: return s->dma_rx_count;
    default: return 0;
    }
}

static void syborg_serial_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    SyborgSerialState *s = opaque;
    uint8_t ch;

    switch (offset >> 2) {
    case 1:
        ch = value;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        break;
    case 3:
        s->int_enable = value & 7;
        syborg_serial_update(s);
        break;
    case 4:
        s->dma_tx_addr = value;
        break;
    case 5:
        syborg_serial_dma_tx(s, value);
        break;
    case 6:
        s->dma_rx_addr = value;
        break;
    case 7:
        syborg_serial_dma_rx(s, value);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps syborg_serial_ops = {
    .read = syborg_serial_read,
    .write = syborg_serial_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void syborg_serial_realize(DeviceState *dev, Error **errp)
{
    SyborgSerialState *s = SYBORG_SERIAL(dev);

    if (s->fifo_size == 0 || s->fifo_size > SERIAL_FIFO_MAX) {
        error_setg(errp, "syborg-serial: fifo-size must be 1..%d", SERIAL_FIFO_MAX);
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &syborg_serial_ops, s,
                          "syborg-serial", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    qemu_chr_fe_set_handlers(&s->chr, syborg_serial_can_receive,
                             syborg_serial_receive, NULL, NULL, s, NULL, true);
}

static const VMStateDescription vmstate_syborg_serial = {
    .name = "syborg-serial",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(int_enable, SyborgSerialState),
        VMSTATE_UINT32(dma_tx_addr, SyborgSerialState),
        VMSTATE_UINT32(dma_rx_addr, SyborgSerialState),
        VMSTATE_UINT32(dma_rx_count, SyborgSerialState),
        VMSTATE_UINT32(fifo_count, SyborgSerialState),
        VMSTATE_UINT32(fifo_head, SyborgSerialState),
        VMSTATE_UINT32_ARRAY(fifo, SyborgSerialState, SERIAL_FIFO_MAX),
        VMSTATE_END_OF_LIST()
    }
};

static const Property syborg_serial_properties[] = {
    DEFINE_PROP_CHR("chardev", SyborgSerialState, chr),
    DEFINE_PROP_UINT32("fifo-size", SyborgSerialState, fifo_size, 16),
};

/* a machine reset: no DMA (its addresses are the old boot's), nothing received, no interrupt */
static void syborg_serial_reset(DeviceState *dev)
{
    SyborgSerialState *s = SYBORG_SERIAL(dev);
    s->int_enable = 0;
    s->dma_tx_addr = s->dma_rx_addr = s->dma_rx_count = 0;
    s->fifo_count = s->fifo_head = 0;
    syborg_serial_update(s);
}

static void syborg_serial_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = syborg_serial_realize;
    device_class_set_legacy_reset(dc, syborg_serial_reset);
    dc->vmsd = &vmstate_syborg_serial;
    device_class_set_props(dc, syborg_serial_properties);
}

static const TypeInfo syborg_serial_info = {
    .name = TYPE_SYBORG_SERIAL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SyborgSerialState),
    .class_init = syborg_serial_class_init,
};

static void syborg_serial_register_types(void)
{
    type_register_static(&syborg_serial_info);
}

type_init(syborg_serial_register_types)
