// nk_lxinit: the host thread record at process start.
// Regression for the bug of 2026-11-12: TPIDRURO (the register the hosted layer keeps its per-thread
// record in) is not zero when a 32-bit process starts on arm64 Linux; LxInit took a nonzero value for
// "already initialised" and skipped the set-up (silent crash at kernel boot). Checks: whatever
// TPIDRURO held, LxInit makes LxSelf() the main thread's record; a second LxInit keeps it; threads
// created afterwards get their own records; the user's TPIDRURW is left alone.
#include <nk_priv.h>

static volatile TInt Ok;
static SLxThread* Seen;
static TInt Thread(TAny*)
	{
	Seen = LxSelf();
	Ok = 1;
	return 0;
	}

int main(int, char**)
	{
	TUint32 before;
	__asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(before));
	htest_note("TPIDRURO at process start: %08x", before);
	hsys(SYS_ARM_set_tls, 0x1e3a2740);		// a stale value, as observed
	__asm__ volatile("mcr p15, 0, %0, c13, c0, 2" :: "r"(0x5eed));	// the user side's register
	// bug 2026-11-12 (kn_modem): code asking LxSelf() before LxInit got the stale value
	HCHECK(LxSelf() == NULL, "LxSelf() is NULL before LxInit, whatever TPIDRURO holds");
	LxInit();
	SLxThread* me = LxSelf();
	HCHECK(me && (TUint32)me != 0x1e3a2740 && me->iTid == hgettid(), "LxInit set up the main record over a stale TPIDRURO (%p)", me);
	LxInit();
	HCHECK(LxSelf() == me, "a second LxInit keeps it");
	TUint32 rw;
	__asm__ volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(rw));
	HCHECK(rw == 0x5eed, "TPIDRURW (the user side's) untouched (%x)", rw);
	SLxThread* t = LxCreateThread(&Thread, 0, ETrue, 0);
	while (!Ok) hsys(SYS_sched_yield);
	HCHECK(Seen == t && Seen != me, "a new thread sees its own record");
	return htest_done();
	}
