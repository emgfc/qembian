// nk_exitrace: interrupts racing thread exits.
// Regression for the bug of 2026-11-12 (nk_kill hung once on 2 CPUs): an interrupt read the current
// thread just as an exiting thread handed over to the next one and signalled it to suspend; the thread
// exited without handling the signal and the interrupt waited forever. Provoked here on purpose: an
// interrupt storm thread (StartOfInterrupt/EndOfInterrupt back to back, plus IDFCs) while 300 short
// threads are created, run and exit, and 100 more are killed while running. Must finish, all exits seen.
// (The storm leaves short gaps: back-to-back interrupts livelock the interrupted thread, as on hardware.)
#include <nk_priv.h>

extern void HostedStartSystemTimer(TInt aPeriodUs);

static NThread Initial;
static NFastSemaphore ExitSem;
static volatile TInt Exits;
static volatile TInt StormStop;
static volatile TUint32 Storms;

static void ExitDfcFn(TAny*) { ++Exits; NKern::FSSignal(&ExitSem); }
static TDfc* ExitDfc;
static TDfc* ExitHandler(NThread*) { return ExitDfc; }
static const SNThreadHandlers Handlers = { &ExitHandler, NTHREAD_DEFAULT_STATE_HANDLER, 0, 0 };

static void Nop(TAny*) {}
static void IdfcFn(TAny*) {}
static TDfc* StormIdfc;

static TInt Storm(TAny*)
	{
	while (!StormStop)
		{
		StartOfInterrupt();
		if ((Storms & 7) == 0)
			StormIdfc->Add();
		EndOfInterrupt();
		++Storms;
		// gaps: a storm without any starves the interrupted thread for ever (it is resumed and suspended
		// again before it runs: a livelock real hardware has too); short ones still race every exit
		hsys(SYS_sched_yield);
		if ((Storms & 3) == 0)
			hsleep_us(50);
		}
	return 0;
	}

static void Spinner(TAny*)
	{
	for (;;) {}
	}

static NThread* NewThread(NThreadFunction aFunc, TInt aPri)
	{
	NThread* t = new NThread;
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iFunction = aFunc;
	info.iPriority = aPri;
	info.iTimeslice = -1;
	info.iHandlers = &Handlers;
	if (NKern::ThreadCreate(t, info) != KErrNone)
		return 0;
	NKern::ThreadResume(t);
	return t;
	}

static void MainFn(TAny*)
	{
	ExitSem.iOwningThread = NKern::CurrentThread();
	CreateLinuxThread(EThreadEvent, &Storm, 0, ETrue);
	TInt created = 0;
	for (TInt i = 0; i < 300; ++i)
		{
		if (NewThread(&Nop, 10 + (i % 20)))		// below and above main: exits in both orders
			++created;
		NKern::FSWait(&ExitSem);
		}
	HCHECK(created == 300 && Exits == 300, "300 short threads created and exited under an interrupt storm (%d, %d)", created, Exits);
	TInt killed = 0;
	for (TInt i = 0; i < 100; ++i)
		{
		NThread* t = NewThread(&Spinner, 12);
		NKern::Sleep(1);
		NKern::ThreadKill(t);
		NKern::FSWait(&ExitSem);
		++killed;
		}
	HCHECK(killed == 100 && Exits == 400, "100 running threads killed under the storm (%d)", Exits);
	StormStop = 1;
	htest_note("%u interrupts in the storm", Storms);
	hexit(htest_done());
	}

int main(int, char**)
	{
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
	ExitDfc = new TDfc(&ExitDfcFn, 0, q, 1);
	StormIdfc = new TDfc(&IdfcFn, 0);
	HostedStartSystemTimer(1000);
	NThread* m = new NThread;
	SNThreadCreateInfo mi;
	memclr(&mi, sizeof(mi));
	mi.iFunction = &MainFn;
	mi.iPriority = 20;
	mi.iTimeslice = -1;
	NKern::ThreadCreate(m, mi);
	NKern::ThreadResume(m);
	for (;;)
		NKern::Idle();
	}
