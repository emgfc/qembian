// Copyright (c) 2026 syborg-pg contributors. Licensed under the Eclipse Public License v1.0.
//
// MEDROFS.PDD: read-only "NAND" media serving the N95 ROFS partition (ROFS1 + ROFx extension
// at ROFS1's iMaxImageSize, as on the phone's flash) from the board's read-only flash window at
// physical 0x60000000 (stands for the N95's NAND; not RAM). tools/romcompose.py --rofs writes the
// flash image <rom>.rofs = header page { "SYBROFS\0", TUint32 size } + blob at +0x1000;
// scripts/run-modern.sh loads it there.
// Registered as local drive SYBROFS_DRIVE (8, the N95's own ESTARTCOMP.TXT number).
//
// Header version 2 (words 3.. = { 2, count, then count x {local drive, offset, size} }): phones whose
// composite Z: mounts several ROFS images on their own local drives (9.3 RAPIDO: ROFS1/2/3 on
// drives 9/10/11, ESTARTCOMP "Z: 9 EROFS ROFS 0 FS_COMPOSITE" x3). Each image is one partition of
// the same media, mapped to its drive; tools/romcompose.py --rofs-drives writes this layout.

#include "locmedia.h"
#include "platform.h"
#include <kernel/kernel.h>
#include <e32rom.h>
#include <partitions.h>

#define SYBROFS_DRIVE 8
_LIT(KPddName, "Media.SybRofs");
_LIT(KDriveName, "SybRofs");
const TInt KMaxRofs = 4;
static TInt KDriveNumbers[KMaxRofs] = { SYBROFS_DRIVE };
static TInt RofsCount = 1;						// partitions (= drives)
static TInt RofsOffset[KMaxRofs], RofsLen[KMaxRofs];

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
	TBool ok = t[0] == 0x52425953 /*"SYBR"*/ && t[1] == 0x0053464f /*"OFS\0"*/ && t[2] > 0
			   && t[2] <= (TUint32)(KFlashWindow - 0x1000);
	RofsSize = ok ? (TInt)t[2] : 0;
	RofsCount = 1;
	RofsOffset[0] = 0;
	RofsLen[0] = RofsSize;
	if (ok && t[3] == 2 && t[4] >= 1 && t[4] <= (TUint32)KMaxRofs)	// version 2: one drive per image
		{
		RofsCount = t[4];
		for (TInt i = 0; i < RofsCount; ++i)
			{
			KDriveNumbers[i] = t[5 + 3*i];
			RofsOffset[i] = t[6 + 3*i];
			RofsLen[i] = t[7 + 3*i];
			if (RofsOffset[i] < 0 || RofsLen[i] <= 0 || RofsOffset[i] + RofsLen[i] > RofsSize)
				ok = EFalse;
			}
		}
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
		anInfo.iPartitionCount = RofsCount;
		anInfo.iMediaSizeInBytes = RofsSize;
		for (TInt i = 0; i < RofsCount; ++i)
			{
			anInfo.iEntry[i].iPartitionBaseAddr = RofsOffset[i];
			anInfo.iEntry[i].iPartitionLen = RofsLen[i];
			anInfo.iEntry[i].iPartitionType = KPartitionTypeRofs;
			}
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
	Kern::Printf("SYBROFS: %s base %08x size %08x -> %d image(s), local drive %d..", r == KErrNone ? "flash" : "NO ROFS in flash",
				 RofsBase, RofsSize, RofsCount, KDriveNumbers[0]);
	if (r != KErrNone)
		return KErrNone;	// nothing to serve; don't fail the boot
	DPrimaryMediaBase* pM = new DPrimaryMediaBase;
	if (!pM)
		return KErrNoMemory;
	return LocDrv::RegisterMediaDevice(EFixedMedia2, RofsCount, &KDriveNumbers[0], pM, 1, KDriveName);
	}
