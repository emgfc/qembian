// Hosted EKA2: the hosted board. Drivers written for our syborg board (the ARM QEMU machine) find their
// device pages at the syborg I/O addresses (KPrimaryIOBase 0xC6000000 + device * 0x1000, the peripheral
// window at physical 0xC0000000) and their "physical" windows (ROFS flash at 0x60000000, driver RAM at
// 0x20000000). Register access goes through the I/O bus (include/board/syborg.h routes ReadReg/WriteReg
// here): device models answer with the QEMU syborg devices' semantics (NVMemory: syborg_nvmem.c), other
// pages are plain memory. Interrupts: class Interrupt, delivered by an 'interrupt' host thread. The
// platform device's board-config string (key=value;..., QEMU's syborg-platform.config) is filled in.
// Original work for qembian.

#include <kernel/kern_priv.h>
#include <assp.h>
#include <platform.h>

extern TInt HostedMapPhysicalFile(TPhysAddr aPhys, const char* aPath);

static const TLinAddr KHostedIoBase = 0xC6000000u;		// KPrimaryIOBase (multiple memory model)
static const TUint32 KHostedIoSize = 0x40000;			// 64 device pages
static const TLinAddr KPlatformDevice = KHostedIoBase + 0x11 * 0x1000;	// KHwSVPPlatformDevice
static const TPhysAddr KFlashPhys = 0x60000000u;
static const TPhysAddr KHostedIoPhys = 0xC0000000u;
extern TInt HostedAddPhysicalWindow(TPhysAddr aPhys, TUint32 aSize, TLinAddr aLin);

TInt HostedBoardRam();
TInt HostedNvmemAttach(TInt aIndex, TUint32 aIoBase, TInt aIrq, const char* aImage);

// the drives: C: (NVMemory, device 0x10, interrupt 12) and E: (device 0x0d, interrupt 14), as the board
TInt HostedBoardDrives(const char* aNvmem, const char* aNvmem2)
	{
	TInt r = HostedNvmemAttach(0, KHostedIoBase + 0x10 * 0x1000, 12, aNvmem);
	if (r == KErrNone)
		r = HostedNvmemAttach(1, KHostedIoBase + 0x0d * 0x1000, 14, aNvmem2);
	return r;
	}

// set up before the kernel boots; aBoardConfig: the device profile's syborg-platform.config string
TInt HostedBoardInit(const char* aBoardConfig, const char* aRofsImage)
	{
	if (TInt r = HostedBoardRam())
		return r;
	TAny* io = hmmap((TAny*)KHostedIoBase, KHostedIoSize, PROT_READ | PROT_WRITE,
	                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if ((TLinAddr)io != KHostedIoBase)
		return KErrInUse;
	// the I/O area is the board's peripheral window (KHwBasePeripherals 0xC0000000 on the ARM board)
	if (TInt r = HostedAddPhysicalWindow(KHostedIoPhys, KHostedIoSize, KHostedIoBase))
		return r;
	if (aBoardConfig)
		{
		char* cfg = (char*)(KPlatformDevice + 0x400);
		TInt n = 0;
		while (aBoardConfig[n] && n < 0x3ff)
			{
			cfg[n] = aBoardConfig[n];
			++n;
			}
		cfg[n] = 0;
		}
	if (aRofsImage)
		{
		TInt r = HostedMapPhysicalFile(KFlashPhys, aRofsImage);
		if (r != KErrNone)
			return r;
		}
	return KErrNone;
	}

// ======================================================================== the I/O bus
typedef TUint32 (*TIoRead)(TAny* aCtx, TUint32 aOffset);
typedef void (*TIoWrite)(TAny* aCtx, TUint32 aOffset, TUint32 aValue);
struct SIoDevice { TUint32 iBase, iSize; TIoRead iRead; TIoWrite iWrite; TAny* iCtx; volatile TUint32 iAccesses; };
static SIoDevice IoDevices[16];
static TInt IoDeviceCount;

TInt HostedIoAttach(TUint32 aBase, TUint32 aSize, TIoRead aRead, TIoWrite aWrite, TAny* aCtx)
	{
	if (IoDeviceCount == 16)
		return KErrOverflow;
	SIoDevice& d = IoDevices[IoDeviceCount++];
	d.iBase = aBase; d.iSize = aSize; d.iRead = aRead; d.iWrite = aWrite; d.iCtx = aCtx;
	return KErrNone;
	}

// register accesses a device model has served (tests: drivers reach their device through the bus)
TUint32 HostedIoAccesses(TUint32 aBase)
	{
	for (TInt i = 0; i < IoDeviceCount; ++i)
		if (IoDevices[i].iBase == aBase)
			return IoDevices[i].iAccesses;
	return 0;
	}

TUint32 HostedIoRead(TUint32 aAddr)
	{
	for (TInt i = 0; i < IoDeviceCount; ++i)
		if (aAddr - IoDevices[i].iBase < IoDevices[i].iSize)
			{
			++IoDevices[i].iAccesses;
			return IoDevices[i].iRead(IoDevices[i].iCtx, aAddr - IoDevices[i].iBase);
			}
	return *(volatile TUint32*)aAddr;		// a device without a model: plain memory
	}

void HostedIoWrite(TUint32 aAddr, TUint32 aValue)
	{
	for (TInt i = 0; i < IoDeviceCount; ++i)
		if (aAddr - IoDevices[i].iBase < IoDevices[i].iSize)
			{
			++IoDevices[i].iAccesses;
			IoDevices[i].iWrite(IoDevices[i].iCtx, aAddr - IoDevices[i].iBase, aValue);
			return;
			}
	*(volatile TUint32*)aAddr = aValue;
	}

// ======================================================================== interrupts
// class Interrupt (kernel/arm/assp.h) as the board's interrupt controller: device models raise an
// interrupt line; a delivery thread (an 'interrupt' host thread) runs the bound ISR between
// StartOfInterrupt/EndOfInterrupt while the line is raised and enabled.
static const TInt KMaxIrq = 64;
struct SIrq { TIsr iIsr; TAny* iPtr; volatile TInt iEnabled, iRaised; };
static SIrq Irqs[KMaxIrq];
static volatile TInt IrqSeq;
static TBool IrqThreadStarted;

static TInt IrqThread(TAny*)
	{
	for (;;)
		{
		TInt seq = __atomic_load_n(&IrqSeq, __ATOMIC_SEQ_CST);
		TBool any = EFalse;
		for (TInt n = 0; n < KMaxIrq; ++n)
			{
			SIrq& q = Irqs[n];
			if (q.iRaised && q.iEnabled && q.iIsr)
				{
				any = ETrue;
				__atomic_store_n(&q.iRaised, 0, __ATOMIC_SEQ_CST);	// edge: the device raises again if needed
				StartOfInterrupt();
				q.iIsr(q.iPtr);
				EndOfInterrupt();
				}
			}
		if (!any)
			hfutex_wait(&IrqSeq, seq);
		}
	}

void HostedRaiseIrq(TInt aIrq)
	{
	if (aIrq < 0 || aIrq >= KMaxIrq)
		return;
	__atomic_store_n(&Irqs[aIrq].iRaised, 1, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&IrqSeq, 1, __ATOMIC_SEQ_CST);
	hfutex_wake(&IrqSeq, 1);
	}

TBool HostedIrqRaised(TInt aIrq)		// tests
	{
	return aIrq >= 0 && aIrq < KMaxIrq && Irqs[aIrq].iRaised;
	}

void HostedLowerIrq(TInt aIrq)
	{
	if (aIrq >= 0 && aIrq < KMaxIrq)
		__atomic_store_n(&Irqs[aIrq].iRaised, 0, __ATOMIC_SEQ_CST);
	}

static void StartIrqThread()
	{
	if (!IrqThreadStarted)
		{
		IrqThreadStarted = ETrue;
		CreateLinuxThread(EThreadEvent, &IrqThread, 0, ETrue);
		}
	}

EXPORT_C TInt Interrupt::Bind(TInt anId, TIsr anIsr, TAny* aPtr)
	{
	if (anId < 0 || anId >= KMaxIrq)
		return KErrArgument;
	if (Irqs[anId].iIsr)
		return KErrInUse;
	Irqs[anId].iPtr = aPtr;
	Irqs[anId].iIsr = anIsr;
	StartIrqThread();
	return KErrNone;
	}

EXPORT_C TInt Interrupt::Unbind(TInt anId)
	{
	if (anId < 0 || anId >= KMaxIrq)
		return KErrArgument;
	if (!Irqs[anId].iIsr)
		return KErrGeneral;
	Irqs[anId].iEnabled = 0;
	Irqs[anId].iIsr = NULL;
	return KErrNone;
	}

EXPORT_C TInt Interrupt::Enable(TInt anId)
	{
	if (anId < 0 || anId >= KMaxIrq)
		return KErrArgument;
	if (!Irqs[anId].iIsr)
		return KErrGeneral;
	Irqs[anId].iEnabled = 1;
	if (Irqs[anId].iRaised)
		HostedRaiseIrq(anId);		// a pending line fires once enabled
	return KErrNone;
	}

EXPORT_C TInt Interrupt::Disable(TInt anId)
	{
	if (anId < 0 || anId >= KMaxIrq)
		return KErrArgument;
	Irqs[anId].iEnabled = 0;
	return KErrNone;
	}

EXPORT_C TInt Interrupt::Clear(TInt anId)
	{
	if (anId < 0 || anId >= KMaxIrq)
		return KErrArgument;
	return KErrNone;		// edge delivery: nothing latched
	}

EXPORT_C TInt Interrupt::SetPriority(TInt, TInt)
	{
	return KErrNotSupported;
	}

// ======================================================================== physical RAM (DMA buffers)
extern TInt HostedAddPhysicalWindow(TPhysAddr aPhys, TUint32 aSize, TLinAddr aLin);
extern TLinAddr HostedPhysicalToLinear(TPhysAddr aPhys, TUint32 aSize);
static const TPhysAddr KRamPhys = 0x20000000u;		// a "physical" range for driver buffers
static const TUint32 KRamSize = 0x02000000u;		// 32 MB
static TUint32 RamTop;
static volatile TInt RamLock;

static TInt BoardRamInit()
	{
	TAny* m = hmmap(0, KRamSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (HMAP_FAILED(m))
		return KErrNoMemory;
	return HostedAddPhysicalWindow(KRamPhys, KRamSize, (TLinAddr)m);
	}

// bump allocation (driver buffers live for the system's life; frees are not reused yet)
EXPORT_C TInt Epoc::AllocPhysicalRam(TInt aSize, TPhysAddr& aPhysAddr, TInt aAlign)
	{
	TUint32 align = aAlign > 12 ? (1u << aAlign) : 0x1000;
	while (__atomic_exchange_n(&RamLock, 1, __ATOMIC_ACQUIRE)) {}
	TUint32 at = (RamTop + align - 1) & ~(align - 1);
	TInt r = KErrNoMemory;
	if (aSize > 0 && at + (TUint32)aSize <= KRamSize)
		{
		RamTop = at + ((aSize + 0xfff) & ~0xfff);
		aPhysAddr = KRamPhys + at;
		r = KErrNone;
		}
	__atomic_store_n(&RamLock, 0, __ATOMIC_RELEASE);
	return r;
	}

EXPORT_C TInt Epoc::FreePhysicalRam(TPhysAddr, TInt)
	{
	return KErrNone;
	}

// ======================================================================== NVMemory (syborg-nvmem)
// as src/qemu-syborg/hw/arm/syborg_nvmem.c: registers (word index) ID 0, OFFSET 4, SIZE 8, DIRECTION 12,
// EXECUTE 16, SHM_BASE 20, NV_SIZE 24, SHM_SIZE 28, STATUS 32 (read lowers the interrupt), ENABLE 36;
// 512-byte sectors. The drive image is mapped private (copy-on-write): writes never reach the file.
struct SNvmem
	{
	TUint8* iImage;
	TUint32 iBytes;
	TInt iIrq;
	TUint32 iOffset, iSize, iDirection, iShmBase, iShmSize, iStatus, iEnabled;
	};
static SNvmem Nvmems[2];
static const TUint32 KSector = 512;

static TUint32 NvmemRead(TAny* aCtx, TUint32 aOff)
	{
	SNvmem& s = *(SNvmem*)aCtx;
	switch (aOff >> 2)
		{
	case 0x00: return 0xdeadbeefu;			// SYBORG_ID_NVMEM (syborg.h; not checked by the driver)
	case 0x04: return s.iOffset;
	case 0x08: return s.iSize;
	case 0x0c: return s.iDirection;
	case 0x14: return s.iShmBase;
	case 0x1c: return s.iShmSize;
	case 0x18: return s.iBytes / KSector;
	case 0x20: HostedLowerIrq(s.iIrq); return s.iStatus;
	default: return 0;
		}
	}

static void NvmemExecute(SNvmem& s)
	{
	TUint64 bytes = (TUint64)s.iSize * KSector;
	TUint64 pos = (TUint64)s.iOffset * KSector;
	TUint8* shm = (TUint8*)HostedPhysicalToLinear(s.iShmBase, s.iShmSize);
	TInt ok = s.iImage && s.iSize && bytes <= s.iShmSize && pos + bytes <= s.iBytes && shm &&
	          (s.iDirection == 1 || s.iDirection == 2);
	if (ok)
		{
		if (s.iDirection == 1)
			memcpy(shm, s.iImage + pos, (TUint32)bytes);
		else
			memcpy(s.iImage + pos, shm, (TUint32)bytes);
		}
	s.iStatus = ok ? s.iSize : 0xffffffffu;
	HostedRaiseIrq(s.iIrq);
	}

static void NvmemWrite(TAny* aCtx, TUint32 aOff, TUint32 aValue)
	{
	SNvmem& s = *(SNvmem*)aCtx;
	switch (aOff >> 2)
		{
	case 0x04: s.iOffset = aValue; break;
	case 0x08: s.iSize = aValue; break;
	case 0x0c: s.iDirection = aValue; break;
	case 0x10: NvmemExecute(s); break;
	case 0x14: s.iShmBase = aValue; break;
	case 0x1c: s.iShmSize = aValue; break;
	case 0x24: s.iEnabled = aValue; break;
	default: break;
		}
	}

// an NVMemory device at its syborg I/O page with its interrupt, backed by a drive image
TInt HostedNvmemAttach(TInt aIndex, TUint32 aIoBase, TInt aIrq, const char* aImage)
	{
	SNvmem& s = Nvmems[aIndex];
	s.iIrq = aIrq;
	if (aImage && aImage[0])
		{
		TInt fd = hsys(SYS_openat, -100, aImage, 0, 0);
		if (fd < 0)
			return fd;
		s.iBytes = hsys(19 /* lseek */, fd, 0, 2);
		TAny* m = hmmap(0, s.iBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
		hsys(SYS_close, fd);
		if (HMAP_FAILED(m))
			return KErrNoMemory;
		s.iImage = (TUint8*)m;
		}
	return HostedIoAttach(aIoBase, 0x1000, &NvmemRead, &NvmemWrite, &s);
	}

TInt HostedBoardRam()
	{
	return BoardRamInit();
	}

// ======================================================================== framebuffer (syborg-fb)
// as src/qemu-syborg/hw/arm/syborg_fb.c: registers by word index (ID 0, BASE 1 = physical address of the
// pixels, HEIGHT 2, WIDTH 3, ..., BPP 8, ROW_PITCH 12, ENABLED 13, frame count 17, refresh rate 18;
// palette 0x100..0x1ff), size from the device profile (syborg-fb.width/height). The pixels are the
// driver's own video RAM (a physical RAM window): HostedFbPixels() finds them. No interrupts yet.
static TUint32 FbRegs[0x200];

static TUint32 FbRead(TAny*, TUint32 aOff)
	{
	TUint32 r = aOff >> 2;
	if (r == 0)
		return 0xc51d0006u;				// SYBORG_ID_FB
	if (r == 17)
		return ++FbRegs[17];			// frame count: advances as the display refreshes
	return r < 0x200 ? FbRegs[r] : 0;
	}

static void FbWrite(TAny*, TUint32 aOff, TUint32 aValue)
	{
	TUint32 r = aOff >> 2;
	if (r == 7)
		FbRegs[7] &= ~aValue;			// interrupt cause: write 1 to clear
	else if (r == 4)
		FbRegs[4] = aValue & 3;
	else if (r && r != 17 && r != 18 && r < 0x200)
		FbRegs[r] = aValue;
	}

TInt HostedFbAttach(TInt aWidth, TInt aHeight)
	{
	FbRegs[2] = aHeight;
	FbRegs[3] = aWidth;
	FbRegs[8] = 32;
	FbRegs[18] = 60;
	return HostedIoAttach(KHostedIoBase + 0x05 * 0x1000, 0x1000, &FbRead, &FbWrite, NULL);	// KHwBaseClcd
	}

// the visible pixels (enabled, base set), or NULL; geometry out
const TUint32* HostedFbPixels(TInt& aWidth, TInt& aHeight, TInt& aPitch)
	{
	aWidth = FbRegs[3];
	aHeight = FbRegs[2];
	aPitch = FbRegs[12] ? FbRegs[12] : aWidth * 4;
	if (!FbRegs[13] || !FbRegs[1])
		return NULL;
	return (const TUint32*)HostedPhysicalToLinear(FbRegs[1], aPitch * aHeight);
	}
