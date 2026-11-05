// Copyright (c) 2026 syborg-pg contributors. Licensed under the Eclipse Public License v1.0.
//
// Table-driven stand-ins for Nokia N95 logical devices (kernel side). Each LDD declares
//   - the factory name its user-side clients open (RBusLogicalChannel::DoCreate)
//   - a table of DoControl() numbers with an action:
//       EWriteInt   *(TInt*)a1 = value (a1 points at a TInt or a TPckgBuf<TInt> payload)
//       EComplete   complete the TRequestStatus* found in the first word at a1 with value
//       EReturn     return value
//     anything not in the table returns KErrNone and touches nothing
// Every channel open and control is logged on the debug port ("N95STUB:"), which is the
// specification for proper fakes later.

#include <kernel/kernel.h>

enum TStubAction { EWriteInt, EComplete, EReturn, EWriteBlock, EPendRequest };
// EPendRequest: table entry { KStubRequest|n, EPendRequest } keeps DoRequest(n) pending even when
// the device completes requests by default (e.g. 'wait for next job' notifications);
// { KStubRequest|n, EComplete, value } completes DoRequest(n) with that value
const TInt KStubRequest = 0x10000;
// EWriteBlock: copy iSize bytes from iData to a1 (e.g. a capabilities structure)
struct SStubCtl { TInt iFn; TStubAction iAction; TInt iValue; const TAny* iData; TInt iSize; };
// optional per-device request hook: return KErrNone..KErrXxx to complete the request with it, or
// KStubNotHandled to fall back to the table/default. aArgs = the two DoRequest argument words.
const TInt KStubNotHandled = 0x7fff0000;
typedef TInt (*TStubRequestHook)(DThread* aClient, TInt aReq, const TUint32* aArgs);

class DNokiaStubFactory : public DLogicalDevice
	{
public:
	DNokiaStubFactory(const TDesC& aName, const SStubCtl* aCtl, TInt aCount, TBool aChannels, TBool aCompleteRequests,
					  TStubRequestHook aHook = NULL)
		: iName(aName), iCtl(aCtl), iCount(aCount), iChannels(aChannels), iCompleteRequests(aCompleteRequests), iHook(aHook)
		{
		iVersion = TVersion(255, 255, 0xffff);	// accept any client version
		iParseMask = KDeviceAllowUnit | KDeviceAllowInfo;
		iUnitsMask = 0xffffffffu;
		}
	virtual TInt Install()
		{
		Kern::Printf("N95STUB: install %S%s", &iName, iChannels ? "" : " (load only)");
		return SetName(&iName);
		}
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DLogicalChannelBase*& aChannel);
public:
	const TDesC& iName;
	const SStubCtl* iCtl;
	TInt iCount;
	TBool iChannels;
	TBool iCompleteRequests;	// DoRequest(): complete at once with KErrNone, else leave pending
	TStubRequestHook iHook;
	};

class DNokiaStubChannel : public DLogicalChannelBase
	{
public:
	DNokiaStubChannel(DNokiaStubFactory* aF) : iF(aF) {}
	virtual TInt DoCreate(TInt aUnit, const TDesC8*, const TVersion& aVer)
		{
		iUnit = aUnit;
		Kern::Printf("N95STUB: %S open unit %d ver %d.%d.%d by %O", &iF->iName, aUnit,
					 aVer.iMajor, aVer.iMinor, aVer.iBuild, &Kern::CurrentThread());
		return KErrNone;
		}
	virtual TInt Request(TInt aFn, TAny* a1, TAny* a2)
		{
		if (aFn < 0 && aFn != (TInt)KMaxTInt)
			{
			// DoRequest(n, status, args): aFn = ~n, a1 = TRequestStatus*, a2 -> { arg1, arg2 }
			TRequestStatus* s = (TRequestStatus*)a1;
			TUint32 args[2] = {0, 0};
			if (a2)
				Kern::ThreadRawRead(&Kern::CurrentThread(), a2, args, sizeof(args));
			TBool pend = !iF->iCompleteRequests;
			TInt result = KErrNone;
			TInt hr = iF->iHook ? iF->iHook(&Kern::CurrentThread(), ~aFn, args) : KStubNotHandled;
			for (TInt i=0; i<iF->iCount; ++i)
				if (iF->iCtl[i].iFn == (KStubRequest | ~aFn))
					{
					if (iF->iCtl[i].iAction == EPendRequest)
						pend = ETrue;
					else if (iF->iCtl[i].iAction == EComplete)
						{ pend = EFalse; result = iF->iCtl[i].iValue; }
					}
			if (hr != KStubNotHandled)
				{ pend = EFalse; result = hr; }
			Kern::Printf("N95STUB: %S u%d request 0x%x args %08x %08x %s %d by %O", &iF->iName, iUnit, ~aFn,
						 args[0], args[1], pend ? "pending" : "completed", result, &Kern::CurrentThread());
			if (!pend && s)
				Kern::RequestComplete(s, result);
			return KErrNone;
			}
		if (aFn == (TInt)KMaxTInt)
			{
			Kern::Printf("N95STUB: %S u%d cancel mask %08x", &iF->iName, iUnit, a1);
			return KErrNone;
			}
		TUint32 w[4] = {0, 0, 0, 0};
		TInt rr = KErrArgument;
		if (((TLinAddr)a1 & 3) == 0 && (TLinAddr)a1 >= 0x1000)
			rr = Kern::ThreadRawRead(&Kern::CurrentThread(), a1, w, sizeof(w));
		const SStubCtl* c = NULL;
		for (TInt i=0; i<iF->iCount; ++i)
			if (iF->iCtl[i].iFn == aFn)
				c = &iF->iCtl[i];
		TInt r = KErrNone;
		const char* what = "ignored";
		if (c && c->iAction == EWriteInt && rr == KErrNone)
			{
			Kern::ThreadRawWrite(&Kern::CurrentThread(), a1, &c->iValue, sizeof(TInt));
			what = "wrote int";
			}
		else if (c && c->iAction == EComplete && rr == KErrNone && w[0])
			{
			TRequestStatus* s = (TRequestStatus*)w[0];
			Kern::RequestComplete(s, c->iValue);
			what = "completed";
			}
		else if (c && c->iAction == EWriteBlock && rr == KErrNone)
			{
			Kern::ThreadRawWrite(&Kern::CurrentThread(), a1, c->iData, c->iSize);
			what = "wrote block";
			}
		else if (c && c->iAction == EReturn)
			{
			r = c->iValue;
			what = "returned";
			}
		Kern::Printf("N95STUB: %S u%d ctl %d a1=%08x a2=%08x [%08x %08x %08x %08x] %s %d",
					 &iF->iName, iUnit, aFn, a1, a2, w[0], w[1], w[2], w[3], what, c ? c->iValue : 0);
		return r;
		}
private:
	DNokiaStubFactory* iF;
	TInt iUnit;
	};

inline TInt DNokiaStubFactory::Create(DLogicalChannelBase*& aChannel)
	{
	if (!iChannels)
		{
		aChannel = NULL;
		return KErrNotSupported;
		}
	aChannel = new DNokiaStubChannel(this);
	return aChannel ? KErrNone : KErrNoMemory;
	}

#define NOKIA_STUB_LDD2(name, table, channels, completeRequests) \
	_LIT(KStubName, name); \
	DECLARE_STANDARD_LDD() \
		{ return new DNokiaStubFactory(KStubName, table, sizeof(table)/sizeof(table[0]), channels, completeRequests); }
#define NOKIA_STUB_LDD(name, table, channels) NOKIA_STUB_LDD2(name, table, channels, EFalse)
#define NOKIA_STUB_LDD_HOOK(name, table, hook) \
	_LIT(KStubName, name); \
	DECLARE_STANDARD_LDD() \
		{ return new DNokiaStubFactory(KStubName, table, sizeof(table)/sizeof(table[0]), ETrue, ETrue, hook); }

#define NO_CONTROLS static const SStubCtl KCtl[] = { { -1, EReturn, 0, 0, 0 } };
