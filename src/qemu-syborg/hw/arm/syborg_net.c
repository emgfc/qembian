/*
 * Syborg Ethernet (ours): a mailbox NIC for the phone's LAN stack (EPL enet.ldd + our PDD stand-in
 * src/standins/sybeth -> etherpkt.drv -> EthInt.nif), on a QEMU network backend (-nic user: slirp, DHCP
 * 10.0.2.x). docs/research/connectivity-options.md section 4.
 *
 * One page at 0xc000c000 (the baseport's KHwSVPNetDevice), IRQ 9 (EIntNet0). No DMA, no rings: the guest
 * copies each frame through a window.
 *   0x000 ID (SYBORG_ID_NET)
 *   0x004 STATUS      bit 0 a received frame is waiting, bit 1 link up (a backend is attached)
 *   0x008 INT_ENABLE  bit 0 interrupt while a received frame waits (level)
 *   0x00c TX_LEN      write N: send the first N bytes of the TX window as one frame (14..1514)
 *   0x010 RX_LEN      the waiting frame's length (0: none)
 *   0x014 RX_NEXT     write: done with the waiting frame, the next one (if any) moves into the RX window
 *   0x018 RX_DROPPED  frames dropped (queue full or too long)
 *   0x01c TX_COUNT    0x020 RX_COUNT
 *   0x100..0x105 MAC  (byte or word reads)
 *   0x200..0x7ff TX window (1536 bytes)   0x800..0xdff RX window (1536 bytes, the waiting frame)
 * Received frames queue in the device (RX_QUEUE); while it is full the backend holds back (can_receive).
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
#include "net/net.h"
#include "qom/object.h"
#include "syborg.h"

enum { R_ID = 0, R_STATUS = 1, R_INT_ENABLE = 2, R_TX_LEN = 3, R_RX_LEN = 4, R_RX_NEXT = 5,
       R_RX_DROPPED = 6, R_TX_COUNT = 7, R_RX_COUNT = 8 };
#define MAC_BASE   0x100
#define TX_BASE    0x200
#define RX_BASE    0x800
#define WIN_SIZE   1536
#define FRAME_MIN  14
#define FRAME_MAX  1514
#define RX_QUEUE   64

OBJECT_DECLARE_SIMPLE_TYPE(SyborgNetState, SYBORG_NET)

struct SyborgNetState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    NICState *nic;
    NICConf conf;
    uint32_t int_enable;
    uint32_t dropped, tx_count, rx_count;
    uint8_t tx[WIN_SIZE];
    /* RX: rx[head] is the frame in the window; count frames waiting */
    uint32_t head, count;
    uint16_t rx_len[RX_QUEUE];
    uint8_t rx[RX_QUEUE][WIN_SIZE];
};

static void net_update(SyborgNetState *s)
{
    qemu_set_irq(s->irq, s->count && (s->int_enable & 1));
}

static bool net_can_receive(NetClientState *nc)
{
    SyborgNetState *s = qemu_get_nic_opaque(nc);
    return s->count < RX_QUEUE;
}

static ssize_t net_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    SyborgNetState *s = qemu_get_nic_opaque(nc);
    if (size > FRAME_MAX || s->count >= RX_QUEUE) {
        s->dropped++;
        return size;                /* consumed (dropped): a giant frame never fits */
    }
    uint32_t i = (s->head + s->count) % RX_QUEUE;
    memcpy(s->rx[i], buf, size);
    if (size < 60) {                /* the guest sees at least a minimum Ethernet frame */
        memset(s->rx[i] + size, 0, 60 - size);
        size = 60;
    }
    s->rx_len[i] = size;
    s->count++;
    s->rx_count++;
    net_update(s);
    return size;
}

static uint32_t win_read(const uint8_t *b, unsigned size)
{
    uint32_t v = 0;
    for (unsigned k = 0; k < size; k++) {
        v |= (uint32_t)b[k] << (8 * k);
    }
    return v;
}

static uint64_t net_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgNetState *s = opaque;
    if (offset >= RX_BASE && offset + size <= RX_BASE + WIN_SIZE) {
        return s->count ? win_read(s->rx[s->head] + (offset - RX_BASE), size) : 0;
    }
    if (offset >= TX_BASE && offset + size <= TX_BASE + WIN_SIZE) {
        return win_read(s->tx + (offset - TX_BASE), size);
    }
    if (offset >= MAC_BASE && offset + size <= MAC_BASE + 8) {
        uint8_t mac[8] = { 0 };
        memcpy(mac, s->conf.macaddr.a, 6);
        return win_read(mac + (offset - MAC_BASE), size);
    }
    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-net: %u-byte read of register 0x%x\n", size, (int)offset);
        return 0;
    }
    NetClientState *nc = qemu_get_queue(s->nic);
    switch (offset >> 2) {
    case R_ID:         return SYBORG_ID_NET;
    case R_STATUS:     return (s->count ? 1 : 0) | (nc->peer && !nc->link_down ? 2 : 0);
    case R_INT_ENABLE: return s->int_enable;
    case R_RX_LEN:     return s->count ? s->rx_len[s->head] : 0;
    case R_RX_DROPPED: return s->dropped;
    case R_TX_COUNT:   return s->tx_count;
    case R_RX_COUNT:   return s->rx_count;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-net: read of register 0x%x\n", (int)offset);
        return 0;
    }
}

static void net_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SyborgNetState *s = opaque;
    if (offset >= TX_BASE && offset + size <= TX_BASE + WIN_SIZE) {
        for (unsigned k = 0; k < size; k++) {
            s->tx[offset - TX_BASE + k] = value >> (8 * k);
        }
        return;
    }
    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-net: %u-byte write of register 0x%x\n", size, (int)offset);
        return;
    }
    switch (offset >> 2) {
    case R_INT_ENABLE:
        s->int_enable = value & 1;
        net_update(s);
        break;
    case R_TX_LEN:
        if (value >= FRAME_MIN && value <= FRAME_MAX) {
            qemu_send_packet(qemu_get_queue(s->nic), s->tx, value);
            s->tx_count++;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "syborg-net: TX length %u\n", (unsigned)value);
        }
        break;
    case R_RX_NEXT:
        if (s->count) {
            s->head = (s->head + 1) % RX_QUEUE;
            s->count--;
            net_update(s);
            if (s->count == RX_QUEUE - 1) {      /* room again: the backend may deliver what it held */
                qemu_flush_queued_packets(qemu_get_queue(s->nic));
            }
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-net: write to register 0x%x\n", (int)offset);
        break;
    }
}

static const MemoryRegionOps net_ops = {
    .read = net_read,
    .write = net_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static NetClientInfo net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = net_can_receive,
    .receive = net_receive,
};

static void net_realize(DeviceState *dev, Error **errp)
{
    SyborgNetState *s = SYBORG_NET(dev);
    memory_region_init_io(&s->iomem, OBJECT(s), &net_ops, s, "syborg-net", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_info, &s->conf, object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void net_reset(DeviceState *dev)
{
    SyborgNetState *s = SYBORG_NET(dev);
    s->int_enable = 0;
    s->head = s->count = 0;
    net_update(s);
}

static const VMStateDescription vmstate_net = {
    .name = "syborg-net",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(int_enable, SyborgNetState),
        VMSTATE_UINT32(dropped, SyborgNetState),
        VMSTATE_UINT32(tx_count, SyborgNetState),
        VMSTATE_UINT32(rx_count, SyborgNetState),
        VMSTATE_UINT8_ARRAY(tx, SyborgNetState, WIN_SIZE),
        VMSTATE_UINT32(head, SyborgNetState),
        VMSTATE_UINT32(count, SyborgNetState),
        VMSTATE_UINT16_ARRAY(rx_len, SyborgNetState, RX_QUEUE),
        VMSTATE_UINT8_2DARRAY(rx, SyborgNetState, RX_QUEUE, WIN_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static const Property net_properties[] = {
    DEFINE_NIC_PROPERTIES(SyborgNetState, conf),
};

static void net_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = net_realize;
    device_class_set_legacy_reset(dc, net_reset);
    dc->vmsd = &vmstate_net;
    device_class_set_props(dc, net_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo syborg_net_info = {
    .name = TYPE_SYBORG_NET,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SyborgNetState),
    .class_init = net_class_init,
};

static void syborg_net_register(void)
{
    type_register_static(&syborg_net_info);
}

type_init(syborg_net_register)
