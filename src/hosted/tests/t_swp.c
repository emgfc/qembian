// t_swp: ARMv5 EUSER uses SWP/SWPB (request completion, locks). On arm64 Linux they run only through the
// kernel's emulation (CONFIG_SWP_EMULATION, /proc/sys/abi/swp = 1). Also the CP15 barrier operations
// (mcr p15 c7) that ARMv5/v6 code uses (abi.cp15_barrier), and LDREX/STREX natively.
#include "hrt.h"

static uint32_t swp(uint32_t v, volatile uint32_t *a) {
	register uint32_t r0 __asm__("r0");
	register uint32_t r1 __asm__("r1") = v;
	register volatile uint32_t *r2 __asm__("r2") = a;
	__asm__ volatile(".inst 0xe1020091" : "=r"(r0) : "r"(r1), "r"(r2) : "memory");  // swp r0, r1, [r2]
	return r0;
}
static uint32_t swpb(uint32_t v, volatile uint8_t *a) {
	register uint32_t r0 __asm__("r0");
	register uint32_t r1 __asm__("r1") = v;
	register volatile uint8_t *r2 __asm__("r2") = a;
	__asm__ volatile(".inst 0xe1420091" : "=r"(r0) : "r"(r1), "r"(r2) : "memory");  // swpb r0, r1, [r2]
	return r0;
}

static void write_sysctl(const char *path, const char *v) {
	int fd = hsys(SYS_openat, -100, path, 1 /* O_WRONLY */, 0);
	if (fd >= 0) { hsys(SYS_write, fd, v, strlen(v)); hsys(SYS_close, fd); }
}

static void read_sysctl(const char *path, char *out, int n) {
	int fd = hsys(SYS_openat, -100, path, 0, 0);
	out[0] = 0;
	if (fd < 0) { hsnprintf(out, n, "missing"); return; }
	int r = hsys(SYS_read, fd, out, n - 1);
	hsys(SYS_close, fd);
	if (r > 0) { out[r] = 0; if (out[r - 1] == '\n') out[r - 1] = 0; }
}

static volatile int got_sigill;
static void on_sigill(int s, hsiginfo *si, hucontext *uc) {
	(void)s; (void)si;
	got_sigill++;
	uc->mc.pc += 4;
}

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	char v[16], c[16];
	read_sysctl("/proc/sys/abi/swp", v, sizeof v);
	read_sysctl("/proc/sys/abi/cp15_barrier", c, sizeof c);
	htest_note("abi.swp = %s, abi.cp15_barrier = %s (0 undef, 1 emulate, 2 hw)", v, c);
	hsigaction(SIGILL, on_sigill, 0);

	volatile uint32_t w = 5;
	uint32_t old = swp(7, &w);
	HCHECK(!got_sigill && old == 5 && w == 7, "swp: old %u new %u (SIGILL %d)", old, w, got_sigill);
	volatile uint8_t bytes[4] = { 1, 2, 3, 4 };
	old = swpb(0x99, &bytes[2]);
	HCHECK(!got_sigill && old == 3 && bytes[2] == 0x99 && bytes[1] == 2 && bytes[3] == 4, "swpb: old %u new %x", old, bytes[2]);

	// cost of one emulated swp
	got_sigill = 0;
	uint64_t t0 = hnow_ns();
	for (int i = 0; i < 20000; i++) swp(i, &w);
	uint64_t t1 = hnow_ns();
	htest_note("swp: %u ns each (TCG time, not phone time)", (uint32_t)((t1 - t0) / 20000));

	// is it the kernel's emulation, or does the CPU execute SWP itself? With abi.swp = 0 a real ARMv8
	// core raises SIGILL; QEMU TCG is known to execute SWP in AArch32 user mode regardless.
	write_sysctl("/proc/sys/abi/swp", "0");
	got_sigill = 0;
	swp(1, &w);
	htest_note("with abi.swp = 0: %s", got_sigill ? "SIGILL (the CPU traps: the emulation path is what ran above)"
	                                              : "no SIGILL (this CPU executes SWP itself: Linux's emulation not exercised)");
	write_sysctl("/proc/sys/abi/swp", v);
	got_sigill = 0;

	// CP15 barriers (ARMv6 style) as EUSER/kernel code of the ARMv5/v6 era issues them
	got_sigill = 0;
	register uint32_t z __asm__("r0") = 0;
	__asm__ volatile("mcr p15, 0, %0, c7, c10, 5" :: "r"(z) : "memory");   // DMB
	__asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(z) : "memory");   // DSB (drain write buffer)
	__asm__ volatile("mcr p15, 0, %0, c7, c5, 4" :: "r"(z) : "memory");    // ISB (prefetch flush)
	HCHECK(!got_sigill, "cp15 dmb/dsb/isb (SIGILL %d)", got_sigill);

	// LDREX/STREX: native on ARMv6K+ (the N8's EUSER)
	volatile uint32_t x = 10;
	uint32_t tmp, fail;
	__asm__ volatile("1: ldrex %0, [%2]\n add %0, %0, #1\n strex %1, %0, [%2]\n cmp %1, #0\n bne 1b"
	                 : "=&r"(tmp), "=&r"(fail) : "r"(&x) : "memory", "cc");
	HCHECK(x == 11, "ldrex/strex increment: %u", x);
	return htest_done();
}
