// oracle: records what a real phone does at boot, for comparison with qembian running the same firmware
// (docs/real-phone-tracing.md, stage 1). Started by the boot hook (boothook.exe as C:\sys\bin\HALSettings.exe)
// about 0.5 s into the boot; runs on the phone and in qembian alike.
//
// Safety rule: it runs only when E:\oracle\boot.log does not exist. It creates the file first, so if the
// phone crashes or hangs during a recording, the next boot finds the file and does nothing. Re-arm =
// copy the file off the phone and delete it.
//
// Records (one line each, "T.ttt ORACLE: ..." with T = seconds since boot):
//   keys N SOURCE             the P&S keys watched: E:\oracle\keys.txt if present (one CAT/KEY in hex per
//                             line, '#' comments; written by `tools/oracle.py keys`), else the N95 list below
//   prop CAT/KEY = VALUE      every change of the P&S keys qembian publishes on this phone (subscribed)
//   thread+ NAME              a thread seen for the first time (polled every 200 ms)
//   thread? NAME open R       a thread the kernel would not open for us (not tried again)
//   thread- NAME TYPE REASON CATEGORY   a thread's exit (type: kill, terminate, panic), from the kernel's
//                             undertaker: every death, also of threads that lived less than a poll
//   end                       after KRunSeconds, file closed
#include <e32base.h>
#include <e32property.h>
#include <e32svr.h>
#include <f32file.h>
#include <hal.h>

_LIT(KDir, "E:\\oracle\\");
_LIT(KLog, "E:\\oracle\\boot.log");
_LIT(KKeysFile, "E:\\oracle\\keys.txt");
const TInt KMaxKeys = 1024;
const TInt KRunSeconds = 120;
const TInt KPollMs = 200;
const TInt KMaxThreads = 1000;

struct SKey { TUint32 iCat, iKey; };
// the keys the N95 8GB (RM-320, V 35.0.001) boot sets in qembian (SYBPROP lines of a smoke boot)
static const SKey KKeys[] =
	{
	{0x101f75b6, 0x1},
	{0x101f75b6, 0x100052c5},
	{0x101f75b6, 0x100052c6},
	{0x101f75b6, 0x100052c7},
	{0x101f75b6, 0x100052c8},
	{0x101f75b6, 0x100052c9},
	{0x101f75b6, 0x100052ca},
	{0x101f75b6, 0x100052cb},
	{0x101f75b6, 0x100052cc},
	{0x101f75b6, 0x100052cd},
	{0x101f75b6, 0x100052ce},
	{0x101f75b6, 0x100052cf},
	{0x101f75b6, 0x100052d0},
	{0x101f75b6, 0x100052d1},
	{0x101f75b6, 0x100052d2},
	{0x101f75b6, 0x100052d3},
	{0x101f75b6, 0x100052d4},
	{0x101f75b6, 0x100052d5},
	{0x101f75b6, 0x100052d6},
	{0x101f75b6, 0x100052d7},
	{0x101f75b6, 0x100052d8},
	{0x101f75b6, 0x100052d9},
	{0x101f75b6, 0x100052da},
	{0x101f75b6, 0x100052db},
	{0x101f75b6, 0x100052dc},
	{0x101f75b6, 0x100052dd},
	{0x101f75b6, 0x100052de},
	{0x101f75b6, 0x100052df},
	{0x101f75b6, 0x100052e0},
	{0x101f75b6, 0x100052e1},
	{0x101f75b6, 0x100052e2},
	{0x101f75b6, 0x100052e3},
	{0x101f75b6, 0x100052e4},
	{0x101f75b6, 0x100052e5},
	{0x101f75b6, 0x100052e6},
	{0x101f75b6, 0x100052e7},
	{0x101f75b6, 0x100052e8},
	{0x101f75b6, 0x100052e9},
	{0x101f75b6, 0x100052ea},
	{0x101f75b6, 0x100052eb},
	{0x101f75b6, 0x100052ec},
	{0x101f75b6, 0x100052ed},
	{0x101f75b6, 0x100052ee},
	{0x101f75b6, 0x100052ef},
	{0x101f75b6, 0x100052f0},
	{0x101f75b6, 0x100052f1},
	{0x101f75b6, 0x100052f2},
	{0x101f75b6, 0x100052f3},
	{0x101f75b6, 0x100052f4},
	{0x101f75b6, 0x100052fb},
	{0x101f75b6, 0x10005301},
	{0x101f75b6, 0x101f79dd},
	{0x101f75b6, 0x102045dd},
	{0x101f75b6, 0x102045de},
	{0x101f75b6, 0x10204bb5},
	{0x101f75b6, 0x1020762e},
	{0x101f75b6, 0x1020e34a},
	{0x101f75b6, 0x1020e34e},
	{0x101f75b6, 0xa0000694},
	{0x101f8766, 0x11},
	{0x101f8766, 0x12},
	{0x101f8766, 0x301},
	{0x101f8766, 0x4},
	{0x101f8766, 0x41},
	{0x101f8766, 0x42},
	{0x101f8766, 0x421},
	{0x101f8773, 0x1},
	{0x101f8773, 0x2},
	{0x101f8773, 0x3},
	{0x101f8773, 0x4},
	{0x101f8773, 0x5},
	{0x101f8773, 0x6},
	{0x101f8773, 0x7},
	{0x101f8773, 0x8},
	{0x101f8773, 0x9},
	{0x101f8789, 0x2},
	{0x101f8789, 0x3}
	};
const TInt KNumBuiltInKeys = sizeof(KKeys) / sizeof(KKeys[0]);

// no globals with constructors: startup.s does not run an EXE's static constructors (a global TBuf8
// stayed all zero and the first Format panicked USER 23)
static RFile Log;
static TInt TickUs;
static SKey* Keys;
static TInt NumKeys;

static void Put(const TDesC8& aText)
	{
	TBuf8<512> Line;
	TInt64 t = (TInt64)User::NTickCount() * TickUs;	// microseconds since boot
	Line.Format(_L8("%d.%03d ORACLE: "), I64INT(t / 1000000), I64INT((t / 1000) % 1000));
	Line.Append(aText.Left(Line.MaxLength() - Line.Length() - 2));
	Line.Append('\n');
	Log.Write(Line);
	}

struct SThread { TThreadId iId; RThread iT; TBool iDead; };
static SThread* Threads;
static TInt NumThreads;
// threads the kernel will not open for us (Belle: anonymous / non-global ones, process isolation): their
// names, hashed, so a poll does not try again (every refusal is a PlatSec diagnostic)
static TUint32* Refused;
static TInt NumRefused;

static TUint32 Hash(const TDesC& aName)
	{
	TUint32 h = 2166136261u;
	for (TInt i = 0; i < aName.Length(); ++i)
		h = (h ^ aName[i]) * 16777619u;
	return h;
	}

static void LogDeath(RThread& aT)
	{
	TBuf8<300> b;
	TExitCategoryName cat = aT.ExitCategory();
	TFullName n = aT.FullName();
	const TText8* type = aT.ExitType() == EExitKill ? _S8("kill")
		: aT.ExitType() == EExitTerminate ? _S8("terminate") : _S8("panic");
	b.Copy(_L8("thread- "));
	b.Append(n.Left(200));
	b.AppendFormat(_L8(" %s %d "), type, aT.ExitReason());
	b.Append(cat);
	Put(b);
	}

static void PollThreads()
	{
	TFindThread f(_L("*"));
	TFullName name;
	TBuf8<300> b;
	while (f.Next(name) == KErrNone)
		{
		TUint32 h = Hash(name);
		TBool refused = EFalse;
		for (TInt i = 0; i < NumRefused && !refused; ++i)
			refused = Refused[i] == h;
		if (refused)
			continue;
		RThread t;
		TInt r = t.Open(f);
		if (r != KErrNone)
			{
			if (NumRefused < KMaxThreads)
				Refused[NumRefused++] = h;
			b.Copy(_L8("thread? "));
			b.Append(name.Left(250));
			b.AppendFormat(_L8(" open %d"), r);
			Put(b);
			continue;
			}
		TThreadId id = t.Id();
		TBool known = EFalse;
		for (TInt i = 0; i < NumThreads && !known; ++i)
			known = Threads[i].iId == id;
		if (known || NumThreads >= KMaxThreads)
			{
			t.Close();
			continue;
			}
		Threads[NumThreads].iId = id;
		Threads[NumThreads].iT = t;			// kept open: the exit information stays readable
		Threads[NumThreads].iDead = EFalse;
		++NumThreads;
		b.Copy(_L8("thread+ "));
		b.Append(name.Left(250));
		Put(b);
		}
	}

// E:\oracle\keys.txt: "CAT/KEY" in hex, one per line; anything after '#' is a comment
static TBool LoadKeys(RFs& aFs)
	{
	RFile f;
	if (f.Open(aFs, KKeysFile, EFileRead | EFileShareReadersOnly) != KErrNone)
		return EFalse;
	TInt size = 0;
	f.Size(size);
	HBufC8* buf = HBufC8::New(size);
	Keys = new SKey[KMaxKeys];
	if (!buf || !Keys)
		{
		delete buf;
		f.Close();
		return EFalse;
		}
	TPtr8 p = buf->Des();
	f.Read(p);
	f.Close();
	while (p.Length() && NumKeys < KMaxKeys)
		{
		TInt nl = p.Locate('\n');
		TPtrC8 line = nl < 0 ? TPtrC8(p) : p.Left(nl);
		TInt hash = line.Locate('#');
		if (hash >= 0)
			line.Set(line.Left(hash));
		TLex8 lx(line);
		lx.SkipSpace();
		TUint32 cat, key;
		if (lx.Val(cat, EHex) == KErrNone && lx.Get() == '/' && lx.Val(key, EHex) == KErrNone)
			{
			Keys[NumKeys].iCat = cat;
			Keys[NumKeys].iKey = key;
			++NumKeys;
			}
		p.Set(nl < 0 ? TPtr8(NULL, 0) : p.MidTPtr(nl + 1));
		}
	delete buf;
	return NumKeys > 0;
	}

static TInt Run()
	{
	RFs fs;
	TInt r = fs.Connect();
	if (r != KErrNone)
		return r;
	// the mass memory may come up a moment after EStart starts the hook
	TVolumeInfo vi;
	for (TInt i = 0; i < 50 && fs.Volume(vi, EDriveE) != KErrNone; ++i)
		User::After(100000);
	TEntry e;
	if (fs.Entry(KLog, e) == KErrNone)
		{
		fs.Close();
		return KErrAlreadyExists;			// done before, or the previous attempt crashed: do nothing
		}
	fs.MkDirAll(KDir);
	r = Log.Create(fs, KLog, EFileWrite | EFileShareAny);
	if (r != KErrNone)
		{
		fs.Close();
		return r;
		}
	TInt tick = 0;
	HAL::Get(HAL::ENanoTickPeriod, tick);
	TickUs = tick > 0 ? tick : 1000;
	Put(_L8("start"));
	TBuf8<64> b;
	if (LoadKeys(fs))
		b.Format(_L8("keys %d keys.txt"), NumKeys);
	else
		{
		delete[] Keys;
		Keys = const_cast<SKey*>(KKeys);
		NumKeys = KNumBuiltInKeys;
		b.Format(_L8("keys %d built-in"), NumKeys);
		}
	Put(b);
	Log.Flush();

	RProperty* props = new RProperty[NumKeys];
	TRequestStatus* st = new TRequestStatus[NumKeys];
	TInt* last = new TInt[NumKeys];
	Threads = new SThread[KMaxThreads];
	Refused = new TUint32[KMaxThreads];
	if (!props || !st || !last || !Threads || !Refused)
		{
		Put(_L8("no memory"));
		Log.Close();
		fs.Close();
		return KErrNoMemory;
		}
	for (TInt i = 0; i < NumKeys; ++i)
		{
		last[i] = KMinTInt;
		props[i].Attach(TUid::Uid(Keys[i].iCat), Keys[i].iKey);
		props[i].Subscribe(st[i]);
		TInt v;
		if (props[i].Get(v) == KErrNone)
			{
			last[i] = v;
			b.Format(_L8("prop %08x/%x = %d (initial)"), Keys[i].iCat, Keys[i].iKey, v);
			Put(b);
			}
		}
	RUndertaker undertaker;
	TRequestStatus us;
	TInt dying = 0;
	TBool haveUndertaker = undertaker.Create() == KErrNone;
	if (haveUndertaker)
		undertaker.Logon(us, dying);
	else
		Put(_L8("no undertaker: deaths not recorded"));
	RTimer timer;
	timer.CreateLocal();
	TRequestStatus ts;
	timer.After(ts, KPollMs * 1000);
	TInt polls = 0, flushTick = User::NTickCount();
	const TInt end = KRunSeconds * 1000 / KPollMs;
	while (polls < end)
		{
		User::WaitForAnyRequest();
		if (haveUndertaker && us != KRequestPending)
			{
			TInt died = us.Int(), h = dying;
			undertaker.Logon(us, dying);		// again first: a death before the logon is not reported
			if (died == KErrDied)
				{
				RThread t;
				t.SetHandle(h);
				LogDeath(t);
				t.Close();
				}
			}
		if (ts != KRequestPending)
			{
			PollThreads();
			++polls;
			timer.After(ts, KPollMs * 1000);
			}
		for (TInt i = 0; i < NumKeys; ++i)
			{
			if (st[i] == KRequestPending)
				continue;
			props[i].Subscribe(st[i]);			// again first, so no change is missed
			TInt v;
			TInt e2 = props[i].Get(v);
			if (e2 == KErrNone && v != last[i])
				{
				last[i] = v;
				b.Format(_L8("prop %08x/%x = %d"), Keys[i].iCat, Keys[i].iKey, v);
				Put(b);
				}
			}
		if ((TInt)(User::NTickCount() - flushTick) * TickUs >= 1000000)
			{
			Log.Flush();
			flushTick = User::NTickCount();
			}
		}
	if (haveUndertaker)
		{
		undertaker.LogonCancel();
		User::WaitForRequest(us);
		undertaker.Close();
		}
	Put(_L8("end"));
	Log.Close();
	fs.Close();
	return KErrNone;
	}

GLDEF_C TInt E32Main()
	{
	return Run();
	}
// Startup glue called from startup.s (see src/n95apps/dsbtest/dsbtest.cpp).
struct SDaThreadCreateInfo
	{
	TAny* iHandle;
	TInt iType;
	TThreadFunction iFunction;
	TAny* iPtr;
	};
struct SStdEpocThreadCreateInfo;
IMPORT_C TInt DaSetupThreadHeap(TBool, SStdEpocThreadCreateInfo&) asm("_ZN8UserHeap15SetupThreadHeapEiR24SStdEpocThreadCreateInfo");

// TCppRTExceptionsGlobals (drtaeabi): the thread's C++ exception globals and emergency buffer, which the
// real EXE startup (CallThrdProcEntry) keeps on each thread's entry stack frame; it registers itself in
// drtaeabi's TLS. Without it the first leave in any thread of this process (also one that library code
// traps itself) dies in __cxa_allocate_exception (KERN-EXEC 3). ~0x90 bytes; room for more.
IMPORT_C void DaExceptionsGlobals(TAny* aThis) asm("_ZN23TCppRTExceptionsGlobalsC1Ev");

extern "C" void DaRunThread(TBool aNotFirst, SDaThreadCreateInfo& aInfo)
	{
	TUint64 exceptionGlobals[64];
	TInt r = DaSetupThreadHeap(aNotFirst, (SStdEpocThreadCreateInfo&)aInfo);
	if (r == KErrNone)
		{
		DaExceptionsGlobals(exceptionGlobals);		// after the heap: its Dll::SetTls allocates
		if (aNotFirst)
			r = (*aInfo.iFunction)(aInfo.iPtr);
		else
			{
			User::InitProcess();
			r = E32Main();
			}
		}
	User::Exit(r);
	}

extern "C" void DaInvariant() { User::Invariant(); }
extern "C" void DaHandleException(TAny* aInfo) { User::HandleException(aInfo); }
