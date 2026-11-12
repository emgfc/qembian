// Hosted EKA2: the few Win32 virtual-memory calls the emulator memory model uses (memmodel/emul/win32),
// on Linux mmap/mprotect/madvise, so that its files port with minimal edits. Original work for qembian.
// Semantics kept: reserve = address space only (PROT_NONE), commit = accessible + zeroed on first use,
// decommit = pages dropped (madvise DONTNEED) and inaccessible again, release = unmap. Commit state is
// tracked per 4 KB page in a 1 MB table covering the 32-bit address space (VirtualQuery).
#ifndef __LXWIN32_H__
#define __LXWIN32_H__
#include <e32def.h>

typedef void* LPVOID;
typedef TUint32 DWORD;
typedef TUint32 SIZE_T;

enum { MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_DECOMMIT = 0x4000, MEM_RELEASE = 0x8000, MEM_FREE = 0x10000 };
enum { PAGE_NOACCESS = 0x01, PAGE_READONLY = 0x02, PAGE_READWRITE = 0x04, PAGE_EXECUTE_READWRITE = 0x40 };

struct MEMORY_BASIC_INFORMATION
	{
	LPVOID BaseAddress;
	SIZE_T RegionSize;
	DWORD State;			// MEM_COMMIT, MEM_RESERVE or MEM_FREE
	};

LPVOID VirtualAlloc(LPVOID aAddr, SIZE_T aSize, DWORD aType, DWORD aProtect);
TBool VirtualFree(LPVOID aAddr, SIZE_T aSize, DWORD aType);
SIZE_T VirtualQuery(LPVOID aAddr, MEMORY_BASIC_INFORMATION* aInfo, SIZE_T aLength);

// emulator host lock (Win32 calls are not reentrant there); nothing to do on Linux
#define __LOCK_HOST

#endif
