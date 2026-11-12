// nk_exec: Symbian exec calls on the hosted nanokernel (P2's first piece). Code in a "ROM stub" section
// (EUSER-shaped `svc #n; bx lr`) runs in nanothreads; the svcs trap (seccomp, by address) into Dispatch,
// which runs the thread's fast/slow exec tables exactly as the Win32/ARM kernels do.
// Checks: fast exec with a result; 4-argument fast exec; fast exec 0 = WaitForAnyRequest woken by
// ThreadRequestSignal; slow exec; slow exec with KExecFlagClaim/Release (system lock held inside, free
// after); slow exec with preprocessing (arg 0 replaced); invalid fast and slow numbers reach the invalid
// handler and leave the kernel balanced (regression of the Win32 version's missing LeaveKernel); two
// threads hammering execs under round robin; exec cost through the trap vs a direct Dispatch call.
#include <nk_priv.h>

extern void HostedStartSystemTimer(TInt aPeriodUs);
extern TInt HostedExecTrapInit(TLinAddr aLo, TLinAddr aHi);
extern volatile TUint32 HostedExecTraps;
extern EXPORT_C TInt Dispatch(TInt aFunction, TInt* aArgs);

__asm__(
	".pushsection romstubs, \"ax\"\n .arm\n .balign 4\n"
	".global ExecWaitAny\n ExecWaitAny: svc #0x800000\n bx lr\n"
	".global ExecFast1\n ExecFast1: svc #0x800001\n bx lr\n"
	".global ExecFast2\n ExecFast2: svc #0x800002\n bx lr\n"
	".global ExecFastBad\n ExecFastBad: svc #0x800009\n bx lr\n"
	".global ExecSlow0\n ExecSlow0: svc #0x0\n bx lr\n"
	".global ExecSlow1\n ExecSlow1: svc #0x1\n bx lr\n"
	".global ExecSlow2\n ExecSlow2: svc #0x2\n bx lr\n"
	".global ExecSlowBad\n ExecSlowBad: svc #0x77\n bx lr\n"
	".popsection\n");
extern "C" {
extern TUint32 __start_romstubs[], __stop_romstubs[];
TInt ExecWaitAny();
TInt ExecFast1(TInt);
TInt ExecFast2(TInt, TInt, TInt, TInt);
TInt ExecFastBad(TInt);
TInt ExecSlow0(TInt);
TInt ExecSlow1(TInt);
TInt ExecSlow2(TInt, TInt);
TInt ExecSlowBad(TInt);
}

// ---- the exec handlers ("kernel side") ----
static TInt Fast1(TInt a) { return a + 1; }
static TInt Fast2(TInt a, TInt b, TInt c, TInt d) { return a + 10 * b + 100 * c + 1000 * d; }
static TInt Slow0(TInt a) { return a * 2; }
static volatile TInt LockHeldInside;
static TInt Slow1(TInt a) { LockHeldInside = TheScheduler.iLock.iHoldingThread == NKern::CurrentThread(); return a; }
static TInt Slow2(TInt a, TInt b) { return a + b; }		// a is the preprocessed value
static volatile TInt InvalidCalls;
static TInt Invalid(TInt a) { ++InvalidCalls; return -5; }	// KErrNotSupported
static void Preprocess(TInt* aArgs, TUint32) { aArgs[0] = aArgs[0] * 1000; }

static const struct { TInt iCount; TLinAddr iFn[2]; } FastTable = { 3, { (TLinAddr)&Fast1, (TLinAddr)&Fast2 } };
static const struct { TInt iCount; TLinAddr iInvalid; TLinAddr iPre; SSlowExecEntry iE[3]; } SlowTable = {
	3, (TLinAddr)&Invalid, (TLinAddr)&Preprocess, {
		{ 0, (TLinAddr)&Slow0 },
		{ KExecFlagClaim | KExecFlagRelease, (TLinAddr)&Slow1 },
		{ KExecFlagPreprocess, (TLinAddr)&Slow2 } } };

static NThread Initial;
static NFastSemaphore Done;

static NThread* NewThread(NThreadFunction aFunc, TInt aPri, TAny* aArg, TInt aTimeslice = -1)
	{
	NThread* t = new NThread;
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iFunction = aFunc;
	info.iPriority = aPri;
	info.iTimeslice = aTimeslice;
	info.iParameterBlock = (const TUint32*)aArg;
	info.iFastExecTable = (const SFastExecTable*)&FastTable;
	info.iSlowExecTable = (const SSlowExecTable*)&SlowTable;
	HCHECK(NKern::ThreadCreate(t, info) == KErrNone, "thread created");
	NKern::ThreadResume(t);
	return t;
	}

// "user mode": the thread leaves the kernel before calling the stubs, as an EKA2 user thread runs
static void EnterUser() { static_cast<NThread*>(NKern::CurrentThread())->iInKernel = 0; }
static void BackToKernel() { static_cast<NThread*>(NKern::CurrentThread())->iInKernel = 1; }

static NThread* Waiter;
static volatile TInt WaitReturned;
static void WaiterFn(TAny*)
	{
	EnterUser();
	ExecWaitAny();				// fast exec 0: wait for any request
	WaitReturned = 1;
	BackToKernel();
	NKern::FSSignal(&Done);
	}

static volatile TUint32 Hammer[2];
static volatile TInt HammerBad;
static volatile TInt HammerStop;
static void HammerFn(TAny* a)
	{
	TInt me = (TInt)a;
	EnterUser();
	while (!HammerStop)
		{
		TInt x = (TInt)Hammer[me];
		if (ExecFast1(x) != x + 1 || ExecSlow0(x) != 2 * x)
			++HammerBad;
		++Hammer[me];
		}
	BackToKernel();
	NKern::FSSignal(&Done);
	}

static void MainFn(TAny*)
	{
	Done.iOwningThread = NKern::CurrentThread();
	NThread& me = *static_cast<NThread*>(NKern::CurrentThread());
	EnterUser();
	TUint32 traps0 = HostedExecTraps;
	HCHECK(ExecFast1(41) == 42, "fast exec 1: 41 -> 42");
	HCHECK(ExecFast2(1, 2, 3, 4) == 4321, "fast exec 2 with 4 arguments: %d", ExecFast2(1, 2, 3, 4));
	HCHECK(ExecSlow0(21) == 42, "slow exec 0");
	HCHECK(ExecSlow1(7) == 7 && LockHeldInside == 1, "slow exec with claim: system lock held inside");
	HCHECK(TheScheduler.iLock.iHoldingThread == 0, "system lock released after the exec");
	HCHECK(ExecSlow2(3, 4) == 3004, "slow exec with preprocessing: arg 0 replaced (%d)", ExecSlow2(3, 4));
	HCHECK(ExecFastBad(1) == -5 && ExecSlowBad(1) == -5 && InvalidCalls == 2, "invalid fast and slow numbers -> invalid handler (%d)", InvalidCalls);
	HCHECK(me.iInKernel == 0, "kernel entry balanced after every exec, the invalid ones too (iInKernel %d)", me.iInKernel);
	HCHECK(HostedExecTraps - traps0 >= 8, "they all came through the trap (%u)", HostedExecTraps - traps0);
	BackToKernel();

	// fast exec 0 blocks until a request is signalled
	Waiter = NewThread(&WaiterFn, 20, 0);
	NKern::Sleep(5);
	HCHECK(WaitReturned == 0, "WaitForAnyRequest blocks");
	NKern::ThreadRequestSignal(Waiter);
	NKern::FSWait(&Done);
	HCHECK(WaitReturned == 1, "and returns after ThreadRequestSignal");

	// two threads hammering execs, preempted by the timer in the middle of them
	NKern::ThreadSetPriority(NKern::CurrentThread(), 30);
	NewThread(&HammerFn, 15, (TAny*)0, 2);
	NewThread(&HammerFn, 15, (TAny*)1, 2);
	NKern::Sleep(200);
	HammerStop = 1;
	NKern::FSWait(&Done);
	NKern::FSWait(&Done);
	HCHECK(HammerBad == 0 && Hammer[0] > 0 && Hammer[1] > 0, "2 threads x execs under round robin: %u + %u loops, %d wrong", Hammer[0], Hammer[1], HammerBad);

	// cost
	EnterUser();
	TUint64 t0 = hnow_ns();
	for (TInt i = 0; i < 2000; ++i) ExecFast1(i);
	TUint64 t1 = hnow_ns();
	TInt args[4] = { 1, 0, 0, 0 };
	for (TInt i = 0; i < 2000; ++i) Dispatch(0x800001, args);
	TUint64 t2 = hnow_ns();
	BackToKernel();
	htest_note("fast exec: %u ns through the trap, %u ns as a direct Dispatch call (TCG)",
	           (TUint32)((t1 - t0) / 2000), (TUint32)((t2 - t1) / 2000));
	hexit(htest_done());
	}

int main(int, char**)
	{
	LxInit();
	HCHECK(HostedExecTrapInit((TLinAddr)__start_romstubs, (TLinAddr)__stop_romstubs) == 0, "exec trap installed");
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iTimeslice = -1;
	NTimerQ::Init1(1000);
	NKern::Init(&Initial, info);
	TDfcQue* q = new TDfcQue;
	NThread* qt = new NThread;
	SNThreadCreateInfo qi;
	memclr(&qi, sizeof(qi));
	qi.iFunction = &TDfcQue::ThreadFunction;
	qi.iPriority = 48;
	qi.iTimeslice = -1;
	qi.iParameterBlock = (const TUint32*)q;
	NKern::ThreadCreate(qt, qi);
	q->iThread = qt;
	NKern::ThreadResume(qt);
	NTimerQ::Init3(q);
	HostedStartSystemTimer(1000);
	NewThread(&MainFn, 25, 0);
	for (;;)
		NKern::Idle();
	}
