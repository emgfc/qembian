// hrt: the hosted-EKA2 freestanding runtime for 32-bit ARM (AArch32 EL0) Linux processes.
// No libc: raw Linux EABI syscalls (number in r7, svc #0), a tiny printf, a test-check helper.
// It is the base of the hosted kernel's test programs (src/hosted/tests) and later of the hosted
// nanokernel itself, which must own signals, TLS registers and the address space without a libc
// getting in the way. docs/hosted-eka2/.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Linux ARM EABI syscall numbers (arch/arm/tools/syscall.tbl) ----
enum {
	SYS_exit = 1, SYS_fork = 2, SYS_read = 3, SYS_write = 4, SYS_open = 5, SYS_close = 6,
	SYS_getpid = 20, SYS_kill = 37, SYS_munmap = 91, SYS_clone = 120, SYS_mprotect = 125,
	SYS_sched_yield = 158, SYS_nanosleep = 162, SYS_prctl = 172, SYS_rt_sigreturn = 173,
	SYS_rt_sigaction = 174, SYS_rt_sigprocmask = 175, SYS_mmap2 = 192, SYS_gettid = 224,
	SYS_futex = 240, SYS_exit_group = 248, SYS_tgkill = 268, SYS_waitid = 280,
	SYS_openat = 322, SYS_wait4 = 114, SYS_clock_gettime = 263,
	SYS_seccomp = 383, SYS_memfd_create = 385, SYS_process_vm_readv = 376,
	SYS_ARM_cacheflush = 0x0f0002, SYS_ARM_set_tls = 0x0f0005, SYS_ARM_get_tls = 0x0f0006,
};

static inline long hsys6(long n, long a, long b, long c, long d, long e, long f) {
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d;
	register long r4 __asm__("r4") = e;
	register long r5 __asm__("r5") = f;
	register long r7 __asm__("r7") = n;
	__asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7) : "memory");
	return r0;
}
#define hsys(n, ...) hsys_n(n, ##__VA_ARGS__, 0, 0, 0, 0, 0, 0)
#define hsys_n(n, a, b, c, d, e, f, ...) hsys6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

// ---- memory ----
enum { PROT_NONE = 0, PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4,
       MAP_SHARED = 1, MAP_PRIVATE = 2, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20,
       MAP_NORESERVE = 0x4000, MAP_FIXED_NOREPLACE = 0x100000 };
void *hmmap(void *addr, size_t len, int prot, int flags, int fd, uint32_t off);
static inline int hmunmap(void *a, size_t n) { return hsys(SYS_munmap, a, n); }
static inline int hmemfd(const char *name) { return hsys(SYS_memfd_create, name, 0); }
static inline int hftruncate(int fd, uint32_t n) { return hsys(194 /* ftruncate64 */, fd, 0, n, 0); }
#define HMAP_FAILED(p) ((uint32_t)(p) > 0xfffff000u)

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

// ---- process / thread ----
__attribute__((noreturn)) void hexit(int code);
static inline int hgetpid(void) { return hsys(SYS_getpid); }
static inline int hgettid(void) { return hsys(SYS_gettid); }
static inline int hfork(void) { return hsys(SYS_fork); }
int hwait(int pid);                                           // exit code, or 128+signal
// a raw thread sharing the address space; fn runs on its own stack, returns -> thread exits.
// *ctid is cleared and futex-woken when it exits (CLONE_CHILD_CLEARTID).
int hthread(int (*fn)(void *), void *arg, void *stack, size_t stacksize, volatile int *ctid);
void hthread_join(volatile int *ctid);

// ---- futex (shared, i.e. without FUTEX_PRIVATE: works across processes on shared memory) ----
enum { FUTEX_WAIT = 0, FUTEX_WAKE = 1, FUTEX_PRIVATE = 128 };
static inline int hfutex_wait(volatile int *a, int v) { return hsys(SYS_futex, a, FUTEX_WAIT, v, 0); }
static inline int hfutex_wake(volatile int *a, int n) { return hsys(SYS_futex, a, FUTEX_WAKE, n); }
// wait at most aUs microseconds (returns -110 ETIMEDOUT on timeout)
static inline int hfutex_wait_us(volatile int *a, int v, uint32_t aUs) {
	struct { int32_t s, ns; } ts = { (int32_t)(aUs / 1000000), (int32_t)(aUs % 1000000) * 1000 };
	return hsys(SYS_futex, a, FUTEX_WAIT, v, &ts);
}

// ---- time ----
uint64_t hnow_ns(void);                                       // monotonic ns (generic timer, else syscall)
uint64_t hnow_ns_syscall(void);                               // CLOCK_MONOTONIC through the syscall
uint64_t hcounter(void);                                      // the generic timer count (CNTVCT)
uint32_t hcounter_freq(void);                                 // its frequency (CNTFRQ), 0 if unknown
void hsleep_us(uint32_t us);

// ---- signals (ARM compat ucontext layout, arch/arm/include/asm/ucontext.h) ----
typedef struct {
	uint32_t trap_no, error_code, oldmask;
	uint32_t r[11];         // r0..r10
	uint32_t fp, ip, sp, lr, pc, cpsr, fault_address;
} hsigcontext;
typedef struct {
	uint32_t uc_flags, uc_link;
	uint32_t ss_sp, ss_flags, ss_size;
	hsigcontext mc;
	uint32_t sigmask[32];   // sigset_t (128 bytes in the kernel's compat ucontext)
} hucontext;
typedef struct {
	int si_signo, si_errno, si_code;
	union {
		struct { uint32_t addr; } fault;              // SIGSEGV/SIGILL/SIGBUS: si_addr
		struct { uint32_t call_addr; int syscall; uint32_t arch; } sys;  // SIGSYS
		uint32_t pad[29];
	};
} hsiginfo;
enum { SIGILL = 4, SIGTRAP = 5, SIGABRT = 6, SIGBUS = 7, SIGKILL = 9, SIGUSR1 = 10, SIGSEGV = 11,
       SIGUSR2 = 12, SIGALRM = 14, SIGTERM = 15, SIGCHLD = 17, SIGSYS = 31 };
typedef void (*hsighandler)(int, hsiginfo *, hucontext *);
int hsigaction(int sig, hsighandler h, uint32_t extra_flags);  // SA_SIGINFO|SA_RESTORER always set
enum { SA_NODEFER = 0x40000000, SA_ONSTACK = 0x08000000, SA_RESTART = 0x10000000 };
int hsigunblock(int sig);

// ---- seccomp: an svc instruction (ARM, 4 bytes) at an address in [lo, hi) raises SIGSYS instead of
// making a Linux syscall (si_call_addr = the address after it); every other svc goes through ----
int hseccomp_trap_range(uint32_t lo, uint32_t hi);

// ---- environment (htest --env NAME=VALUE / --trace MASK) ----
const char *hgetenv(const char *name);
uint32_t hgetenv_hex(const char *name, uint32_t dflt);

// ---- output ----
void hwrite(int fd, const void *p, size_t n);
int hvsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int hsnprintf(char *buf, size_t n, const char *fmt, ...);
void hprintf(const char *fmt, ...);                           // to fd 1, one write per call

// ---- test checks: "  ok <what>" / "  FAIL <what>" lines, the runner counts them ----
extern int htest_failures;
#define HCHECK(cond, ...) htest_check(!!(cond), __LINE__, __VA_ARGS__)
void htest_check(int ok, int line, const char *fmt, ...);
int htest_done(void);                                         // exit code: 0 all ok, 1 otherwise
void htest_note(const char *fmt, ...);                        // "  note ..." (measurements)

int main(int argc, char **argv);

#ifdef __cplusplus
}
#endif
