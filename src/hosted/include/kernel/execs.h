// Hosted EKA2: kernel/execs.h for the hosted platform. The EPL header builds the exec tables per CPU;
// its WINS branch is plain C++ (arrays of words and function addresses), which is what a hosted
// kernel wants: present __WINS__ to this header only. Its preprocessing handler is then named
// Win32PreprocessHandler (provided by src/hosted/kernel/linux/cexec.cpp).
#ifndef __HOSTED_KERNEL_EXECS_H__
#define __HOSTED_KERNEL_EXECS_H__
#include <u32exec.h>
#if defined(__HOSTED__) && !defined(__WINS__)
#define __WINS__
#define __HOSTED_FAKE_WINS
#endif
#include_next <kernel/execs.h>
#ifdef __HOSTED_FAKE_WINS
#undef __WINS__
#undef __HOSTED_FAKE_WINS
#endif
#endif
