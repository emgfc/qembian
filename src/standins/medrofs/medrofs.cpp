// Copyright (c) 2026 syborg-pg contributors. Licensed under the Eclipse Public License v1.0.
//
// MEDROFS.PDD: read-only "NAND" media serving the N95 ROFS partition (ROFS1 + ROFx extension
// at ROFS1's iMaxImageSize, as on the phone's flash) from the board's read-only flash window at
// physical 0x60000000 (stands for the N95's NAND; not RAM). tools/romcompose.py --rofs writes the
// flash image <rom>.rofs = header page { "N95ROFS\0", TUint32 size } + blob at +0x1000;
// scripts/run-modern.sh loads it there.
// Registered as local drive N95ROFS_DRIVE (8, the N95's own ESTARTCOMP.TXT number).

#include "locmedia.h"
#include "platform.h"
#include <kernel/kernel.h>
#include <e32rom.h>
#include <partitions.h>

#define N95ROFS_DRIVE 8
_LIT(KPddName, "Media.N95Rofs");
_LIT(KDriveName, "N95Rofs");
static const TInt KDriveNumbers[1] = { N95ROFS_DRIVE };

static TLinAddr RofsBase = 0;
static TInt RofsSize = 0;

const TPhysAddr KFlashPhys = 0x60000000;
const TInt KFlashWindow = 128 << 20;

static TInt FindRofs()
	{
	DPlatChunkHw* hdr = NULL;
	TInt r = DPlatChunkHw::New(hdr, KFlashPhys, 0x1000, EMapAttrSupRo | EMapAttrCachedMax);
	if (r != KErrNone)
		return r;
	const TUint32* t = (const TUint32*)hdr->LinearAddress();
	TBool ok = t[0] == 0x5235394e /*"N95R"*/ && t[1] == 0x0053464f /*"OFS\0"*/ && t[2] > 0
			   && t[2] <= (TUint32)(KFlashWindow - 0x1000);
	RofsSize = ok ? (TInt)t[2] : 0;
	hdr->Close(NULL);
	if (!ok)
		return KErrNotFound;
	DPlatChunkHw* all = NULL;	// kept for the life of the system
	r = DPlatChunkHw::New(all, KFlashPhys, 0x1000 + RofsSize, EMapAttrSupRo | EMapAttrCachedMax);
	if (r != KErrNone)
		return r;
	RofsBase = all->LinearAddress() + 0x1000;
	return KErrNone;
	}

class DPhysicalDeviceMediaRofs : public DPhysicalDevice
	{
public:
	DPhysicalDeviceMediaRofs()
		{
		iUnitsMask = 0x1;
		iVersion = TVersion(KMediaDriverInterfaceMajorVersion, KMediaDriverInterfaceMinorVersion, KMediaDriverInterfaceBuildVersion);
		}
	virtual TInt Install() { return SetName(&KPddName); }
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DBase*& aChannel, TInt aMediaId, const TDesC8*, const TVersion& aVer);
	virtual TInt Validate(TInt aDeviceType, const TDesC8*, const TVersion& aVer)
		{
		if (!Kern::QueryVersionSupported(iVersion, aVer) || aDeviceType != EFixedMedia2)
			return KErrNotSupported;
		return KErrNone;
		}
	virtual TInt Info(TInt aFunction, TAny*)
		{
		return aFunction == EPriority ? KMediaDriverPriorityNormal : KErrNotSupported;
		}
	};

class DMediaDriverRofs : public DMediaDriver
	{
public:
	DMediaDriverRofs(TInt aMediaId) : DMediaDriver(aMediaId) {}
	virtual TInt Request(TLocDrvRequest& aRequest);
	virtual void Disconnect(DLocalDrive*, TThreadMessage*) {}
	virtual TInt PartitionInfo(TPartitionInfo& anInfo)
		{
		anInfo.iPartitionCount = 1;
		anInfo.iEntry[0].iPartitionBaseAddr = 0;
		anInfo.iEntry[0].iPartitionLen = anInfo.iMediaSizeInBytes = RofsSize;
		anInfo.iEntry[0].iPartitionType = KPartitionTypeRofs;
		return KErrNone;
		}
	virtual void NotifyPowerDown() {}
	virtual void NotifyEmergencyPowerDown() {}
	};

TInt DPhysicalDeviceMediaRofs::Create(DBase*& aChannel, TInt aMediaId, const TDesC8*, const TVersion& aVer)
	{
	if (!Kern::QueryVersionSupported(iVersion, aVer))
		return KErrNotSupported;
	if (!RofsSize && FindRofs() != KErrNone)
		return KErrNotReady;
	DMediaDriverRofs* pD = new DMediaDriverRofs(aMediaId);
	aChannel = pD;
	if (!pD)
		return KErrNoMemory;
	pD->SetTotalSizeInBytes(RofsSize);
	return KErrNone;
	}

TInt DMediaDriverRofs::Request(TLocDrvRequest& m)
	{
	switch (m.Id())
		{
		case DLocalDrive::ECaps:
			{
			TLocalDriveCapsV6& caps = *(TLocalDriveCapsV6*)m.RemoteDes();
			caps.iType = EMediaNANDFlash;
			caps.iConnectionBusType = EConnectionBusInternal;
			caps.iDriveAtt = KDriveAttLocal | KDriveAttInternal;
			caps.iMediaAtt = KMediaAttWriteProtected;
			caps.iFileSystemId = KDriveFileSysROFS;
			caps.iPartitionType = KPartitionTypeRofs;
			caps.iSize = m.Drive()->iPartitionLen;
			caps.iHiddenSectors = 0;
			caps.iEraseBlockSize = 0x20000;
			caps.iBlockSize = 512;
			SetTotalSizeInBytes(caps);
			return KErrNone;
			}
		case DLocalDrive::ERead:
			{
			Int64 pos = m.Pos();
			Int64 len = m.Length();
			if (pos < 0 || len < 0 || pos + len > RofsSize)
				return KErrArgument;
			TPtrC8 des((const TUint8*)(RofsBase + (TInt)pos), (TInt)len);
			return m.WriteRemote(&des, 0);
			}
		case DLocalDrive::EWrite:
		case DLocalDrive::EFormat:
			return KErrAccessDenied;
		default:
			return KErrNotSupported;
		}
	}

DECLARE_EXTENSION_PDD()
	{
	return new DPhysicalDeviceMediaRofs;
	}

DECLARE_STANDARD_EXTENSION()
	{
	TInt r = FindRofs();
	Kern::Printf("N95ROFS: %s base %08x size %08x -> local drive %d", r == KErrNone ? "flash" : "NO ROFS in flash", RofsBase, RofsSize, N95ROFS_DRIVE);
	if (r != KErrNone)
		return KErrNone;	// nothing to serve; don't fail the boot
	DPrimaryMediaBase* pM = new DPrimaryMediaBase;
	if (!pM)
		return KErrNoMemory;
	return LocDrv::RegisterMediaDevice(EFixedMedia2, 1, &KDriveNumbers[0], pM, 1, KDriveName);
	}
