// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// accelerometer.pdd stand-in: accssy.dll loads "accelerometer.pdd" and names it in the channel's DoCreate,
// so the kernel needs a physical device of that name (Validate, Create). The work is the LDD's
// (accelerometer.cpp); the PDD object is empty.
#include <kernel/kernel.h>

class DAccPddObject : public DBase
	{
	};

class DAccPdd : public DPhysicalDevice
	{
public:
	DAccPdd() { iVersion = TVersion(2, 9, 0xffff); }
	virtual TInt Install() { _LIT(KName, "accelerometer.pdd"); return SetName(&KName); }
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DBase*& aChannel, TInt, const TDesC8*, const TVersion&)
		{ aChannel = new DAccPddObject; return aChannel ? KErrNone : KErrNoMemory; }
	virtual TInt Validate(TInt aUnit, const TDesC8*, const TVersion& aVer)
		{ return aUnit == 0 && aVer.iMajor == 2 ? KErrNone : KErrNotSupported; }
	};

DECLARE_STANDARD_PDD()
	{
	return new DAccPdd;
	}
