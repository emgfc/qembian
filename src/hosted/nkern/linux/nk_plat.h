// Hosted EKA2: the nanokernel's Linux platform (replaces include/nkern/win32/nk_plat.h).
// Derived from the Symbian Win32 emulator nanokernel (EPL v1.0, Nokia), same model: every nanothread
// is a host thread that runs only while the EKA2 scheduler has picked it; a thread parks on its own
// schedule lock (an auto-reset futex event in place of a Win32 event); "interrupts" are host threads
// that suspend the current nanothread (a signal in place of SuspendThread) while they run.
// docs/hosted-eka2/architecture.md, docs/hosted-eka2/nanokernel.md.

#ifndef __NK_LINUX_H__
#define __NK_LINUX_H__

#include <hrt.h>

// ---- host primitives ----
// auto-reset event (Win32 CreateEvent(bManualReset=FALSE)): Set wakes one waiter or stays signalled
struct TLxEvent
	{
	volatile TInt iState;
	inline TLxEvent() : iState(0) {}
	void Set();
	void Wait();
	};

// counting semaphore (Win32 CreateSemaphore)
struct TLxSemaphore
	{
	volatile TInt iCount;
	inline TLxSemaphore() : iCount(0) {}
	void Signal();
	void Wait();
	};

class NThread;
// per host thread record; TPIDRURO of every host thread we create points at its own (LxSelf)
struct SLxThread
	{
	TInt iTid;
	NThread* iNThread;		// null for an 'interrupt' (event) thread
	volatile TInt iSuspend;	// host suspend count (Win32 SuspendThread/ResumeThread)
	volatile TInt iAck;		// 1 while parked in the suspend handler
	hucontext* iCtx;		// the parked thread's saved context (valid while iAck)
	};

SLxThread* LxSelf();
TInt LxSuspend(SLxThread* aT);	// previous count; returns once the thread is parked
TInt LxResume(SLxThread* aT);	// previous count
void LxInit();

// The exception context handed to the kernel's exception handlers (Exc::Dispatch, IPC/kernel exception
// traps). Same name and the same leading fields as the Win32 emulator's (TWin32ExcInfo, EPL, Nokia) so
// that the ported kernel code reads it unchanged; ARM registers instead of x86 ones.
typedef void (*TExcHandler)(TAny*,TAny*);
struct TWin32ExcInfo
	{
	enum {EExcInKernel = 0x1};
public:
	TExcHandler iHandler;		// set by the exception handler: called on the faulting thread after return
	TAny* iParam[2];
	TUint iFlags;
	TUint32 iExcType;			// TExcType, filled in by EPOC
	TUint32 iExcCode;			// the Linux signal number
	TUint32 iExcDataAddress;	// the faulting data address (si_addr)
	TUint32 iR[13];				// r0-r12
	TUint32 iSp, iLr, iPc, iCpsr;
	};

typedef TInt (*TLxThreadFunction)(TAny*);
SLxThread* LxCreateThread(TLxThreadFunction aFunc, TAny* aArg, TBool aRun, NThread* aNThread);

enum TEmulThreadType
	{
	EThreadEvent,	// an 'interrupt' thread
	EThreadNKern	// a nanothread
	};

class NThread : public NThreadBase
	{
public:
	typedef void (*TDivert)();
	enum TWakeup {ERelease,EResume,EResumeLocked,EIdle,EEscaped,EResumeDiverted};
public:
	TInt Create(SNThreadCreateInfo& aInfo, TBool aInitial);
	void Stillborn();
	void DoForceExit();
	IMPORT_C static void Idle();
	TBool WakeUp();
	TBool IsSafeToPreempt();
	void Divert(TDivert aDivert);
	void ApplyDiversion();
private:
	static TInt StartThread(TAny* aParam);
	static void ExitSync();
	static void ExitAsync();
public:
	static void Diverted();
public:
	SLxThread* iLx;			// the host thread
	TLxEvent iScheduleLock;	// scheduling interlock
	TBool iDiverted;		// the thread is being diverted
	TDivert iDivert;		// function to invoke after reschedule, may be null
	TAny* iDivertReturn;	// return address from diversion
	TInt iInKernel;			// the thread is running 'in' the kernel
	TWakeup iWakeup;		// how to wake up the thread
	TLinAddr iUserStackBase;
	};

IMPORT_C SLxThread* CreateLinuxThread(TEmulThreadType aType, TLxThreadFunction aThreadFunc, TAny* aPtr, TBool aRun);
IMPORT_C void StartOfInterrupt();
IMPORT_C void EndOfInterrupt();

void SchedulerInit(NThread& aInit);
void SchedulerRegister(NThread& aSelf);
NThread* SchedulerThread();
NThread& CheckedCurrentThread();
void SchedulerLock();
void SchedulerUnlock();
void SchedulerEscape();
void SchedulerReenter();

inline void EnterKernel(TBool aCheck=TRUE)
	{
	if (++CheckedCurrentThread().iInKernel==1 && aCheck)
		{
		NThread& t = CheckedCurrentThread();
		__NK_ASSERT_ALWAYS(t.iCsCount==0);
		__NK_ASSERT_ALWAYS(t.iHeldFastMutex==0);
		__NK_ASSERT_ALWAYS(TheScheduler.iKernCSLocked==0);
		}
	}

void LeaveKernel();

class LinuxInterrupt
	{
public:
	void Init();
	TInt Mask();
	void Restore(TInt aLevel);
	void Begin();
	void End();
	inline TBool InInterrupt() const
		{return iInterrupted!=0;}
	void ForceReschedule();
	inline TBool InterruptsStatus(TBool aRequest) const
		{return aRequest?(iLevel==0):(iLevel!=0);}
	void Dump();
private:
	static void Reschedule(TAny*);
private:
	volatile TInt iLock;
	TLxSemaphore iQ;
	TBool iQReady;
	volatile TInt iOwner;
	TInt iLevel;
	TBool iRescheduleOnExit;
	NThread* iInterrupted;
	NThread iScheduler;
	};

extern LinuxInterrupt TheInterrupt;
void FastCounterInit();

#endif
