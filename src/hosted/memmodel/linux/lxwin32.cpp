// Hosted EKA2: Win32 virtual-memory calls on Linux (see lxwin32.h). Original work for qembian.
#include <hrt.h>
#include "lxwin32.h"

static const TUint32 KPageShift = 12;
static TUint8 PageState[1u << (32 - KPageShift)];		// 0 free, 1 reserved, 2 committed (1 MB)
static TUint32 ReservedSize[1u << (32 - KPageShift)] __attribute__((section(".bss.lxwin32")));
static volatile TInt Lock;

static void Take() { while (__atomic_exchange_n(&Lock, 1, __ATOMIC_ACQUIRE)) hsys(SYS_sched_yield); }
static void Give() { __atomic_store_n(&Lock, 0, __ATOMIC_RELEASE); }

static void SetState(TUint32 aBase, TUint32 aSize, TUint8 aState)
	{
	for (TUint32 p = aBase >> KPageShift, e = (aBase + aSize + 0xfff) >> KPageShift; p < e; ++p)
		PageState[p] = aState;
	}

static int Prot(DWORD aProtect)
	{
	switch (aProtect)
		{
	case PAGE_NOACCESS: return 0;
	case PAGE_READONLY: return PROT_READ;
	case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
	default: return PROT_READ | PROT_WRITE;
		}
	}

LPVOID VirtualAlloc(LPVOID aAddr, SIZE_T aSize, DWORD aType, DWORD aProtect)
	{
	if (!aSize)
		return 0;
	TUint32 size = (aSize + 0xfff) & ~0xfffu;
	Take();
	LPVOID r = 0;
	if (aType & MEM_RESERVE)
		{
		TInt flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (aAddr ? MAP_FIXED_NOREPLACE : 0);
		void* m = hmmap(aAddr, size, (aType & MEM_COMMIT) ? Prot(aProtect) : 0, flags, -1, 0);
		if (!HMAP_FAILED(m) && (!aAddr || m == aAddr))
			{
			SetState((TUint32)m, size, (aType & MEM_COMMIT) ? 2 : 1);
			ReservedSize[(TUint32)m >> KPageShift] = size;
			r = m;
			}
		}
	else if (aType & MEM_COMMIT)
		{
		TUint32 base = (TUint32)aAddr & ~0xfffu;
		size = ((TUint32)aAddr + aSize + 0xfff - base) & ~0xfffu;
		TBool ok = 1;
		for (TUint32 p = base >> KPageShift, e = (base + size) >> KPageShift; p < e; ++p)
			if (!PageState[p]) ok = 0;			// must be reserved first
		if (ok && hsys(SYS_mprotect, base, size, Prot(aProtect)) == 0)
			{
			SetState(base, size, 2);
			r = aAddr;
			}
		}
	Give();
	return r;
	}

TBool VirtualFree(LPVOID aAddr, SIZE_T aSize, DWORD aType)
	{
	TUint32 base = (TUint32)aAddr & ~0xfffu;
	Take();
	TBool ok = 0;
	if (aType & MEM_DECOMMIT)
		{
		TUint32 size = ((TUint32)aAddr + aSize + 0xfff - base) & ~0xfffu;
		hsys(220 /* madvise */, base, size, 4 /* MADV_DONTNEED: zero on next commit */);
		ok = hsys(SYS_mprotect, base, size, 0) == 0;
		SetState(base, size, 1);
		}
	else if (aType & MEM_RELEASE)
		{
		TUint32 size = ReservedSize[base >> KPageShift];
		if (size && hmunmap((void*)base, size) == 0)
			{
			SetState(base, size, 0);
			ReservedSize[base >> KPageShift] = 0;
			ok = 1;
			}
		}
	Give();
	return ok;
	}

SIZE_T VirtualQuery(LPVOID aAddr, MEMORY_BASIC_INFORMATION* aInfo, SIZE_T)
	{
	TUint32 p = (TUint32)aAddr >> KPageShift, e = p;
	TUint8 s = PageState[p];
	while (e < (1u << (32 - KPageShift)) && PageState[e] == s)
		++e;
	aInfo->BaseAddress = (LPVOID)(p << KPageShift);
	aInfo->RegionSize = (e - p) << KPageShift;
	aInfo->State = s == 2 ? MEM_COMMIT : s == 1 ? MEM_RESERVE : MEM_FREE;
	return sizeof(*aInfo);
	}
