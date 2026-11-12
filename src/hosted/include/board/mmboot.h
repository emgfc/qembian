// Hosted EKA2: what our board's headers (syborg.h) take from the multiple memory model's mmboot.h (whose
// real copy includes the ARM CPU headers): the fixed linear layout. Values of
// kernel/eka/include/memmodel/epoc/multiple/arm/mmboot.h (EPL, Nokia).
#ifndef __HOSTED_BOARD_MMBOOT_H__
#define __HOSTED_BOARD_MMBOOT_H__
#include <e32def.h>
const TLinAddr KPrimaryIOBase = 0xC6000000u;
const TLinAddr KRomLinearBase = 0x80000000u;
const TLinAddr KKernelDataBase = 0xC8000000u;
#endif
