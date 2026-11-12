// Hosted EKA2 nanokernel, Linux platform: exec call dispatch.
// Dispatch() is ported from e32/nkern/win32/vectors.cpp (Copyright (c) 1998-2009 Nokia Corporation
// and/or its subsidiary(-ies), Eclipse Public License v1.0). Change: an invalid exec number also leaves
// the kernel (the Win32 version returned with iInKernel still raised).
// Entry: the phone's EUSER stubs are `svc #n; bx lr` (n = exec number, bit 23 = fast exec). A seccomp
// filter keyed on the instruction pointer turns every svc in the ROM stub range into SIGSYS; the handler
// runs Dispatch on the thread's own stack with r0-r3 from the signal context as the argument block,
// and returns r0. The intended fast path patches the stubs into direct calls (architecture.md); this
// trap stays as the safety net.

#include "nk_priv.h"

typedef TInt (*TExecHandler)(TInt, TInt, TInt, TInt);
typedef void (*TPreprocessHandler)(TInt*, TUint32);

inline TInt Invoke(TLinAddr aHandler, const TInt* aArgs)
	{
	return (TExecHandler(aHandler))(aArgs[0], aArgs[1], aArgs[2], aArgs[3]);
	}

volatile TUint32 HostedExecTraps;	// execs that arrived through SIGSYS
TInt HostedTraceExecs;				// > 0: print that many trapped execs (number, arguments, result)
void (*HostedExecTraceWho)(char* aBuf, TInt aMax);	// the kernel layer: the current thread's full name
const char* HostedTraceExecsMatch;	// trace only threads whose name contains this (htest XTRACE_THREAD)

static TBool Contains(const char* aH, const char* aN)
	{
	TInt l = strlen(aN);
	for (; *aH; ++aH)
		if (!memcmp(aH, aN, l))
			return ETrue;
	return EFalse;
	}

/** Executive dispatch: aArgs is an array of up to 4 arguments; returns the exec's result */
EXPORT_C TInt Dispatch(TInt aFunction, TInt* aArgs)
	{
	NThread& me = *static_cast<NThread*>(TheScheduler.iCurrentThread);
	__NK_ASSERT_ALWAYS(!me.iDiverted);
	EnterKernel();
	if (aFunction & 0x800000)
		{
		aFunction &= 0x7fffff;
		// fast exec
		const SFastExecTable* table = me.iFastExecTable;
		if (aFunction == 0)
			{
			// special case fast exec call
			NKern::WaitForAnyRequest();
			LeaveKernel();
			return 0;
			}
		if (table && TUint(aFunction) < TUint(table->iFastExecCount))
			{
			NKern::Lock();
			TInt r = Invoke(table->iFunction[aFunction-1], aArgs);
			NKern::Unlock();
			LeaveKernel();
			return r;
			}
		// invalid exec number passed, so ensure we invoke the invalid exec handler
		aFunction = -1;
		}

	// slow exec
	const SSlowExecTable* table = (const SSlowExecTable*)((const TUint8*)me.iSlowExecTable - _FOFF(SSlowExecTable,iEntries));
	if (TUint(aFunction) >= TUint(table->iSlowExecCount))
		{
		TInt r = Invoke(table->iInvalidExecHandler, aArgs);
		LeaveKernel();
		return r;
		}
	const SSlowExecEntry& e = table->iEntries[aFunction];
	if (e.iFlags & KExecFlagClaim)
		NKern::LockSystem();
	if (e.iFlags & KExecFlagPreprocess)
		{
		// replace the first argument with the result of preprocessing
		TPreprocessHandler preprocesser = (TPreprocessHandler)table->iPreprocessHandler;
		preprocesser(aArgs, e.iFlags);
		}
	TInt r = Invoke(e.iFunction, aArgs);
	if (e.iFlags & KExecFlagRelease)
		NKern::UnlockSystem();
	LeaveKernel();
	return r;
	}

// one exec call from user code: trace (htest --xtrace) and dispatch; aArgs = r0..r3 as the stub got them
static TInt ExecCall(TUint32 n, TInt* aArgs, TUint32 lr)
	{
	++HostedExecTraps;
	TUint32 a0 = aArgs[0], a1 = aArgs[1], a2 = aArgs[2], a3 = aArgs[3];
	TBool trace = HostedTraceExecs > 0;
	char who[80];
	who[0] = 0;
	if (trace && HostedExecTraceWho)
		{
		HostedExecTraceWho(who, sizeof who);
		if (HostedTraceExecsMatch && !Contains(who, HostedTraceExecsMatch))
			trace = EFalse;
		}
	if (trace)
		{
		--HostedTraceExecs;
		hprintf("EXEC %s %x (%08x %08x %08x %08x) [lr %08x] %s\n", (n & 0x800000) ? "fast" : "slow",
		        n & 0x7fffff, a0, a1, a2, a3, lr, who);
		}
	TInt r = Dispatch((TInt)n, aArgs);
	if (trace)
		hprintf("     -> %08x\n", r);
	return r;
	}

static void ExecTrap(int, hsiginfo* aInfo, hucontext* aUc)
	{
	TUint32 insn = *(const TUint32*)(aInfo->sys.call_addr - 4);	// ARM svc (EUSER stubs are ARM code)
	aUc->mc.r[0] = (TUint32)ExecCall(insn & 0x00ffffff, (TInt*)&aUc->mc.r[0], aUc->mc.lr);
	}

// ---- direct exec calls ----
// EUSER's exec stubs are `svc #n; bx lr` pairs. Each is patched into `ldr pc, [pc, #-4]; .word T`, T a
// trampoline of its own (`movw/movt ip, #n; ldr pc, =HostedExecDirect`): the exec becomes a plain call
// into the kernel's dispatcher on the thread's own stack, with the same arguments (r0-r3) and result (r0)
// as through the SIGSYS trap, without a signal (~34 us per exec under TCG). A stub that does not match is
// left as it is and still traps.
extern "C" TInt HostedExecDirectC(TUint32 aN, TInt* aArgs, TUint32 aLr)
	{
	return ExecCall(aN, aArgs, aLr);
	}

// The stack is realigned to 8 bytes: EUSER's own callers reach a stub with sp only 4-byte aligned (legal
// inside a module), and clang code relies on AAPCS alignment. Bug 2026-11-12: with sp = 4 mod 8 the modem
// core computed `d + 6` as `d | 6` (= d + 2): the IMEI reply came out shifted, the TSY never got an IMEI,
// Etel answered -1 and the DRM server retried for ever (the trap path was aligned by Linux's signal frame).
// Register contract = the trap's: only r0 changes; r1-r3 come back as the argument array holds them after
// the exec (a preprocess handler may rewrite it), r12, lr and the flags as they were. Bug 2026-11-12: the
// first version let the C code clobber r1-r3/r12/flags and start-up stalled after the splash screen.
// Trampoline: push {r0-r3, ip, lr}; movw/movt r0, #n; ldr pc, =HostedExecDirect
extern "C" void HostedExecDirect();
__asm__(
	".text\n .arm\n .balign 4\n .global HostedExecDirect\n HostedExecDirect:\n"
	"	mrs	r1, apsr\n"
	"	push	{r1, r4}\n"				// flags, r4
	"	mov	r4, sp\n"
	"	bic	sp, sp, #7\n"				// AAPCS: 8-byte aligned for the kernel (see below)
	"	add	r1, r4, #8\n"				// the saved r0-r3: the arguments array
	"	ldr	r2, [r4, #28]\n"			// the caller's lr (trace)
	"	bl	HostedExecDirectC\n"
	"	mov	sp, r4\n"
	"	pop	{r1, r4}\n"
	"	msr	apsr_nzcvqg, r1\n"
	"	add	sp, sp, #4\n"				// r0 = the result: drop the saved one
	"	pop	{r1-r3, ip, lr}\n"
	"	bx	lr\n"
	// the same for a lone `svc #n` (not followed by `bx lr`): its trampoline saved the resume address (the
	// instruction after the svc) above lr; continue there
	" .global HostedExecDirectB\n HostedExecDirectB:\n"
	"	mrs	r1, apsr\n"
	"	push	{r1, r4}\n"
	"	mov	r4, sp\n"
	"	bic	sp, sp, #7\n"
	"	add	r1, r4, #8\n"
	"	ldr	r2, [r4, #28]\n"
	"	bl	HostedExecDirectC\n"
	"	mov	sp, r4\n"
	"	pop	{r1, r4}\n"
	"	msr	apsr_nzcvqg, r1\n"
	"	add	sp, sp, #4\n"
	"	pop	{r1-r3, ip, lr}\n"
	"	ldr	pc, [sp], #4\n");
extern "C" void HostedExecDirectB();

volatile TUint32 HostedExecStubsPatched;

// trampolines within branch range (+-32 MB) of the ROM's stub block: a page below the ROM (0x80000000), in
// the top of the hosted DLL data reservation (memmodel/linux/mcodeseg.cpp leaves it out)
static const TLinAddr KTrampolineBase = 0x7ff00000u;

static TInt PatchStubs(TLinAddr aLo, TLinAddr aHi)
	{
	TUint32 count = (aHi - aLo) / 4;
	TUint32* tramp = (TUint32*)hmmap((TAny*)KTrampolineBase, (count * 36 + 0xfff) & ~0xfffu,
	                                 PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if ((TLinAddr)tramp != KTrampolineBase)
		return -1;
	TLinAddr pg = aLo & ~0xfffu, pgEnd = (aHi + 0xfff) & ~0xfffu;
	if (hsys(SYS_mprotect, pg, pgEnd - pg, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
		return -1;
	TUint32* t = tramp;
	for (TUint32* st = (TUint32*)aLo; (TLinAddr)st < aHi; ++st)
		{
		if ((st[0] & 0xff000000u) != 0xef000000u)
			continue;		// not an unconditional svc (conditional ones keep trapping)
		TUint32 n = st[0] & 0x00ffffff;
		TUint32 movw = 0xe3000000u | ((n & 0xf000) << 4) | (n & 0xfff);					// movw r0, #lo16
		TUint32 movt = 0xe3400000u | (((n >> 16) & 0xf000) << 4) | ((n >> 16) & 0xfff);	// movt r0, #hi16
		if ((TLinAddr)(st + 1) < aHi && st[1] == 0xe12fff1eu)
			{
			// `svc #n; bx lr` -> `ldr pc, [pc, #-4]; .word T`; T: push; n; -> HostedExecDirect (returns to lr)
			t[0] = 0xe92d500fu;			// push {r0-r3, ip, lr}
			t[1] = movw;
			t[2] = movt;
			t[3] = 0xe51ff004u;			// ldr pc, [pc, #-4]
			t[4] = (TUint32)&HostedExecDirect;
			st[0] = 0xe51ff004u;
			st[1] = (TUint32)t;
			t += 5;
			++st;
			}
		else
			{
			// a lone `svc #n` -> `b T`; T saves the resume address (svc + 4) above lr -> HostedExecDirectB
			TInt off = ((TInt)t - ((TInt)st + 8)) >> 2;
			if (off < -0x800000 || off >= 0x800000)
				continue;
			t[0] = 0xe24dd004u;			// sub sp, sp, #4
			t[1] = 0xe92d500fu;			// push {r0-r3, ip, lr}
			t[2] = 0xe59fc010u;			// ldr ip, [pc, #16]   (t[8]: the resume address)
			t[3] = 0xe58dc018u;			// str ip, [sp, #24]
			t[4] = movw;
			t[5] = movt;
			t[6] = 0xe51ff004u;			// ldr pc, [pc, #-4]
			t[7] = (TUint32)&HostedExecDirectB;
			t[8] = (TUint32)(st + 1);	// resume
			st[0] = 0xea000000u | ((TUint32)off & 0x00ffffffu);	// b T
			t += 9;
			}
		++HostedExecStubsPatched;
		}
	hsys(SYS_ARM_cacheflush, (TUint32)tramp, (TUint32)t, 0);
	hsys(SYS_ARM_cacheflush, aLo, aHi, 0);
	hsys(SYS_mprotect, pg, pgEnd - pg, PROT_READ | PROT_EXEC);
	return 0;
	}

/** Route every svc executed in [aLo, aHi) (the ROM's exec stubs) to Dispatch. Call before the threads
	that will run that code are created: the filter is inherited by new threads. */
TInt HostedExecTrapInit(TLinAddr aLo, TLinAddr aHi)
	{
	TInt r = hsigaction(SIGSYS, ExecTrap, SA_NODEFER);
	if (r == 0)
		r = hseccomp_trap_range(aLo, aHi);
	if (r == 0 && !hgetenv_hex("EXEC_TRAP_ONLY", 0))	// htest --env EXEC_TRAP_ONLY=1: the trap path only
		r = PatchStubs(aLo, aHi);
	return r;
	}
