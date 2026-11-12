// kn_boot: the unmodified Symbian kernel (EKERN's generic layer) boots on the hosted platform.
// The N95's composed core ROM is mapped at 0x80000000 (its TRomHeader is the kernel's ROM header), the
// test variant (src/hosted/variant) is registered, boot tracing (KBOOT) is on, and HostedBootEpoc runs
// KernelMain: variant creation, nanokernel and microkernel initialisation, the supervisor thread, the
// kernel heap and object containers, timers, power model, then the kernel extensions. A test extension
// (HostedAddExtension) is started by P::StartExtensions late in that sequence: it proves the boot got
// there, checks a few kernel facts and ends the test. SKIP without a ROM.
#include <kernel/kern_priv.h>
#include "romload.h"
#include <e32rom.h>

extern void HostedUseTestVariant();
extern TInt HostedAddExtension(TInt (*aEntry)(TInt));
extern void HostedSetDebugMask(TInt aIndex, TUint32 aMask);
extern void HostedBootEpoc(TLinAddr aRomHeader);

// tick-queue timers (TTickLink: what User::After / RTimer::After use): when each one fired (nanokernel
// tick), 0 = not yet
static volatile TUint32 Fired[4];
static void TickFired(TAny* aPtr) { Fired[(TInt)aPtr] = NKern::TickCount(); }
static TTickLink* links;

static TInt ExtensionEntry(TInt aReason)
	{
	if (aReason == KModuleEntryReasonExtensionInit0)
		return KErrNone;		// "not started yet": start it
	HCHECK(ETrue, "kernel boot reached the extensions (P::StartExtensions)");
	DThread* t = &Kern::CurrentThread();
	HCHECK(t != NULL && t->iOwningProcess == K::TheKernelProcess, "running in a kernel thread of the kernel process");
	TBuf8<64> name;
	t->FullName(name);
	htest_note("current thread: %.*s", name.Length(), name.Ptr());
	HCHECK(K::TheKernelProcess != NULL, "the kernel process exists");
	TAny* p = Kern::Alloc(1000);
	HCHECK(p != NULL, "the kernel heap allocates (%p)", p);
	Kern::Free(p);
	// the ROM root directory (of the active variant) is a TRomDir with "sys" in it (regression: the
	// root directory list was returned)
	extern TLinAddr HostedRomRootDir();
	const TRomDir* rd = (const TRomDir*)HostedRomRootDir();
	TBool sys = EFalse;
	if (rd)
		{
		// TRomEntry: names are UTF-16 (2 bytes per character); entries are 4-byte aligned
		const TUint8* p = (const TUint8*)&rd->iEntry;
		const TUint8* end = p + rd->iSize;
		while (p < end)
			{
			const TRomEntry* e = (const TRomEntry*)p;
			if (e->iNameLength == 3 && (e->iName[0] | 32) == 's' && (e->iName[2] | 32) == 'y' && (e->iName[4] | 32) == 's')
				sys = ETrue;
			p += (_FOFF(TRomEntry, iName) + e->iNameLength * 2 + 3) & ~3;
			}
		}
	HCHECK(sys, "the ROM root directory %p has a 'sys' entry", rd);
	// the kernel's code segment is found by its ROM image (the loader's dependency lookup: CodeSegNext ->
	// DCodeSeg::FindRomCode). Regression: the tagged-pointer comparator of scodeseg.cpp folded by clang -O2
	const TRomEntry* pe = (const TRomEntry*)Epoc::RomHeader().iPrimaryFile;
	DCodeSeg::Wait();
	DCodeSeg* ks = pe ? DCodeSeg::FindRomCode((const TAny*)pe->iAddressLin) : NULL;
	DCodeSeg::Signal();
	HCHECK(ks && ks == K::TheKernelProcess->iCodeSeg, "the kernel's code segment is found by its ROM image (%p)", ks);
	TUint32 t0 = NKern::TickCount();
	NKern::Sleep(10);
	HCHECK(NKern::TickCount() - t0 >= 10, "the tick runs (%u ticks over a 10-tick sleep)", NKern::TickCount() - t0);
	// tick-queue timers (TTickLink::OneShot takes microseconds) fire when due, rounded up to 1/64 s
	// ticks; several at once, a later one queued first
	const TInt us[4] = { 2000000, 78125, 1000000, 500000 };
	links = new TTickLink[4];
	TUint32 start = NKern::TickCount();
	for (TInt i = 0; i < 4; ++i)
		links[i].OneShot(us[i], &TickFired, (TAny*)i);
	extern volatile TUint64 HostedIdleNs;
	extern volatile TUint32 HostedNullIdles;
	TUint64 idle0 = HostedIdleNs, w0 = hnow_ns();
	NKern::Sleep(2600);
	// regression 2026-11-12 (nk_idlewait in kplat.cpp's note): the null thread spun in NKIdle instead of
	// waiting for an interrupt (100% CPU for ever after the phone's boot). Asleep here, the machine is idle.
	TUint32 idleMs = (TUint32)((HostedIdleNs - idle0) / 1000000), wallMs = (TUint32)((hnow_ns() - w0) / 1000000);
	HCHECK(HostedNullIdles > 0 && idleMs * 2 > wallMs, "the null thread waits for interrupts: idle %u ms of a %u ms sleep (%u idle waits)",
	       idleMs, wallMs, HostedNullIdles);
	for (TInt i = 0; i < 4; ++i)
		{
		TUint32 want = us[i] / 1000;		// ms
		TUint32 got = Fired[i] ? Fired[i] - start : 0;
		HCHECK(Fired[i] && got >= want && got <= want + 200, "tick timer of %d us fired after %u ms (due %u ms)",
		       us[i], got, want);
		}
	hexit(htest_done());
	return KErrNone;
	}

int main(int, char**)
	{
	SRomArgs a;
	if (!RomReadArgs(a))
		{
		htest_note("SKIP: no phone ROM given (htest --rom DEVICE)");
		hexit(77);
		}
	HCHECK(RomLoad(a), "core ROM loaded at %08x", a.iBase);
	// our own image must not overlap a Symbian region (regression: .bss over 0x00400000)
	extern char __executable_start[], _end[];
	TUint32 lo = (TUint32)__executable_start, hi = (TUint32)_end;
	HCHECK(lo >= 0xC9200000u && hi <= 0xF0000000u, "hosted image %08x..%08x clear of user data, ROM and kernel data", lo, hi);
	LxInit();
	// the board the ROM's kernel extensions expect (LCD.DLL reads the framebuffer's registers at boot).
	// Bug 2026-11-12: kn_boot booted without it and faulted once LCD.DLL had a hosted build.
	extern TInt HostedBoardInit(const char* aBoardConfig, const char* aRofsImage);
	extern TInt HostedFbAttach(TInt aWidth, TInt aHeight);
	HCHECK(HostedBoardInit(a.iBoardConfig, a.iRofs[0] ? a.iRofs : NULL) == KErrNone, "hosted board");
	HCHECK(HostedFbAttach(a.iFbWidth ? a.iFbWidth : 240, a.iFbHeight ? a.iFbHeight : 320) == KErrNone, "framebuffer");
	HostedUseTestVariant();
	HostedAddExtension(&ExtensionEntry);
	HostedSetDebugMask(0, hgetenv_hex("KTRACE", (1u << KBOOT) | (1u << KPROC) | (1u << KDLL) | (1u << KTHREAD)));
	HostedBootEpoc(a.iBase);		// the TRomHeader is at the start of the core ROM
	HCHECK(EFalse, "HostedBootEpoc returned");
	return htest_done();
	}
