// wgprobe: prints WSERV's focused window group and the window-group order (front first) to the
// serial log whenever they change, polled every 500 ms, for 10 minutes. Plain EXE, no window group
// of its own, so it does not disturb focus. Output lines are prefixed "WGPROBE:".
// Name fields (status, UID, caption, document) are separated by '|'.
#include <e32base.h>
#include <e32debug.h>
#include <w32std.h>
#ifndef WGP_BUILD
#define WGP_BUILD "dev"
#endif

static void Name(RWsSession& aWs, TInt aId, TDes& aOut)
	{
	aOut.Zero();
	if (aWs.GetWindowGroupNameFromIdentifier(aId, aOut) != KErrNone)
		aOut.Copy(_L("?"));
	for (TInt i = 0; i < aOut.Length(); i++)
		if (aOut[i] == 0)
			aOut[i] = '|';
	if (aOut.Length() > 60)
		aOut.SetLength(60);
	}

static void Run(RWsSession& aWs)
	{
	CArrayFixFlat<TInt>* list = new CArrayFixFlat<TInt>(16);
	TInt lastFocus = -1;
	TUint32 lastHash = 0;
	TBuf<64> name;
	for (TInt n = 0; n < 1200; n++)
		{
		TInt focus = aWs.GetFocusWindowGroup();
		list->Reset();
		TInt r = aWs.WindowGroupList(list);
		TUint32 hash = (TUint32)r * 2654435761u;
		for (TInt i = 0; i < list->Count(); i++)
			hash = (hash ^ (TUint32)(*list)[i]) * 16777619u;
		if (focus != lastFocus || hash != lastHash)
			{
			lastFocus = focus;
			lastHash = hash;
			Name(aWs, focus, name);
			RDebug::Print(_L("WGPROBE: focus %d %S"), focus, &name);
			RDebug::Print(_L("WGPROBE: order (%d groups, front first, r=%d)"), list->Count(), r);
			for (TInt i = 0; i < list->Count(); i++)
				{
				TInt id = (*list)[i];
				Name(aWs, id, name);
				RDebug::Print(_L("WGPROBE:  #%d id %d prio %d %S"), i, id, aWs.GetWindowGroupOrdinalPriority(id), &name);
				}
			}
		User::After(500000);
		}
	delete list;
	}

GLDEF_C TInt E32Main()
	{
	RDebug::Printf("WGPROBE: start build " WGP_BUILD);
	CTrapCleanup* cleanup = CTrapCleanup::New();
	RWsSession ws;
	while (ws.Connect() != KErrNone)
		User::After(1000000);
	Run(ws);
	ws.Close();
	delete cleanup;
	RDebug::Printf("WGPROBE: done");
	return KErrNone;
	}

// Startup glue called from startup.s (see src/n95apps/dsbtest/dsbtest.cpp).
struct SWgpThreadCreateInfo
	{
	TAny* iHandle;
	TInt iType;
	TThreadFunction iFunction;
	TAny* iPtr;
	};
struct SStdEpocThreadCreateInfo;
IMPORT_C TInt WgpSetupThreadHeap(TBool, SStdEpocThreadCreateInfo&) asm("_ZN8UserHeap15SetupThreadHeapEiR24SStdEpocThreadCreateInfo");

extern "C" void WgpRunThread(TBool aNotFirst, SWgpThreadCreateInfo& aInfo)
	{
	TInt r = WgpSetupThreadHeap(aNotFirst, (SStdEpocThreadCreateInfo&)aInfo);
	if (r == KErrNone)
		{
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

extern "C" void WgpInvariant() { User::Invariant(); }
extern "C" void WgpHandleException(TAny* aInfo) { User::HandleException(aInfo); }
