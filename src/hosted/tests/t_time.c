// t_time: the clocks the hosted kernel uses. The ARM generic timer (CNTVCT/CNTFRQ) read directly from
// 32-bit user mode (no syscall) must exist, be monotonic and agree with CLOCK_MONOTONIC; hnow_ns (built
// on it) must agree too and be much cheaper than the syscall.
#include "hrt.h"

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	uint32_t f = hcounter_freq();
	HCHECK(f >= 1000000 && f <= 1000000000, "CNTFRQ = %u Hz", f);
	uint64_t a = hcounter(), b = hcounter();
	HCHECK(b >= a, "CNTVCT monotonic (%llu, %llu)", a, b);
	uint64_t s0 = hnow_ns_syscall(), n0 = hnow_ns();
	hsleep_us(50000);
	uint64_t s1 = hnow_ns_syscall(), n1 = hnow_ns();
	int64_t ds = (int64_t)(s1 - s0), dn = (int64_t)(n1 - n0);
	HCHECK(ds >= 50000000 && dn >= 50000000, "both clocks advanced >= 50 ms (%lld, %lld ns)", ds, dn);
	int64_t diff = ds - dn; if (diff < 0) diff = -diff;
	HCHECK(diff < 5000000, "they agree within 5 ms over the sleep (diff %lld ns)", diff);
	int mono = 1; uint64_t last = hnow_ns();
	for (int i = 0; i < 20000; i++) { uint64_t t = hnow_ns(); if (t < last) mono = 0; last = t; }
	HCHECK(mono, "hnow_ns monotonic over 20000 reads");
	uint64_t t0 = hnow_ns_syscall();
	for (int i = 0; i < 2000; i++) hnow_ns_syscall();
	uint64_t t1 = hnow_ns_syscall();
	for (int i = 0; i < 2000; i++) hnow_ns();
	uint64_t t2 = hnow_ns_syscall();
	htest_note("per read: clock_gettime %u ns, generic timer %u ns (TCG)", (uint32_t)((t1 - t0) / 2000), (uint32_t)((t2 - t1) / 2000));
	HCHECK((t2 - t1) < (t1 - t0), "the counter read is cheaper than the syscall");
	return htest_done();
}
