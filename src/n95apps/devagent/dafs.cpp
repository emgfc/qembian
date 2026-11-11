// dafs: TCB helper of the sydb agent. ALL capabilities, links only EUSER/EFSRV (the only ROM DLLs
// that carry TCB, so the loader accepts it; devagent.exe itself must be All-TCB because it links
// WSERV/apparc/avkon).
//   dafs mv SRC DST     move/replace SRC to DST (creating DST's directory); for \sys\, \resource\
//   dafs rm PATH        delete
//   dafs upgrade PIDHEX SRC   wait for process PID to exit, move SRC over c:\sys\bin\devagent.exe,
//                             start the new agent (agent self-update, sydb update-agent)
// Exit reason = Symbian error code of the operation.
#include <e32base.h>
#include <e32debug.h>
#include <f32file.h>

static TInt Main()
	{
	TBuf<512> cl;
	User::CommandLine(cl);
	TLex lex(cl);
	TPtrC op = lex.NextToken();
	TPtrC a1 = lex.NextToken();
	TPtrC a2 = lex.NextToken();
	RFs fs;
	TInt r = fs.Connect();
	if (r != KErrNone)
		return r;
	if (op == _L("mv") && a1.Length() && a2.Length())
		{
		fs.MkDirAll(a2);
		fs.Delete(a2);
		r = fs.Rename(a1, a2);
		}
	else if (op == _L("rm") && a1.Length())
		r = fs.Delete(a1);
	else if (op == _L("upgrade") && a1.Length() && a2.Length())
		{
		TUint32 pid = 0;
		TLex l(a1);
		l.Val(pid, EHex);
		RProcess old;
		if (old.Open(TProcessId(pid)) == KErrNone)
			{
			TRequestStatus st;
			old.Logon(st);
			if (st == KRequestPending)
				User::WaitForRequest(st);
			old.Close();
			}
		// sydb stamps a new module version into every deployed EXE: the loader would otherwise
		// reuse the dead agent's code segment (it matches name/UIDs/version, not the file)
		for (TInt i = 0; i < 50; i++)
			{
			RProcess gone;
			if (gone.Open(TProcessId(pid)) != KErrNone)
				break;
			gone.Close();
			User::After(100000);
			}
		_LIT(KAgent, "c:\\sys\\bin\\devagent.exe");
		fs.Delete(KAgent);
		r = fs.Rename(a2, KAgent);
		RProcess a;
		TInt r2 = a.Create(KAgent, KNullDesC);
		if (r2 == KErrNone)
			{
			a.Resume();
			a.Close();
			}
		RDebug::Printf("DAFS: upgrade: rename %d, start %d", r, r2);
		if (r == KErrNone) r = r2;
		}
	else
		r = KErrArgument;
	fs.Close();
	return r;
	}

GLDEF_C TInt E32Main()
	{
	return Main();
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
