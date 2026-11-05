/*
 * Syborg NVMemory device (the C: drive / media device used by mednvmemory.pdd).
 *
 * Written from the register description in the Syborg baseport (guest driver
 * mednvmemory) — not from the EPL-licensed plugin or library. Register indices (the guest
 * accesses base + index*4; the header names them with byte-like constants):
 *   0x00 ID (0xdeadbeef), 0x04 TRANSACTION_OFFSET (sector), 0x08 TRANSACTION_SIZE (sectors),
 *   0x0c TRANSACTION_DIRECTION (1 read, 2 write), 0x10 TRANSACTION_EXECUTE,
 *   0x14 SHARED_MEMORY_BASE (guest physical), 0x18 NV_MEMORY_SIZE (sectors),
 *   0x1c SHARED_MEMORY_SIZE, 0x20 STATUS (read lowers the IRQ), 0x24 ENABLE.
 * EXECUTE performs the transfer between the drive and the shared buffer synchronously,
 * sets STATUS to the number of sectors transferred (negative on error) and raises the IRQ.
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
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "syborg.h"

enum {
    R_ID = 0x00, R_OFFSET = 0x04, R_SIZE = 0x08, R_DIRECTION = 0x0c, R_EXECUTE = 0x10,
    R_SHM_BASE = 0x14, R_NV_SIZE = 0x18, R_SHM_SIZE = 0x1c, R_STATUS = 0x20, R_ENABLE = 0x24,
};

#define NVMEM_READ  1
#define NVMEM_WRITE 2
#define NVMEM_ERROR (-1)

OBJECT_DECLARE_SIMPLE_TYPE(SyborgNvmemState, SYBORG_NVMEM)

struct SyborgNvmemState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    BlockBackend *blk;
    uint32_t sector_size;
    uint32_t offset, size, direction;
    uint32_t shm_base, shm_size;
    uint32_t status;
    uint32_t enabled;
};

static uint32_t sector_count(SyborgNvmemState *s)
{
    int64_t len = s->blk ? blk_getlength(s->blk) : 0;

    return len > 0 ? len / s->sector_size : 0;
}

static void syborg_nvmem_execute(SyborgNvmemState *s)
{
    uint64_t bytes = (uint64_t)s->size * s->sector_size;
    int64_t pos = (int64_t)s->offset * s->sector_size;
    g_autofree uint8_t *buf = NULL;
    int ret = -1;

    if (!s->blk || !s->size || bytes > s->shm_size ||
        (s->direction != NVMEM_READ && s->direction != NVMEM_WRITE)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "syborg-nvmem: bad transaction off %u size %u dir %u shm %u\n",
                      s->offset, s->size, s->direction, s->shm_size);
    } else {
        buf = g_malloc(bytes);
        if (s->direction == NVMEM_READ) {
            ret = blk_pread(s->blk, pos, bytes, buf, 0);
            if (ret >= 0) {
                address_space_write(&address_space_memory, s->shm_base,
                                    MEMTXATTRS_UNSPECIFIED, buf, bytes);
            }
        } else {
            address_space_read(&address_space_memory, s->shm_base,
                               MEMTXATTRS_UNSPECIFIED, buf, bytes);
            ret = blk_pwrite(s->blk, pos, bytes, buf, 0);
        }
    }
    s->status = ret >= 0 ? s->size : (uint32_t)NVMEM_ERROR;
    qemu_irq_raise(s->irq);
}

static uint64_t syborg_nvmem_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgNvmemState *s = opaque;

    switch (offset >> 2) {
    case R_ID: return SYBORG_ID_NVMEM;
    case R_OFFSET: return s->offset;
    case R_SIZE: return s->size;
    case R_DIRECTION: return s->direction;
    case R_SHM_BASE: return s->shm_base;
    case R_SHM_SIZE: return s->shm_size;
    case R_NV_SIZE: return sector_count(s);
    case R_STATUS:
        qemu_irq_lower(s->irq);
        return s->status;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-nvmem: bad read at 0x%x\n", (int)offset);
        return 0;
    }
}

static void syborg_nvmem_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    SyborgNvmemState *s = opaque;

    switch (offset >> 2) {
    case R_OFFSET: s->offset = value; break;
    case R_SIZE: s->size = value; break;
    case R_DIRECTION: s->direction = value; break;
    case R_EXECUTE: syborg_nvmem_execute(s); break;
    case R_SHM_BASE: s->shm_base = value; break;
    case R_SHM_SIZE: s->shm_size = value; break;
    case R_ENABLE: s->enabled = value; break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-nvmem: bad write at 0x%x\n", (int)offset);
        break;
    }
}

static const MemoryRegionOps syborg_nvmem_ops = {
    .read = syborg_nvmem_read,
    .write = syborg_nvmem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void syborg_nvmem_realize(DeviceState *dev, Error **errp)
{
    SyborgNvmemState *s = SYBORG_NVMEM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &syborg_nvmem_ops, s,
                          "syborg-nvmem", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    if (s->blk) {
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
    }
}

static const VMStateDescription vmstate_syborg_nvmem = {
    .name = "syborg-nvmem",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(offset, SyborgNvmemState),
        VMSTATE_UINT32(size, SyborgNvmemState),
        VMSTATE_UINT32(direction, SyborgNvmemState),
        VMSTATE_UINT32(shm_base, SyborgNvmemState),
        VMSTATE_UINT32(shm_size, SyborgNvmemState),
        VMSTATE_UINT32(status, SyborgNvmemState),
        VMSTATE_UINT32(enabled, SyborgNvmemState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property syborg_nvmem_properties[] = {
    DEFINE_PROP_DRIVE("drive", SyborgNvmemState, blk),
    DEFINE_PROP_UINT32("sector-size", SyborgNvmemState, sector_size, 512),
};

static void syborg_nvmem_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = syborg_nvmem_realize;
    dc->vmsd = &vmstate_syborg_nvmem;
    device_class_set_props(dc, syborg_nvmem_properties);
}

static const TypeInfo syborg_nvmem_info = {
    .name = TYPE_SYBORG_NVMEM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SyborgNvmemState),
    .class_init = syborg_nvmem_class_init,
};

static void syborg_nvmem_register_types(void)
{
    type_register_static(&syborg_nvmem_info);
}

type_init(syborg_nvmem_register_types)
