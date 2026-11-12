// Hosted EKA2 port of memmodel/emul/win32/minit.cpp (EPL v1.0, Nokia; header above): class names kept,
// Win32 virtual memory calls through lxwin32.h (mmap/mprotect). Other changes are marked "hosted:".
// Copyright (c) 1996-2009 Nokia Corporation and/or its subsidiary(-ies).
// All rights reserved.
// This component and the accompanying materials are made available
// under the terms of the License "Eclipse Public License v1.0"
// which accompanies this distribution, and is available
// at the URL "http://www.eclipse.org/legal/epl-v10.html".
//
// Initial Contributors:
// Nokia Corporation - initial contribution.
//
// Contributors:
//
// Description:
// e32\memmodel\emul\win32\minit.cpp
// 
//

#include "memmodel.h"
#include "lxwin32.h"
#include <e32rom.h>

const TUint KHostedRamSize = 0x08000000;			// 128 MB
const TUint KHostedRamDriveMaxSize = 0x02000000;	// 32 MB
_LIT8(KKernelFullPathName,"EKern.exe");
_LIT8(KLitRamAlloc,"RamAlloc");

void M::Init1()
	{
	__KTRACE_OPT2(KBOOT,KMMU,Kern::Printf("M::Init1"));
	
	// hosted: the phone's RAM sizes are a platform constant (the N95: 128 MB, RAM drive up to 32 MB);
	// the Win32 emulator asked its variant (Asic::AddressInfo, which the ARM Asic does not have)
	struct { TUint iTotalRamSize, iRamDriveMaxSize; } ainfo = { KHostedRamSize, KHostedRamDriveMaxSize };

	MM::RamChunkSize = KRamChunkSize;
	MM::RamChunkShift = KRamChunkShift;
	MM::RamPageSize = KRamPageSize;
	MM::RamPageShift = KRamPageShift;
	MM::FreeMemory = ainfo.iTotalRamSize;
	TheSuperPage().iTotalRamSize = ainfo.iTotalRamSize;
	K::MaxFreeRam = ainfo.iTotalRamSize;
	PP::RamDriveMaxSize = ainfo.iRamDriveMaxSize;

	K::SupervisorThreadStackSize=0x1000;	// 4K

	K::MachineConfig=(TMachineConfig*)A::MachineConfiguration().Ptr();

	K::MaxMemCopyInOneGo=KDefaultMaxMemCopyInOneGo;
	__KTRACE_OPT(KBOOT,Kern::Printf("K::MaxMemCopyInOneGo=0x%x",K::MaxMemCopyInOneGo));

	LPVOID kheap = VirtualAlloc(NULL, KKernelHeapSizeMax, MEM_RESERVE, PAGE_READWRITE);
	__ASSERT_ALWAYS(kheap, MM::Panic(MM::EKernelHeapReserveFailed));
	kheap = VirtualAlloc(kheap, KKernelHeapSizeMin, MEM_COMMIT, PAGE_READWRITE);
	__ASSERT_ALWAYS(kheap, MM::Panic(MM::EKernelHeapCommitFailed));
	MM::KernelHeapAddress = kheap;
	K::MemModelAttributes = (TUint32)(EMemModelTypeEmul|EMemModelAttrNonExProt|EMemModelAttrWriteProt|EMemModelAttrVA);
	
	MM::Init1();
	}

void M::Init2()
	{
	__KTRACE_OPT2(KBOOT,KMMU,Kern::Printf("M::Init2"));
	TInt r=K::MutexCreate((DMutex*&)MM::RamAllocatorMutex, KLitRamAlloc, NULL, EFalse, KMutexOrdRamAlloc);
	__ASSERT_ALWAYS(r==KErrNone, MM::Panic(MM::ERamAllocMutexCreateFailed));
	}

void M::Init3()
	{
	__KTRACE_OPT2(KBOOT,KMMU,Kern::Printf("M::Init3"));
	}

void M::Init4()
    {
    // Fourth phase MMU initialisation - Not required on this memory model.
    }

// kernel heap construction

void P::KernelInfo(TProcessCreateInfo& aInfo, TAny*& aStack, TAny*& aHeap)
	{
	memclr(&aInfo.iUids, sizeof(TProcessCreateInfo)-sizeof(aInfo.iFileName));
	aInfo.iFileName=KKernelFullPathName;
	aInfo.iRootNameOffset=0;
	aInfo.iRootNameLength=9;
	aInfo.iExtOffset=5;
	aInfo.iAttr=ECodeSegAttKernel|ECodeSegAttFixed;

	aStack=NULL;
	aHeap=(TAny*)MM::KernelHeapAddress;

	// hosted: no PE image of the kernel to read its UIDs and sizes from (the Win32 emulator did): EKERN's
	// UIDs (as in every EKA2 ROM: exe, kernel, ekern) and the fixed sizes below. Without the UIDs
	// DCodeSeg::Create refuses the kernel's code segment (KErrNotSupported -> KERN 50 at boot).
	aInfo.iUids.iUid[0] = TUid::Uid(KExecutableImageUidValue);
	aInfo.iUids.iUid[1] = TUid::Uid(0x1000008b);
	aInfo.iUids.iUid[2] = TUid::Uid(0x100041af);
	aInfo.iS.iSecureId = 0x100041af;
	aInfo.iS.iVendorId = 0x70000001;
	// the kernel has every *defined* capability (the N95 ROM's EKERN: 0x000fffff). Bug 2026-11-12: all
	// ones (undefined bits too) made every LDD fail the loader's check (ELOCD.LDD: "capabilities are
	// missing:" with an empty list), Fserv fault 33. Regression: kn_efile.
	memclr(&aInfo.iS.iCaps, sizeof(aInfo.iS.iCaps));
	((TUint32*)&aInfo.iS.iCaps)[0] = (1u << ECapability_Limit) - 1;
	// With a ROM: the kernel's identity is the ROM's primary image's (as memmodel/epoc/putils.cpp
	// P::KernelInfo): name z:\sys\bin\ekern.exe, UIDs, version, export directory and dependency count,
	// which the loader checks every kernel-side module's imports against. Bug 2026-11-12: a synthetic
	// "EKern.exe" without exports made the loader refuse ELOCD.LDD (import check: KErrNotSupported,
	// Fserv fault 33). Memory stays ours: kernel heap from the platform, no ARM kernel data.
	const TRomEntry* pe = (const TRomEntry*)TheSuperPage().iPrimaryEntry;
	if (pe)
		{
		_LIT(KKernelSysBin, "z:\\sys\\bin\\ekern.exe");
		Epoc::RomProcessInfo(aInfo, *(const TRomImageHeader*)pe->iAddressLin);
		aInfo.iFileName = KKernelSysBin;
		aInfo.iRootNameOffset = 11;
		aInfo.iRootNameLength = 9;
		aInfo.iExtOffset = 16;
		// keep RomProcessInfo's attributes (the image's ABI bits): the loader checks a kernel module's
		// ABI against EKERN's. Bug 2026-11-12: overwritten with Kernel|Fixed (no ABI) -> ELOCD.LDD
		// "not supported" (Fserv fault 33). Regression: kn_efile.
		aInfo.iAttr |= ECodeSegAttKernel | ECodeSegAttFixed;
		aInfo.iDataSize = aInfo.iBssSize = aInfo.iTotalDataSize = 0;
		aInfo.iDataLoadAddress = 0;
		}
	aInfo.iHeapSizeMin = KKernelHeapSizeMin;
	aInfo.iHeapSizeMax = KKernelHeapSizeMax;
	aInfo.iDataRunAddress = (TLinAddr)MM::KernelHeapAddress;
	aInfo.iStackSize = 0;
	}

TInt M::InitSvHeapChunk(DChunk* aChunk, TInt aSize)
	{
	DWin32Chunk* pC=(DWin32Chunk*)aChunk;
	aSize = MM::RoundToPageSize(aSize);
	pC->iSize = aSize;
	pC->iBase = (TUint8*)K::Allocator;
// mark the kernel heap pages as allocated
	MM::FreeMemory -= aSize;
	__KTRACE_OPT(KBOOT,Kern::Printf("Created SvHeap chunk, addr %08X, init size %08X max size %08X",pC->Base(),aSize,pC->MaxSize()));
	K::HeapInfo.iChunk = aChunk;
	K::HeapInfo.iBase = pC->iBase;
	K::HeapInfo.iMaxSize = pC->MaxSize();
	return KErrNone;
	}

void RHeapK::Mutate(TInt aOffset, TInt aMaxLength)
//
// Used by the kernel to mutate a fixed heap into a chunk heap.
//
	{
	iMinLength += aOffset;
	iMaxLength = aMaxLength + aOffset;
	iOffset = aOffset;
	iChunkHandle = (TInt)K::HeapInfo.iChunk;
	iPageSize = M::PageSizeInBytes();
	iGrowBy = iPageSize;
	iFlags &= ~(RAllocator::ESingleThreaded|RAllocator::EFixedSize);
	}

//

TInt M::InitSvStackChunk()
	{
	return KErrNone;
	}

