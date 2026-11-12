// kn_efile: the phone's user side starts on the hosted kernel.
// State 2026-11-12 (the frontier this test pins): the EXSTART extension creates the ROM header's secondary
// process, Nokia's efile.exe (unmodified ROM code), which mounts the drives (ROFS Z: from the board's
// flash window, NVMemory C:/E: from the seed images, mapped private) and starts EStart.exe; EStart loads
// the ISC stack (our stand-ins as built-in kernel modules), starts domainSrv, SecEnvInit, EInfoServer, the
// central repository and LanguageSelection, which waits for modem replies (the board has no fake modem
// yet: requests stay unanswered). Every exec call from EUSER's stubs is trapped into the kernel's own exec
// tables. A probe (a timer DFC on a kernel DFC queue, 15 s, htest --env PROBE_MS=hex) lists the processes
// and threads and checks the milestones; a kernel fault before it ends the test through the fault hook.
// SKIP without a ROM.
#define __INCLUDE_NTHREADBASE_DEFINES__
#include <kernel/kern_priv.h>
#include "romload.h"
#include <e32rom.h>

extern void HostedUseTestVariant();
extern TInt HostedAddExtension(TInt (*aEntry)(TInt));
typedef void (*THostedCtor)();
extern TInt HostedAddExtension(TInt (*aEntry)(TInt), THostedCtor* aCtors, THostedCtor* aCtorsEnd);
extern "C" THostedCtor __start_kmodinit_Exstart[] __attribute__((weak)), __stop_kmodinit_Exstart[] __attribute__((weak));
extern void HostedSetDebugMask(TInt aIndex, TUint32 aMask);
extern void HostedBootEpoc(TLinAddr aRomHeader);
extern void HostedSetKernelConfig(TUint32 aSet);
extern TInt HostedExecTrapInit(TLinAddr aLo, TLinAddr aHi);
extern volatile TUint32 HostedExecTraps;
extern volatile TUint32 HostedExeDataSwitches;
extern TInt HostedTraceExecs;
TInt Exstart_KernelModuleEntry(TInt aReason);

static void Name(DObject* aObj, char* aOut, TInt aMax)
	{
	TBuf8<128> n;
	aObj->FullName(n);
	TInt l = Min(n.Length(), aMax - 1);
	memcpy(aOut, n.Ptr(), l);
	aOut[l] = 0;
	}

static const char* strstr_(const char* h, const char* n);
#define strstr strstr_
static TBool WasSeen(const char* aPart);
static char Seen[96][32];
static TUint32 SeenMs[96];			// when it started (ms of host time since the test started)
static TInt SeenCount;
static TUint64 T0;
extern const TUint32* HostedFbPixels(TInt& aWidth, TInt& aHeight, TInt& aPitch);
static void ProbeFn(TAny*)
	{
	DObjectCon& procs = *K::Containers[EProcess];
	DObjectCon& threads = *K::Containers[EThread];
	procs.Wait();
	TBool efile = EFalse;
	char n[128];
	for (TInt i = 0; i < procs.Count(); ++i)
		{
		DProcess* p = (DProcess*)procs[i];
		Name(p, n, sizeof n);
		htest_note("process %s exit type %d reason %d", n, p->iExitType, p->iExitReason);
		if (strstr(n, "efile") || strstr(n, "EFile") || strstr(n, "EFILE"))
			efile = p->iExitType == EExitPending;
		}
	TInt alive = 0;
	// EStart ends once start-up is handed over (as on the ARM board: exit type Kill, reason 0)
	TBool fbserv = EFalse;
	TBool estart = WasSeen("EStart"), sysagent = WasSeen("sysagt2svr"), wserv = WasSeen("EwSrv");
	for (TInt i = 0; i < procs.Count(); ++i)
		{
		DProcess* p = (DProcess*)procs[i];
		Name(p, n, sizeof n);
		if (p->iExitType == EExitPending)
			++alive;
		if (strstr(n, "FbServ") && p->iExitType == EExitPending)
			fbserv = ETrue;


		}
	procs.Signal();
	// the ISC stand-ins: both built from iscpeer.inl, each with its own port
	DObjectCon& ldds = *K::Containers[ELogicalDevice];
	ldds.Wait();
	TBool isc = EFalse, iscext = EFalse;
	for (TInt i = 0; i < ldds.Count(); ++i)
		{
		Name(ldds[i], n, sizeof n);
		if (!strcmp(n, "NokiaISCDriver")) isc = ETrue;
		if (!strcmp(n, "NokiaISCDriverExtension")) iscext = ETrue;
		}
	ldds.Signal();
	threads.Wait();
	TInt stuck = 0;
	for (TInt i = 0; i < threads.Count(); ++i)
		{
		DThread* t = (DThread*)threads[i];
		// a user thread waiting inside an exec is at kernel level 1; deeper, its completion callbacks never run
		// (a callback may legitimately wait unsignalled: completions without a status)
		if (t->iThreadType == EThreadUser && t->iNThread.iRequestSemaphore.iCount < 0 && ((NThread&)t->iNThread).iInKernel > 1)
			++stuck;
		Name(t, n, sizeof n);
		htest_note("thread %s state %d exit %d/%d sem %d callbacks %p timer %d inkernel %d", n, t->iNThread.iNState, t->iExitType,
		           t->iExitReason, t->iNThread.iRequestSemaphore.iCount, t->iNThread.iUserModeCallbacks, t->iTimer.iState,
		           ((NThread&)t->iNThread).iInKernel);
		}
	threads.Signal();
	htest_note("%u exec calls trapped from EUSER", HostedExecTraps);
	extern volatile TUint32 HostedSwitches;
	htest_note("thread switches %u", HostedSwitches);
	extern volatile TUint64 HostedIdleNs;
	htest_note("idle %u ms of %u ms", (TUint32)(HostedIdleNs / 1000000), (TUint32)((hnow_ns() - T0) / 1000000));
	extern volatile TUint32 HostedTicks, HostedTicksSkipped;
	htest_note("kernel ticks %u at %u ms (%u tick interrupts, %u suppressed)", NKern::TickCount(),
	           (TUint32)((hnow_ns() - T0) / 1000000), HostedTicks - HostedTicksSkipped, HostedTicksSkipped);
	htest_note("tick queue ms timer: state %d trigger %u (now %u) dfc queued %d", K::TickQ->iMsTimer.i_NTimer_iState,
	           K::TickQ->iMsTimer.iTriggerTime, NKern::TickCount(), K::TickQ->iTickDfc.Queued());
	NThreadBase* tt = K::TimerDfcQ->iThread;
	htest_note("scheduler dfc pending %d reschedule %d kernlock %d; timer thread state %d dfcq ready %d",
	           TheScheduler.iDfcPendingFlag, TheScheduler.iRescheduleNeededFlag, TheScheduler.iKernCSLocked,
	           tt->iNState, K::TimerDfcQ->iPresent[0] | K::TimerDfcQ->iPresent[1]);
	extern TUint32 DiagMs, DiagMsAdded, DiagTickDfc, DiagSetTime;
	htest_note("DIAG ms callbacks %u (dfc added %u), tick dfc runs %u, set-time cancels %u", DiagMs, DiagMsAdded, DiagTickDfc, DiagSetTime);
	// the tick queue's links: delta (ticks after the previous one), the owner of thread timers
	{
	TTickQ::Wait();
	TInt n = 0;
	TInt acc = 0;
	for (SDblQueLink* l = K::TickQ->iA.iNext; l != &K::TickQ->iA && n < 16; l = l->iNext, ++n)
		{
		TTickLink* tl = (TTickLink*)l;
		acc += tl->iDelta;
		char on[64] = "";
		for (TInt i = 0; i < threads.Count(); ++i)
			if ((TAny*)&((DThread*)threads[i])->iTimer == tl->iPtr)
				Name(threads[i], on, sizeof on);
		htest_note("tick link %d: delta %d (at +%d ticks) cb %p ptr %p %s", n, tl->iDelta, acc, tl->iCallBack, tl->iPtr, on);
		}
	TTickQ::Signal();
	}
	htest_note("tick queue: lastms %u lastdelta %d tickperiod %d msperiod %d rounding %d/%d now %u", K::TickQ->iLastMs,
	           K::TickQ->iLastDelta, K::TickQ->iTickPeriod, K::TickQ->iMsTickPeriod, K::TickQ->iRounding,
	           K::TickQ->iPrevRounding, NKern::TickCount());
	for (TInt i = 0; i < SeenCount; ++i)
		htest_note("started: %s at %u ms", Seen[i], SeenMs[i]);
	// servers with outstanding messages (who waits on whom): delivered-not-received count and, per
	// session, messages not completed yet, with the session's owner
	DObjectCon& servers = *K::Containers[EServer];
	servers.Wait();
	for (TInt i = 0; i < servers.Count(); ++i)
		{
		DServer* sv = (DServer*)servers[i];
		TInt delivered = 0;
		for (SDblQueLink* l = sv->iDeliveredQ.First(); l != &sv->iDeliveredQ.iA; l = l->iNext)
			++delivered;
		for (SDblQueLink* l = sv->iSessionQ.First(); l != &sv->iSessionQ.iA; l = l->iNext)
			{
			DSession* ss = _LOFF(l, DSession, iServerLink);
			if (ss->iMsgCount <= 0)
				continue;
			char sn[64], on[96];
			Name(sv, sn, sizeof sn);
			if (ss->Owner())
				Name(ss->Owner(), on, sizeof on);
			else
				on[0] = 0;
			htest_note("server %s: session of %s has %d outstanding (server queue %d)", sn, on, ss->iMsgCount, delivered);
			}
		}
	servers.Signal();
	// efile.exe's .data at its run address was initialised from the ROM (regression: it was left zero)
	const TRomEntry* se = (const TRomEntry*)Epoc::RomHeader().iSecondaryFile;
	const TRomImageHeader& sh = *(const TRomImageHeader*)se->iAddressLin;
	HCHECK(sh.iDataSize == 0 || !memcmp((const TAny*)sh.iDataBssLinearBase, (const TAny*)sh.iDataAddress, 16) ||
	       efile, "efile.exe .data at %08x came from the ROM", sh.iDataBssLinearBase);
	HCHECK(efile, "efile.exe was created and is alive");
	HCHECK(HostedExecTraps > 0, "it made exec calls through EUSER (%u)", HostedExecTraps);
	// regressions 2026-11-12: a second EXE's .data at the same run address (KErrInUse, Fserv fault 11);
	// a loader-created EXE copied its .data from address 0; modules from one source shared inline code
	HCHECK(estart, "EStart.exe (the second EXE, data at the same address) ran");
	// past the modem (start-up waits for its replies): the System Agent and the window server
	HCHECK(sysagent, "the System Agent server was started (modem replies arrive)");
	HCHECK(wserv, "the window server was started");
	// regressions 2026-11-12: the variant HAL (EVariantHalVariantInfo) and kernel-side faults on a bad
	// client descriptor (IPC exception trap) each killed FbServ
	extern volatile TUint32 HostedExceptions;
	HCHECK(fbserv, "the font and bitmap server is alive (%u hardware exceptions handled)", HostedExceptions);
	// regression 2026-11-12: an empty command line (address 0, length 0) made the loader fail (-38)
	// regression 2026-11-12: a trapped kernel-side fault left iInKernel one too high (FbServ deadlock)
	HCHECK(stuck == 0, "no user thread waits at a kernel level above 1 (callbacks would never run) (%d)", stuck);
	// regression 2026-11-12: a module's own include path gave it the real syborg.h (plain memory reads)
	extern TUint32 HostedIoAccesses(TUint32 aBase);
	extern const TUint32* HostedFbPixels(TInt& aWidth, TInt& aHeight, TInt& aPitch);
	HCHECK(HostedIoAccesses(0xC6005000u) > 4, "the display drivers reached the framebuffer model through the bus (%u accesses)",
	       HostedIoAccesses(0xC6005000u));
	HCHECK(WasSeen("C_info"), "mobilecrashstarter started mc_info.exe (a RAM-loaded EXE from ROFS)");
	HCHECK(HostedExeDataSwitches > 0, "EXE data areas were switched on process switch (%u)", HostedExeDataSwitches);
	HCHECK(isc && iscext, "NokiaISCDriver and NokiaISCDriverExtension both installed (%d %d)", isc, iscext);
	HCHECK(alive >= 6, "%d processes alive (EStart starts the system servers)", alive);
	extern void HostedProfileReport();
	HostedProfileReport();
	// the screen as the phone shows it: the framebuffer, written for the host (/out, htest's vmout/)
	TInt fw = 0, fh = 0, pitch = 0;
	const TUint32* px = HostedFbPixels(fw, fh, pitch);
	if (px)
		{
		TInt lit = 0;
		for (TInt y = 0; y < fh; ++y)
			for (TInt x = 0; x < fw; ++x)
				lit += (((const TUint32*)((const TUint8*)px + y * pitch))[x] & 0xffffff) != 0;
		htest_note("framebuffer %dx%d pitch %d: %d non-black pixels", fw, fh, pitch, lit);
		TInt fd = hsys(SYS_openat, -100, "/out/kn_efile-fb.ppm", 0x241, 0644);
		if (fd >= 0)
			{
			char hdr[32];
			TInt hl = hsnprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", fw, fh);
			hsys(SYS_write, fd, hdr, hl);
			TUint8 line[3 * 1024];
			for (TInt y = 0; y < fh; ++y)
				{
				const TUint32* row = (const TUint32*)((const TUint8*)px + y * pitch);
				for (TInt x = 0; x < fw && x < 1024; ++x)
					{
					line[3 * x] = (TUint8)(row[x] >> 16);
					line[3 * x + 1] = (TUint8)(row[x] >> 8);
					line[3 * x + 2] = (TUint8)row[x];
					}
				hsys(SYS_write, fd, line, 3 * fw);
				}
			hsys(SYS_close, fd);
			}
		}
	else
		htest_note("framebuffer not enabled");
	hexit(htest_done());
	}

static void Name(DObject* aObj, char* aOut, TInt aMax);
extern void (*HostedFaultHook)(const TDesC8& aCategory, TInt aReason);
static void FaultHook(const TDesC8& aCategory, TInt aReason)
	{
	TBuf8<32> cat(aCategory.Left(32));
	// start-up now runs well past the media drivers: a kernel fault before the probe is a regression
	HCHECK(EFalse, "no kernel fault before the probe (fault %d after %u exec calls)", aReason, HostedExecTraps);
	HCHECK(HostedExecTraps > 3000, "efile.exe made more than 3000 exec calls through EUSER (%u)", HostedExecTraps);
	DObjectCon& procs = *K::Containers[EProcess];
	TBool efile = EFalse;
	char n[128];
	for (TInt i = 0; i < procs.Count(); ++i)
		{
		Name(procs[i], n, sizeof n);
		if (strstr(n, "efile"))
			efile = ETrue;
		}
	HCHECK(efile, "efile.exe was created");
	const TRomEntry* se = (const TRomEntry*)Epoc::RomHeader().iSecondaryFile;
	const TRomImageHeader& sh = *(const TRomImageHeader*)se->iAddressLin;
	HCHECK(!memcmp((const TAny*)(sh.iDataBssLinearBase + 0x100), (const TAny*)(sh.iDataAddress + 0x100), 8) ||
	       HostedExecTraps > 3000, "efile.exe .data came from the ROM");
	hexit(htest_done());
	}

// every process that started a thread (ended ones leave the container before the probe)
static TUint StartHook(TKernelEvent aEvent, TAny* a1, TAny*, TAny*)
	{
	if (aEvent == EEventStartThread && SeenCount < 96)
		{
		char n[128];
		Name(((DThread*)a1)->iOwningProcess, n, sizeof n);
		TInt l = Min((TInt)strlen(n), 31);
		n[l] = 0;
		for (TInt i = 0; i < SeenCount; ++i)
			if (!strcmp(Seen[i], n))
				return DKernelEventHandler::ERunNext;
		memcpy(Seen[SeenCount], n, l);
		Seen[SeenCount][l] = 0;
		SeenMs[SeenCount++] = (TUint32)((hnow_ns() - T0) / 1000000);
		}
	return DKernelEventHandler::ERunNext;
	}
static TBool WasSeen(const char* aPart)
	{
	for (TInt i = 0; i < SeenCount; ++i)
		if (strstr(Seen[i], aPart))
			return ETrue;
	return EFalse;
	}

static TDfc* ProbeDfc;
static NTimer* ProbeTimer;
static void ProbeTimerFn(TAny*) { ProbeDfc->Enque(); }

static TInt ProbeEntry(TInt aReason)
	{
	if (aReason == KModuleEntryReasonExtensionInit0)
		return KErrNone;
	(new DKernelEventHandler(&StartHook, NULL))->Add();
	if (hgetenv("PHONE"))
		return KErrNone;			// htest --phone: the phone runs until the window closes
	ProbeDfc = new TDfc(&ProbeFn, 0, K::DfcQ0, 3);
	ProbeTimer = new NTimer(&ProbeTimerFn, 0);
	ProbeTimer->OneShot(hgetenv_hex("PROBE_MS", 0) ? hgetenv_hex("PROBE_MS", 0) : 15000, ETrue);	// htest --env PROBE_MS=hex
	return KErrNone;
	}

static const char* strstr_(const char* h, const char* n)
	{
	TInt l = strlen(n);
	for (; *h; ++h)
		if (!memcmp(h, n, l)) return h;
	return 0;
	}

int main(int, char**)
	{
	T0 = hnow_ns();
	extern void HostedProfileStart();
	if (hgetenv_hex("PROFILE", 0))
		HostedProfileStart();
	SRomArgs a;
	if (!RomReadArgs(a))
		{
		htest_note("SKIP: no phone ROM given (htest --rom DEVICE)");
		hexit(77);
		}
	HCHECK(RomLoad(a), "core ROM loaded at %08x", a.iBase);
	LxInit();
	HCHECK(HostedExecTrapInit(a.iStubLo, a.iStubHi) == 0, "exec trap on EUSER's stubs %08x..%08x", a.iStubLo, a.iStubHi);
	extern TInt HostedBoardInit(const char* aBoardConfig, const char* aRofsImage);
	HCHECK(HostedBoardInit(a.iBoardConfig, a.iRofs[0] ? a.iRofs : NULL) == KErrNone, "hosted board: I/O area, board config, ROFS window %s", a.iRofs);
	extern TInt HostedBoardDrives(const char* aNvmem, const char* aNvmem2);
	HCHECK(HostedBoardDrives(a.iNvmem, a.iNvmem2) == KErrNone, "NVMemory drives C: %s, E: %s (private mappings)", a.iNvmem, a.iNvmem2);
	extern TInt HostedFbAttach(TInt aWidth, TInt aHeight);
	HCHECK(a.iFbWidth && HostedFbAttach(a.iFbWidth, a.iFbHeight) == KErrNone, "framebuffer %ux%u", a.iFbWidth, a.iFbHeight);
	hsys(SYS_prctl, 15 /* PR_SET_NAME */, "symbian");		// the process as Linux's top shows it
	if (hgetenv("PHONE") && !hgetenv("PHONE_NODISPLAY"))
		{
		// htest --phone: PHONE = the ivshmem BAR's sysfs file; the host window (tools/hosted/phone.py) maps it too
		extern TInt HostedDisplayStart(const char* aPath);
		HCHECK(HostedDisplayStart(hgetenv("PHONE")) == KErrNone, "the phone's screen in shared memory %s", hgetenv("PHONE"));
		}
	extern TInt HostedKeybAttach();
	HCHECK(HostedKeybAttach() == KErrNone, "keyboard (syborg-keyboard, keys from the phone window)");
	extern TInt HostedSndAttach();
	HCHECK(HostedSndAttach() == KErrNone, "sound device (syborg-snd, no output)");
	extern TInt HostedModemAttach(const char* aConfig, const char* aReplies);
	HCHECK(HostedModemAttach(a.iBoardConfig, a.iReplies) == KErrNone, "fake modem (shared core), replies %s", a.iReplies);
	HostedUseTestVariant();
	htest_note("exec personality %u", a.iPersonality);
	HostedSetKernelConfig(a.iPersonality << 25);		// the device's user-side exec numbering
	HostedSetKernelConfig(EKernelConfigPlatSecDiagnostics);	// print every failed capability check
	HostedAddExtension(&ProbeEntry);
	HostedSetDebugMask(0, hgetenv_hex("KTRACE", (1u << KBOOT) | (1u << KPANIC)));
	extern TInt HostedTickSkip;
	HostedTickSkip = hgetenv_hex("TICK_SKIP", 1);
	HostedTraceExecs = hgetenv_hex("XTRACE", 0);	// htest --xtrace N: the first N exec calls (execnames.py)
	extern const char* HostedTraceExecsMatch;
	HostedTraceExecsMatch = hgetenv("XTRACE_THREAD");	// htest --env XTRACE_THREAD=FbServ: only those
	HostedFaultHook = &FaultHook;
	// htest --env PROBES=addr,addr,...: one-shot code probes (Thumb addresses with bit 0 set)
	extern TInt HostedAddProbe(TUint32 aAddr);
	if (const char* pr = hgetenv("PROBES"))
		for (const char* q = pr; *q; )
			{
			TUint32 x = 0;
			while (*q && *q != ',') { x = x * 16 + (*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10); ++q; }
			if (*q == ',') ++q;
			if (x) HostedAddProbe(x);
			}
	HostedBootEpoc(a.iBase);
	return htest_done();
	}
