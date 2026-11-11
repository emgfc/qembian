// boothook: the sydb agent's boot hook (docs/agent-autostart.md).
// Nokia's EStart starts a few programs by bare name early in boot, and the loader searches C: before Z:.
// `sydb install` puts this exe on C: under the name of one of them, read from the firmware's EStart:
// HALSettings.exe (9.2 and later, "HALSettings INITIALISE") or LanguageSelection.exe (9.1). It starts the
// agent (once: HALSettings also runs again later, "PERSIST"), then runs the real z:\sys\bin\<own name>
// with the same command line and returns its exit value, so the caller sees what it always sees.
// The real HALSettings accepts INITIALISE only from a process whose SID is EStart's (0x10272C04), so this
// exe carries that SID (build.sh); it is the creator of the real one.
#include <e32base.h>
#include <e32debug.h>

static TBool AgentRunning()
	{
	TFullName name;
	TFindProcess fp(_L("devagent*"));
	while (fp.Next(name) == KErrNone)
		{
		RProcess p;
		if (p.Open(fp) != KErrNone)
			continue;
		TBool alive = p.ExitType() == EExitPending;
		p.Close();
		if (alive)
			return ETrue;
		}
	return EFalse;
	}

GLDEF_C TInt E32Main()
	{
	TInt ra = KErrAlreadyExists;
	if (!AgentRunning())
		{
		RProcess agent;
		ra = agent.Create(_L("c:\\sys\\bin\\devagent.exe"), KNullDesC);
		if (ra == KErrNone)
			{
			agent.Resume();
			agent.Close();
			}
		}
	// the real-phone recorder (docs/real-phone-tracing.md): present only where it was installed; it
	// decides itself whether to run (its output file must not exist yet)
	RProcess oracle;
	if (oracle.Create(_L("c:\\sys\\bin\\oracle.exe"), KNullDesC) == KErrNone)
		{
		oracle.Resume();
		oracle.Close();
		}
	TFileName self = RProcess().FileName();
	TFileName real(_L("z:\\sys\\bin\\"));
	real.Append(self.Mid(self.LocateReverse('\\') + 1));	// own name and extension
	HBufC* cl = HBufC::New(User::CommandLineLength());
	TPtr clp(cl ? cl->Des() : TPtr(NULL, 0));
	if (cl)
		User::CommandLine(clp);
	RProcess proc;
	TInt r = proc.Create(real, clp);
	RDebug::Print(_L("BOOTHOOK: agent %d, %S %d"), ra, &real, r);
	delete cl;
	if (r != KErrNone)
		return r;
	TRequestStatus st;
	proc.Logon(st);
	proc.Resume();
	User::WaitForRequest(st);
	r = proc.ExitType() == EExitPanic ? KErrGeneral : st.Int();
	proc.Close();
	RDebug::Printf("BOOTHOOK: exit %d", r);
	return r;
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
