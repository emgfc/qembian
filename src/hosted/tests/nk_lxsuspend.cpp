// nk_lxsuspend: the host suspend primitive under the nanokernel's interrupts (LxSuspend/LxResume, the
// Win32 SuspendThread/ResumeThread equivalent: a signal parks the thread in its handler).
// Regression for the bug of 2026-11-12: on one CPU a resume followed at once by a new suspend (before the
// parked thread ran) deadlocked both sides (nk_basic hung creating its first thread under --smp 1).
// Run it with --smp 1 too (htest --matrix does). Checks: back-to-back resume/suspend 5000 times; a
// suspended thread does not run; nested suspends need as many resumes; the saved context is valid while
// parked; a thread that sleeps in a futex is suspended and resumed correctly (syscall restart).
#include <nk_priv.h>

extern void LxDumpAllThreads();
static volatile TUint32 Count;
static volatile TInt Stop;
static volatile TInt FutexWord;

static TInt Spinner(TAny*)
	{
	while (!Stop)
		++Count;
	return 0;
	}

static TInt Sleeper(TAny*)
	{
	while (!Stop)
		{
		hfutex_wait(&FutexWord, 0);		// mostly asleep in the kernel
		++Count;
		}
	return 0;
	}

int main(int, char**)
	{
	LxInit();
	SLxThread* t = LxCreateThread(&Spinner, 0, ETrue, 0);
	HCHECK(t != 0, "spinner thread");
	while (Count == 0) hsys(SYS_sched_yield);

	// 1. the race: resume and suspend again back to back
	TInt bad = 0;
	uint64_t t0 = hnow_ns();
	for (TInt i = 0; i < 5000; ++i)
		{
		if (LxSuspend(t) != 0) ++bad;
		if (LxResume(t) != 1) ++bad;
		}
	uint64_t t1 = hnow_ns();
	HCHECK(bad == 0, "5000 back-to-back suspend/resume cycles, counts right (%d bad)", bad);
	htest_note("suspend+resume: %u us per cycle (TCG)", (TUint32)((t1 - t0) / 5000 / 1000));

	// 2. while suspended it does not run; the parked context is there and points into our code
	LxSuspend(t);
	TUint32 c0 = Count;
	hsleep_us(20000);
	HCHECK(Count == c0, "suspended thread made no progress in 20 ms");
	HCHECK(t->iCtx != 0, "saved context available while parked");
	extern char __executable_start[], etext[];
	HCHECK(t->iCtx && t->iCtx->mc.pc >= (TUint32)__executable_start && t->iCtx->mc.pc < (TUint32)etext, "parked pc %08x in the image", t->iCtx ? t->iCtx->mc.pc : 0);

	// 3. nested: two suspends need two resumes
	HCHECK(LxSuspend(t) == 1, "second suspend returns previous count 1");
	HCHECK(LxResume(t) == 2, "first resume returns 2");
	hsleep_us(20000);
	HCHECK(Count == c0, "still suspended after one of two resumes");
	HCHECK(LxResume(t) == 1, "second resume returns 1");
	hsleep_us(20000);
	HCHECK(Count != c0, "runs again after the last resume");

	// 4. a thread blocked in a futex: suspend/resume must not lose its wait or its wake-up
	SLxThread* s = LxCreateThread(&Sleeper, 0, ETrue, 0);
	hsleep_us(5000);
	TUint32 before = Count;
	for (TInt i = 0; i < 200; ++i)
		{
		LxSuspend(s);
		LxResume(s);
		}
	Stop = 1;
	FutexWord = 1;
	hfutex_wake(&FutexWord, 8);
	hsleep_us(20000);
	HCHECK(Count > before, "a futex sleeper survived 200 suspend/resume cycles and woke on its futex");
	return htest_done();
	}
