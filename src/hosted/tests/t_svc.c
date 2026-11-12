// t_svc: Symbian exec calls are `svc #n` (n = exec number, fast execs with bit 23) in EUSER stubs; on
// arm64 Linux a 32-bit svc is a Linux syscall numbered by r7. The hosted kernel traps the ROM's svcs by
// address with a seccomp filter (SIGSYS) and lets its own svcs through (docs/hosted-eka2/architecture.md).
// Checks: every svc in the stub range traps, whatever r7 holds (exit, set_tls, garbage); svcs outside
// still reach Linux; registers in and out; nesting (a Linux syscall inside the SIGSYS handler); cost.
#include "hrt.h"

// "ROM stubs": EUSER-shaped `svc #n; bx lr` pairs in their own section (the trap range)
__asm__(
	".pushsection hstubs, \"ax\"\n .arm\n .balign 4\n"
	".global stub_fast1\n stub_fast1: svc #0x800001\n bx lr\n"          // fast exec 1 (User::Heap-like)
	".global stub_slow12\n stub_slow12: svc #0x12\n bx lr\n"            // a slow exec
	".global stub_r7\n stub_r7: push {r7, lr}\n mov r7, r1\n svc #0x34\n pop {r7, pc}\n"  // r7 = arg
	".global stub_regs\n stub_regs: svc #0x55\n bx lr\n"
	".popsection\n");
extern uint32_t __start_hstubs[], __stop_hstubs[];
uint32_t stub_fast1(uint32_t);
uint32_t stub_slow12(uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t stub_r7(uint32_t, uint32_t r7);
uint64_t stub_regs(uint32_t, uint32_t, uint32_t, uint32_t);

static volatile uint32_t traps, last_r7, nested_pid;
static void on_sigsys(int s, hsiginfo *si, hucontext *uc) {
	(void)s;
	uint32_t insn = *(uint32_t *)(si->sys.call_addr - 4);
	uint32_t n = insn & 0x00ffffff;
	traps++;
	last_r7 = uc->mc.r[7];
	switch (n) {
	case 0x800001: uc->mc.r[0] = uc->mc.r[0] + 1; break;                       // fast: r0 in, r0 out
	case 0x12: uc->mc.r[0] = uc->mc.r[0] ^ uc->mc.r[1] ^ uc->mc.r[2] ^ uc->mc.r[3]; break;
	case 0x34: nested_pid = hgetpid(); uc->mc.r[0] = 0xc0de; break;           // a real syscall inside
	case 0x55: uc->mc.r[0] = uc->mc.r[3]; uc->mc.r[1] = uc->mc.r[2]; break;   // two words out
	default: uc->mc.r[0] = 0xdeadbeef; break;
	}
	(void)si->sys.syscall;
}

uint32_t direct_call(uint32_t x) { return x + 1; }

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	uint32_t lo = (uint32_t)__start_hstubs, hi = (uint32_t)__stop_hstubs;
	htest_note("stub range %08x..%08x", lo, hi);
	HCHECK(hsigaction(SIGSYS, on_sigsys, SA_NODEFER) == 0, "sigaction SIGSYS");
	int r = hseccomp_trap_range(lo, hi);
	HCHECK(r == 0, "seccomp filter installed (%d)", r);

	HCHECK(stub_fast1(41) == 42 && traps == 1, "fast exec 0x800001: 41 -> 42 (traps %u)", traps);
	HCHECK(stub_slow12(1, 2, 4, 8) == 15, "slow exec 0x12 with 4 args");
	uint64_t two = stub_regs(0, 0, 0x22222222, 0x11111111);
	HCHECK(two == 0x2222222211111111ull, "r0/r1 out: %llx", two);

	// whatever r7 holds inside the range, Linux must not run it
	uint32_t before = traps;
	HCHECK(stub_r7(0, 1 /* exit */) == 0xc0de, "r7 = 1 (exit) trapped, process alive");
	HCHECK(stub_r7(0, 0x0f0005 /* set_tls */) == 0xc0de, "r7 = 0xf0005 (ARM set_tls) trapped");
	HCHECK(stub_r7(0, 0xffffffff) == 0xc0de, "r7 = -1 trapped");
	HCHECK(stub_r7(0, 248 /* exit_group */) == 0xc0de && last_r7 == 248, "r7 = exit_group trapped (r7 seen %u)", last_r7);
	HCHECK(traps == before + 4, "4 traps counted");
	HCHECK(nested_pid == (uint32_t)hgetpid(), "Linux syscall inside the SIGSYS handler works");
	HCHECK(hgetpid() > 0, "svc outside the range still reaches Linux");

	// cost: SIGSYS round trip vs a direct call (the patched-stub path) vs a plain Linux syscall
	enum { N = 20000 };
	uint64_t t0 = hnow_ns();
	for (uint32_t i = 0; i < N; i++) stub_fast1(i);
	uint64_t t1 = hnow_ns();
	volatile uint32_t sink = 0;
	uint32_t (*volatile dc)(uint32_t) = direct_call;
	for (uint32_t i = 0; i < N; i++) sink += dc(i);
	uint64_t t2 = hnow_ns();
	for (uint32_t i = 0; i < N; i++) hgetpid();
	uint64_t t3 = hnow_ns();
	htest_note("per call: SIGSYS exec %u ns, direct call %u ns, getpid %u ns (TCG time)",
	           (uint32_t)((t1 - t0) / N), (uint32_t)((t2 - t1) / N), (uint32_t)((t3 - t2) / N));
	return htest_done();
}
