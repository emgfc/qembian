// N95 ehwa.ldd stand-in (see nokiastub.h). "Hwa" = TI DSP/BIOS bridge driver used by the audio
// stack (Bal.dll -> bridgeapi.dll) in MmfAudioServer's resource-manager thread. Hack H-dspbridge
// (docs/hacks.md). There is no DSP: the stand-in answers just enough for MmRmServerThread's init
// (docs/nokia-dspbridge.md), so MmfAudioServer starts (Phone.exe and EikSrvs' key-sound server need
// it). No audio.
//
//   0x400001e  DCD create manager {mutex, out mgr, thread id, out char step}: mgr token, step 2
//   0x4000020  DCD register {count, TDesC16* path, uuid[64][16], type[64]}: remembered
//   0x4000021  DCD unregister
//   0x4000022  DCD enumerate {index, out uuid, type}
//   0x400001f  DCD get object def {uuid, type, TBuf16<256> out path}
//   0x4000002  PROC load: returns a user handle of a chunk >= the base image size (client reads the
//              image into it and closes the handle)
//   0x400000b  PROC shared buffer: a1 = TPtr8 out, a2 = TBuf8<4>{size} (seen from eapgwc.dll; the
//              static analysis guessed 0x4000016, kept as an alias) -> chunk-backed TPtr8
//   4          Bal node create: *a2 = node token
//   5, 6, 0x8000007  Bal node start/stop/wait ready: complete the TRequestStatus* in a2 with 0
//   0x8000005  Bal node message listener: stays pending (no DSP messages)
//   others     return 0, write nothing (logged)
#include "nokiastub.h"
#include <memmodel/epoc/platform.h>

struct SDcdEntry { TUint8 iUuid[16]; TInt iType; TInt iPathLen; TUint16 iPath[256]; };	// path in 16-bit chars
struct SDcdRegisterBlock { TInt iCount; const TAny* iPath; TUint8 iUuid[64][16]; TInt iType[64]; };	// 0x508
const TInt KMaxDcd = 96;
static SDcdEntry* Dcd;			// KMaxDcd entries, allocated on first use
static TInt DcdCount;
static DChunk* LoadChunk;		// 0x4000002 upload chunk
static DChunk* BufChunk;		// 0x4000016 shared buffer
const TInt KMgrToken = 0x44434401;	// opaque DCD manager token (never used as a kernel handle)
const TInt KNodeToken = 0x4e4f4445;	// opaque Bal node token
const TInt KLoadChunkSize = 0x40000;	// >= baseimage.dof (155756 bytes)

static TBool SameUuid(const TUint8* a, const TUint8* b)
	{
	for (TInt i = 0; i < 16; ++i)
		if (a[i] != b[i])
			return EFalse;
	return ETrue;
	}

static TInt DcdFind(const TUint8* aUuid, TInt aType)
	{
	for (TInt i = 0; i < DcdCount; ++i)
		if (Dcd[i].iType == aType && SameUuid(Dcd[i].iUuid, aUuid))
			return i;
	return KErrNotFound;
	}

// a shared chunk mapped into the client; returns the client's handle (or an error)
static TInt NewChunk(DThread* aClient, TInt aSize, DChunk*& aChunk, TLinAddr& aUserBase)
	{
	NKern::ThreadEnterCS();
	if (aChunk)
		{
		Kern::ChunkClose(aChunk);
		aChunk = NULL;
		}
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = Kern::RoundToPageSize(aSize);
	info.iMapAttr = EMapAttrCachedMax;
	info.iOwnsMemory = ETrue;
	info.iDestroyedDfc = NULL;
	TLinAddr kaddr;
	TUint32 attr;
	TInt r = Kern::ChunkCreate(info, aChunk, kaddr, attr);
	if (r == KErrNone)
		r = Kern::ChunkCommit(aChunk, 0, info.iMaxSize);
	if (r == KErrNone)
		r = Kern::MakeHandleAndOpen(aClient, reinterpret_cast<DObject*>(aChunk));
	if (r < 0 && aChunk)
		{
		Kern::ChunkClose(aChunk);
		aChunk = NULL;
		}
	if (r >= 0)
		aUserBase = (TLinAddr)Kern::ChunkUserBase(aChunk, aClient);
	NKern::ThreadLeaveCS();
	return r;
	}

static TInt DcdRegister(DThread* aClient, TAny* a1)
	{
	NKern::ThreadEnterCS();
	SDcdRegisterBlock* b = new SDcdRegisterBlock;	// 0x508 bytes: not on the kernel stack
	if (!Dcd)
		Dcd = new SDcdEntry[KMaxDcd];
	NKern::ThreadLeaveCS();
	if (!b || !Dcd)
		return KErrNoMemory;
	TInt r = Kern::ThreadRawRead(aClient, a1, b, sizeof(*b));
	// 16-bit user descriptor: with KChunkShiftBy1 the kernel counts lengths in characters (the
	// TPtr8's length/max length are characters, its buffer holds 2 bytes each)
	TUint16 path16[256];
	TPtr8 path8((TUint8*)path16, 0, 256);
	if (r == KErrNone)
		r = Kern::ThreadDesRead(aClient, b->iPath, path8, 0, KChunkShiftBy1);
	for (TInt i = 0; r == KErrNone && i < b->iCount && i < 64; ++i)
		{
		TInt k = DcdFind(b->iUuid[i], b->iType[i]);
		if (k < 0)
			{
			if (DcdCount >= KMaxDcd)
				{
				r = KErrOverflow;
				break;
				}
			k = DcdCount++;
			}
		memcpy(Dcd[k].iUuid, b->iUuid[i], 16);
		Dcd[k].iType = b->iType[i];
		Dcd[k].iPathLen = path8.Length();
		memcpy(Dcd[k].iPath, path16, Dcd[k].iPathLen * 2);
		}
	TInt n = b->iCount;
	NKern::ThreadEnterCS();
	delete b;
	NKern::ThreadLeaveCS();
	Kern::Printf("N95STUB: Hwa DCD register %d objects -> %d (%d known)", n, r, DcdCount);
	return r;
	}

static TInt HwaCtl(DThread* aClient, TInt aFn, TAny* a1, TAny* a2, const TUint32* w)
	{
	switch (aFn)
		{
	case 0x400001e:	// DCD create manager
		{
		TInt mgr = KMgrToken, step = 2;
		Kern::ThreadRawWrite(aClient, (TUint8*)a1 + 4, &mgr, 4);
		Kern::ThreadRawWrite(aClient, (TUint8*)a1 + 12, &step, 4);
		return KErrNone;
		}
	case 0x4000020:
		return DcdRegister(aClient, a1);
	case 0x4000021:	// DCD unregister {1, 0, uuid, type at +0x408}
		{
		TUint8 uuid[16];
		TInt type = -1;
		Kern::ThreadRawRead(aClient, (TUint8*)a1 + 8, uuid, 16);
		Kern::ThreadRawRead(aClient, (TUint8*)a1 + 0x408, &type, 4);
		TInt k = Dcd ? DcdFind(uuid, type) : KErrNotFound;
		if (k >= 0)
			Dcd[k] = Dcd[--DcdCount];
		return KErrNone;
		}
	case 0x4000022:	// DCD enumerate {index, out uuid[16], type}
		{
		TInt index = w[0], type = -1, n = 0;
		Kern::ThreadRawRead(aClient, (TUint8*)a1 + 20, &type, 4);
		for (TInt i = 0; i < DcdCount; ++i)
			if (Dcd[i].iType == type && n++ == index)
				return Kern::ThreadRawWrite(aClient, (TUint8*)a1 + 4, Dcd[i].iUuid, 16);
		return KErrNotFound;
		}
	case 0x400001f:	// DCD get object def {uuid[16], type, TBuf16<256> path at +0x14}
		{
		TUint8 uuid[16];
		TInt type = -1;
		Kern::ThreadRawRead(aClient, a1, uuid, 16);
		Kern::ThreadRawRead(aClient, (TUint8*)a1 + 16, &type, 4);
		TInt k = Dcd ? DcdFind(uuid, type) : KErrNotFound;
		if (k < 0)
			return KErrArgument;
		TPtrC8 bytes((const TUint8*)Dcd[k].iPath, Dcd[k].iPathLen);	// length in characters (shift 1)
		return Kern::ThreadDesWrite(aClient, (TUint8*)a1 + 0x14, bytes, 0, KChunkShiftBy1, aClient);
		}
	case 0x4000002:	// PROC load: chunk for the base image
		{
		TLinAddr base = 0;
		TInt size = (TInt)w[0] > KLoadChunkSize ? (TInt)w[0] : KLoadChunkSize;
		TInt h = NewChunk(aClient, size, LoadChunk, base);
		Kern::Printf("N95STUB: Hwa image chunk %d bytes at %08x -> handle %08x", size, base, h);
		return h;
		}
	case 0x400000b:	// PROC shared buffer: a1 = TPtr8 out, a2 = TBuf8<4>{size}
	case 0x4000016:
		{
		TInt size = 0;
		TLinAddr base = 0;
		Kern::ThreadRawRead(aClient, (TUint8*)a2 + 8, &size, 4);	// TBuf8<4>: {typelen, maxlen, data}
		TInt h = NewChunk(aClient, size > 0 ? size : 0x1000, BufChunk, base);
		Kern::Printf("N95STUB: Hwa shared buffer %d bytes at %08x -> %d", size, base, h);
		if (h < 0)
			return h;
		TUint32 ptr[3] = { 0x20000000u | (TUint32)size, (TUint32)size, (TUint32)base };	// TPtr8
		return Kern::ThreadRawWrite(aClient, a1, ptr, sizeof(ptr));
		}
	case 4:			// Bal node create: *a2 = node token
		{
		TInt tok = KNodeToken;
		return Kern::ThreadRawWrite(aClient, a2, &tok, 4);
		}
	case 5:			// Bal node start
	case 6:			// Bal node stop
	case 0x8000007:	// Bal wait node ready
		{
		TRequestStatus* s = (TRequestStatus*)a2;
		if (s)
			Kern::RequestComplete(aClient, s, KErrNone);
		return KErrNone;
		}
	case 0x8000005:	// Bal node message listener: stays pending
		{
		TUint32 l[2] = {0, 0};
		Kern::ThreadRawRead(aClient, a2, l, sizeof(l));
		Kern::Printf("N95STUB: Hwa node listener status %08x arg %08x (pending)", l[0], l[1]);
		return KErrNone;
		}
	case 0x8000014:	// Bal send message to node: log it (spec for a DSP-side reply)
		{
		TUint32 m[8] = {0};
		Kern::ThreadRawRead(aClient, a2, m, sizeof(m));
		Kern::Printf("N95STUB: Hwa node message %08x %08x %08x %08x %08x %08x %08x %08x",
					 m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
		return KErrNone;
		}
		}
	return KStubNotHandled;
	}

NO_CONTROLS
NOKIA_STUB_LDD_CTLHOOK("Hwa", KCtl, ETrue, HwaCtl)
