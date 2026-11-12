// kn_devlock: a nanothread using a board device while that device's interrupt fires. Regression for bug
// 2026-11-12 (start-up hung at the N95's start-up tone): the device models (hsnd.cpp, hmodem.cpp) took
// their spinlock with only the kernel locked; an interrupt suspended the nanothread holding it, and the ISR,
// which reads the device's status as ehwa's does, spun for ever on that lock. Now the lock is taken with
// interrupts disabled. The loop raises the sound interrupt on every START (an invalid ring: error event at
// once) and reads registers in between; the ISR reads and acknowledges. A hang is the watchdog's failure.
#include <kernel/kern_priv.h>
#include <assp.h>

extern void HostedStartSystemTimer(TInt aPeriodUs);
extern void HostedStartWatchdog(TInt aSeconds);
extern volatile TUint32 HostedProgress;
extern TInt HostedBoardInit(const char* aBoardConfig, const char* aRofsImage);
extern TInt HostedSndAttach();
extern TUint32 HostedIoRead(TUint32 aAddr);
extern void HostedIoWrite(TUint32 aAddr, TUint32 aValue);

static const TUint32 KSnd = 0xC6000000u + 0x0e * 0x1000, KSlot = KSnd + 0x100;
static const TInt KSndIrq = 13, KRounds = 20000;
static NThread InitialThread, MainThread;
extern TSuperPage SuperPage;				// kplat.cpp: the kernel boot sets this up; no kernel boot here
extern TLinAddr SuperPageAddress;
static volatile TInt IsrRuns;

static void SndIsr(TAny*)
	{
	TUint32 st = HostedIoRead(KSnd + 6 * 4);		// INT_STATUS, as ehwa's ISR
	(void)HostedIoRead(KSlot + 11 * 4);
	HostedIoWrite(KSnd + 6 * 4, st);				// acknowledge
	++IsrRuns;
	}

static void Main(TAny*)
	{
	HostedIoWrite(KSnd + 7 * 4, 1);					// slot 0 interrupts
	HostedIoWrite(KSlot + 5 * 4, 4);				// a ring too small: START reports an error
	for (TInt i = 0; i < KRounds; ++i)
		{
		HostedIoWrite(KSlot + 0 * 4, 1);
		for (TInt j = 0; j < 8; ++j)
			(void)HostedIoRead(KSlot + 11 * 4);
		++HostedProgress;
		}
	HCHECK(IsrRuns > 0, "%d rounds done, the ISR ran %d times, no deadlock", KRounds, IsrRuns);
	hexit(htest_done());
	}

int main(int, char**)
	{
	LxInit();
	SuperPageAddress = (TLinAddr)&SuperPage;		// KDebugNum (NTimerQ::Init1) reads its trace mask
	HCHECK(HostedBoardInit(NULL, NULL) == KErrNone, "board");
	HCHECK(HostedSndAttach() == KErrNone, "sound device");
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iTimeslice = -1;
	NKern::Init(&InitialThread, info);
	HostedStartWatchdog(20);
	HCHECK(Interrupt::Bind(KSndIrq, &SndIsr, NULL) == KErrNone && Interrupt::Enable(KSndIrq) == KErrNone, "ISR bound");
	NThread* t = &MainThread;
	memclr(&info, sizeof(info));
	info.iFunction = &Main;
	info.iPriority = 16;
	info.iTimeslice = -1;
	HCHECK(NKern::ThreadCreate(t, info) == KErrNone, "nanothread");
	NKern::ThreadResume(t);
	for (;;)
		NKern::Idle();
	}
