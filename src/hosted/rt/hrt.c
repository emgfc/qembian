// hrt: freestanding runtime for 32-bit ARM Linux processes (see hrt.h).
#include "hrt.h"

int htest_failures;
static int htest_checks;

// ---- memory and strings (the compiler may emit calls to these) ----
__attribute__((weak)) void *memcpy(void *d, const void *s, size_t n) {
	uint8_t *dp = d; const uint8_t *sp = s;
	while (n--) *dp++ = *sp++;
	return d;
}
__attribute__((weak)) void *memmove(void *d, const void *s, size_t n) {
	uint8_t *dp = d; const uint8_t *sp = s;
	if (dp < sp) while (n--) *dp++ = *sp++;
	else while (n--) dp[n] = sp[n];
	return d;
}
__attribute__((weak)) void *memset(void *d, int c, size_t n) {
	uint8_t *dp = d;
	while (n--) *dp++ = (uint8_t)c;
	return d;
}
int memcmp(const void *a, const void *b, size_t n) {
	const uint8_t *x = a, *y = b;
	for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
	return 0;
}
__attribute__((weak)) size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
	while (*a && *a == *b) a++, b++;
	return (uint8_t)*a - (uint8_t)*b;
}
__attribute__((weak)) void *memclr(void *d, size_t n) { return memset(d, 0, n); }
void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memset4(void *d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }

// 64-bit unsigned division (EABI helper; the CPU has 32-bit sdiv/udiv only)
typedef struct { uint64_t q, r; } hu64qr;
static hu64qr udivmod64(uint64_t n, uint64_t d) {
	hu64qr x = { 0, 0 };
	if (!d) return x;
	for (int i = 63; i >= 0; i--) {
		x.r = (x.r << 1) | ((n >> i) & 1);
		if (x.r >= d) { x.r -= d; x.q |= 1ull << i; }
	}
	return x;
}
// returns {quotient in r0:r1, remainder in r2:r3}
__attribute__((naked)) void __aeabi_uldivmod(void) {
	__asm__ volatile(
		"push {r4, lr}\n"
		"sub sp, sp, #16\n"
		"mov r4, sp\n"
		"push {r2, r3}\n"           // d on the stack as the 3rd/4th argument words
		"mov r3, r1\n"
		"mov r2, r0\n"
		"mov r0, r4\n"              // struct return pointer
		"bl hrt_udivmod64_s\n"
		"add sp, sp, #8\n"
		"ldm sp, {r0, r1, r2, r3}\n"
		"add sp, sp, #16\n"
		"pop {r4, pc}\n");
}
hu64qr hrt_udivmod64_s(uint64_t n, uint64_t d) { return udivmod64(n, d); }

// signed 64-bit division: quotient r0:r1, remainder r2:r3 (truncating, remainder has the dividend's sign)
typedef struct { int64_t q, r; } hs64qr;
hs64qr hrt_divmod64_s(int64_t n, int64_t d) {
	uint64_t un = n < 0 ? -(uint64_t)n : (uint64_t)n, ud = d < 0 ? -(uint64_t)d : (uint64_t)d;
	hu64qr x = udivmod64(un, ud);
	hs64qr y = { (int64_t)x.q, (int64_t)x.r };
	if ((n < 0) != (d < 0)) y.q = -y.q;
	if (n < 0) y.r = -y.r;
	return y;
}
__attribute__((naked)) void __aeabi_ldivmod(void) {
	__asm__ volatile(
		"push {r4, lr}\n"
		"sub sp, sp, #16\n"
		"mov r4, sp\n"
		"push {r2, r3}\n"
		"mov r3, r1\n"
		"mov r2, r0\n"
		"mov r0, r4\n"
		"bl hrt_divmod64_s\n"
		"add sp, sp, #8\n"
		"ldm sp, {r0, r1, r2, r3}\n"
		"add sp, sp, #16\n"
		"pop {r4, pc}\n");
}

// ---- process ----
void hexit(int code) {
	for (;;) hsys(SYS_exit_group, code);
}

void *hmmap(void *addr, size_t len, int prot, int flags, int fd, uint32_t off) {
	return (void *)hsys(SYS_mmap2, addr, len, prot, flags, fd, off >> 12);
}

int hwait(int pid) {
	int st = 0;
	int r = hsys(SYS_wait4, pid, &st, 0, 0);
	if (r < 0) return -1;
	if ((st & 0x7f) == 0) return (st >> 8) & 0xff;
	return 128 + (st & 0x7f);
}

enum { CLONE_VM = 0x100, CLONE_FS = 0x200, CLONE_FILES = 0x400, CLONE_SIGHAND = 0x800,
       CLONE_THREAD = 0x10000, CLONE_SYSVSEM = 0x40000, CLONE_SETTLS = 0x80000,
       CLONE_PARENT_SETTID = 0x100000, CLONE_CHILD_CLEARTID = 0x200000 };
int hclone_raw(uint32_t flags, void *stack, volatile int *ptid, uint32_t tls, volatile int *ctid,
               int (*fn)(void *), void *arg);

int hthread(int (*fn)(void *), void *arg, void *stack, size_t stacksize, volatile int *ctid) {
	uint32_t flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM |
	                 CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_SETTLS;
	void *top = (void *)(((uint32_t)stack + stacksize) & ~7u);
	// TLS (TPIDRURO) 0: a new thread is no nanothread until it says so (LxSetSelf). Bug 2026-11-12: without
	// CLONE_SETTLS it inherited its creator's record, and the phone window's display thread (started by the
	// main thread) passed for a nanothread in the keyboard model's lock: it masked interrupts as the main
	// thread and hung on the first key. Regression: t_tls.
	return hclone_raw(flags, top, ctid, 0, ctid, fn, arg);
}

void hthread_join(volatile int *ctid) {
	int t;
	while ((t = *ctid) != 0) hfutex_wait(ctid, t);
}

// ---- time ----
// Monotonic time. The ARM generic timer's virtual count (CNTVCT, mrrc p15 1 c14) is readable from 32-bit
// user mode on arm64 Linux (CNTKCTL_EL1.EL0VCTEN, set for the vDSO): no syscall. CNTFRQ gives its rate.
// Falls back to clock_gettime when the frequency reads as 0.
static uint32_t cnt_freq;
static uint64_t cnt_base_count, cnt_base_ns;
static int cnt_state;           // 0 unknown, 1 counter, -1 syscall

uint64_t hnow_ns_syscall(void) {
	struct { int32_t s, ns; } ts;
	hsys(SYS_clock_gettime, 1, &ts);
	return (uint64_t)(uint32_t)ts.s * 1000000000ull + (uint32_t)ts.ns;
}

uint64_t hcounter(void) {
	uint32_t lo, hi;
	__asm__ volatile("isb\n mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));
	return ((uint64_t)hi << 32) | lo;
}

uint32_t hcounter_freq(void) {
	uint32_t f;
	__asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(f));
	return f;
}

uint64_t hnow_ns(void) {
	if (cnt_state == 0) {
		uint32_t f = hcounter_freq();
		if (f) {
			cnt_base_ns = hnow_ns_syscall();
			cnt_base_count = hcounter();
			cnt_freq = f;
			cnt_state = 1;
		} else
			cnt_state = -1;
	}
	if (cnt_state < 0)
		return hnow_ns_syscall();
	uint64_t d = hcounter() - cnt_base_count;
	// d * 1e9 / f without overflow for ~centuries: split seconds and remainder
	hu64qr x = udivmod64(d, cnt_freq);
	return cnt_base_ns + x.q * 1000000000ull + udivmod64(x.r * 1000000000ull, cnt_freq).q;
}

void hsleep_us(uint32_t us) {
	struct { int32_t s, ns; } ts = { (int32_t)(us / 1000000), (int32_t)(us % 1000000) * 1000 };
	hsys(SYS_nanosleep, &ts, 0);
}

// ---- signals ----
void hrt_restorer(void);
enum { SA_SIGINFO = 4, SA_RESTORER = 0x04000000 };
int hsigaction(int sig, hsighandler h, uint32_t extra) {
	struct { void *handler; uint32_t flags; void *restorer; uint32_t mask[2]; } sa = {
		(void *)h, SA_SIGINFO | SA_RESTORER | extra, (void *)hrt_restorer, { 0, 0 } };
	return hsys(SYS_rt_sigaction, sig, &sa, 0, 8);
}
int hsigunblock(int sig) {
	uint32_t m[2] = { 0, 0 };
	m[(sig - 1) / 32] = 1u << ((sig - 1) % 32);
	return hsys(SYS_rt_sigprocmask, 1 /* SIG_UNBLOCK */, m, 0, 8);
}

// ---- seccomp ----
struct hbpf { uint16_t code; uint8_t jt, jf; uint32_t k; };
int hseccomp_trap_range(uint32_t lo, uint32_t hi) {
	// seccomp_data: nr @0, arch @4, instruction_pointer @8 (the address after the svc)
	struct hbpf f[] = {
		{ 0x20, 0, 0, 4 },                    // ld arch
		{ 0x15, 1, 0, 0x40000028 },           // jeq AUDIT_ARCH_ARM ? +1 : allow
		{ 0x06, 0, 0, 0x7fff0000 },           // ret ALLOW
		{ 0x20, 0, 0, 8 },                    // ld ip (low word)
		{ 0x35, 0, 2, lo + 4 },               // jge lo+4 ? next : allow
		{ 0x35, 1, 0, hi + 4 },               // jge hi+4 ? allow : trap
		{ 0x06, 0, 0, 0x00030000 },           // ret TRAP
		{ 0x06, 0, 0, 0x7fff0000 },           // ret ALLOW
	};
	struct { uint16_t len; uint16_t pad; struct hbpf *f; } prog = { sizeof f / sizeof f[0], 0, f };
	int r = hsys(SYS_prctl, 38 /* PR_SET_NO_NEW_PRIVS */, 1, 0, 0, 0);
	if (r) return r;
	return hsys(SYS_seccomp, 1 /* SECCOMP_SET_MODE_FILTER */, 0, &prog);
}

// ---- output ----
void hwrite(int fd, const void *p, size_t n) {
	const char *c = p;
	while (n) {
		int r = hsys(SYS_write, fd, c, n);
		if (r <= 0) return;
		c += r; n -= r;
	}
}

static void put(char *buf, size_t n, size_t *i, char c) { if (*i + 1 < n) buf[*i] = c; (*i)++; }

int hvsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
	size_t i = 0;
	for (; *fmt; fmt++) {
		if (*fmt != '%') { put(buf, n, &i, *fmt); continue; }
		fmt++;
		int zero = 0, width = 0, ll = 0, prec = -1;
		if (*fmt == '0') zero = 1, fmt++;
		while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {          // precision: only for %s (%.*s: length from the arguments)
			fmt++;
			if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
			else { prec = 0; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
		}
		while (*fmt == 'l') ll++, fmt++;
		char tmp[24]; int t = 0;
		uint64_t v = 0; int neg = 0, base = 10;
		switch (*fmt) {
		case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
			int l = 0; while ((prec < 0 || l < prec) && s[l]) l++;
			while (width-- > l) put(buf, n, &i, ' ');
			for (int k = 0; k < l; k++) put(buf, n, &i, s[k]); continue; }
		case 'c': put(buf, n, &i, (char)va_arg(ap, int)); continue;
		case '%': put(buf, n, &i, '%'); continue;
		case 'p': v = (uint32_t)va_arg(ap, void *); base = 16; zero = 1; width = 8;
			put(buf, n, &i, '0'); put(buf, n, &i, 'x'); break;
		case 'x': case 'X': base = 16; /* fallthrough */
		case 'u': v = ll >= 2 ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t); break;
		case 'd': { int64_t s = ll >= 2 ? va_arg(ap, int64_t) : va_arg(ap, int32_t);
			if (s < 0) neg = 1, s = -s; v = (uint64_t)s; break; }
		default: put(buf, n, &i, '?'); continue;
		}
		do {
			uint32_t dgt;
			if (base == 16) { dgt = v & 15; v >>= 4; }
			else { hu64qr x = udivmod64(v, 10); dgt = (uint32_t)x.r; v = x.q; }
			tmp[t++] = "0123456789abcdef"[dgt];
		} while (v);
		if (neg) tmp[t++] = '-';
		while (width-- > t) put(buf, n, &i, zero ? '0' : ' ');
		while (t) put(buf, n, &i, tmp[--t]);
	}
	if (n) buf[i < n ? i : n - 1] = 0;
	return (int)i;
}

int hsnprintf(char *buf, size_t n, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	int r = hvsnprintf(buf, n, fmt, ap);
	va_end(ap);
	return r;
}

void hprintf(const char *fmt, ...) {
	char buf[512];
	va_list ap; va_start(ap, fmt);
	int r = hvsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	hwrite(1, buf, r < (int)sizeof buf ? r : (int)sizeof buf - 1);
}

// ---- tests ----
static void tline(const char *tag, int line, const char *fmt, va_list ap) {
	char buf[400], out[480];
	hvsnprintf(buf, sizeof buf, fmt, ap);
	int r = line ? hsnprintf(out, sizeof out, "  %s %s (line %d)\n", tag, buf, line)
	             : hsnprintf(out, sizeof out, "  %s %s\n", tag, buf);
	hwrite(1, out, r < (int)sizeof out ? r : (int)sizeof out - 1);
}
void htest_check(int ok, int line, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	htest_checks++;
	if (!ok) htest_failures++;
	tline(ok ? "ok" : "FAIL", ok ? 0 : line, fmt, ap);
	va_end(ap);
}
void htest_note(const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	tline("note", 0, fmt, ap);
	va_end(ap);
}
int htest_done(void) {
	hprintf("  checks %d failures %d\n", htest_checks, htest_failures);
	return htest_failures ? 1 : 0;
}

// static constructors (C++ globals such as the nanokernel's TheScheduler): run before main.
// Bug 2026-11-12: _start called main directly, globals stayed zero, the first timer tick crashed in
// SDblQue::Add; regression test src/hosted/tests/t_ctors.cpp.
typedef void (*hinitfn)(int, char **, char **);
extern hinitfn __preinit_array_start[] __attribute__((weak)), __preinit_array_end[] __attribute__((weak));
extern hinitfn __init_array_start[] __attribute__((weak)), __init_array_end[] __attribute__((weak));

static char **hrt_envp;
const char *hgetenv(const char *name) {
	size_t n = strlen(name);
	for (char **e = hrt_envp; e && *e; e++)
		if (!memcmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
	return 0;
}

uint32_t hgetenv_hex(const char *name, uint32_t dflt) {
	const char *v = hgetenv(name);
	if (!v) return dflt;
	uint32_t x = 0;
	if (v[0] == '0' && (v[1] | 32) == 'x') v += 2;
	for (; *v; v++) x = x * 16 + (*v <= '9' ? *v - '0' : (*v | 32) - 'a' + 10);
	return x;
}

int hrt_main(int argc, char **argv, char **envp) {
	hrt_envp = envp;
	for (hinitfn *f = __preinit_array_start; f && f < __preinit_array_end; f++) (*f)(argc, argv, envp);
	for (hinitfn *f = __init_array_start; f && f < __init_array_end; f++) (*f)(argc, argv, envp);
	return main(argc, argv);
}
