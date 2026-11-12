// t_tls: the thread ID registers. EKA2 (ARMv6K+, flexible model: N8) keeps the current thread's
// local data pointer in TPIDRURO (c13,c0,3, user read-only: on Linux set with the ARM set_tls call)
// and user code may use TPIDRURW (c13,c0,2). Both must be per thread and survive switches and signals.
#include "hrt.h"

static uint32_t get_ro(void) { uint32_t v; __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v)); return v; }
static uint32_t get_rw(void) { uint32_t v; __asm__ volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(v)); return v; }
static void set_rw(uint32_t v) { __asm__ volatile("mcr p15, 0, %0, c13, c0, 2" :: "r"(v)); }

static volatile int ok_thread = -1;
static volatile uint32_t sig_ro, sig_rw;
static void on_usr1(int s, hsiginfo *si, hucontext *uc) {
	(void)s; (void)si; (void)uc;
	sig_ro = get_ro(); sig_rw = get_rw();
	set_rw(0x5151);   // a handler changing it: the interrupted code must get its own value back?
}

static volatile uint32_t start_ro = 0xdead;
static int thread_fn(void *arg) {
	(void)arg;
	start_ro = get_ro();
	hsys(SYS_ARM_set_tls, 0xbbbb0000);
	set_rw(0xbbbb1111);
	int ok = 1;
	for (int i = 0; i < 200; i++) {
		hsys(SYS_sched_yield);
		if (get_ro() != 0xbbbb0000 || get_rw() != 0xbbbb1111) ok = 0;
	}
	ok_thread = ok;
	return 0;
}

static uint8_t stack[16384] __attribute__((aligned(16)));

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	HCHECK(hsys(SYS_ARM_set_tls, 0xaaaa0000) == 0, "set_tls");
	HCHECK(get_ro() == 0xaaaa0000, "TPIDRURO reads the set_tls value: %x", get_ro());
	htest_note("ARM get_tls (0xf0006): %ld (arm64 compat has no such call: read TPIDRURO instead)", hsys(SYS_ARM_get_tls));
	set_rw(0xaaaa1111);
	HCHECK(get_rw() == 0xaaaa1111, "TPIDRURW user-writable: %x", get_rw());

	volatile int ctid = 0;
	int tid = hthread(thread_fn, 0, stack, sizeof stack, &ctid);
	HCHECK(tid > 0, "thread created %d", tid);
	int ok = 1;
	for (int i = 0; i < 200; i++) {
		hsys(SYS_sched_yield);
		if (get_ro() != 0xaaaa0000 || get_rw() != 0xaaaa1111) ok = 0;
	}
	hthread_join(&ctid);
	HCHECK(ok, "main thread keeps its values across switches");
	HCHECK(ok_thread == 1, "second thread keeps its own values");
	// bug 2026-11-12: a new host thread inherited its creator's TPIDRURO, so LxSelf() named the creator's
	// record (the display thread passed for the main nanothread and hung in the keyboard model's lock)
	HCHECK(start_ro == 0, "a new thread starts with TPIDRURO 0, not its creator's (%x)", start_ro);

	hsigaction(SIGUSR1, on_usr1, 0);
	hsys(SYS_tgkill, hgetpid(), hgettid(), SIGUSR1);
	HCHECK(sig_ro == 0xaaaa0000 && sig_rw == 0xaaaa1111, "signal handler sees the thread's values (%x %x)", sig_ro, sig_rw);
	htest_note("TPIDRURW after a handler that changed it: %x (Linux does not restore it on sigreturn)", get_rw());
	HCHECK(get_ro() == 0xaaaa0000, "TPIDRURO unchanged after the signal");
	return htest_done();
}
