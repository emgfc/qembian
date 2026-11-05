// Second Syborg NVMemory media driver = the N95's E: drive (memory card / mass memory).
// Same driver source as C: (baseport mednvmemory, made parametrisable by patch qemu-baseport 0006),
// on the second QEMU NVMemory device (src/qemu-syborg/hw/arm/syborg.c: NAND slot 0xc000d000,
// irq 14, -drive if=mtd,index=1), local drive 2, removable like a memory card.
#include <kernel/kernel.h>
#include "syborg.h"
#define NVMEM_HWBASE		KHwSVPNandDevice
#define NVMEM_IRQ			14
#define NVMEM_PDDNAME		"Media.MEDNVMEM2"
#define NVMEM_MEDIADEVICE	EFixedMedia3	// 2 is MEDROFS (local drive 8)
#define NVMEM_DRIVEATT		(KDriveAttLocal | KDriveAttRemovable)
#define NVMEM_DRIVECOUNT	1
#define NVMEM_DRIVELIST		2
#define NVMEM_NUMMEDIA		1
#define NVMEM_DRIVENAME		"NVMem2"
#include "../../mednvmemory/syborg_mednvmemory.cpp"
