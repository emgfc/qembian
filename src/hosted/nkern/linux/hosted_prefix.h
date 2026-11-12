// Hosted EKA2: compiler prefix for kernel code built for the Linux-hosted platform (__HOSTED__),
// force-included after gcce/gcce.h. Fills what cpudefs.h and the kernel headers expect from a CPU
// branch (__CPU_ARM / __CPU_X86 / Win32) that the hosted platform does not select.
#ifndef __HOSTED_PREFIX_H__
#define __HOSTED_PREFIX_H__
// a deliberate crash: an undefined instruction (SIGILL with the faulting pc in the context)
#define __crash() __asm__ volatile(".inst 0xe7ffdeff" ::: "memory")
// a spin-wait hint (nk_cpu.h defines it per CPU branch)
#define __chill() __asm__ volatile("yield" ::: "memory")
// exception trap context: ARM layout (r4-r11, sp, pc), cpudefs.h defines it per CPU branch
#define EXC_TRAP_CTX_SZ 10
#ifdef __cplusplus
extern "C" {
#endif
void* memclr(void* aPtr, unsigned int aLength);
void* memset(void*, int, unsigned int);
void* memcpy(void*, const void*, unsigned int);
void* memmove(void*, const void*, unsigned int);
int memcmp(const void*, const void*, unsigned int);
#ifdef __cplusplus
}
#endif
#endif
