// Hosted EKA2: the kernel's platform layer on Linux. Replaces kernel/eka/kernel/win32 (cglobals, cboot,
// cinit, cutils, csched, cexec, ckernel, cache) and memmodel/emul/win32/zrom.cpp + memmodel/emul/nvram.cpp
// (Copyright (c) Nokia Corporation and/or its subsidiary(-ies), EPL v1.0), ported function by function:
// Win32 specifics (DLL variant/extension loading, SEH, JIT dialogs, PE images, host files) are replaced
// by hosted equivalents and marked "hosted:". Plus the 9.1/9.2 personality exec shims (personalities/)
// in C instead of ARM .cia.

#define __INCLUDE_NTHREADBASE_DEFINES__
#include "memmodel.h"
#include <kernel/cache.h>
#include <kernel/emi.h>
#include <e32rom.h>
#include <e32hal.h>

// ============================================================ globals (cglobals.cpp)
Asic* Hosted::TheAsic;
TBool DisableWSDWarning = ETrue;		// hosted: no warning per DLL with writable static data

extern "C" void HostedBacktrace();		// nkhost.cpp

// DObject::ObjectId() is declared inline (kernel.h) and defined only in object.cpp, but other files call
// it: emit a copy (used: an inline definition is otherwise not emitted)
__attribute__((used)) TUint64 DObject::ObjectId() const
	{
	return iObjectId >> 18;
	}

// ============================================================ variant and extensions (zrom.cpp)
// hosted: no DLLs to load. The variant registers itself (HostedSetVariant) before the kernel boots;
// extensions are a list of entry points (HostedAddExtension), started in order.
typedef TInt (*THostedEntry)(TInt aReason);
static THostedEntry VariantEntry;
static Asic* (*VariantInitialise)(TBool);
static THostedEntry Extensions[16];
static TInt ExtensionCount;

void HostedSetVariant(THostedEntry aEntry, Asic* (*aInitialise)(TBool))
	{
	VariantEntry = aEntry;
	VariantInitialise = aInitialise;
	}

typedef void (*THostedCtor)();
static THostedCtor* ExtensionCtors[16][2];

// aCtors..aCtorsEnd: the module's static constructors (its kmodinit_<Name> section, htest.py), run just
// before its first entry call, as the kernel's loader runs a kernel DLL's. Bug 2026-11-12: run before
// main, EXSTART's global TDfc took Kern::SvMsgQue() before the kernel existed (NULL queue, crash at
// the first Enque). Regression: kn_efile.
TInt HostedAddExtension(THostedEntry aEntry, THostedCtor* aCtors, THostedCtor* aCtorsEnd)
	{
	if (ExtensionCount == 16)
		return KErrOverflow;
	ExtensionCtors[ExtensionCount][0] = aCtors;
	ExtensionCtors[ExtensionCount][1] = aCtorsEnd;
	Extensions[ExtensionCount++] = aEntry;
	return KErrNone;
	}

TInt HostedAddExtension(THostedEntry aEntry)
	{
	return HostedAddExtension(aEntry, 0, 0);
	}

TInt P::DefaultInitialTime()
	{
	return KErrGeneral;
	}

TInt P::InitSystemTime()
	{
	TUint dummy;
	K::SetSystemTimeAndOffset(0, 0, 0, dummy, ETimeSetOffset|ETimeSetNoTimeUpdate);
	__KTRACE_OPT(KBOOT,Kern::Printf("Use Host time to set system time"));
	TInt seconds;
	A::SystemTimeInSecondsFrom2000(seconds);
	return seconds;
	}

void P::CreateVariant()
	{
	__KTRACE_OPT(KBOOT,Kern::Printf("CreateVariant"));
	if (!VariantEntry || !VariantInitialise)
		Kern::Fault("NoVariant",0);
	TInt r = VariantEntry(KModuleEntryReasonVariantInit0);
	if (r < 0)
		Kern::Fault("VariantEntry",r);
	Hosted::TheAsic = VariantInitialise(EFalse);
	if (!Hosted::TheAsic)
		Kern::Fault("VariantInit",KErrGeneral);
	__KTRACE_OPT(KBOOT,Kern::Printf("Variant installed"));
	BTrace::Init0();	// after the variant has initialised the SuperPage
	}

extern void HostedRomExtensionsInit0();
extern void HostedRomExtensionsInit1();

void P::StartExtensions()
	{
	// the ROM's extension chain (built-in modules: kmodules.cpp), then the ones a test registered
	HostedRomExtensionsInit0();
	HostedRomExtensionsInit1();
	for (TInt i = 0; i < ExtensionCount; ++i)
		{
		for (THostedCtor* c = ExtensionCtors[i][0]; c && c < ExtensionCtors[i][1]; ++c)
			(*c)();
		TInt r = Extensions[i](KModuleEntryReasonExtensionInit0);
		if (r != KErrNone)
			continue;	// already started
		r = Extensions[i](KModuleEntryReasonExtensionInit1);
		if (r != KErrNone)
			K::Fault(K::EStartExtensionsFailed);
		}
	}

// ============================================================ boot (cboot.cpp)
extern "C" void KernelMain();
GLDEF_D TSuperPage SuperPage;
extern TLinAddr SuperPageAddress;		// sglobals.cpp
static TLinAddr RomHeaderAddress;		// the composed core ROM's TRomHeader (mapped by the host process)
static void FindRomRootDirectory();

EXPORT_C const TRomHeader& Epoc::RomHeader()
	{
	return *(const TRomHeader*)RomHeaderAddress;
	}

// hosted: kernel trace bits before boot (TSuperPage::iDebugMask, read by KDebugNum)
void HostedSetDebugMask(TInt aIndex, TUint32 aMask)
	{
	if (aIndex >= 0 && aIndex < 8)
		SuperPage.iDebugMask[aIndex] = aMask;
	}

// hosted: extra kernel config bits (the exec personality: bits 25..28, personalities/registry.toml)
// that the ROM composer writes into a composed ROM's header; the hosting process passes them for a
// phone's stock core image
static TUint32 ExtraKernelConfig;
void HostedSetKernelConfig(TUint32 aSet)
	{
	ExtraKernelConfig |= aSet;
	}

// hosted: called by the hosting process (after LxInit) with the ROM mapped; does not return
void HostedBootEpoc(TLinAddr aRomHeader)
	{
	RomHeaderAddress = aRomHeader;
	SuperPageAddress = (TLinAddr)&SuperPage;
	// what the ARM bootstrap does: kernel config flags from the ROM header (+ the hosting process's),
	// the active hardware variant from the ROM's primary (kernel) image, the ROM root directory for it
	const TRomHeader& rh = Epoc::RomHeader();
	SuperPage.SetKernelConfigFlags(rh.iKernelConfigFlags | ExtraKernelConfig);
	SuperPage.iPrimaryEntry = rh.iPrimaryFile;
	if (rh.iPrimaryFile)
		{
		const TRomEntry* pe = (const TRomEntry*)rh.iPrimaryFile;
		SuperPage.iActiveVariant = ((const TRomImageHeader*)pe->iAddressLin)->iHardwareVariant;
		}
	FindRomRootDirectory();
	KernelMain();
	}

// ============================================================ ASIC glue (cinit.cpp, cutils.cpp)
// The null thread's idle: the power controller's CpuIdle is where an ARM variant executes WFI (wait for an
// interrupt). No power controller here does, so after it the null thread waits for the next interrupt
// itself (NThread::Idle: parked until an interrupt reschedules). Bug 2026-11-12: without the wait the null
// thread spun at 100% CPU for ever after boot (phone window, Linux top: tid "symbian" at 98%).
// Regression: kn_boot (idle during a 2.6 s sleep).
volatile TUint32 HostedNullIdles;
extern "C" void NKIdle(TUint32 aStage)
	{
	SCpuIdleHandler* cih = NKern::CpuIdleHandler();
	if (cih && cih->iHandler)
		(*cih->iHandler)(cih->iPtr, aStage);
	else if (K::PowerModel)
		K::PowerModel->CpuIdle();
	else
		Hosted::TheAsic->Idle();
	++HostedNullIdles;
	NThread::Idle();				// WFI
	}

void A::Init1()
	{
	__KTRACE_OPT(KBOOT,Kern::Printf("A::Init1()"));
	Hosted::TheAsic->Init1();
	TheSuperPage().iStartupReason=EStartupColdReset;
	K::ColdStart=ETrue;
	NTimerQ::Init1(Hosted::TheAsic->MsTickPeriod());
	}

void A::Init2()
	{
	__KTRACE_OPT(KBOOT,Kern::Printf("A::Init2()"));
	}

void A::Init3()
	{
	__KTRACE_OPT(KBOOT,Kern::Printf("A::Init3()"));
	Hosted::TheAsic->Init3();	// hosted: no just-in-time debugger property
	}

TInt A::VariantHal(TInt aFunction, TAny* a1, TAny* a2)
	{
	return Hosted::TheAsic->VariantHal(aFunction,a1,a2);
	}

// hosted: the ARM Asic has DebugOutput(char) where the Win32 one had DebugPrint(TDesC8)
void A::DebugPrint(const TText* aPtr, TInt aLen, TBool)
	{
	if (!Hosted::TheAsic)
		{
		// before the variant exists (KernelMain's first trace): straight to the console, as the ARM
		// bootstrap's debug port does
		hwrite(1, aPtr, aLen);
		if (aLen && aPtr[aLen - 1] != '\n')
			hwrite(1, "\n", 1);
		return;
		}
	for (TInt i = 0; i < aLen; ++i)
		Hosted::TheAsic->DebugOutput(aPtr[i]);
	if (aLen && aPtr[aLen - 1] != '\n')
		Hosted::TheAsic->DebugOutput('\n');
	}

EXPORT_C Asic* Arch::TheAsic()
	{
	return Hosted::TheAsic;
	}

TPtr8 A::MachineConfiguration()
	{
	return Hosted::TheAsic->MachineConfiguration();
	}

TInt A::CallSupervisorFunction(TSupervisorFunction aFunction, TAny* aPtr)
	{
	return aFunction(aPtr);
	}

TInt A::SystemTimeInSecondsFrom2000(TInt& aTime)
	{
	return Hosted::TheAsic->SystemTimeInSecondsFrom2000(aTime);
	}

TInt A::SetSystemTimeInSecondsFrom2000(TInt aTime)
	{
	return Hosted::TheAsic->SetSystemTimeInSecondsFrom2000(aTime);
	}

void K::DoNanoWait(TUint32 anInterval)
	{
	TUint64 end = hnow_ns() + anInterval;
	while (hnow_ns() < end) {}
	}

extern "C" void NKCrashHandler(TInt aPhase, const TAny* a0, TInt a1)
	{
	if (aPhase==0)
		return;
	K::DoFault(a0,a1);
	}

// tests: called on a kernel fault before the process exits (a test can check how far it got)
void (*HostedFaultHook)(const TDesC8& aCategory, TInt aReason);

void A::StartCrashDebugger(const TAny* a0, TInt a1)
	{
	if (HostedFaultHook)
		HostedFaultHook(*(const TDesC8*)a0, a1);
	const TDesC8& msg = *(const TDesC8*)a0;
	char m[80];
	TInt n = Min(msg.Length(), (TInt)sizeof(m) - 1);
	memcpy(m, msg.Ptr(), n);
	m[n] = 0;
	extern volatile TUint32 HostedExecTraps;
	hprintf("KERNEL FAULT: %s %d (0x%x), %u exec calls trapped so far\n", m, a1, a1, HostedExecTraps);
	HostedBacktrace();
	hexit(100);
	}

EXPORT_C TInt Kern::KUSafeInc(TInt& aValue)
	{
	return __e32_atomic_tas_ord32(&aValue, 1, 1, 0);
	}

EXPORT_C TInt Kern::KUSafeDec(TInt& aValue)
	{
	return __e32_atomic_tas_ord32(&aValue, 1, -1, 0);
	}

TInt K::FloatingPointTypes(TUint32& aTypes)
	{
	aTypes = EFpTypeNone;
	return KErrNotSupported;
	}

TInt K::FloatingPointSystemId(TUint32&)
	{
	return KErrNotSupported;
	}

TBool InterruptsStatus(TBool aRequest)
	{
	return TheInterrupt.InterruptsStatus(aRequest);
	}

// ============================================================ caches (cache.cpp): nothing to maintain
// code written to memory then run (loader, AknIconServer's copied rasterizer): ARM hosts need the caches
// synchronised (QEMU TCG does not, a phone does)
EXPORT_C void Cache::IMB_Range(TLinAddr aBase, TUint aSize) { hsys(SYS_ARM_cacheflush, aBase, aBase + aSize, 0); }
EXPORT_C void Cache::SyncMemoryBeforeDmaWrite(TLinAddr, TUint) {}
EXPORT_C void Cache::SyncMemoryBeforeDmaRead(TLinAddr, TUint) {}
EXPORT_C void Cache::SyncMemoryAfterDmaRead(TLinAddr, TUint) {}
EXPORT_C void Cache::AtomicSyncMemory() {}
EXPORT_C void Cache::CpuRetires() {}
EXPORT_C void Cache::KernelRetires() {}
EXPORT_C void Cache::SyncMemoryBeforeDmaWrite(TLinAddr, TUint, TUint32) {}
EXPORT_C void Cache::SyncMemoryBeforeDmaRead(TLinAddr, TUint, TUint32) {}
EXPORT_C void Cache::SyncMemoryAfterDmaRead(TLinAddr, TUint, TUint32) {}
EXPORT_C TInt Cache::GetThresholds(TCacheThresholds&, TUint) { return KErrNotSupported; }
EXPORT_C TInt Cache::SetThresholds(const TCacheThresholds&, TUint) { return KErrNotSupported; }
EXPORT_C TUint Cache::DmaBufferAlignment() { return 1; }

// ============================================================ user memory access (ARM cumem.cia)
// hosted: one address space, kernel and user alike: plain copies. A fault inside one is caught by the
// thread's TExcTrap (XTRAP) through the fault handler, as on the phone.
EXPORT_C void kumemget(TAny* aKernAddr, const TAny* aAddr, TInt aLength) { memcpy(aKernAddr, aAddr, aLength); }
EXPORT_C void kumemget32(TAny* aKernAddr, const TAny* aAddr, TInt aLength) { memcpy(aKernAddr, aAddr, aLength); }
EXPORT_C void kumemput(TAny* aAddr, const TAny* aKernAddr, TInt aLength) { memcpy(aAddr, aKernAddr, aLength); }
EXPORT_C void kumemput32(TAny* aAddr, const TAny* aKernAddr, TInt aLength) { memcpy(aAddr, aKernAddr, aLength); }
EXPORT_C void kumemset(TAny* aAddr, const TUint8 aValue, TInt aLength) { memset(aAddr, aValue, aLength); }
EXPORT_C void umemget(TAny* aKernAddr, const TAny* aUserAddr, TInt aLength) { memcpy(aKernAddr, aUserAddr, aLength); }
EXPORT_C void umemget32(TAny* aKernAddr, const TAny* aUserAddr, TInt aLength) { memcpy(aKernAddr, aUserAddr, aLength); }
EXPORT_C void umemput(TAny* aUserAddr, const TAny* aKernAddr, TInt aLength) { memcpy(aUserAddr, aKernAddr, aLength); }
EXPORT_C void umemput32(TAny* aUserAddr, const TAny* aKernAddr, TInt aLength) { memcpy(aUserAddr, aKernAddr, aLength); }
EXPORT_C void umemset(TAny* aUserAddr, const TUint8 aValue, TInt aLength) { memset(aUserAddr, aValue, aLength); }
EXPORT_C void uumemcpy(TAny* aUserDst, const TAny* aUserSrc, TInt aLength) { memcpy(aUserDst, aUserSrc, aLength); }
EXPORT_C void uumemcpy32(TAny* aUserDst, const TAny* aUserSrc, TInt aLength) { memcpy(aUserDst, aUserSrc, aLength); }
extern "C" EXPORT_C void kumemget_no_paging_assert(TAny* aKernAddr, const TAny* aAddr, TInt aLength) { memcpy(aKernAddr, aAddr, aLength); }
extern "C" EXPORT_C void kumemput_no_paging_assert(TAny* aAddr, const TAny* aKernAddr, TInt aLength) { memcpy(aAddr, aKernAddr, aLength); }

// ============================================================ personality exec shims
// The 9.1/9.2 user-side ABI (personalities/sym9x/patches): fast execs run with the kernel locked, so
// these read-modify-writes are atomic with respect to other threads, as the ARM .cia versions.
TInt Exec92Shim_SafeInc32(TInt* a) { TInt v = *a; if (v > 0) *a = v + 1; return v; }
TInt Exec92Shim_SafeDec32(TInt* a) { TInt v = *a; if (v > 0) *a = v - 1; return v; }
TInt Exec92Shim_LockedInc91(TInt* a) { TInt v = *a; *a = v + 1; return v; }
TInt Exec92Shim_LockedDec91(TInt* a) { TInt v = *a; *a = v - 1; return v; }

// ============================================================ exception traps (ARM ckernel.cia)
// A TExcTrap is a setjmp: Trap() saves r4-r11, sp, lr into iState (offset 0, as on ARM) and returns 0;
// Exception(r) restores them and "returns from Trap()" with r. The register save/restore is assembler,
// the bookkeeping (iHandler, iThread, the thread's iExcTrap/iIpcClient) is C++.
extern "C" {
void DefaultExcTrapHandler(TExcTrap* xt, DThread*, TAny*)
	{
	xt->Exception(KErrBadDescriptor);
	}

void IpcExcHandler(TExcTrap* xt, DThread* aThread, TAny* aContext)
	{
	DThread::IpcExcHandler(xt, aThread, aContext);
	}

TInt HostedTrapFinish(TExcTrap* aTrap, TExcTrapHandler aHandler)
	{
	DThread* t = TheCurrentThread;
	aTrap->iHandler = aHandler;
	aTrap->iThread = t;
	t->iExcTrap = aTrap;
	return 0;
	}

TInt HostedIpcTrapFinish(TIpcExcTrap* aTrap, DThread* aClient)
	{
	DThread* t = TheCurrentThread;
	aTrap->iHandler = &IpcExcHandler;
	aTrap->iThread = t;
	t->iIpcClient = aClient;
	t->iExcTrap = aTrap;
	return 0;
	}

void HostedTrapRestore(TExcTrap* aTrap, TInt aResult);
}

__asm__(
	".text\n .arm\n .balign 4\n"
	".global HostedTrapRestore\n HostedTrapRestore:\n"
	"	ldmia	r0, {r4-r11, sp, lr}\n"
	"	mov	r0, r1\n"
	"	bx	lr\n");

EXPORT_C __NAKED__ TInt TExcTrap::Trap()
	{
	asm("stmia r0, {r4-r11, sp, lr}");
	asm("ldr r1, =DefaultExcTrapHandler");
	asm("b HostedTrapFinish");
	}

EXPORT_C __NAKED__ TInt TExcTrap::Trap(TExcTrapHandler /*aHandler*/)
	{
	asm("stmia r0, {r4-r11, sp, lr}");
	asm("b HostedTrapFinish");
	}

__NAKED__ TInt TIpcExcTrap::Trap(DThread* /*aClient*/)
	{
	asm("stmia r0, {r4-r11, sp, lr}");
	asm("b HostedIpcTrapFinish");
	}

// As the Win32 kernel (kernel/win32/ckernel.cpp): the exception path entered the kernel once more
// (nanokernel exception handler); resuming at the trap leaves that level. Bug 2026-11-12: without the
// decrement a thread that took a trapped kernel-side fault (FbServ, an IPC read of a bad descriptor) kept
// iInKernel one too high for ever: leaving an exec never reached level 0, its request-completion
// callbacks never ran, the next completed request was never seen and start-up deadlocked (kn_efile).
EXPORT_C void TExcTrap::Exception(TInt aResult)
	{
	DThread* t = iThread;
	t->iExcTrap = NULL;
	t->iPagingExcTrap = NULL;
	--static_cast<NThread*>(TheScheduler.iCurrentThread)->iInKernel;
	HostedTrapRestore(this, aResult);
	}

// ---- safe kernel reads/writes (csched.cpp) ----
class TKernExcTrap : public TIpcExcTrap
	{
public:
	TLinAddr iFaultAddr;
	};

static void KernExcHandler(TExcTrap* aExcTrap, DThread*, TAny* aContext)
	{
	TKernExcTrap& xt = *(TKernExcTrap*)aExcTrap;
	TWin32ExcInfo& info = *(TWin32ExcInfo*)aContext;
	TLinAddr va = (TLinAddr)info.iExcDataAddress;
	xt.iFaultAddr = va;
	if (va>=xt.iRemoteBase && va<xt.iRemoteBase+xt.iSize)
		xt.Exception(KErrGeneral);	// problem accessing remote address - 'leave' so an error code will be returned
	}

EXPORT_C TAny* Kern::SafeRead(const TAny* aSrc, TAny* aDest, TInt aSize)
	{
	DThread& t = Kern::CurrentThread();
	TExcTrap* oldxt = t.iExcTrap;
	TKernExcTrap xt;
	xt.iLocalBase = (TLinAddr)aDest;
	xt.iRemoteBase = (TLinAddr)aSrc;
	xt.iSize = aSize;
	xt.iDir = 0;
	xt.iFaultAddr = 0;
	TInt r = xt.Trap(NULL);
	xt.iHandler = &KernExcHandler;
	if (r == 0)
		{
		memcpy(aDest, aSrc, aSize);
		xt.UnTrap();
		}
	t.iExcTrap = oldxt;
	return (TAny*)xt.iFaultAddr;
	}

EXPORT_C TAny* Kern::SafeWrite(TAny* aDest, const TAny* aSrc, TInt aSize)
	{
	DThread& t = Kern::CurrentThread();
	TExcTrap* oldxt = t.iExcTrap;
	TKernExcTrap xt;
	xt.iLocalBase = (TLinAddr)aSrc;
	xt.iRemoteBase = (TLinAddr)aDest;
	xt.iSize = aSize;
	xt.iDir = 1;
	xt.iFaultAddr = 0;
	TInt r = xt.Trap(NULL);
	xt.iHandler = &KernExcHandler;
	if (r == 0)
		{
		memcpy(aDest, aSrc, aSize);
		xt.UnTrap();
		}
	t.iExcTrap = oldxt;
	return (TAny*)xt.iFaultAddr;
	}

EXPORT_C TAny* Kern::KUSafeRead(const TAny* aSrc, TAny* aDest, TInt aSize) { return Kern::SafeRead(aSrc, aDest, aSize); }
TAny* K::USafeRead(const TAny* aSrc, TAny* aDest, TInt aSize) { return Kern::SafeRead(aSrc, aDest, aSize); }
EXPORT_C TAny* Kern::KUSafeWrite(TAny* aDest, const TAny* aSrc, TInt aSize) { return Kern::SafeWrite(aDest, aSrc, aSize); }
TAny* K::USafeWrite(TAny* aDest, const TAny* aSrc, TInt aSize) { return Kern::SafeWrite(aDest, aSrc, aSize); }

// ============================================================ execs (cexec.cpp)
GLREF_C TInt CalcKernelHeapUsed();
void GetLatencyValues(TInt aMode, TInt& aCount, TInt* aDest);
void KernMsgTest();
void InvalidExecHandler();
void InvalidFastExec();
void Win32PreprocessHandler(TInt* aArgs, TUint32 aFlags);

#define __GEN_KERNEL_EXEC_CODE__
#include "execs.h"

TInt ExecHandler::BreakPoint() { return 0; }

// as memmodel/epoc/putils.cpp (EPL, Nokia)
TLinAddr ExecHandler::RomHeaderAddress()
	{
	return ::RomHeaderAddress;
	}

// The ROM root directory of the active hardware variant (FindRomRootDirectory below). Bug 2026-11-12:
// this returned the root directory *list*; F32 reads it as a TRomDir, found nothing (KErrPathNotFound)
// and faulted (Fserv 38). Regression: kn_boot checks the directory.
static TLinAddr RomRootDirAddress;
TLinAddr ExecHandler::RomRootDirectoryAddress()
	{
	return RomRootDirAddress;
	}

TLinAddr HostedRomRootDir()		// for tests
	{
	return RomRootDirAddress;
	}

static void FindRomRootDirectory()
	{
	TUint variant = TheSuperPage().iActiveVariant;
	TUint cpu = (variant >> 16) & 0xff;
	TUint asic = (variant >> 24);
	RomRootDirAddress = 0;
	TRomRootDirectoryList* pL = (TRomRootDirectoryList*)TheSuperPage().iRootDirList;
	if (!pL)
		pL = (TRomRootDirectoryList*)Epoc::RomHeader().iRomRootDirectoryList;
	for (TInt i = 0; i < pL->iNumRootDirs; i++)
		{
		if (THardwareVariant(pL->iRootDir[i].iHardwareVariant).IsCompatibleWith(cpu, asic, variant))
			{
			__KTRACE_OPT(KBOOT,Kern::Printf("Found ROM root dir index %d addr %08x", i, pL->iRootDir[i].iAddressLin));
			RomRootDirAddress = pL->iRootDir[i].iAddressLin;
			return;
			}
		}
	Kern::Fault("FindRomRootDirectory", variant);
	}

void GetLatencyValues(TInt, TInt&, TInt*)
	{
	}

// as kernel/arm/cexec.cpp (EPL, Nokia): a message round trip through a DFC-queue message queue
static void DummyMsg(TAny*);
static TMessageQue DummyMsgQ(DummyMsg,NULL,NULL,1);
GLDEF_C void InitDummyMsgQ()
	{
	DummyMsgQ.SetDfcQ(K::DfcQ0);
	DummyMsgQ.Receive();
	}
static void DummyMsg(TAny*)
	{
	DummyMsgQ.iMessage->Complete(KErrNone,ETrue);
	}
void KernMsgTest()
	{
	TMessageBase& m=Kern::Message();
	m.SendReceive(&DummyMsgQ);
	}
TInt ExecHandler::ProfileStart(TInt) { return 0; }
TInt ExecHandler::ProfileEnd(TInt) { return 0; }

void InvalidExecHandler()
	{
	K::PanicKernExec(EInvalidSystemCall);
	}

void InvalidFastExec()
	{
	NKern::Unlock();		// fast exec calls hold the preemption lock
	InvalidExecHandler();
	}

// handle lookup for slow execs flagged EF_P (the Win32 name is kept: kernel/execs.h WINS branch)
void Win32PreprocessHandler(TInt* aArgs, TUint32 aFlags)
	{
	DThread* t = TheCurrentThread;
	aFlags &= 0x3f;
	if (aFlags >= EIpcMessageD)
		{
		RMessageK* m = RMessageK::MessageK(aArgs[0], t);
		if (m == NULL || (aFlags != EIpcMessageD && m->iFunction == RMessage2::EDisConnect))
			K::PanicCurrentThread(EBadMessageHandle);
		if (aFlags == EIpcClient)
			aArgs[0] = TInt(m->iClient);
		return;
		}
	DObject* obj = Kern::ObjectFromHandle(t, aArgs[0], TObjectType(TInt(aFlags)-1));
	if (obj)
		aArgs[0] = TInt(obj);
	else
		K::PanicCurrentThread(EBadHandle);
	}

EXPORT_C TInt Kern::HalFunction(TInt aGroup, TInt aFunction, TAny* a1, TAny* a2)
	{
	CHECK_PRECONDITIONS(MASK_THREAD_STANDARD,"Kern::HalFunction(TInt, TInt, TAny*, TAny*)");
	return ExecHandler::HalFunction(aGroup, aFunction, a1, a2);
	}

EXPORT_C TInt Kern::HalFunction(TInt aGroup, TInt aFunction, TAny* a1, TAny* a2, TInt aDeviceNumber)
	{
	CHECK_PRECONDITIONS(MASK_THREAD_STANDARD,"Kern::HalFunction(TInt, TInt, TAny*, TAny*, TInt)");
	return ExecHandler::HalFunction(aGroup | (aDeviceNumber<<16), aFunction, a1, a2);
	}

TInt ExecHandler::SetFloatingPointMode(TFloatingPointMode, TFloatingPointRoundingMode)
	{
	return KErrNotSupported;
	}

TInt ExecHandler::SetWin32RuntimeHook(TAny*)
	{
	return KErrNotSupported;	// hosted: no Win32 runtime
	}

// ============================================================ threads and exceptions (ckernel.cpp)
DWin32Thread::~DWin32Thread()
	{
	DThread::Destruct();
	}

void DWin32Thread::DoExit2()
	{
	}

// Enter user code as the ARM kernel does (kernel/arm/ckernel.cia EpocThreadFunction): at the top of the
// thread's stack a zeroed TLocalThreadData area (TPIDRURW points at it; its size and the offset of the
// thread ID are the personality's: sexecpers.cpp), below it a word, below that the creation info block;
// sp -> the block, r4 = entry reason, jump to the entry point (ARM or Thumb). Does not return.
// hosted: the host thread's own stack is the user stack. (Win32 called entry(reason, &info): the ARM
// _E32Startup expects r4/sp and panicked USER 0 on the first run.)
#ifdef __USERSIDE_THREAD_DATA__
extern "C" TInt KernLocalThreadDataSize;		// the personality's (sexecpers.cpp)
extern "C" TInt KernLocalThreadDataIdOffset;
#else
// a CPU without user thread ID registers (the N95's ARM1136 r0 setup): no user-side thread data
static const TInt KernLocalThreadDataSize = 0;
static const TInt KernLocalThreadDataIdOffset = -1;
#endif
extern "C" void HostedEnterUser(TLinAddr aEntry, TInt aReason, const TAny* aBlock, TInt aBlockSize, TInt aLtdSize, TUint32 aThreadId, TInt aIdOffset);
__asm__(
	".text\n .arm\n .balign 4\n .global HostedEnterUser\n HostedEnterUser:\n"
	"	mov	r8, r0\n"				// entry
	"	mov	r9, r1\n"				// reason
	"	mov	r10, r2\n"				// block
	"	mov	r11, r3\n"				// block size
	"	ldr	r5, [sp]\n"				// TLocalThreadData size
	"	ldr	r6, [sp, #4]\n"			// thread ID
	"	ldr	r7, [sp, #8]\n"			// its offset (< 0: none)
	"	bic	sp, sp, #7\n"
	"	sub	sp, sp, r5\n"				// TLocalThreadData
	"	mov	r0, sp\n"
	"	mcr	p15, 0, r0, c13, c0, 2\n"	// TPIDRURW -> it
	"	mov	r1, #0\n"
	"	mov	r2, r5\n"
	"	bl	memset\n"
	"	cmp	r7, #0\n"
	"	strge	r6, [sp, r7]\n"
	"	sub	sp, sp, #4\n"
	"	sub	sp, sp, r11\n"			// the creation info block
	"	mov	r0, sp\n"
	"	mov	r1, r10\n"
	"	mov	r2, r11\n"
	"	bl	memcpy\n"
	"	mov	r4, r9\n"
	"	mov	r0, #0\n mov r1, #0\n mov r2, #0\n mov r3, #0\n"
	"	bx	r8\n");

extern void HostedNameHostThread(DThread& aThread);		// kthreadname.cpp

void DThread::EpocThreadFunction(TAny* aParam)
	{
	SThreadCreateInfo& info = *static_cast<SThreadCreateInfo*>(aParam);
	DThread& t = *TheCurrentThread;
	HostedNameHostThread(t);
	__DEBUG_EVENT(EEventStartThread, &t);
	if (t.iThreadType != EThreadUser)
		Kern::Exit(info.iFunction(info.iPtr));
	else
		{
		TLinAddr entry;
		TInt reason;
		if (t.iFlags & KThreadFlagOriginal)
			{
			entry = t.iOwningProcess->iCodeSeg->iEntryPtVeneer;
			reason = KModuleEntryReasonProcessInit;
			}
		else
			{
			entry = t.iOwningProcess->iReentryPoint;
			reason = KModuleEntryReasonThreadInit;
			}
#ifdef __USERSIDE_THREAD_DATA__
		if (KernLocalThreadDataIdOffset >= 0)
			t.iFlags |= KThreadFlagLocalThreadDataValid;
#endif
		t.iUserThreadState = EUserThreadRunning;
		LeaveKernel();
		HostedEnterUser(entry, reason, &info, info.iTotalSize, KernLocalThreadDataSize, t.iId, KernLocalThreadDataIdOffset);
		}
	}

TInt DWin32Thread::Context(TDes8&)
	{
	if (iThreadType!=EThreadUser || this==TheCurrentThread)
		return KErrAccessDenied;
	return KErrNotSupported;
	}

DProcess* P::NewProcess()
	{
	return new DWin32Process;
	}

TInt DWin32Process::GetNewThread(DThread*& aThread, SThreadCreateInfo&)
	{
	if (aThread)
		new (aThread) DWin32Thread;
	else
		{
		aThread=new DWin32Thread;
		if (!aThread)
			return KErrNoMemory;
		}
	aThread->iOwningProcess=this;
	return KErrNone;
	}

static TExcType ExceptionType(TUint32 aSignal)
	{
	switch (aSignal)
		{
	case SIGSEGV: return EExcAccessViolation;
	case SIGBUS: return EExcAlignment;
	case SIGILL: return EExcInvalidOpCode;
	case SIGTRAP: return EExcBreakPoint;
	default: return EExcGeneral;
		}
	}

void Exc::Dispatch(TAny* aPtr, NThread*)
	{
	TWin32ExcInfo* pR=(TWin32ExcInfo*)aPtr;
	DThread* pT=TheCurrentThread;
	TExcTrap* xt=pT->iExcTrap;
	if (xt)
		(*xt->iHandler)(xt,pT,aPtr);
	if ((pR->iFlags&TWin32ExcInfo::EExcInKernel) && NKern::HeldFastMutex())
		Exc::Fault(aPtr);
	NKern::LockSystem();
	if (pT->iThreadType==EThreadUser && !(pR->iFlags&TWin32ExcInfo::EExcInKernel))
		{
		TExcType type=ExceptionType(pR->iExcCode);
		pR->iExcType = type;
		if (pT->IsExceptionHandled(type))
			{
			pT->iFlags |= KThreadFlagLastChance;
			NKern::UnlockSystem();
			pR->iHandler = (TExcHandler)pT->iOwningProcess->iReentryPoint;
			pR->iParam[0] = (TAny*)KModuleEntryReasonException;
			pR->iParam[1] = (TAny*)&pR->iExcType;
			return;
			}
		}
	NKern::UnlockSystem();
	// as the ARM kernel's exception report ("Exc n Cpsr= FAR= ... R0..", serial log)
	Kern::Printf("Exc signal %d addr %08x pc %08x lr %08x sp %08x cpsr %08x %s", pR->iExcCode, pR->iExcDataAddress,
	             pR->iPc, pR->iLr, pR->iSp, pR->iCpsr, (pR->iFlags & TWin32ExcInfo::EExcInKernel) ? "(kernel)" : "(user)");
	Kern::Printf(" R0=%08x R1=%08x R2=%08x R3=%08x R4=%08x R5=%08x R6=%08x R7=%08x", pR->iR[0], pR->iR[1], pR->iR[2],
	             pR->iR[3], pR->iR[4], pR->iR[5], pR->iR[6], pR->iR[7]);
	Kern::Printf(" R8=%08x R9=%08x R10=%08x R11=%08x R12=%08x thread %O", pR->iR[8], pR->iR[9], pR->iR[10], pR->iR[11],
	             pR->iR[12], pT);
	if (pR->iFlags & TWin32ExcInfo::EExcInKernel)
		{
		// the hosted kernel is built with frame pointers: its call chain (symbolise against the test binary)
		TUint32* fp = (TUint32*)pR->iR[11];
		TUint32 f[12];
		TInt n = 0;
		for (; n < 12 && fp && ((TUint32)fp & 3) == 0 && (TUint32)fp > 0x1000; ++n)
			{
			f[n] = fp[1];		// clang ARM frame (push {fp, lr}): [fp] = previous fp, [fp+4] = return address
			TUint32* next = (TUint32*)fp[0];
			if (next <= fp)
				{
				++n;
				break;
				}
			fp = next;
			}
		Kern::Printf(" frames: %08x %08x %08x %08x %08x %08x %08x %08x", n > 0 ? f[0] : 0, n > 1 ? f[1] : 0, n > 2 ? f[2] : 0,
		             n > 3 ? f[3] : 0, n > 4 ? f[4] : 0, n > 5 ? f[5] : 0, n > 6 ? f[6] : 0, n > 7 ? f[7] : 0);
		}
	NKern::LockSystem();
	if (pT->iFlags & (KThreadFlagSystemCritical|KThreadFlagSystemPermanent))
		Exc::Fault(aPtr);
	NKern::UnlockSystem();
	TUint m = DKernelEventHandler::Dispatch(EEventHwExc, pR, NULL);
	if (m & DKernelEventHandler::EExcHandled)
		return;
	K::PanicKernExec(ECausedException);
	}

EXPORT_C void Exc::Fault(TAny* aPtr)
	{
	TWin32ExcInfo* pR=(TWin32ExcInfo*)aPtr;
	if (pR)
		hprintf("KERNEL EXCEPTION: signal %u addr %08x pc %08x lr %08x\n", pR->iExcCode, pR->iExcDataAddress, pR->iPc, pR->iLr);
	Kern::Fault("Exception",K::ESystemException);
	}

extern "C" void ExcFault(TAny* aExcInfo)
	{
	Exc::Fault(aExcInfo);
	}

// ============================================================ internal RAM drive (nvram.cpp)
_LIT(KLitMachineConfigMutex,"MCConfMutex");
void K::InitNvRam()
	{
	__KTRACE_OPT(KBOOT,Kern::Printf("K::InitNvRam"));
	if (K::MutexCreate(K::MachineConfigMutex, KLitMachineConfigMutex, NULL, EFalse, KMutexOrdMachineConfig) != KErrNone)
		K::Fault(K::EMachineConfigMutexCreateFailed);
	TInt r=TInternalRamDrive::Create();
	if (r!=KErrNone)
		K::Fault(K::ERamDriveInitFailed);
	__KTRACE_OPT(KBOOT,Kern::Printf("K::InitNvRam() completed"));
	}

_LIT(KLitRamDriveMutex, "RamDriveMutex");

// hosted: the RAM drive is anonymous memory (lost at exit, like a cold-booted phone's); the Win32
// emulator kept it in a host file
TInt TInternalRamDrive::Create()
	{
	TInt r = K::MutexCreate((DMutex*&)Mutex, KLitRamDriveMutex, NULL, EFalse, KMutexOrdRamDrive);
	if (r != KErrNone)
		return r;
	TUint size = PP::RamDriveMaxSize + sizeof(TRamDriveInfo);
	TAny* m = hmmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (HMAP_FAILED(m))
		return KErrNoMemory;
	PP::RamDriveStartAddress = (TLinAddr)m;
	PP::RamDriveInfo = (TRamDriveInfo*)(PP::RamDriveStartAddress + PP::RamDriveMaxSize);
	PP::RamDriveInfo->iSize = 0;
	TheSuperPage().iRamDriveSize = 0;
	return KErrNone;
	}

EXPORT_C TLinAddr TInternalRamDrive::Base() { return PP::RamDriveStartAddress; }
EXPORT_C TInt TInternalRamDrive::Size() { return PP::RamDriveInfo->iSize; }

EXPORT_C TInt TInternalRamDrive::Adjust(TInt aNewSize)
	{
	if (aNewSize<0)
		return KErrArgument;
	if (aNewSize>PP::RamDriveMaxSize)
		return KErrDiskFull;
	PP::RamDriveInfo->iSize = aNewSize;
	return KErrNone;
	}

EXPORT_C void TInternalRamDrive::Wait() { Kern::MutexWait(*Mutex); }
EXPORT_C void TInternalRamDrive::Signal() { Kern::MutexSignal(*Mutex); }
void ExecHandler::UnlockRamDrive() {}
EXPORT_C void TInternalRamDrive::Unlock() {}
EXPORT_C void TInternalRamDrive::Lock() {}

// __DEBUGGER_SUPPORT__ (as our ARM kernel, variant.mmh): kernel event hooks for debug agents. The code
// modifier (debugger breakpoints written into code, memmodel/epoc/putils.cpp) is not ported: no
// breakpoint can exist, so a removed code segment has none to restore.
void CodeModifier::CodeSegRemoved(DCodeSeg*, DProcess*)
	{
	}

// the exec trace's thread names (nanokernel layer: src/hosted/nkern/linux/vectors.cpp)
extern void (*HostedExecTraceWho)(char* aBuf, TInt aMax);
static void ExecTraceWho(char* aBuf, TInt aMax)
	{
	TBuf8<KMaxFullName> n;
	TheCurrentThread->FullName(n);
	TInt l = Min(n.Length(), aMax - 1);
	memcpy(aBuf, n.Ptr(), l);
	aBuf[l] = 0;
	}
static struct SExecTraceWhoInit { SExecTraceWhoInit() { HostedExecTraceWho = &ExecTraceWho; } } ExecTraceWhoInit;
