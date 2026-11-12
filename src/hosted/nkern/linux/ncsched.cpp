// Hosted EKA2 nanokernel, Linux platform: the scheduler.
// Ported from e32/nkern/win32/ncsched.cpp (Copyright (c) 1998-2009 Nokia Corporation and/or its
// subsidiary(-ies), Eclipse Public License v1.0). Changes: Win32 events -> TLxEvent futex events,
// SuspendThread/ResumeThread -> LxSuspend/LxResume (a signal parks the thread), TLS -> SLxThread via
// TPIDRURW, no process affinity, no Win32 non-preemptible function scan (see IsSafeToPreempt).

#define __INCLUDE_NTHREADBASE_DEFINES__
#include <e32cmn.h>
#include <e32cmn_private.h>
#include "nk_priv.h"

typedef void (*ProcessHandler)(TAny* aAddressSpace);

static NThreadBase* SelectThread(TScheduler& aS)
//
// Select the next thread to run (identical to the Win32 and ARM algorithm).
//
	{
	NThreadBase* t = static_cast<NThreadBase*>(aS.First());
	__NK_ASSERT_DEBUG(t);
	if (t->iTime == 0 && !t->Alone())
		{
		// round robin
		if (t->iHeldFastMutex)
			{
			// round-robin deferred due to fast mutex held
			t->iHeldFastMutex->iWaiting = 1;
			return t;
			}
		t->iTime = t->iTimeslice;		// reset old thread time slice
		t = static_cast<NThreadBase*>(t->iNext);					// next thread
		aS.iQueue[t->iPriority] = t;		// make it first in list
		__KTRACE_OPT(KSCHED2,DEBUGPRINT("RoundRobin->%T",t));
		}
	if (t->iHeldFastMutex)
		{
		if (t->iHeldFastMutex == &aS.iLock)
			{
			// thread holds system lock: use it
			return t;
			}
		if ((t->i_ThrdAttr & KThreadAttImplicitSystemLock) != 0 && aS.iLock.iHoldingThread)
			t->iHeldFastMutex->iWaiting = 1;
		__NK_ASSERT_DEBUG((t->i_ThrdAttr & KThreadAttAddressSpace) == 0);
		}
	else if (t->iWaitFastMutex && t->iWaitFastMutex->iHoldingThread)
		{
		__KTRACE_OPT(KSCHED2,DEBUGPRINT("Resched inter->%T, Blocked on %M",t->iWaitFastMutex->iHoldingThread,t->iWaitFastMutex));
		t = t->iWaitFastMutex->iHoldingThread;
		}
	else if (t->i_ThrdAttr & KThreadAttImplicitSystemLock)
		{
		// implicit system lock required
		if (aS.iLock.iHoldingThread)
			{
			// system lock held, switch to that thread
			t = aS.iLock.iHoldingThread;
			__KTRACE_OPT(KSCHED2,DEBUGPRINT("Resched inter->%T (IMP SYS)",t));
			t->iHeldFastMutex->iWaiting = 1;	// aS.iLock.iWaiting = 1;
			return t;
			}
		__NK_ASSERT_DEBUG((t->i_ThrdAttr & KThreadAttAddressSpace) == 0);
		}
	return t;
	}

// from NThread
#undef i_ThrdAttr

TBool NThread::WakeUp()
//
// Wake up the thread. What to do depends on whether we were preempted or voluntarily
// rescheduled.
//
// Return TRUE if we need to immediately reschedule again because we had to unlock
// the kernel but there are DFCs pending. In this case, the thread does not wake up.
//
// NB. kernel is locked
//
	{
	switch (iWakeup)
		{
	default:
		FAULT();
	case EIdle:
		__NK_ASSERT_ALWAYS(TheScheduler.iCurrentThread == this);
		iScheduleLock.Set();
		break;
	case ERelease:
		TheScheduler.iCurrentThread = this;
		iScheduleLock.Set();
		break;
	case EResumeLocked:
		// The thread is host-suspended (new) and must be resumed; it does not need the kernel unlocked
		__KTRACE_OPT(KSCHED,DEBUGPRINT("LxResume->%T",this));
		iWakeup = ERelease;
		TheScheduler.iCurrentThread = this;
		if (TheScheduler.iProcessHandler)
			(*ProcessHandler(TheScheduler.iProcessHandler))(iAddressSpace);
		__NK_ASSERT_ALWAYS(LxResume(iLx) > 0);	// check thread was previously suspended
		break;
	case EResumeDiverted:
		// The thread is host-suspended (preempted) and must be resumed into a diversion
		__KTRACE_OPT(KSCHED,DEBUGPRINT("LxResume->%T (Resuming diverted thread)",this));
		iWakeup = ERelease;
		ApplyDiversion();
		TheScheduler.iCurrentThread = this;
		// >= 1, not == 1 as in Win32: once this thread is current, an interrupt may suspend it again
		// before this resume (WakeUp does not hold the interrupt lock here). Bug 2026-11-12, found by
		// nk_exitrace (kills under an interrupt storm, one CPU).
		__NK_ASSERT_ALWAYS(LxResume(iLx) >= 1);
		break;
	case EResume:
		// The thread is host-suspended (preempted) and must be resumed: unlock the kernel on its behalf,
		// unless DFCs or a reschedule became pending meanwhile (then go round again in this thread)
		{
		__KTRACE_OPT(KSCHED,DEBUGPRINT("LxResume->%T",this));
		TInt irq = NKern::DisableAllInterrupts();
		if (TheScheduler.iDfcPendingFlag || TheScheduler.iRescheduleNeededFlag)
			{
			// we were interrrupted... back to the top
			TheScheduler.iRescheduleNeededFlag = TRUE;	// ensure we do the reschedule
			return TRUE;
			}
		iWakeup = ERelease;
		TheScheduler.iCurrentThread = this;
		if (TheScheduler.iProcessHandler)
			(*ProcessHandler(TheScheduler.iProcessHandler))(iAddressSpace);
		if (iInKernel == 0 && iUserModeCallbacks != NULL)
			ApplyDiversion();
		else
			TheScheduler.iKernCSLocked = 0;		// have to unlock the kernel on behalf of the new thread
		TheScheduler.iCurrentThread = this;
		NKern::RestoreInterrupts(irq);
		__NK_ASSERT_ALWAYS(LxResume(iLx) > 0);	// check thread was previously suspended
		}
		break;
		}
	return FALSE;
	}

extern void LxThreadExit();

static void ThreadExit(NThread& aCurrent, NThread& aNext)
//
// The final context switch of a thread: wake the next one and end this host thread
//
	{
	if (aNext.WakeUp())
		return;			// need to re-reschedule in this thread
	LxThreadExit();		// does not return
	}

volatile TUint32 HostedSwitches;		// host thread hand-overs (speed measurements)

static void SwitchThreads(NThread& aCurrent, NThread& aNext)
//
// The fundamental context switch: wake the next thread, park the current one
//
	{
	++HostedSwitches;
	if (aCurrent.iNState == NThread::EDead)
		ThreadExit(aCurrent, aNext);
	else
		{
		if (aNext.WakeUp())
			return;			// need to re-reschedule in this thread
		aCurrent.iScheduleLock.Wait();
		}
	}

void TScheduler::YieldTo(NThreadBase*)
	{
	RescheduleNeeded();
	TScheduler::Reschedule();
	}

// Idle: no thread is ready (every one, the initial thread included, is blocked). The ARM scheduler
// waits for an interrupt in that case; the Win32 one asserts because its null thread never blocks.
// Here the blocked current thread waits until an interrupt bumps IdleSeq, running pending IDFCs.
// Bug 2026-11-12: nkernsa's initial thread calls NKern::Sleep (init_fast_counter) -> SelectThread
// assert. Regression: nk_idle.
static volatile TInt IdleSeq;

void HostedPokeIdle()
	{
	__atomic_add_fetch(&IdleSeq, 1, __ATOMIC_SEQ_CST);
	hfutex_wake(&IdleSeq, 1);
	}

static void IdleUntilReady()
	{
	while (!TheScheduler.First())
		{
		TInt seq = __atomic_load_n(&IdleSeq, __ATOMIC_SEQ_CST);
		NKern::DisableAllInterrupts();
		if (TheScheduler.iDfcPendingFlag)
			TheScheduler.QueueDfcs();
		TBool ready = TheScheduler.First() != 0;
		NKern::EnableAllInterrupts();
		if (!ready)
			hfutex_wait(&IdleSeq, seq);
		}
	}

void TScheduler::Reschedule()
//
// Enter with kernel locked, exit with kernel unlocked, interrupts disabled.
//
	{
	__NK_ASSERT_ALWAYS(TheScheduler.iKernCSLocked == 1);
	NThread& me = *static_cast<NThread*>(TheScheduler.iCurrentThread);
	for (;;)
		{
		NKern::DisableAllInterrupts();
		if (TheScheduler.iDfcPendingFlag)
			TheScheduler.QueueDfcs();
		if (!TheScheduler.iRescheduleNeededFlag)
			break;
		NKern::EnableAllInterrupts();
		TheScheduler.iRescheduleNeededFlag = FALSE;
		IdleUntilReady();
		NThread* t = static_cast<NThread*>(SelectThread(TheScheduler));
		__KTRACE_OPT(KSCHED,DEBUGPRINT("Reschedule->%T (%08x%08x)",t,TheScheduler.iPresent[1],TheScheduler.iPresent[0]));
		SwitchThreads(me, *t);

		// we have just been scheduled to run... check for diversion/new Dfcs
		NThread::TDivert divert = me.iDivert;
		if (divert)
			{
			me.iDivert = NULL;
			divert();						// does not return
			}
		}
	if (TheScheduler.iProcessHandler)
		(*ProcessHandler(TheScheduler.iProcessHandler))(me.iAddressSpace);
	TheScheduler.iKernCSLocked = 0;
	}

/**	Put the emulator into 'idle'.
	This is called by the idle thread when there is nothing else to do.
 */
volatile TUint64 HostedIdleNs;		// time the system spent idle (the null thread parked): speed measurements

EXPORT_C void NThread::Idle()
	{
	NThread& me = *static_cast<NThread*>(TheScheduler.iCurrentThread);
	me.iWakeup = EIdle;
	TUint64 t0 = hnow_ns();
	me.iScheduleLock.Wait();
	HostedIdleNs += hnow_ns() - t0;
	me.iWakeup = ERelease;
	TScheduler::Reschedule();
	NKern::EnableAllInterrupts();
	}

void SchedulerInit(NThread& aInit)
	{
	SchedulerRegister(aInit);
	TheInterrupt.Init();
	}

void SchedulerRegister(NThread& aSelf)
	{
	LxSelf()->iNThread = &aSelf;
	aSelf.iLx = LxSelf();
	}

NThread* SchedulerThread()
	{
	SLxThread* x = LxSelf();
	return x ? x->iNThread : NULL;
	}

inline TBool IsScheduledThread()
	{
	return SchedulerThread() == TheScheduler.iCurrentThread;
	}

NThread& CheckedCurrentThread()
	{
	NThread* t = SchedulerThread();
	__NK_ASSERT_ALWAYS(t == TheScheduler.iCurrentThread);
	return *t;
	}

EXPORT_C TInt NKern::DisableInterrupts(TInt /*aLevel*/)
	{
	return TheInterrupt.Mask();
	}

EXPORT_C TInt NKern::DisableAllInterrupts()
	{
	return TheInterrupt.Mask();
	}

EXPORT_C void NKern::EnableAllInterrupts()
	{
	TheInterrupt.Restore(0);
	}

EXPORT_C void NKern::RestoreInterrupts(TInt aLevel)
	{
	TheInterrupt.Restore(aLevel);
	}

EXPORT_C void NKern::Unlock()
	{
	CHECK_PRECONDITIONS(MASK_NOT_ISR,"NKern::Unlock");
	__NK_ASSERT_ALWAYS(TheScheduler.iKernCSLocked > 0);	// Can't unlock if it isn't locked!
	if (--TheScheduler.iKernCSLocked == 0)
		{
		if (TheScheduler.iRescheduleNeededFlag || TheScheduler.iDfcPendingFlag)
			{
			TheScheduler.iKernCSLocked = 1;
			TScheduler::Reschedule();
			NKern::EnableAllInterrupts();
			}
		}
	}

EXPORT_C void NKern::Lock()
	{
	CHECK_PRECONDITIONS(MASK_NOT_ISR,"NKern::Lock");
	__NK_ASSERT_ALWAYS(IsScheduledThread());	// check that we are a scheduled thread
	++TheScheduler.iKernCSLocked;
	}

EXPORT_C NThread* NKern::LockC()
	{
	CHECK_PRECONDITIONS(MASK_NOT_ISR,"NKern::Lock");
	__NK_ASSERT_ALWAYS(IsScheduledThread());	// check that we are a scheduled thread
	++TheScheduler.iKernCSLocked;
	return (NThread*)TheScheduler.iCurrentThread;
	}

EXPORT_C TInt NKern::PreemptionPoint()
	{
	CHECK_PRECONDITIONS(MASK_NOT_ISR,"NKern::PreemptionPoint");
	if (TheScheduler.iKernCSLocked == 1 &&
		(TheScheduler.iRescheduleNeededFlag || TheScheduler.iDfcPendingFlag))
		{
		TScheduler::Reschedule();
		TheScheduler.iKernCSLocked = 1;
		NKern::EnableAllInterrupts();
		return TRUE;
		}
	return FALSE;
	}

/**	Mark the start of an 'interrupt': called in interrupt (event) threads before any other kernel
	API, paired with EndOfInterrupt(). */
EXPORT_C void StartOfInterrupt()
	{
	__NK_ASSERT_DEBUG(!IsScheduledThread());
	TheInterrupt.Begin();
	}

/**	Mark the end of an 'interrupt': reschedules if needed. */
EXPORT_C void EndOfInterrupt()
	{
	__NK_ASSERT_DEBUG(!IsScheduledThread());
	TheInterrupt.End();
	}

void LinuxInterrupt::Init()
	{
	iQReady = TRUE;
	SNThreadCreateInfo ni;
	memclr(&ni, sizeof(ni));
	ni.iFunction=&Reschedule;
	ni.iTimeslice=-1;
	ni.iPriority=1;
	NKern::ThreadCreate(&iScheduler, ni);
	NKern::Lock();
	TScheduler::YieldTo(&iScheduler);
	Restore(0);
	}

TInt LinuxInterrupt::Mask()
	{
	if (!iQReady)
		return 0;				// interrupt scheme not enabled yet
	TInt id = LxSelf()->iTid;
	if (__e32_atomic_add_ord32(&iLock, 1))
		{
		if (id==iOwner)
			return iLevel++;
		iQ.Wait();
		iRescheduleOnExit=IsScheduledThread() &&
				(TheScheduler.iRescheduleNeededFlag || TheScheduler.iDfcPendingFlag);
		}
	else
		iRescheduleOnExit=FALSE;
	__NK_ASSERT_ALWAYS(iOwner==0 && iLevel==0);
	iOwner=id;
	iLevel=1;
	return 0;
	}

void LinuxInterrupt::Restore(TInt aLevel)
	{
	if (!iQReady)
		return;				// interrupt scheme not enabled yet
	TInt id = LxSelf()->iTid;
	for (;;)
		{
		__NK_ASSERT_ALWAYS(id == iOwner);
		TInt count = iLevel - aLevel;
		if (count <= 0)
			return;						// already restored to that level
		TBool reschedule = FALSE;
		iLevel = aLevel;		// update this value before releasing the lock
		if (aLevel == 0)
			{
			// we release the lock
			iOwner = 0;
			if (iRescheduleOnExit && TheScheduler.iKernCSLocked == 0)
				reschedule = TRUE;		// need to trigger reschedule on full release
			}
		// now release the lock
		if (__e32_atomic_add_ord32(&iLock, TUint32(-count)) == (TUint32)count)
			{	// fully released, check for reschedule
			if (!reschedule)
				return;
			}
		else
			{	// not fully released
			if (aLevel == 0)
				iQ.Signal();
			return;
			}
		// unlocked everything but a reschedule may be required
		TheScheduler.iKernCSLocked = 1;
		TScheduler::Reschedule();
		// return with the kernel unlocked, but interrupts disabled
		// instead of going recursive with a call to EnableAllInterrupts() we iterate
		aLevel=0;
		}
	}

void LinuxInterrupt::Begin()
	{
	Mask();
	__NK_ASSERT_ALWAYS(iInterrupted==0);	// check we haven't done this already
	__NK_ASSERT_ALWAYS(!IsScheduledThread());	// check that we aren't a scheduled thread
	NThread* pC;
	for (;;)
		{
		pC=static_cast<NThread*>(TheScheduler.iCurrentThread);
		TInt r = LxSuspend(pC->iLx);
		if (pC == TheScheduler.iCurrentThread)
			{
			__NK_ASSERT_ALWAYS(r >= 0);	// the current thread cannot be gone
			break;
			}
		// another thread got in first (or pC was exiting): let it go, suspend the new current one
		if (r >= 0)
			__NK_ASSERT_ALWAYS(LxResume(pC->iLx) > 0);
		}
	iInterrupted = pC;
	}

void LinuxInterrupt::End()
	{
	__NK_ASSERT_ALWAYS(iOwner == LxSelf()->iTid);	// check we are the interrupting thread
	NThread* pC = iInterrupted;
	__NK_ASSERT_ALWAYS(pC==TheScheduler.iCurrentThread);
	iInterrupted = 0;
	if (iLock == 1 && TheScheduler.iKernCSLocked == 0 &&
		(TheScheduler.iRescheduleNeededFlag || TheScheduler.iDfcPendingFlag) &&
		pC->IsSafeToPreempt())
		{
		TheScheduler.iKernCSLocked = 1;		// prevent further pre-emption
		if (pC->iWakeup == NThread::EIdle)
			{
			// wake up the NULL thread, it will always reschedule immediately
			pC->WakeUp();
			}
		else
			{
			// pre-empt the current thread and poke the 'scheduler' thread
			__NK_ASSERT_ALWAYS(pC->iWakeup == NThread::ERelease);
			pC->iWakeup = NThread::EResume;
			RescheduleNeeded();
			// Make the scheduler thread current before releasing the interrupt lock (Win32 released it
			// first). Bug 2026-11-12: in that gap a second interrupt thread took the lock, adopted the
			// preempted thread as its interrupted one, and at its End found iCurrentThread changed to the
			// scheduler thread (assert pC==current). Regression: nk_exitrace (timer + interrupt storm).
			iScheduler.WakeUp();
			NKern::EnableAllInterrupts();
			return;
			}
		}
	// no thread reschedle, so emit trace...
	if (((NThread*)pC)->iInKernel == 0 &&		// thread is running in user mode
		pC->iUserModeCallbacks != NULL && 		// and has callbacks queued
		TheScheduler.iKernCSLocked == 0 &&		// and is not currently processing a diversion
		pC->IsSafeToPreempt())					// and can be safely prempted at this point
		{
		TheScheduler.iKernCSLocked = 1;
		pC->ApplyDiversion();
		}
	NKern::EnableAllInterrupts();
	__NK_ASSERT_ALWAYS(LxResume(pC->iLx) > 0);	// check thread was previously suspended
	HostedPokeIdle();		// a thread idling in Reschedule rechecks the ready list and the DFCs
	}

void LinuxInterrupt::Dump()
	{
	hprintf("  interrupt lock: count %d owner tid %d level %d sem %d interrupted %p resched-on-exit %d; scheduler thread %p wakeup %d\n",
	        iLock, iOwner, iLevel, iQ.iCount, iInterrupted, iRescheduleOnExit, &iScheduler, iScheduler.iWakeup);
	}

void LinuxInterrupt::Reschedule(TAny*)
//
// The entry-point for the interrupt-rescheduler thread.
// This spends its whole life going around the TScheduler::Reschedule() loop
// selecting another thread to run.
//
	{
	TheScheduler.iKernCSLocked = 1;
	RescheduleNeeded();
	TScheduler::Reschedule();
	FAULT();
	}

void LinuxInterrupt::ForceReschedule()
	{
	RescheduleNeeded();
	iScheduler.WakeUp();
	}

void SchedulerEscape()
	{
	NThread& me=CheckedCurrentThread();
	EnterKernel();
	__NK_ASSERT_ALWAYS(TheScheduler.iKernCSLocked==0);	// Can't call Escape() with the Emulator/kernel already locked
	NKern::ThreadEnterCS();
	NKern::Lock();
	me.iNState=NThreadBase::EBlocked;
	TheScheduler.Remove(&me);
	me.iWakeup=NThread::EEscaped;
	TheInterrupt.ForceReschedule();	// schedules some other thread so we can carry on outside the scheduler domain
	// this will change the value of iCurrentThread to ensure the 'escaped' invariants are set
	}

void ReenterDfc(TAny* aPtr)
	{
	NThread& me = *static_cast<NThread*>(aPtr);
	me.iWakeup = NThread::ERelease;
	me.CheckSuspendThenReady();
	}

void SchedulerReenter()
	{
	NThread* me=SchedulerThread();
	__NK_ASSERT_ALWAYS(me);
	__NK_ASSERT_ALWAYS(me->iWakeup == NThread::EEscaped);
	TDfc idfc(&ReenterDfc, me);
	StartOfInterrupt();
	idfc.Add();
	EndOfInterrupt();
	me->iScheduleLock.Wait();
	TScheduler::Reschedule();
	NKern::EnableAllInterrupts();
	NKern::ThreadLeaveCS();
	LeaveKernel();
	}

/**	Return the current processor context type (thread, IDFC, interrupt or escaped thread) */
EXPORT_C TInt NKern::CurrentContext()
	{
	NThread* t = SchedulerThread();
	if (!t)
		return NKern::EInterrupt;
	if (TheScheduler.iInIDFC)
		return NKern::EIDFC;
	if (t->iWakeup == NThread::EEscaped)
		return NKern::EEscaped;
	__NK_ASSERT_ALWAYS(NKern::Crashed() || t == TheScheduler.iCurrentThread);
	return NKern::EThread;
	}

TBool NThread::IsSafeToPreempt()
	{
	// The Win32 version refuses while the thread is inside Win32 exception dispatch. A hosted thread
	// parked by our suspend signal can be resumed anywhere; the one unsafe place would be our own
	// signal handlers, which never run with the thread scheduled-out (they are leaf code).
	return TRUE;
	}

void LeaveKernel()
	{
	TInt& k=CheckedCurrentThread().iInKernel;
	__NK_ASSERT_DEBUG(k>0);
	if (k==1)  // just about to leave kernel
		{
		NThread& t = CheckedCurrentThread();
		__NK_ASSERT_ALWAYS(t.iCsCount==0);
		__NK_ASSERT_ALWAYS(t.iHeldFastMutex==0);
		__NK_ASSERT_ALWAYS(TheScheduler.iKernCSLocked==0);
		NKern::DisableAllInterrupts();
		t.CallUserModeCallbacks();
		NKern::EnableAllInterrupts();
		}
	--k;
	}
