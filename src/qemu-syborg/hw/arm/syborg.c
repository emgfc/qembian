/*
 * Syborg (Symbian Virtual Platform) board for modern QEMU.
 *
 * Memory map (fixed by the Symbian baseport's syborg.h, same as the 0.9.1-symbian DTS):
 *   0x00000000 RAM (168 MB default: N95 8GB free RAM with an unpaged core image)
 *   0xc0000000 interrupt controller -> CPU IRQ   0xc0001000 RTC   0xc0002000 timer (irq 1)
 *   0xc0003000 keyboard (2)   0xc0004000 pointer (3)   0xc0005000 framebuffer (4)
 *   0xc0006000..0xc0009000 serial 0..3 (5..8)   0xc000d000 NVMemory 2 = E: (14)
 *   0xc000e000 audio (13)   0xc000f000 GL (host GLES 2 backend, syborg_gl.c)   0xc0010000 NVMemory (12)
 *   0xc0011000 platform (ID, tree offset, POWER (write 1 off, 2 reset), RAM_MB (read: the machine's
 *              RAM; the bootstrap sizes its RAM bank by it), CONFIG ("key=value;..." at +0x400: stand-in
 *              settings, -global syborg-platform.config=...), DEVICE_ID (NUL-terminated string at +0x800,
 *              -global syborg-platform.device-id=ID, the device profile's id), the device tree blob at
 *              +0x1000, 16 MB window)
 *   0x60000000 flash: 128 MB read-only window, the N95's ROFS partition (stands for its NAND);
 *              filled with -device loader,file=ROFS,addr=0x60000000,force-raw=on (scripts/run-modern.sh
 *              does it for ROM.rofs); read by MEDROFS.PDD, so ROFS takes no RAM as on the phone
 * hostfs/snapshot/virtio/nand/usbtest are not modelled; accesses to them read as zero
 * (ignore_memory_transaction_failures), like unmapped addresses on 0.9.1.
 *
 * The ROM image is loaded with -kernel as a raw image at 0x10000 with the standard ARM boot
 * stub, which is what the 0.9.1 fork did too. Options:
 *   -global syborg-fb.width=W -global syborg-fb.height=H   framebuffer size (240x320 default)
 *   -drive if=mtd,format=raw,file=IMG                       NVMemory drive (C:)
 *   -drive if=mtd,index=1,format=raw,file=IMG               second NVMemory drive (E:)
 *   -dtb FILE                                               blob served by the platform device
 *   -audiodev pipewire,id=snd0 -machine audiodev=snd0        host audio for the N95 fake DSP
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/runstate.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "chardev/char-fe.h"
#include "target/arm/cpu-qom.h"
#include "qom/object.h"
#include "syborg.h"

/* ---- platform device ---- */

OBJECT_DECLARE_SIMPLE_TYPE(SyborgPlatformState, SYBORG_PLATFORM)

#define PLATFORM_TREE_OFFSET 0x1000
#define PLATFORM_CONFIG      0x400      /* .. 0x7ff: "key=value;..." from the device profile */
#define PLATFORM_DEVICE_ID   0x800      /* .. 0xfff: the device id string */

struct SyborgPlatformState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint8_t *dtb;
    uint32_t dtb_size;
    uint32_t ram_mb;            /* set by the board from -m */
    char *device_id;            /* device profile id (tools/device.py qemu-args) */
    char *config;               /* stand-in settings: "key=value;..." (tools/device.py qemu-args) */
};

static uint64_t syborg_platform_read(void *opaque, hwaddr offset, unsigned size)
{
    SyborgPlatformState *s = opaque;

    if (offset >= PLATFORM_TREE_OFFSET) {
        uint64_t v = 0;
        offset -= PLATFORM_TREE_OFFSET;
        for (unsigned i = 0; i < size; i++) {
            if (offset + i < s->dtb_size) {
                v |= (uint64_t)s->dtb[offset + i] << (8 * i);
            }
        }
        return v;
    }
    if (offset >= PLATFORM_CONFIG) {
        const char *str = offset >= PLATFORM_DEVICE_ID ? s->device_id : s->config;
        hwaddr base = offset >= PLATFORM_DEVICE_ID ? PLATFORM_DEVICE_ID : PLATFORM_CONFIG;
        uint64_t v = 0;
        size_t n = str ? strlen(str) : 0;
        for (unsigned i = 0; i < size; i++) {
            size_t k = offset - base + i;
            if (k < n) {
                v |= (uint64_t)(uint8_t)str[k] << (8 * i);
            }
        }
        return v;
    }
    switch (offset >> 2) {
    case 0: return SYBORG_ID_PLATFORM;
    case 1: return PLATFORM_TREE_OFFSET;
    case 3: return s->ram_mb;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "syborg-platform: bad read 0x%x\n", (int)offset);
        return 0;
    }
}

static void syborg_platform_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size)
{
    /* word 2 POWER: 1 = switch off, 2 = reset (the N95's modem owns power; our fake modem
       writes it after MTC_POWER_OFF_REQ / MTC_RESET_GENERATE_REQ) */
    if (offset == 8 && value == 1) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        return;
    }
    if (offset == 8 && value == 2) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "syborg-platform: write 0x%x ignored\n", (int)offset);
}

static const MemoryRegionOps syborg_platform_ops = {
    .read = syborg_platform_read,
    .write = syborg_platform_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void syborg_platform_realize(DeviceState *dev, Error **errp)
{
    SyborgPlatformState *s = SYBORG_PLATFORM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &syborg_platform_ops, s,
                          "syborg-platform", 0x1000000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const Property syborg_platform_properties[] = {
    DEFINE_PROP_STRING("device-id", SyborgPlatformState, device_id),
    DEFINE_PROP_STRING("config", SyborgPlatformState, config),
};

static void syborg_platform_class_init(ObjectClass *oc, const void *data)
{
    DEVICE_CLASS(oc)->realize = syborg_platform_realize;
    device_class_set_props(DEVICE_CLASS(oc), syborg_platform_properties);
}

static const TypeInfo syborg_platform_info = {
    .name = TYPE_SYBORG_PLATFORM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SyborgPlatformState),
    .class_init = syborg_platform_class_init,
};

/* ---- machine ---- */

static struct arm_boot_info syborg_binfo = {
    .loader_start = 0x0,
    .board_id = 0,
};

static void syborg_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj;
    DeviceState *intc, *dev;
    qemu_irq pic[32];
    DriveInfo *dinfo;
    SyborgPlatformState *plat;

    cpuobj = object_new(machine->cpu_type);
    /* 0.9.1 had no TrustZone: keep the CPU without EL3 so CP15 behaves the same */
    if (object_property_find(cpuobj, "has_el3")) {
        object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
    }
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);

    memory_region_add_subregion(sysmem, 0, machine->ram);

    intc = qdev_new(TYPE_SYBORG_INTC);
    qdev_prop_set_uint32(intc, "num-interrupts", 32);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(intc), 0, 0xc0000000);
    sysbus_connect_irq(SYS_BUS_DEVICE(intc), 0,
                       qdev_get_gpio_in(DEVICE(cpuobj), ARM_CPU_IRQ));
    for (int i = 0; i < 32; i++) {
        pic[i] = qdev_get_gpio_in(intc, i);
    }

    sysbus_create_simple(TYPE_SYBORG_RTC, 0xc0001000, NULL);

    dev = qdev_new(TYPE_SYBORG_TIMER);
    qdev_prop_set_uint32(dev, "frequency", 100000);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xc0002000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, pic[1]);

    sysbus_create_simple(TYPE_SYBORG_KEYBOARD, 0xc0003000, pic[2]);
    sysbus_create_simple(TYPE_SYBORG_POINTER, 0xc0004000, pic[3]);
    sysbus_create_simple(TYPE_SYBORG_FB, 0xc0005000, pic[4]);

    dev = qdev_new(TYPE_SYBORG_AUDIO);
    if (machine->audiodev) {
        qdev_prop_set_string(dev, "audiodev", machine->audiodev);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xc000e000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, pic[13]);

    /* host GL backend for the guest's GLES rasteriser (syborg_gl.c; -global syborg-gl.backend=off) */
    sysbus_create_simple(TYPE_SYBORG_GL, 0xc000f000, NULL);

    for (int i = 0; i < 4; i++) {
        dev = qdev_new(TYPE_SYBORG_SERIAL);
        qdev_prop_set_chr(dev, "chardev", serial_hd(i));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xc0006000 + i * 0x1000);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, pic[5 + i]);
    }

    dev = qdev_new(TYPE_SYBORG_NVMEM);
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(dev, "drive", blk_by_legacy_dinfo(dinfo), &error_fatal);
    } else {
        warn_report("syborg: no NVMemory drive (-drive if=mtd,format=raw,file=...)");
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xc0010000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, pic[12]);

    /* second NVMemory for the N95's E: (memory card / mass memory), in the unused NAND slot */
    dev = qdev_new(TYPE_SYBORG_NVMEM);
    dinfo = drive_get(IF_MTD, 0, 1);
    if (dinfo) {
        qdev_prop_set_drive_err(dev, "drive", blk_by_legacy_dinfo(dinfo), &error_fatal);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xc000d000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, pic[14]);

    dev = qdev_new(TYPE_SYBORG_PLATFORM);
    plat = SYBORG_PLATFORM(dev);
    plat->ram_mb = machine->ram_size / MiB;
    if (machine->dtb) {
        gsize len = 0;
        g_autoptr(GError) err = NULL;
        gchar *data = NULL;
        if (!g_file_get_contents(machine->dtb, &data, &len, &err)) {
            error_report("syborg: cannot read dtb %s: %s", machine->dtb, err->message);
            exit(1);
        }
        plat->dtb = (uint8_t *)data;
        plat->dtb_size = len;
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xc0011000);

    /* read-only flash window (ROFS partition), outside RAM */
    {
        MemoryRegion *flash = g_new(MemoryRegion, 1);
        memory_region_init_rom(flash, NULL, "syborg.flash", 128 * MiB, &error_fatal);
        memory_region_add_subregion(sysmem, 0x60000000, flash);
    }

    syborg_binfo.ram_size = machine->ram_size;
    arm_load_kernel(ARM_CPU(cpuobj), machine, &syborg_binfo);
}

static void syborg_machine_init(MachineClass *mc)
{
    mc->desc = "Syborg (Symbian Virtual Platform) as a virtual N95: ARM1136 r0p2 (-cpu cortex-a8 for the old A8 builds)";
    machine_add_audiodev_property(mc);
    mc->init = syborg_init;
    mc->max_cpus = 1;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1136-r2");   /* the N95 OMAP2420 core (r0p2); kernel built __CPU_ARM1136__ (patch 0011) */
    mc->default_ram_size = 168 * MiB;     /* must match the bootstrap (KHwRamSizeMb, patch 0010) */
    mc->default_ram_id = "syborg.ram";
}

DEFINE_MACHINE_ARM("syborg", syborg_machine_init)

static void syborg_register_types(void)
{
    type_register_static(&syborg_platform_info);
}

type_init(syborg_register_types)
