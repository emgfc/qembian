// Hosted EKA2: u32exec.h for the hosted platform. The EPL header selects its exec-call macros per CPU
// (__WINS__ x86 asm, __CPU_X86, __CPU_ARM); the hosted kernel is ARM code, so the ARM branch (swi
// stubs) is the right one: we present __CPU_ARM to this header only, then take it back.
// (EPL tree unchanged: this file is first on the include path, #include_next the original.)
#ifndef __HOSTED_U32EXEC_H__
#define __HOSTED_U32EXEC_H__
#include <cpudefs.h>
#if defined(__HOSTED__) && !defined(__CPU_ARM)
#define __CPU_ARM
#define __HOSTED_FAKE_CPU_ARM
#endif
#include_next <u32exec.h>
#ifdef __HOSTED_FAKE_CPU_ARM
#undef __CPU_ARM
#undef __HOSTED_FAKE_CPU_ARM
#endif
#endif
