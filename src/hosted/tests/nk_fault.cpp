// nk_fault: the fault paths report instead of recursing.
// Regression for the bug of 2026-11-12: NKFault called NKern::CurrentContext(), which asserts in an
// inconsistent state and faults again: endless recursion, the process died of a stack overflow with
// no report. Here a non-scheduled host thread (an 'interrupt' thread outside StartOfInterrupt, where
// CurrentContext's own assertion fails) calls a nanokernel API that faults; the child must print one
// "NKERN FAULT" line and exit 101. Also: a stack overflow in a host thread is reported (alternate
// signal stack) instead of a silent SIGSEGV.
#include <nk_priv.h>

static NThread Initial;

static TInt Faulter(TAny*)
	{
	FAULT();			// as a nanokernel assertion would
	return 0;
	}

static TInt Deep(TInt n) { volatile TUint8 pad[512]; pad[0] = (TUint8)n; return n ? Deep(n - 1) + pad[0] : 0; }
static TInt Overflow(TAny*) { return Deep(1000000); }

static TInt RunChild(TLxThreadFunction aFn)
	{
	TInt pid = hfork();
	if (pid == 0)
		{
		CreateLinuxThread(EThreadEvent, aFn, 0, ETrue);
		for (;;) hsleep_us(100000);
		}
	return hwait(pid);
	}

int main(int, char**)
	{
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iTimeslice = -1;
	NTimerQ::Init1(1000);
	NKern::Init(&Initial, info);
	TInt r = RunChild(&Faulter);
	HCHECK(r == 101, "a fault in a host thread exits 101 after one report (got %d)", r);
	r = RunChild(&Overflow);
	HCHECK(r == 128 + SIGSEGV, "a stack overflow is caught on the alternate stack and reported (exit %d)", r);
	return htest_done();
	}
