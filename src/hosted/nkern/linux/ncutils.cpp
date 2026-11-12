// Hosted EKA2 nanokernel, Linux platform: utilities (fast counter, context id, BTrace output, crash).
// Ported from e32/nkern/win32/ncutils.cpp and ncutilf.cpp (Copyright (c) 1998-2009 Nokia Corporation
// and/or its subsidiary(-ies), Eclipse Public License v1.0). Changes: the fast counter is
// CLOCK_MONOTONIC in microseconds; the BTrace "pc" is the caller's return address
// (__builtin_return_address) instead of the x86 stack slot; crash exits the process.

#include "nk_priv.h"

void FastCounterInit()
	{
	}

/** Return the fast counter: microseconds of CLOCK_MONOTONIC, wrapping at 32 bits (~71 minutes) */
EXPORT_C TUint32 NKern::FastCounter()
	{
	return (TUint32)(hnow_ns() / 1000);
	}

EXPORT_C TInt NKern::FastCounterFrequency()
	{
	return 1000000;
	}

void NKern::Init0(TAny*)
	{
	}

TUint32 ContextId()
	{
	switch(NKern::CurrentContext())
		{
	case NKern::EThread:
		return (TUint32)NKern::CurrentThread();
	case NKern::EIDFC:
		return 3;
	case NKern::EInterrupt:
		return 2;
	default:
		return 0;
		}
	}

EXPORT_C TBool BTrace::Out(TUint32 a0, TUint32 a1, TUint32 a2, TUint32 a3)
	{
	SBTraceData& traceData = BTraceData;
	if(!traceData.iFilter[(a0>>BTrace::ECategoryIndex*8)&0xff])
		return FALSE;

	TUint32 pc = (TUint32)__builtin_return_address(0);
	return traceData.iHandler(a0,0,0,a1,a2,a3,0,pc);
	}

EXPORT_C TBool BTrace::OutX(TUint32 a0, TUint32 a1, TUint32 a2, TUint32 a3)
	{
	SBTraceData& traceData = BTraceData;
	if(!traceData.iFilter[(a0>>BTrace::ECategoryIndex*8)&0xff])
		return FALSE;

	TUint32 context = ContextId();
	TUint32 pc = (TUint32)__builtin_return_address(0);
	return traceData.iHandler(a0,0,context,a1,a2,a3,0,pc);
	}

EXPORT_C TBool BTrace::OutN(TUint32 a0, TUint32 a1, TUint32 a2, const TAny* aData, TInt aDataSize)
	{
	SBTraceData& traceData = BTraceData;
	if(!traceData.iFilter[(a0>>BTrace::ECategoryIndex*8)&0xff])
		return FALSE;

	if(TUint(aDataSize)>KMaxBTraceDataArray)
		{
		aDataSize = KMaxBTraceDataArray;
		a0 |= BTrace::ERecordTruncated<<(BTrace::EFlagsIndex*8);
		}
	a0 += aDataSize<<(BTrace::ESizeIndex*8);

	TUint32 pc = (TUint32)__builtin_return_address(0);
	if(!aDataSize)
		return traceData.iHandler(a0,0,0,a1,a2,0,0,pc);
	else if(aDataSize<=4)
		return traceData.iHandler(a0,0,0,a1,a2,*(TUint32*)aData,0,pc);
	else
		return traceData.iHandler(a0,0,0,a1,a2,(TUint32)aData,0,pc);
	}

EXPORT_C TBool BTrace::OutNX(TUint32 a0, TUint32 a1, TUint32 a2, const TAny* aData, TInt aDataSize)
	{
	SBTraceData& traceData = BTraceData;
	if(!traceData.iFilter[(a0>>BTrace::ECategoryIndex*8)&0xff])
		return FALSE;

	if(TUint(aDataSize)>KMaxBTraceDataArray)
		{
		aDataSize = KMaxBTraceDataArray;
		a0 |= BTrace::ERecordTruncated<<(BTrace::EFlagsIndex*8);
		}
	a0 += aDataSize<<(BTrace::ESizeIndex*8);

	TUint32 context = ContextId();
	TUint32 pc = (TUint32)__builtin_return_address(0);
	if(!aDataSize)
		return traceData.iHandler(a0,0,context,a1,a2,0,0,pc);
	else if(aDataSize<=4)
		return traceData.iHandler(a0,0,context,a1,a2,*(TUint32*)aData,0,pc);
	else
		return traceData.iHandler(a0,0,context,a1,a2,(TUint32)aData,0,pc);
	}

EXPORT_C TBool BTrace::OutBig(TUint32 a0, TUint32 a1, const TAny* aData, TInt aDataSize)
	{
	TUint32 context = ContextId();
	TUint32 pc = (TUint32)__builtin_return_address(0);
	SBTraceData& traceData = BTraceData;
	if(!traceData.iFilter[(a0>>BTrace::ECategoryIndex*8)&0xff])
		return FALSE;
	return DoOutBig(a0,a1,aData,aDataSize,context,pc);
	}

EXPORT_C TBool BTrace::OutFiltered(TUint32 a0, TUint32 a1, TUint32 a2, TUint32 a3)
	{
	SBTraceData& traceData = BTraceData;
	if(!traceData.iFilter[(a0>>BTrace::ECategoryIndex*8)&0xff])
		return FALSE;
	if(!traceData.CheckFilter2(a1))
		return FALSE;

	TUint32 pc = (TUint32)__builtin_return_address(0);
	return traceData.iHandler(a0,0,0,a1,a2,a3,0,pc);
	}

EXPORT_C TBool BTrace::OutFilteredX(TUint32 a0, TUint32 a1, TUint32 a2, TUint32 a3)
	{
	SBTraceData& traceData = BTraceData;
	if(!traceData.iFilter[(a0>>BTrace::ECategoryIndex*8)&0xff])
		return FALSE;
	if(!traceData.CheckFilter2(a1))
		return FALSE;

	TUint32 context = ContextId();
	TUint32 pc = (TUint32)__builtin_return_address(0);
	return traceData.iHandler(a0,0,context,a1,a2,a3,0,pc);
	}

EXPORT_C TBool BTrace::OutFilteredN(TUint32 a0, TUint32 a1, TUint32 a2, const TAny* aData, TInt aDataSize)
	{
	SBTraceData& traceData = BTraceData;
	if(!traceData.iFilter[(a0>>BTrace::ECategoryIndex*8)&0xff])
		return FALSE;
	if(!traceData.CheckFilter2(a1))
		return FALSE;

	if(TUint(aDataSize)>KMaxBTraceDataArray)
		{
		aDataSize = KMaxBTraceDataArray;
		a0 |= BTrace::ERecordTruncated<<(BTrace::EFlagsIndex*8);
		}
	a0 += aDataSize<<(BTrace::ESizeIndex*8);

	TUint32 pc = (TUint32)__builtin_return_address(0);
	if(!aDataSize)
		return traceData.iHandler(a0,0,0,a1,a2,0,0,pc);
	else if(aDataSize<=4)
		return traceData.iHandler(a0,0,0,a1,a2,*(TUint32*)aData,0,pc);
	else
		return traceData.iHandler(a0,0,0,a1,a2,(TUint32)aData,0,pc);
	}

EXPORT_C TBool BTrace::OutFilteredNX(TUint32 a0, TUint32 a1, TUint32 a2, const TAny* aData, TInt aDataSize)
	{
	SBTraceData& traceData = BTraceData;
	if(!traceData.iFilter[(a0>>BTrace::ECategoryIndex*8)&0xff])
		return FALSE;
	if(!traceData.CheckFilter2(a1))
		return FALSE;

	if(TUint(aDataSize)>KMaxBTraceDataArray)
		{
		aDataSize = KMaxBTraceDataArray;
		a0 |= BTrace::ERecordTruncated<<(BTrace::EFlagsIndex*8);
		}
	a0 += aDataSize<<(BTrace::ESizeIndex*8);

	TUint32 context = ContextId();
	TUint32 pc = (TUint32)__builtin_return_address(0);
	if(!aDataSize)
		return traceData.iHandler(a0,0,context,a1,a2,0,0,pc);
	else if(aDataSize<=4)
		return traceData.iHandler(a0,0,context,a1,a2,*(TUint32*)aData,0,pc);
	else
		return traceData.iHandler(a0,0,context,a1,a2,(TUint32)aData,0,pc);
	}

EXPORT_C TBool BTrace::OutFilteredBig(TUint32 a0, TUint32 a1, const TAny* aData, TInt aDataSize)
	{
	TUint32 context = ContextId();
	TUint32 pc = (TUint32)__builtin_return_address(0);
	SBTraceData& traceData = BTraceData;
	if(!traceData.iFilter[(a0>>BTrace::ECategoryIndex*8)&0xff])
		return FALSE;
	if(!traceData.CheckFilter2(a1))
		return FALSE;
	return DoOutBig(a0,a1,aData,aDataSize,context,pc);
	}


EXPORT_C TBool BTrace::OutFilteredPcFormatBig(TUint32 /*aHeader*/, TUint32 /*aModuleUid*/, TUint32 /*aPc*/, TUint16 /*aFormatId*/, const TAny* /*aData*/, TInt /*aDataSize*/)
	{
	return FALSE; //kernel side not implemented yet
	}

TInt BTraceDefaultControl(BTrace::TControl /*aFunction*/, TAny* /*aArg1*/, TAny* /*aArg2*/)
	{
	return KErrNotSupported;
	}


EXPORT_C void BTrace::SetHandlers(BTrace::THandler aNewHandler, BTrace::TControlFunction aNewControl, BTrace::THandler& aOldHandler, BTrace::TControlFunction& aOldControl)
	{
	TUint irq = NKern::DisableAllInterrupts();

	aOldHandler = BTraceData.iHandler;
	BTraceData.iHandler = aNewHandler;
	TheScheduler.iBTraceHandler = aNewHandler;

	aOldControl = BTraceData.iControl;
	BTraceData.iControl = aNewControl ? aNewControl : BTraceDefaultControl;

	NKern::RestoreInterrupts(irq);
	}


EXPORT_C TInt BTrace::SetFilter(TUint aCategory, TInt aValue)
	{
	if(!IsSupported(aCategory))
		return KErrNotSupported;
	TUint8* filter = BTraceData.iFilter+aCategory;
	TUint oldValue = *filter;
	if(TUint(aValue)<=1u)
		{
		*filter = (TUint8)aValue;
		BTraceContext4(BTrace::EMetaTrace, BTrace::EMetaTraceFilterChange, (TUint8)aCategory | (aValue<<8));
		if(aCategory==ECpuUsage)
			{
			TheScheduler.iCpuUsageFilter = (TUint8)aValue;
			}
		}
	return oldValue;
	}

EXPORT_C SCpuIdleHandler* NKern::CpuIdleHandler()
	{
	return 0;
	}

EXPORT_C TUint32 NKern::CpuTimeMeasFreq()
	{
	return 0;
	}

/** Called to indicate that the system has crashed: dump and leave the process */
void NKern::NotifyCrash(const TAny* a0, TInt a1)
	{
	NKern::DisableAllInterrupts();
	CrashState = 1;
	NKCrashHandler(0,0,0);
	NKCrashHandler(1,a0,a1);
	hexit(101);
	}
