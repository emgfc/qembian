// nk_rom: unmodified Nokia ROM code running natively in a hosted EKA2 process.
// The phone's composed core ROM (shared read-only over 9p by htest --rom, never in the repo) is loaded
// at its link address 0x80000000; EUSER exports are found through the ROM's own export directory by
// ordinal (the Symbian 9.2 EUSER ABI). Checks: pure Thumb exports run (User::StringLength,
// Mem::Compare); exports that make exec calls (User::TickCount = slow exec 5, User::NTickCount = fast
// exec 0x10, User::FastCounter = fast exec 0x0f: personalities/sym92/exec.map) go through EUSER's own
// ARM stubs into our trap and Dispatch, and their results come back through EUSER; the trap range is
// EUSER's stub block found by romscan. Exit 77 (SKIP) without a ROM.
#include <nk_priv.h>

extern void HostedStartSystemTimer(TInt aPeriodUs);
extern TInt HostedExecTrapInit(TLinAddr aLo, TLinAddr aHi);
extern volatile TUint32 HostedExecTraps;

static TUint32 Base, Size, ExpDir, StubLo, StubHi;
static char RomPath[128];

static TBool ReadArgs()
	{
	char buf[512];
	TInt fd = hsys(SYS_openat, -100, "/t/nk_rom.args", 0, 0);
	if (fd < 0)
		return EFalse;
	TInt n = hsys(SYS_read, fd, buf, sizeof(buf) - 1);
	hsys(SYS_close, fd);
	if (n <= 0)
		return EFalse;
	buf[n] = 0;
	for (char* p = buf; *p; )
		{
		char* e = p; while (*e && *e != '\n') ++e;
		char save = *e; *e = 0;
		char* v = p; while (*v && *v != ' ') ++v;
		if (*v) *v++ = 0;
		TUint32 x = 0, y = 0;
		char* q = v;
		for (; *q && *q != ' '; ++q) x = x * 16 + (*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10);
		if (*q == ' ') for (++q; *q; ++q) y = y * 16 + (*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10);
		if (!strcmp(p, "rom")) { TInt l = strlen(v); if (l > 120) l = 120; memcpy(RomPath, v, l); RomPath[l] = 0; }
		else if (!strcmp(p, "base")) Base = x;
		else if (!strcmp(p, "size")) Size = x;
		else if (!strcmp(p, "euser_expdir")) ExpDir = x;
		else if (!strcmp(p, "stubs")) { StubLo = x; StubHi = y; }
		*e = save;
		p = *e ? e + 1 : e;
		}
	return RomPath[0] && Base && Size && ExpDir && StubLo < StubHi;
	}

static TBool LoadRom()
	{
	TInt fd = hsys(SYS_openat, -100, RomPath, 0, 0);
	if (fd < 0)
		return EFalse;
	TUint8* m = (TUint8*)hmmap((void*)Base, Size, PROT_READ | PROT_WRITE | PROT_EXEC,
	                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if ((TUint32)m != Base)
		return EFalse;
	TUint32 done = 0;
	while (done < Size)
		{
		TInt r = hsys(SYS_read, fd, m + done, Size - done > 0x100000 ? 0x100000 : Size - done);
		if (r <= 0) break;
		done += r;
		}
	hsys(SYS_close, fd);
	hsys(SYS_mprotect, m, Size, PROT_READ | PROT_EXEC);		// ROM: read-only, as on the phone
	return done == Size;
	}

static TAny* Export(TInt aOrdinal) { return (TAny*)((TUint32*)ExpDir)[aOrdinal - 1]; }

// ---- exec handlers behind EUSER (values the test can recognise) ----
static TInt TickCountExec() { return 0x7100c; }
static TInt NTickCountExec() { return 4242; }
static TInt FastCounterExec() { return 0x1234567; }
static TInt InvalidExec() { return -5; }

static TLinAddr FastFns[0x10];
static struct { TInt iCount; TLinAddr iFn[0x10]; } Fast;
static struct { TInt iCount; TLinAddr iInvalid; TLinAddr iPre; SSlowExecEntry iE[6]; } Slow;

static NThread Initial;
static NFastSemaphore Done;

static void UserFn(TAny*)
	{
	NThread& me = *static_cast<NThread*>(NKern::CurrentThread());
	me.iInKernel = 0;		// "user mode" from here: Nokia's EUSER code calls our execs
	typedef TInt (*TStrLen)(const TUint16*);
	typedef TInt (*TCompare)(const TUint16*, TInt, const TUint16*, TInt);
	typedef TUint (*TTick)();
	static const TUint16 hello[] = { 'h', 'o', 's', 't', 'e', 'd', 0 };
	static const TUint16 other[] = { 'h', 'o', 's', 't', 'i', 'l', 'e', 0 };
	TInt len = ((TStrLen)Export(596))(hello);
	TInt cmp = ((TCompare)Export(523))(hello, 6, other, 7);
	TInt same = ((TCompare)Export(523))(hello, 6, hello, 6);
	TUint32 traps0 = HostedExecTraps;
	TUint tick = ((TTick)Export(674))();
	TUint ntick = ((TTick)Export(2103))();
	TUint fc = ((TTick)Export(584))();
	TUint32 traps = HostedExecTraps - traps0;
	me.iInKernel = 1;
	HCHECK(len == 6, "Nokia User::StringLength(\"hosted\") = %d", len);
	HCHECK(cmp < 0 && same == 0, "Nokia Mem::Compare: %d (hosted < hostile), %d (equal)", cmp, same);
	HCHECK(tick == 0x7100c, "Nokia User::TickCount() -> slow exec 5 -> our Dispatch -> %x", tick);
	HCHECK(ntick == 4242, "Nokia User::NTickCount() -> fast exec 0x10 -> %u", ntick);
	HCHECK(fc == 0x1234567, "Nokia User::FastCounter() -> fast exec 0x0f -> %x", fc);
	HCHECK(traps == 3, "three execs trapped from EUSER's own stubs (%u)", traps);
	NKern::FSSignal(&Done);
	}

static void MainFn(TAny*)
	{
	Done.iOwningThread = NKern::CurrentThread();
	NThread* t = new NThread;
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iFunction = &UserFn;
	info.iPriority = 20;
	info.iTimeslice = -1;
	info.iFastExecTable = (const SFastExecTable*)&Fast;
	info.iSlowExecTable = (const SSlowExecTable*)&Slow;
	HCHECK(NKern::ThreadCreate(t, info) == KErrNone, "user thread created");
	NKern::ThreadResume(t);
	NKern::FSWait(&Done);
	hexit(htest_done());
	}

int main(int, char**)
	{
	if (!ReadArgs())
		{
		htest_note("SKIP: no phone ROM given (htest --rom DEVICE with a composed core ROM)");
		hexit(77);
		}
	HCHECK(LoadRom(), "ROM %s loaded at %08x (%u MB)", RomPath, Base, Size >> 20);
	TUint16 firstOrdinalIsCode = (((TUint32*)ExpDir)[0] >= Base && ((TUint32*)ExpDir)[0] < Base + Size);
	HCHECK(firstOrdinalIsCode, "EUSER export directory at %08x points into the ROM", ExpDir);
	htest_note("EUSER exec stubs %08x..%08x (trap range)", StubLo, StubHi);

	Fast.iCount = 0x11;
	for (TInt i = 0; i < 0x10; ++i) Fast.iFn[i] = (TLinAddr)&InvalidExec;
	Fast.iFn[0x0f - 1] = (TLinAddr)&FastCounterExec;
	Fast.iFn[0x10 - 1] = (TLinAddr)&NTickCountExec;
	Slow.iCount = 6;
	Slow.iInvalid = (TLinAddr)&InvalidExec;
	for (TInt i = 0; i < 6; ++i) Slow.iE[i].iFunction = (TLinAddr)&InvalidExec;
	Slow.iE[5].iFunction = (TLinAddr)&TickCountExec;
	(void)FastFns;

	LxInit();
	HCHECK(HostedExecTrapInit(StubLo, StubHi) == 0, "trap on EUSER's stub block");
	SNThreadCreateInfo info;
	memclr(&info, sizeof(info));
	info.iTimeslice = -1;
	NTimerQ::Init1(1000);
	NKern::Init(&Initial, info);
	NThread* m = new NThread;
	SNThreadCreateInfo mi;
	memclr(&mi, sizeof(mi));
	mi.iFunction = &MainFn;
	mi.iPriority = 25;
	mi.iTimeslice = -1;
	NKern::ThreadCreate(m, mi);
	NKern::ThreadResume(m);
	for (;;)
		NKern::Idle();
	}
