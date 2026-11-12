// Hosted EKA2: in place of kernel/arm/arm.h (class Arm: ARM CPU and exception plumbing, ARM assembler)
// for board code that includes it through the variant's private header (syborg_priv.h) without using
// it. The hosted platform has no ARM kernel layer. Original work for qembian.
#ifndef __HOSTED_BOARD_ARM_H__
#define __HOSTED_BOARD_ARM_H__
#include <kernel/kern_priv.h>
#endif
