// Hosted EKA2: code segments. Replaces memmodel/emul/win32/mcodeseg.cpp (which loads Windows DLLs);
// structure and names follow it (EPL v1.0, Nokia). Kinds:
// - EKERN: the hosted process itself (nothing to load).
// - ROM (XIP) images: the loader passes the TRomImageHeader in iCodeLoadAddress; code and export
//   directory are used in place; writable static data is the ROM's .data copied to its run address and
//   .bss zeroed (per process for DLLs, swapped on process switch as in the emulator model: iDataCopy,
//   iDataDest, iBssDest).
// - RAM-loaded E32 images (ROFS, C:, E:): as the multiple memory model's DMemModelCodeSeg::DoCreateRam:
//   code at a run address in the RAM code region, which is also the load address (the loader writes
//   the code there, the .data template right after it, and relocates for the run address); an EXE's data
//   is its process's data area (copied in when loaded), a DLL's data gets an address in the DLL data
//   region, the same in every process, and a copy per process like ROM DLL data.

#include "memmodel.h"
#include <e32rom.h>
#include "lxwin32.h"

static DWin32CodeSeg* KernelCodeSeg;

DCodeSeg* M::NewCodeSeg(TCodeSegCreateInfo&)
	{
	__KTRACE_OPT(KDLL,Kern::Printf("M::NewCodeSeg"));
	return new DWin32CodeSeg;
	}

extern TInt HostedFindModule(const TDesC& aRootName);
extern TInt HostedModuleEntry(TInt aModule, TInt aReason);
extern TLinAddr HostedModuleExport(TInt aModule, TInt aOrdinal);

DWin32CodeSeg::DWin32CodeSeg()
	: iModule(-1)
	{
	}

// a kernel module's entry point as the kernel calls it (TLibraryEntry) -> the built-in's
// the kernel calls a module's entry as TLibraryEntry(reason): one thunk per module index
template <TInt N> static TInt ModuleThunk(TInt aReason) { return HostedModuleEntry(N, aReason); }
typedef TInt (*TThunk)(TInt);
static const TThunk Thunks[] = {
	ModuleThunk<0>, ModuleThunk<1>, ModuleThunk<2>, ModuleThunk<3>, ModuleThunk<4>, ModuleThunk<5>,
	ModuleThunk<6>, ModuleThunk<7>, ModuleThunk<8>, ModuleThunk<9>, ModuleThunk<10>, ModuleThunk<11>,
	ModuleThunk<12>, ModuleThunk<13>, ModuleThunk<14>, ModuleThunk<15>, ModuleThunk<16>, ModuleThunk<17>,
	ModuleThunk<18>, ModuleThunk<19>, ModuleThunk<20>, ModuleThunk<21>, ModuleThunk<22>, ModuleThunk<23>,
	ModuleThunk<24>, ModuleThunk<25>, ModuleThunk<26>, ModuleThunk<27>, ModuleThunk<28>, ModuleThunk<29>,
	ModuleThunk<30>, ModuleThunk<31>, ModuleThunk<32>, ModuleThunk<33>, ModuleThunk<34>, ModuleThunk<35>,
	ModuleThunk<36>, ModuleThunk<37>, ModuleThunk<38>, ModuleThunk<39>, ModuleThunk<40>, ModuleThunk<41>,
	ModuleThunk<42>, ModuleThunk<43>, ModuleThunk<44>, ModuleThunk<45>, ModuleThunk<46>, ModuleThunk<47> };
static TThunk ModuleEntryThunk(TInt aModule)
	{
	__NK_ASSERT_ALWAYS(aModule >= 0 && aModule < (TInt)(sizeof(Thunks) / sizeof(Thunks[0])));
	return Thunks[aModule];
	}

DWin32CodeSeg::~DWin32CodeSeg()
	{
	__KTRACE_OPT(KDLL,Kern::Printf("DWin32CodeSeg::Destruct %C", this));
	extern void HostedModuleUnloaded(TInt aModule);
	if (iModule >= 0)
		HostedModuleUnloaded(iModule);
	K::TheKernelProcess->UnmapCodeSeg(this);
	Kern::Free(iDataCopy);
	DCodeSeg::Destruct();
	}

// a fixed address range readable and writable (pages already mapped by an earlier code segment stay)
static TBool MapFixedRw(TLinAddr aBase, TInt aSize)
	{
	TLinAddr a = aBase & ~0xfffu, end = (aBase + aSize + 0xfff) & ~0xfffu;
	for (; a < end; a += 0x1000)
		{
		TAny* m = hmmap((TAny*)a, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if ((TLinAddr)m != a && (TInt)(TLinAddr)m != -17 /* EEXIST: ours already */)
			return EFalse;
		}
	return ETrue;
	}

// ---- RAM-loaded images: the regions (reserved whole at first use, so no host mapping lands there)
static const TLinAddr KRamCodeBase = 0x70000000u, KRamCodeSize = 0x08000000u;		// as the phone: 0x7xxxxxxx
static const TLinAddr KDllDataBase = 0x78000000u, KDllDataSize = 0x07f00000u;	// 0x7ff00000+: exec trampolines
static TLinAddr RamCodeTop, DllDataTop;		// bump allocation (address space is not reused yet)

static TBool ReserveRegions()
	{
	if (RamCodeTop)
		return ETrue;
	TAny* c = hmmap((TAny*)KRamCodeBase, KRamCodeSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	TAny* d = hmmap((TAny*)KDllDataBase, KDllDataSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
	if ((TLinAddr)c != KRamCodeBase || (TLinAddr)d != KDllDataBase)
		{
		Kern::Printf("HOSTED: RAM code/DLL data regions %08x/%08x not free", KRamCodeBase, KDllDataBase);
		return EFalse;
		}
	RamCodeTop = KRamCodeBase;
	DllDataTop = KDllDataBase;
	return ETrue;
	}

// a page-rounded range of a region, made accessible; 0 when the region is full
static TLinAddr RegionAlloc(TLinAddr& aTop, TLinAddr aEnd, TInt aSize, TInt aProt)
	{
	TUint32 size = (aSize + 0xfff) & ~0xfffu;
	if (!size || aTop + size > aEnd || aTop + size < aTop)
		return 0;
	TLinAddr a = aTop;
	if (hsys(SYS_mprotect, a, size, aProt) != 0)
		return 0;
	aTop += size;
	return a;
	}

TInt DWin32CodeSeg::DoCreateRam(TCodeSegCreateInfo& aInfo, DProcess* aProcess, TBool aExe)
	{
	if (iAttr & ECodeSegAttKernel)
		{
		Kern::Printf("HOSTED: RAM-loaded kernel module %S not supported", &iRootName);
		return KErrNotSupported;
		}
	if (aInfo.iCodeSize <= 0)
		return KErrCorrupt;
	DCodeSeg::Wait();
	TBool ok = ReserveRegions();
	TLinAddr code = 0, data = 0;
	TInt map = aInfo.iCodeSize + aInfo.iDataSize;
	TInt total = aInfo.iDataSize + aInfo.iBssSize;
	if (ok)
		code = RegionAlloc(RamCodeTop, KRamCodeBase + KRamCodeSize, map, PROT_READ | PROT_WRITE | PROT_EXEC);
	if (code && total && !aExe)
		data = RegionAlloc(DllDataTop, KDllDataBase + KDllDataSize, total, PROT_READ | PROT_WRITE);
	DCodeSeg::Signal();
	if (!code || (total && !aExe && !data))
		return KErrNoMemory;
	iRam = ETrue;
	iRamCodeRun = code;
	iRamCodeSize = aInfo.iCodeSize;
	iRamTextSize = aInfo.iTextSize;
	iRamMapSize = map;
	iRamDataLoad = aInfo.iDataSize ? code + aInfo.iCodeSize : 0;
	if (aExe)
		{
		iRamDataRun = total ? 0x00400000u : 0;		// the process data area (KUserLocalDataBase)
		iExeProcess = (DWin32Process*)aProcess;
		}
	else
		iRamDataRun = data;
	iRealDataSize = aInfo.iDataSize;
	iRealBssSize = aInfo.iBssSize;
	iDataDest = iRamDataRun;
	iBssDest = iRamDataRun + aInfo.iDataSize;
	iRamExportDirCount = aInfo.iExportDirCount;
	// as DEpocCodeSeg::DoCreate (pcodeseg.cpp): the loader's offsets become run addresses
	aInfo.iFileEntryPoint += code;
	aInfo.iExportDir += code;
	iRamExportDir = aInfo.iExportDir;
	aInfo.iCodeLoadAddress = code;
	aInfo.iCodeRunAddress = code;
	aInfo.iDataLoadAddress = iRamDataLoad;
	aInfo.iDataRunAddress = iRamDataRun;
	iRamExceptionDescriptor = aInfo.iExceptionDescriptor ? aInfo.iExceptionDescriptor + code : 0;
	aInfo.iExceptionDescriptor = iRamExceptionDescriptor;
	iRunAddress = code;
	iSize = map;
	if (total)
		iMark |= EMarkData | EMarkDataPresent | (aExe ? 0 : EMarkDataInit);
	__KTRACE_OPT(KDLL,Kern::Printf("DWin32CodeSeg %C RAM: code %08x+%x data run %08x load %08x size %x+%x",
		this, code, aInfo.iCodeSize, iRamDataRun, iRamDataLoad, aInfo.iDataSize, aInfo.iBssSize));
	if (aExe && aProcess && total)
		return aProcess->CreateDataBssStackArea((TProcessCreateInfo&)aInfo);	// filled when loaded
	return KErrNone;
	}

TInt DWin32CodeSeg::DoCreate(TCodeSegCreateInfo& aInfo, DProcess* aProcess)
	{
	__KTRACE_OPT(KDLL,Kern::Printf("DWin32CodeSeg::DoCreate %C proc %O", this, aProcess));
	TBool exe = (iExeCodeSeg == this);
	if (exe && (iAttr & ECodeSegAttKernel))
		{
		// EKERN: the hosted kernel is this process; with a ROM its identity (exports for the loader's
		// import checks, version) is the ROM's primary image (P::KernelInfo)
		if (aInfo.iTotalDataSize)
			iMark |= EMarkData | EMarkDataPresent;
		if (aInfo.iCodeLoadAddress)
			{
			const TRomImageHeader& k = *(const TRomImageHeader*)aInfo.iCodeLoadAddress;
			iRomImage = aInfo.iCodeLoadAddress;
			iRunAddress = k.iCodeAddress;
			iSize = k.iCodeSize;
			}
		KernelCodeSeg = this;
		return KErrNone;
		}
	if (!aInfo.iCodeLoadAddress)
		return DoCreateRam(aInfo, aProcess, exe);
	const TRomImageHeader& h = *(const TRomImageHeader*)aInfo.iCodeLoadAddress;
	if (iAttr & ECodeSegAttKernel)
		{
		// a kernel-side module: its ROM image is ARM code linked to the ARM kernel. Serve it from the
		// built-in module of the same file name, or refuse (KErrNotSupported) when there is none.
		iModule = HostedFindModule(iRootName);
		__KTRACE_OPT(KDLL,Kern::Printf("DWin32CodeSeg %C: kernel module, built-in %d", this, iModule));
		if (iModule < 0)
			{
			Kern::Printf("HOSTED: kernel module %S has no hosted build", &iRootName);
			return KErrNotSupported;
			}
		iRomImage = aInfo.iCodeLoadAddress;
		iRunAddress = h.iCodeAddress;
		iSize = h.iCodeSize;
		aInfo.iFileEntryPoint = (TLinAddr)ModuleEntryThunk(iModule);
		return KErrNone;
		}
	iRomImage = aInfo.iCodeLoadAddress;
	iRunAddress = h.iCodeAddress;
	iSize = h.iCodeSize;
	iRealDataSize = h.iDataSize;
	iRealBssSize = h.iBssSize;
	iDataDest = h.iDataBssLinearBase;
	iBssDest = h.iDataBssLinearBase + h.iDataSize;
	// the create info from the ROM image, as the phone memory model's DEpocCodeSeg::DoCreate (XIP).
	// Bug 2026-11-12: a loader-created EXE (EStart.exe) kept a zero data load address and the kernel
	// copied its .data from address 0 (kn_efile: second process).
	aInfo.iFileEntryPoint = h.iEntryPoint;
	aInfo.iExportDir = h.iExportDir;
	aInfo.iCodeLoadAddress = h.iCodeAddress;
	aInfo.iCodeRunAddress = h.iCodeAddress;
	aInfo.iDataLoadAddress = h.iDataAddress;
	aInfo.iDataRunAddress = h.iDataBssLinearBase;
	aInfo.iExceptionDescriptor = h.iExceptionDescriptor;
	if (iRealDataSize)
		{
		// the initial .data is in the ROM at iDataAddress: keep a copy as the template for each process
		iDataCopy = Kern::Alloc(iRealDataSize);
		if (!iDataCopy)
			return KErrNoMemory;
		memcpy(iDataCopy, (const TAny*)h.iDataAddress, iRealDataSize);
		}
	if (iRealDataSize || iRealBssSize)
		{
		iMark |= EMarkData | EMarkDataPresent;
		// the live copy's address (the multiple model's ROM DLL data, below 0x40000000): mapped here, each
		// process's copy is swapped in on process switch (MM::DoProcessSwitch). Bug 2026-11-12: unmapped,
		// the first ROM DLL with static data (MmfAudioServer's) faulted the kernel in the switch.
		if (!exe && !MapFixedRw(iDataDest, iRealDataSize + iRealBssSize))
			return KErrNoMemory;
		}
	// a user process's EXE: its .data/.bss area at the fixed run address, as the phone memory model's
	// DEpocCodeSeg::DoCreate does (pcodeseg.cpp). Bug 2026-11-12: missing, efile.exe ran with zeroed
	// .data and faulted on its first static list (kn_efile checks .data against the ROM).
	if (exe && aProcess && !(aProcess->iAttributes & DProcess::ESupervisor))
		{
		TInt r = aProcess->CreateDataBssStackArea((TProcessCreateInfo&)aInfo);
		if (r != KErrNone)
			return r;
		}
	return KErrNone;
	}

TInt DWin32CodeSeg::CopyDataBss()
	{
	return KErrNone;		// DoCreate took the .data template from the ROM
	}

TInt DWin32CodeSeg::ProcessImports(DProcess*)
	{
	return KErrNone;		// ROM images are prelinked; the loader resolves their dependencies
	}

TInt DWin32CodeSeg::Loaded(TCodeSegCreateInfo& aInfo)
	{
	if (iRam)
		{
		// the loader has written the code and the .data template: the template is kept (each process's
		// copy starts from it), an EXE's data goes into its process's area, the code becomes read/execute
		if (iRealDataSize && !iDataCopy)
			{
			iDataCopy = Kern::Alloc(iRealDataSize);
			if (!iDataCopy)
				return KErrNoMemory;
			memcpy(iDataCopy, (const TAny*)iRamDataLoad, iRealDataSize);
			}
		if (iExeProcess && iExeCodeSeg == this && iExeProcess->iExeDataFd >= 0 && iRealDataSize)
			memcpy((TAny*)(iExeProcess->iExeDataAlias + (iRamDataRun - iExeProcess->iExeDataBase)), iDataCopy, iRealDataSize);
		hsys(SYS_ARM_cacheflush, iRamCodeRun, iRamCodeRun + iRamMapSize, 0);
		hsys(SYS_mprotect, iRamCodeRun, (iRamMapSize + 0xfff) & ~0xfffu, PROT_READ | PROT_EXEC);
		}
	return DCodeSeg::Loaded(aInfo);
	}

// the export directory with its ordinal-0 word, for the loader (RAM images; ROM ones it reads itself)
void DWin32CodeSeg::ReadExportDir(TUint32* aDest)
	{
	if (iRam && iRamExportDir)
		kumemput(aDest, (const TAny*)(iRamExportDir - sizeof(TLinAddr)), (iRamExportDirCount + 1) * sizeof(TLinAddr));
	}

TBool DWin32CodeSeg::FindCheck(DProcess*)
	{
	return ETrue;
	}

TBool DWin32CodeSeg::OpenCheck(DProcess* aProcess)
	{
	if (!aProcess)
		return EFalse;		// never reuse EXE code segments
	return ETrue;
	}

TLibraryFunction DWin32CodeSeg::Lookup(TInt aOrdinal)
	{
	if (iModule >= 0)
		return (TLibraryFunction)HostedModuleExport(iModule, aOrdinal);
	if (iRam)
		{
		if (aOrdinal >= 0 && aOrdinal <= iRamExportDirCount && iRamExportDir)
			return (TLibraryFunction)((const TLinAddr*)iRamExportDir)[aOrdinal - 1];
		return NULL;
		}
	if (!iRomImage)
		return NULL;
	const TRomImageHeader& h = *(const TRomImageHeader*)iRomImage;
	TLibraryFunction f = NULL;
	if (aOrdinal > 0 && aOrdinal <= h.iExportDirCount)
		f = (TLibraryFunction)((const TLinAddr*)h.iExportDir)[aOrdinal - 1];
	else if (aOrdinal == 0)
		f = (TLibraryFunction)((const TLinAddr*)h.iExportDir)[-1];	// ordinal 0: named symbol table
	__KTRACE_OPT(KDLL,Kern::Printf("DWin32CodeSeg::Lookup(%d)->%08x", aOrdinal, f));
	return f;
	}

// as the phone memory model (pcodeseg.cpp): the loaded code segment of a ROM image, by its code address.
// Bug 2026-11-12: NULL (Win32), so the loader never found the running EKERN as ELOCD.LDD's dependency
// and tried to create it (Fserv fault 33). Regression: kn_efile.
DCodeSeg* DCodeSeg::FindRomCode(const TAny* aRomImgHdr)
	{
	const TRomImageHeader& rih = *(const TRomImageHeader*)aRomImgHdr;
	DCodeSeg* s = CodeSegsByAddress.Find(rih.iCodeAddress);
	__KTRACE_OPT(KDLL, Kern::Printf("FindRomCode hdr %08x code %08x -> %08x", aRomImgHdr, rih.iCodeAddress, s));
	return s;
	}

void DWin32CodeSeg::InitData()
	{
	}

TInt DWin32CodeSeg::GetMemoryInfo(TModuleMemoryInfo&, DProcess*)
	{
	return KErrNotSupported;
	}

void DWin32CodeSeg::Info(TCodeSegCreateInfo& aInfo)
	{
	if (iRomImage)
		{
		const TRomImageHeader& h = *(const TRomImageHeader*)iRomImage;
		aInfo.iTextSize = h.iTextSize;
		aInfo.iCodeSize = h.iCodeSize;
		aInfo.iDataSize = h.iDataSize;
		aInfo.iBssSize = h.iBssSize;
		aInfo.iTotalDataSize = h.iTotalDataSize;
		aInfo.iExportDir = h.iExportDir;
		aInfo.iExportDirCount = h.iExportDirCount;
		// as the phone memory model (DEpocCodeSeg::Info, pcodeseg.cpp): the *code* address. Bug
		// 2026-11-12: the image header's address was reported; the 9.2 loader compares it with the ROM
		// image of a dependency (EKERN) and refused ELOCD.LDD (Fserv fault 33). Regression: kn_efile.
		aInfo.iCodeLoadAddress = h.iCodeAddress;
		aInfo.iCodeRunAddress = h.iCodeAddress;
		aInfo.iDataLoadAddress = h.iDataAddress;
		aInfo.iDataRunAddress = h.iDataBssLinearBase;
		aInfo.iExceptionDescriptor = h.iExceptionDescriptor;
		}
	else if (iRam)
		{
		aInfo.iTextSize = iRamTextSize;
		aInfo.iCodeSize = iRamCodeSize;
		aInfo.iDataSize = iRealDataSize;
		aInfo.iBssSize = iRealBssSize;
		aInfo.iTotalDataSize = iRealDataSize + iRealBssSize;
		aInfo.iExportDir = iRamExportDir;
		aInfo.iExportDirCount = iRamExportDirCount;
		aInfo.iCodeLoadAddress = iRamCodeRun;
		aInfo.iCodeRunAddress = iRamCodeRun;
		aInfo.iDataLoadAddress = iRamDataLoad;
		aInfo.iDataRunAddress = iRamDataRun;
		aInfo.iExceptionDescriptor = iRamExceptionDescriptor;
		}
	DCodeSeg::Info(aInfo);
	}

// as the phone memory model (pcodeseg.cpp): the image's exception index (C++ unwinding, User::Leave)
TLinAddr DCodeSeg::ExceptionDescriptor()
	{
	DWin32CodeSeg* s = (DWin32CodeSeg*)this;
	if (s->iRam)
		return s->iRamExceptionDescriptor;
	if (s->iRomImage && s->iModule < 0)
		return ((const TRomImageHeader*)s->iRomImage)->iExceptionDescriptor;
	return 0;
	}

// as the phone memory model (pcodeseg.cpp): names are used as the loader gives them (the Win32 emulator
// prefixed Z:\sys\bin to bare names)
void P::NormalizeExecutableFileName(TDes&)
	{
	}

// as memmodel/epoc/putils.cpp (EPL, Nokia): process creation info from a ROM image header
EXPORT_C void Epoc::RomProcessInfo(TProcessCreateInfo& aInfo, const TRomImageHeader& a)
	{
	aInfo.iUids=*(const TUidType*)&a.iUid1;
	aInfo.iCodeSize=a.iCodeSize;
	aInfo.iTextSize=a.iTextSize;
	aInfo.iDataSize=a.iDataSize;
	aInfo.iBssSize=a.iBssSize;
	aInfo.iTotalDataSize=a.iTotalDataSize;
	aInfo.iEntryPtVeneer=a.iEntryPoint;
	aInfo.iFileEntryPoint=a.iEntryPoint;
	aInfo.iDepCount=a.iDllRefTable ? a.iDllRefTable->iNumberOfEntries : 0;
	aInfo.iExportDir=a.iExportDir;
	aInfo.iExportDirCount=a.iExportDirCount;
	aInfo.iCodeLoadAddress=(TLinAddr)&a;
	aInfo.iCodeRunAddress=a.iCodeAddress;
	aInfo.iDataLoadAddress=a.iDataAddress;
	aInfo.iDataRunAddress=a.iDataBssLinearBase;
	aInfo.iExceptionDescriptor = a.iExceptionDescriptor;
	aInfo.iHeapSizeMin=a.iHeapSizeMin;
	aInfo.iHeapSizeMax=a.iHeapSizeMax;
	aInfo.iStackSize=a.iStackSize;
	aInfo.iPriority=a.iPriority;
	aInfo.iHandle=NULL;
	aInfo.iS = a.iS;
	aInfo.iModuleVersion = a.iModuleVersion;
	if (a.iFlags&KRomImageFlagsKernelMask)
		aInfo.iAttr=ECodeSegAttKernel;
	else
		aInfo.iAttr=ECodeSegAttGlobal;
	if (a.iFlags&KRomImageFlagFixedAddressExe)
		aInfo.iAttr|=ECodeSegAttFixed;
	aInfo.iAttr &= ~ECodeSegAttABIMask;
	aInfo.iAttr |= (a.iFlags & KRomImageABIMask);
	if(a.iFlags&KRomImageSMPSafe)
		aInfo.iAttr |= ECodeSegAttSMPSafe;
	aInfo.iClientHandle = KCurrentThreadHandle;
	aInfo.iClientProcessHandle = 0;
	aInfo.iFinalHandle = 0;
	aInfo.iOwnerType = EOwnerProcess;
	aInfo.iFlags &= ~(TProcessCreateInfo::EDataPagingMask);
	if(a.iFlags&KRomImageFlagDataPaged)
		aInfo.iFlags |= TProcessCreateInfo::EDataPaged;
	if(a.iFlags&KRomImageFlagDataUnpaged)
		aInfo.iFlags |= TProcessCreateInfo::EDataUnpaged;
	}
