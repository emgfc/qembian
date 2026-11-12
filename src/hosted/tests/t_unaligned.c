// t_unaligned: unaligned memory accesses as the phones' user code may issue them, on an ARMv8 AArch32
// core under arm64 Linux (SCTLR.A = 0 at EL0, CONFIG_COMPAT_ALIGNMENT_FIXUPS).
// ARMv5 (N73) had *rotated* unaligned LDR; ARMv6 with U=1 (N95's ARM1136 under EKA2) and ARMv7+ do true
// unaligned LDR/STR/LDRH; LDRD/STRD/LDM/STM must be aligned and fault otherwise: Linux fixes them up
// (compat alignment fixups) or sends SIGBUS. Records what this machine does; fails only where the N95
// (ARMv6, U=1) semantics are not met.
#include "hrt.h"

static volatile int sigbus;
static void on_bus(int s, hsiginfo *si, hucontext *uc) {
	(void)s; (void)si;
	sigbus++;
	uc->mc.pc += 4;       // skip the faulting instruction
}

static uint8_t buf[64] __attribute__((aligned(16)));

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	for (int i = 0; i < 64; i++) buf[i] = (uint8_t)(0x10 + i);
	hsigaction(SIGBUS, on_bus, 0);
	hsigaction(SIGSEGV, on_bus, 0);

	uint32_t v;
	__asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(buf + 1) : "memory");
	HCHECK(!sigbus && v == 0x14131211, "LDR at +1: true unaligned (ARMv6 U=1 / ARMv7 semantics) %08x", v);
	uint32_t h;
	__asm__ volatile("ldrh %0, [%1]" : "=r"(h) : "r"(buf + 3) : "memory");
	HCHECK(!sigbus && h == 0x1413, "LDRH at +3: %04x", h);
	__asm__ volatile("str %0, [%1]" :: "r"(0xa1b2c3d4u), "r"(buf + 5) : "memory");
	HCHECK(!sigbus && buf[5] == 0xd4 && buf[8] == 0xa1 && buf[9] == 0x19, "STR at +5 writes 4 bytes, neighbours kept");

	sigbus = 0;
	register uint32_t lo __asm__("r4"), hi __asm__("r5");
	__asm__ volatile("ldrd r4, r5, [%2]" : "=r"(lo), "=r"(hi) : "r"(buf + 2) : "memory");
	htest_note("LDRD at +2: %s (%08x %08x)", sigbus ? "SIGBUS (no fixup)" : "fixed up by the kernel", lo, hi);
	// bytes 2..9 after the STR above: 12 13 14 d4 c3 b2 a1 19
	HCHECK(sigbus || (lo == 0xd4141312u && hi == 0x19a1b2c3u), "LDRD at +2: SIGBUS or the right words");

	sigbus = 0;
	uint32_t a = 0, b = 0;
	__asm__ volatile("mov r6, %2\n ldm r6, {r4, r5}\n mov %0, r4\n mov %1, r5" : "=r"(a), "=r"(b) : "r"(buf + 1) : "r4", "r5", "r6", "memory");
	htest_note("LDM at +1: %s", sigbus ? "SIGBUS (no fixup)" : "fixed up by the kernel");
	// bytes 1..8: 11 12 13 14 d4 c3 b2 a1
	HCHECK(sigbus || (a == 0x14131211u && b == 0xa1b2c3d4u), "LDM at +1: SIGBUS or the right words (%08x %08x)", a, b);

	// VFP: the N95's EUSER and apps are soft-float ARMv5 code, but some ROM code uses VFPv2
	sigbus = 0;
	hsigaction(SIGILL, on_bus, 0);
	register uint32_t fpsid __asm__("r0") = 0;
	__asm__ volatile(".inst 0xeef10a10" : "+r"(fpsid));      // vmrs r0, fpscr (soft-float build: encoded)
	HCHECK(!sigbus, "VFP present at EL0 (vmrs fpscr = %08x)", fpsid);
	return htest_done();
}
