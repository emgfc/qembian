// nk_kill: thread exit and kill on the hosted nanokernel. Kill paths are where the Linux layer differs
// most from the ARM one: a preempted thread is killed by editing its parked signal context so that it
// resumes in DivertHook (ARM assembler) and exits from there; the host thread then ends (LxThreadExit).
// Cases: normal return; kill of a thread blocked on a fast semaphore; kill of a busy thread preempted by
// the timer (the DivertHook path); kill of a suspended thread; kill of a fast-mutex holder (deferred to
// its FMSignal); a thread killing itself; 50 create/kill cycles (host threads really end).
#include <nk_priv.h>

extern void HostedStartSystemTimer(TInt aPeriodUs);
extern volatile TUint32 HostedDiversions;
extern TInt HostedLiveStacks();

static NThread Initial;
static TDfcQue* Q;
static NFastSemaphore ExitSem;
static volatile TInt Exited[16];

static void ExitDfcFn(TAny* a)
	{
	++Exited[(TInt)a];
	NKern::FSSignal(&ExitSem);
	}
static TDfc* ExitDfcs[16];

static TDfc* ExitHandler(NThread* aT)
	{
	return ExitDfcs[aT->iSpare7];		// iSpare7: our slot number
	}

static const SNThreadHandlers Handlers = { &ExitHandler, NTHREAD_DEFAULT_STATE_HANDLER, 0, 0 };

static NThread* NewThread(TInt aSlot, NThreadFunction aFunc, TInt aPri, TAny* aArg, TBool aResume = ETrue)
	{
	NThread* t = new NThread;
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iFunction = aFunc;
	info.iPriority = aPri;
	info.iTimeslice = -1;
	info.iHandlers = &Handlers;
	info.iParameterBlock = (const TUint32*)aArg;
	TInt r = NKern::ThreadCreate(t, info);
	HCHECK(r == KErrNone, "create slot %d: %d", aSlot, r);
	t->iSpare7 = aSlot;
	if (!ExitDfcs[aSlot])
		ExitDfcs[aSlot] = new TDfc(&ExitDfcFn, (TAny*)aSlot, Q, 1);
	if (aResume)
		NKern::ThreadResume(t);
	return t;
	}

static void WaitExit(TInt aSlot, const char* aWhat)
	{
	TInt before = Exited[aSlot];
	NKern::FSWait(&ExitSem);
	HCHECK(Exited[aSlot] == before + 1, "%s: exit handler ran (slot %d)", aWhat, aSlot);
	}

// ---- thread bodies ----
static volatile TInt Returned;
static void Returns(TAny*) { Returned = 1; }

static NFastSemaphore NeverSignalled;
static volatile TInt PastWait;
static void BlocksForever(TAny*)
	{
	NeverSignalled.iOwningThread = NKern::CurrentThread();
	NKern::FSWait(&NeverSignalled);
	PastWait = 1;
	}

static volatile TUint32 Spins;
static void Spinner(TAny*)
	{
	for (;;)
		++Spins;
	}

static NFastMutex Mutex;
static volatile TInt InsideMutex, AfterSignal, WorkAfterKill;
static volatile TInt KillRequested;
static void MutexHolder(TAny*)
	{
	NKern::FMWait(&Mutex);
	InsideMutex = 1;
	while (!KillRequested)
		NKern::YieldTimeslice();
	for (volatile TInt i = 0; i < 10000; ++i) {}
	WorkAfterKill = 1;			// a kill is deferred while a fast mutex is held
	NKern::FMSignal(&Mutex);	// the deferred kill happens here
	AfterSignal = 1;
	}

static volatile TInt BeforeSelfKill, AfterSelfKill;
static void KillsItself(TAny*)
	{
	BeforeSelfKill = 1;
	NKern::ThreadKill(NKern::CurrentThread());
	AfterSelfKill = 1;
	}

static void MainThread(TAny*)
	{
	ExitSem.iOwningThread = NKern::CurrentThread();

	// 1. normal return
	NewThread(1, &Returns, 20, 0);
	WaitExit(1, "normal return");
	HCHECK(Returned == 1, "body ran before exit");

	// 2. blocked on a fast semaphore
	NThread* b = NewThread(2, &BlocksForever, 20, 0);
	NKern::Sleep(2);
	NKern::ThreadKill(b);
	WaitExit(2, "kill blocked thread");
	HCHECK(PastWait == 0, "the killed thread never returned from its wait");

	// 3. busy and preempted: main (30) sleeps, the spinner (12) runs, the timer preempts it, main kills it
	NThread* s = NewThread(3, &Spinner, 12, 0);
	NKern::Sleep(5);
	HCHECK(Spins > 0, "spinner ran (%u)", Spins);
	TUint32 d0 = HostedDiversions;
	NKern::ThreadKill(s);		// preempted (EResume) -> EResumeDiverted -> DivertHook -> exit
	WaitExit(3, "kill preempted busy thread (DivertHook)");
	HCHECK(HostedDiversions == d0 + 1, "the kill went through a context diversion (%u)", HostedDiversions - d0);
	TUint32 c = Spins;
	NKern::Sleep(10);
	HCHECK(Spins == c, "spinner stopped after the kill (%u -> %u)", c, Spins);

	// 4. suspended
	NThread* u = NewThread(4, &Spinner, 12, 0);
	NKern::Sleep(3);
	NKern::ThreadSuspend(u, 1);
	NKern::Sleep(2);
	NKern::ThreadKill(u);
	WaitExit(4, "kill suspended thread");

	// 5. holding a fast mutex: the kill waits for FMSignal
	NThread* m = NewThread(5, &MutexHolder, 20, 0);
	while (!InsideMutex)
		NKern::Sleep(1);
	NKern::ThreadKill(m);
	KillRequested = 1;
	WaitExit(5, "kill fast mutex holder");
	HCHECK(WorkAfterKill == 1 && AfterSignal == 0, "ran to FMSignal after the kill, died there (%d %d)", WorkAfterKill, AfterSignal);
	HCHECK(Mutex.iHoldingThread == 0, "the mutex is free");

	// 6. a thread killing itself
	NewThread(6, &KillsItself, 20, 0);
	WaitExit(6, "self kill");
	HCHECK(BeforeSelfKill == 1 && AfterSelfKill == 0, "self kill does not return");

	// 7. many create/kill cycles of preempted spinners: host threads end and their stacks are reclaimed
	TInt stacks0 = HostedLiveStacks();
	TInt ok = 0;
	for (TInt i = 0; i < 50; ++i)
		{
		NThread* x = NewThread(7, &Spinner, 12, 0);
		NKern::Sleep(1);
		NKern::ThreadKill(x);
		TInt before = Exited[7];
		NKern::FSWait(&ExitSem);
		if (Exited[7] == before + 1) ++ok;
		}
	HCHECK(ok == 50, "50 create/kill cycles (%d)", ok);
	NKern::Sleep(5);
	NewThread(8, &Returns, 20, 0);		// a creation reaps the stacks of the threads that are gone
	WaitExit(8, "reaping creation");
	TInt stacks1 = HostedLiveStacks();
	HCHECK(stacks1 <= stacks0 + 2, "dead threads' stacks reclaimed: %d live before, %d after 51 threads", stacks0, stacks1);
	hexit(htest_done());
	}

int main(int, char**)
	{
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iTimeslice = -1;
	NTimerQ::Init1(1000);
	NKern::Init(&Initial, info);
	Q = new TDfcQue;
	NThread* qt = new NThread;
	SNThreadCreateInfo qi;
	memclr(&qi, sizeof(qi));
	qi.iFunction = &TDfcQue::ThreadFunction;
	qi.iPriority = 48;
	qi.iTimeslice = -1;
	qi.iParameterBlock = (const TUint32*)Q;
	NKern::ThreadCreate(qt, qi);
	Q->iThread = qt;
	NKern::ThreadResume(qt);
	NTimerQ::Init3(Q);
	HostedStartSystemTimer(1000);
	NThread* mt = new NThread;
	SNThreadCreateInfo mi;
	memclr(&mi, sizeof(mi));
	mi.iFunction = &MainThread;
	mi.iPriority = 30;
	mi.iTimeslice = -1;
	NKern::ThreadCreate(mt, mi);
	NKern::ThreadResume(mt);
	for (;;)
		NKern::Idle();
	}
