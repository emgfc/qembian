/* N-Gage QD board: MMC host controller + DMA engine (ngage_mmc.c). SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NGAGE_MMC_H
#define NGAGE_MMC_H
#include "system/memory.h"
void ngage_mmc_init(MemoryRegion *sys, qemu_irq *intc, const char *card);
#endif
