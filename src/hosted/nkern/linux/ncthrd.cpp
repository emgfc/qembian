// Hosted EKA2 nanokernel, Linux platform: nanothread creation, diversion, exit.
// Ported from e32/nkern/win32/ncthrd.cpp (Copyright (c) 1998-2009 Nokia Corporation and/or its
// subsidiary(-ies), Eclipse Public License v1.0). Changes: Win32 threads -> LxCreateThread, the
// diversion edits the parked thread's signal context instead of GetThreadContext/SetThreadContext,
// DivertHook in ARM assembler, no SEH (exceptions arrive as signals: not handled yet).

#define __INCLUDE_NTHREADBASE_DEFINES__
#include "nk_priv.h"

const TInt KMaxParameterBlock = 512;

struct SCreateThread
	{
	const SNThreadCreateInfo* iInfo;
	NFastMutex iHandoff;
	};

TInt NThread::StartThread(TAny* aParam)
//
// The host thread entry point for nanothreads: runs once scheduled for the first time (resumed
// from its creation suspend with the kernel locked, holding the hand-off fast mutex).
//
	{
	SCreateThread* init = static_cast<SCreateThread*>(aParam);
	NThread& me=*static_cast<NThread*>(init->iHandoff.iHoldingThread);
	SchedulerRegister(me);
	NKern::Unlock();
	const SNThreadCreateInfo& info = *init->iInfo;
	TUint8 parameterBlock[KMaxParameterBlock];
	TAny* parameter=(TAny*)info.iParameterBlock;
	if (info.iParameterBlockSize)
		{
		__NK_ASSERT_DEBUG(TUint(info.iParameterBlockSize)<=TUint(KMaxParameterBlock));
		parameter=parameterBlock;
		memcpy(parameterBlock,info.iParameterBlock,info.iParameterBlockSize);
		}
	NThreadFunction threadFunction=info.iFunction;
	me.iUserStackBase = (((TLinAddr)&parameterBlock)+0xfff)&~0xfff; // base address of stack
	NKern::FMSignal(&init->iHandoff);
	threadFunction(parameter);
	NKern::Exit();
	return 0;
	}

TInt NThread::Create(SNThreadCreateInfo& aInfo, TBool aInitial)
	{
	iLx = NULL;
	iInKernel = 1;
	iDivert = NULL;
	iDiverted = EFalse;
	iDivertReturn = NULL;
	iWakeup = aInitial ? ERelease : EResumeLocked;	// mark new threads as created (=> host suspend)
	new (&iScheduleLock) TLxEvent;
	TInt r=NThreadBase::Create(aInfo,aInitial);
	if (r!=KErrNone)
		return r;
	if (aInitial)
		{
		LxInit();
		iLx = LxSelf();
		FastCounterInit();
		iUserStackBase = (((TLinAddr)&r)+0xfff)&~0xfff; // base address of stack
		SchedulerInit(*this);
		return KErrNone;
		}
	SCreateThread start;
	start.iInfo = &aInfo;
	iLx = LxCreateThread(&StartThread, &start, FALSE, this);
	if (iLx == NULL)
		return KErrNoMemory;
	NKern::Lock();
	ForceResume();	// mark the thread as ready
	// hold the hand-off mutex for the new thread, it releases it once it has copied what it needs
	start.iHandoff.iHoldingThread = this;
	iHeldFastMutex = &start.iHandoff;
	Suspend(1);		// will defer as holding a fast mutex (implicit critical section)
	start.iHandoff.Wait();
	start.iHandoff.Signal();
	NKern::Unlock();
	return KErrNone;
	}

void NThread::Diverted()
//
// Called (through DivertHook) in the context of a diverted thread, kernel locked
//
	{
	NThread& me = *static_cast<NThread*>(TheScheduler.iCurrentThread);
	__NK_ASSERT_ALWAYS(TheScheduler.iKernCSLocked == 1);
	__NK_ASSERT_ALWAYS(me.iDiverted);
	NThread::TDivert divert = me.iDivert;
	me.iDiverted = EFalse;
	me.iDivert = NULL;
	me.iDivertReturn = NULL;
	EnterKernel(FALSE);
	if (divert)
		divert();	// does not return
	NKern::Unlock();
	LeaveKernel();
	}

extern "C" void NThread__Diverted()
	{
	NThread::Diverted();
	}

extern "C" TAny* NThread__DivertReturn()
	{
	return static_cast<NThread*>(TheScheduler.iCurrentThread)->iDivertReturn;
	}

// DivertHook: entered instead of the interrupted instruction; saves every register and the flags,
// runs the diversion, restores, continues at iDivertReturn (bit 0 set = Thumb).
// Frame: [flags, pad][r0-r12, lr][return address, pad] = 72 bytes.
extern "C" void DivertHook();
__asm__(
	".text\n .arm\n .balign 4\n .global DivertHook\n DivertHook:\n"
	"	sub	sp, sp, #8\n"
	"	push	{r0-r12, lr}\n"
	"	mrs	r0, apsr\n"
	"	push	{r0, r1}\n"
	"	mov	r4, sp\n"
	"	bic	sp, sp, #7\n"
	"	bl	NThread__DivertReturn\n"
	"	mov	r5, r0\n"
	"	bl	NThread__Diverted\n"
	"	mov	sp, r4\n"
	"	pop	{r0, r1}\n"
	"	msr	apsr_nzcvqg, r0\n"
	"	str	r5, [sp, #56]\n"
	"	pop	{r0-r12, lr}\n"
	"	ldr	pc, [sp], #8\n");

volatile TUint32 HostedDiversions;		// count of context diversions (tests check the path ran)

void NThread::ApplyDiversion()
	{
	// Called with interrupts disabled and kernel locked, the thread host-suspended (parked in the
	// suspend handler with its context saved)
	__NK_ASSERT_ALWAYS(TheScheduler.iKernCSLocked == 1);
	if (iDiverted)
		return;
	hucontext* c = iLx->iCtx;
	__NK_ASSERT_ALWAYS(c != NULL);
	__NK_ASSERT_ALWAYS(iDivertReturn == NULL);
	TUint32 pc = c->mc.pc;
	if (c->mc.cpsr & 0x20)
		{
		pc |= 1;				// Thumb: return in Thumb state
		c->mc.cpsr &= ~0x0600fc20u;	// T and the IT state bits off: the hook is ARM code
		}
	iDivertReturn = (TAny*)pc;
	c->mc.pc = (TUint32)&DivertHook;
	iDiverted = ETrue;
	++HostedDiversions;
	}

void NThread::Divert(TDivert aDivert)
	{
	iDivert = aDivert;
	if (iWakeup == EResume)
		iWakeup = EResumeDiverted;
	else
		__NK_ASSERT_ALWAYS(iWakeup == ERelease);
	}

void NThread::ExitSync()
//
// Diversion used to terminate 'stillborn' threads.
// On entry, kernel is locked, interrupts are restored
//
	{
	NThreadBase& me=*TheScheduler.iCurrentThread;
	me.iHeldFastMutex->Signal();	// release the interlock
	me.iNState=EDead;				// mark ourselves as dead which will take thread out of scheduler
	TheScheduler.Remove(&me);
	RescheduleNeeded();
	TScheduler::Reschedule();	// this won't return
	FAULT();
	}

void NThread::Stillborn()
//
// Called if the new thread creation was aborted - so it will not be killed in the usual manner
//
	{
	if (!iLx)
		return;
	NKern::Lock();
	Divert(&ExitSync);
	ForceResume();
	// create and assign mutex to stillborn thread
	NFastMutex interlock;
	interlock.iHoldingThread=this;
	iHeldFastMutex=&interlock;
	interlock.Wait();			// interlock on thread exit handler
	interlock.Signal();
	NKern::Unlock();
	}

void NThread::ExitAsync()
//
// Diversion used to terminate 'killed' threads.
// On entry, kernel is locked, interrupts are restored
//
	{
	NThreadBase& me = *TheScheduler.iCurrentThread;
	me.iCsCount = 0;
	__NK_ASSERT_ALWAYS(static_cast<NThread&>(me).iInKernel>0);
	me.Exit();
	}

void NThreadBase::OnKill()
	{
	}

void NThreadBase::OnExit()
	{
	}

inline void NThread::DoForceExit()
	{
	__NK_ASSERT_DEBUG(TheScheduler.iKernCSLocked);
	Divert(&ExitAsync);
	}

void NThreadBase::ForceExit()
//
// Called to force the thread to exit when it resumes
//
	{
	static_cast<NThread*>(this)->DoForceExit();
	}

// ---- hardware exceptions (SIGSEGV, SIGBUS, SIGILL, SIGFPE on a nanothread) ----
// As NThread::ExceptionHandler / NThread::Exception / NThread__HandleException of the Win32 emulator:
// the signal handler enters the kernel and locks it, writes the exception record (TWin32ExcInfo: the
// faulting registers) on the thread's own stack below the faulting sp and sends the thread to
// HostedExcTrampoline. Outside the signal handler that unlocks the kernel and runs the thread's exception
// handler (the kernel's Exc::Dispatch: an exception trap - IPC, SafeRead - takes over, or the thread is
// panicked, or the kernel faults). If the handler returns, every register is restored and execution
// continues at the record's pc.

extern "C" void HostedExcTrampoline();
__asm__(
	".text\n .arm\n .balign 4\n .global HostedExcTrampoline\n HostedExcTrampoline:\n"
	"	mov	r4, r0\n"					// r0 = sp = the exception record
	"	bl	NThread__HostedHandleException\n"
	"	mov	sp, r4\n"
	"	ldr	r1, [sp, #80]\n"			// iSp: the interrupted stack
	"	ldr	r2, [sp, #88]\n"			// iPc (bit 0 = Thumb)
	"	str	r2, [r1, #-4]\n"			// into the gap left below it
	"	ldr	r1, [sp, #92]\n"			// iCpsr
	"	msr	apsr_nzcvqg, r1\n"			// no flag-setting instruction after this
	"	add	r0, sp, #28\n"
	"	ldm	r0, {r0-r12}\n"
	"	ldr	lr, [sp, #84]\n"
	"	ldr	sp, [sp, #80]\n"
	"	ldr	pc, [sp, #-4]\n");

extern "C" void NThread__HostedHandleException(TWin32ExcInfo* aExc)
	{
	NKern::Unlock();
	if (TheScheduler.iKernCSLocked)
		{
		KPrintf("HOSTED: exception with the kernel locked: signal %d addr %08x pc %08x", aExc->iExcCode,
		        aExc->iExcDataAddress, aExc->iPc);
		FAULT();
		}
	NThread* me = static_cast<NThread*>(TheScheduler.iCurrentThread);
	__NK_ASSERT_DEBUG(me->iInKernel);
	aExc->iFlags = me->iInKernel == 1 ? 0 : TWin32ExcInfo::EExcInKernel;
	aExc->iHandler = NULL;
	me->iHandlers->iExceptionHandler(aExc, me);	// may not return (exception trap, panic)
	LeaveKernel();
	if (aExc->iHandler)						// a user handler set by the kernel handler
		aExc->iHandler(aExc->iParam[0], aExc->iParam[1]);
	if (aExc->iCpsr & 0x20)
		aExc->iPc |= 1;
	}

volatile TUint32 HostedExceptions;		// hardware exceptions handed to the kernel (tests)

// called by the fault signal handler on the faulting host thread; EFalse: not a nanothread exception
// the kernel can take (the handler dumps and exits)
TBool HostedNThreadException(TInt aSig, TUint32 aAddr, hucontext* aUc)
	{
	SLxThread* lx = LxSelf();
	if (!lx || !lx->iNThread || lx->iNThread != TheScheduler.iCurrentThread)
		return EFalse;
	NThread& me = *lx->iNThread;
	if (!me.iHandlers || !me.iHandlers->iExceptionHandler)
		return EFalse;
	hsigcontext& c = aUc->mc;
	if (me.iDiverted && me.iDivert)
		{
		// being forced to exit: run the diversion (as Win32)
		__NK_ASSERT_ALWAYS(TheScheduler.iKernCSLocked == 1);
		c.pc = (TUint32)&DivertHook;
		c.cpsr &= ~0x0600fc20u;
		return ETrue;
		}
	if (me.iDiverted)
		{
		// prodded to pick up callbacks: LeaveKernel in the handler does that
		__NK_ASSERT_ALWAYS(TheScheduler.iKernCSLocked == 1);
		if (c.pc == (TUint32)&DivertHook)
			c.pc = (TUint32)me.iDivertReturn & ~1u;
		me.iDiverted = EFalse;
		me.iDivertReturn = NULL;
		EnterKernel(FALSE);
		}
	else
		{
		EnterKernel(FALSE);
		TheScheduler.iKernCSLocked = 1;	// no preemption until the trampoline runs
		}
	// the record below the faulting sp, leaving a gap for the resume address
	TUint32 rec = ((c.sp - 16 - sizeof(TWin32ExcInfo)) & ~7u);
	TWin32ExcInfo& x = *(TWin32ExcInfo*)rec;
	memclr(&x, sizeof(x));
	x.iExcCode = aSig;
	x.iExcDataAddress = aAddr;
	for (TInt i = 0; i < 11; ++i)
		x.iR[i] = c.r[i];
	x.iR[11] = c.fp;
	x.iR[12] = c.ip;
	x.iSp = c.sp;
	x.iLr = c.lr;
	x.iPc = c.pc;
	x.iCpsr = c.cpsr;
	++HostedExceptions;
	c.r[0] = rec;
	c.sp = rec;
	c.pc = (TUint32)&HostedExcTrampoline;
	c.cpsr &= ~0x0600fc20u;					// ARM state, no IT block
	return ETrue;
	}
