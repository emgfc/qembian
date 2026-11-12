// Hosted EKA2: the platform part of Symbian's standalone nanokernel test suite
// (kerneltest/e32test/nkernsa), in place of its arm/armutils.cpp + armutils.cia.
// Fast counter helpers follow arm/armutils.cpp (Copyright (c) 2006-2009 Nokia Corporation and/or its
// subsidiary(-ies), Eclipse Public License v1.0); the rest is ours: the heap is nkhost.cpp's (the suite's
// TLSF setup reads a ROM header and super page that a hosted process does not have), the system timer is
// nkhost.cpp's interrupt thread, output goes to fd 1.
#include <nktest/nkutils.h>

extern void HostedStartSystemTimer(TInt aPeriodUs);
extern "C" void KernelMain();

const TUint32 KPageSize = 0x1000u;
static const TInt KTickPeriodUs = 1000;

extern "C" {
TUint64 fcf;
TUint32 nfcf;
TUint32 nfcfs;

TUint32 round_to_page(TUint32 x) { return (x + KPageSize - 1) & ~(KPageSize - 1); }

TLinAddr __initial_stack_base()
	{
	TUint32 sp;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	return sp & ~0xfff;
	}
TInt __initial_stack_size() { return 0x1000; }

TInt __timer_period() { return KTickPeriodUs; }
TInt __microseconds_to_timeslice_ticks(TInt us) { return (us + KTickPeriodUs - 1) / KTickPeriodUs; }

// the fast counter: CLOCK_MONOTONIC in microseconds, 64 bits
TUint64 fast_counter() { return hnow_ns() / 1000; }
TInt __fast_counter_to_timeslice_ticks(TUint64 aFCdelta) { return (TInt)(aFCdelta / KTickPeriodUs); }
TUint32 norm_fast_counter() { return (TUint32)(fast_counter() >> nfcfs); }
TUint64 fast_counter_freq() { return fcf; }
TUint32 norm_fast_counter_freq() { return nfcf; }

void init_fast_counter()
	{
	NKern::Sleep(30);
	TUint64 initial = fast_counter();
	NKern::Sleep(1000);
	TUint64 final = fast_counter();
	fcf = final - initial;
	TUint64 f = fcf;
	nfcfs = 0;
	while (f > 2000000)
		f >>= 1, ++nfcfs;
	nfcf = (TUint32)(fcf >> nfcfs);
	DEBUGPRINT("fcf=%lx", fcf);
	DEBUGPRINT("nfcf=%d", nfcf);
	}

TInt __microseconds_to_fast_counter(TInt us)
	{
	TUint64 x = TUint64(TUint32(us));
	x *= fcf;
	x += TUint64(500000);
	x /= TUint64(1000000);
	return (TInt)x;
	}

TInt __microseconds_to_norm_fast_counter(TInt us)
	{
	TUint64 x = TUint64(TUint32(us));
	x *= nfcf;
	x += TUint64(500000);
	x /= TUint64(1000000);
	return (TInt)x;
	}

void fcfspin(TUint64 aTicks)
	{
	TUint64 t0 = fast_counter();
	while (fast_counter() - t0 < aTicks) {}
	}

void nfcfspin(TUint32 aTicks)
	{
	fcfspin(TUint64(aTicks) << nfcfs);
	}

TUint32 __cpu_id() { return 0; }
TUint32 __trace_cpu_num() { return 0; }

void CheckPoint()
	{
	KPrintf("CHECKPOINT");
	}

void DebugPrint(const char* aPtr, int aLen)
	{
	hwrite(1, aPtr, aLen);
	}

void SetupMemoryAllocator()
	{
	}

void Hw_Init1()
	{
	}

void Hw_Init3()
	{
	HostedStartSystemTimer(KTickPeriodUs);
	}

void __finish()
	{
	KPrintf("NKERNSA: all tests completed");
	hexit(0);
	}
}

// Complete a request: atomically take the status pointer, store the value, signal the thread
void RequestComplete(NThread* aThread, NRequestStatus*& aStatus, TInt aValue)
	{
	NRequestStatus* s = __atomic_exchange_n(&aStatus, (NRequestStatus*)0, __ATOMIC_SEQ_CST);
	if (s)
		{
		*s = aValue;
		NKern::ThreadRequestSignal(aThread);
		}
	}

// The suite reads its trace mask from the ROM header (kprintf.cpp KDebugNum): a zeroed stand-in header
// (no trace bits) until the hosted kernel has a composed ROM to point at.
static TUint32 FakeRomHeader[1024] __attribute__((aligned(16)));
extern "C" TLinAddr RomHeaderAddress;

int main(int, char**)
	{
	RomHeaderAddress = (TLinAddr)FakeRomHeader;
	LxInit();		// host thread record and the fault/suspend signal handlers, before anything can crash
	KernelMain();
	return 0;
	}
