// chprobe: open a sensor channel through the real client API (CSensrvChannel) and listen 5 s
#include <e32base.h>
#include <e32debug.h>
#include <sensrvchannelfinder.h>
#include <sensrvchannel.h>
#include <sensrvchannelinfo.h>
#include <sensrvdatalistener.h>
class CStop : public CTimer { public: CStop() : CTimer(0) { CActiveScheduler::Add(this); } void Go(TInt us) { ConstructL(); After(us); } void RunL() { CActiveScheduler::Stop(); } };
class TL : public MSensrvDataListener { public: TInt iN, iErr; TL() : iN(0), iErr(0) {}
  void DataReceived(CSensrvChannel& c, TInt aCount, TInt aLost) { iN += aCount; TBuf8<64> d; c.GetData(d);
    if (iN <= 3 || iN % 25 == 0) { RDebug::Printf("CHPROBE: type %08x data count=%d lost=%d len=%d first words %08x %08x %08x %08x %08x", c.GetChannelInfo().iChannelType, aCount, aLost, d.Length(),
      d.Length() >= 4 ? *(TUint32*)d.Ptr() : 0, d.Length() >= 8 ? ((TUint32*)d.Ptr())[1] : 0, d.Length() >= 12 ? ((TUint32*)d.Ptr())[2] : 0, d.Length() >= 16 ? ((TUint32*)d.Ptr())[3] : 0, d.Length() >= 20 ? ((TUint32*)d.Ptr())[4] : 0); } }
  void DataError(CSensrvChannel&, TSensrvErrorSeverity e) { iErr = e; RDebug::Printf("CHPROBE: data error %d", e); }
  void GetDataListenerInterfaceL(TUid, TAny*& a) { a = NULL; } };
static void OneL(TUint32 aType)
	{
	CSensrvChannelFinder* f = CSensrvChannelFinder::NewLC();
	RSensrvChannelInfoList list; CleanupClosePushL(list);
	TSensrvChannelInfo q; q.iChannelType = aType;
	f->FindChannelsL(list, q);
	RDebug::Printf("CHPROBE: type %08x: %d channel(s)", aType, list.Count());
	if (list.Count())
		{
		CSensrvChannel* ch = CSensrvChannel::NewLC(list[0]);
		TRAPD(r, ch->OpenChannelL());
		RDebug::Printf("CHPROBE: type %08x open %d", aType, r);
		if (!r)
			{
			TL l;
			TRAP(r, ch->StartDataListeningL(&l, 1, 1, 0));
			RDebug::Printf("CHPROBE: type %08x start listening %d", aType, r);
			CStop* s = new (ELeave) CStop; s->Go(5000000); CActiveScheduler::Start(); delete s;
			RDebug::Printf("CHPROBE: type %08x received %d samples in 5 s, error %d", aType, l.iN, l.iErr);
			ch->StopDataListening(); ch->CloseChannel();
			}
		CleanupStack::PopAndDestroy(ch);
		}
	CleanupStack::PopAndDestroy(2, f);
	}
GLDEF_C TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	CActiveScheduler* sch = new CActiveScheduler; CActiveScheduler::Install(sch);
	TRAPD(r, OneL(0x10205088));	// orientation
	RDebug::Printf("CHPROBE: orientation done %d", r);
	TRAP(r, OneL(0x1020507e));	// accelerometer XYZ
	RDebug::Printf("CHPROBE: xyz done %d", r);
	delete sch; delete cleanup; return 0;
	}
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
