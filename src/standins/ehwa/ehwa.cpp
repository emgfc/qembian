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
//   0x4000002  PROC load, N73 (a2 = &size): the image is in the client's buffer at a1 -> 0
//   0x4000002  PROC load, N95 (a2 = 0): returns a user handle of a chunk >= the base image size (client reads the
//              image into it and closes the handle)
//   0x400000b  PROC shared buffer: a1 = TPtr8 out, a2 = TBuf8<4>{size} (seen from eapgwc.dll; the
//              static analysis guessed 0x4000016, kept as an alias) -> chunk-backed TPtr8
//   4          Bal node create: *a2 = node token
//   5, 6, 0x8000007  Bal node start/stop/wait ready: complete the TRequestStatus* in a2 with 0
//   0x8000014/0x8000005/0x8000015  node send / listener / receive: the fake DSP task acks every
//              message (send completes 0, listener KErrCompletion, receive returns {cmd,0,0})
//   0x8000006  cancel the listener: completes it with KErrCancel [inferred from the run]
// Playback (raw PCM node, docs/nokia-dsp-playback.md): WAV, the startup tone and key tones arrive
// as PCM blocks on 0x800000e; they are copied into a physically contiguous ring and played by the
// syborg-audio device (src/qemu-syborg/hw/arm/syborg_audio.c). A 10 ms pump DFC paces the client
// (a block's status completes once the device has room), and after the end-of-stream block has
// played the node gets DSP message {8,0,0} = play complete. Raw-node commands (cmd 7 config,
// 3 start, 4 pause, 1 stop, 2 record) get no reply; only the send status completes.
//   others     return 0, write nothing (logged)
#include "nokiastub.h"
#include <memmodel/epoc/platform.h>
#include "syborg.h"

struct SDcdEntry { TUint8 iUuid[16]; TInt iType; TInt iPathLen; TUint16 iPath[256]; };	// path in 16-bit chars
struct SDcdRegisterBlock { TInt iCount; const TAny* iPath; TUint8 iUuid[64][16]; TInt iType[64]; };	// 0x508
const TInt KMaxDcd = 96;
static SDcdEntry* Dcd;			// KMaxDcd entries, allocated on first use
static TInt DcdCount;
static DChunk* LoadChunk;		// 0x4000002 upload chunk
static DChunk* BufChunk;		// 0x4000016 shared buffer
const TInt KMgrToken = 0x44434401;	// opaque DCD manager token (never used as a kernel handle)
const TInt KNodeToken = 0x4e4f0045;	// opaque Bal node tokens: KNodeToken + (index << 8); odd, so the
										// stub framework never tries to read memory at a token
const TInt KLoadChunkSize = 0x40000;
// per node (each Bal node create gets its own token; several EAP modules exist at once)
const TInt KMaxNodes = 16;
struct SNode
	{
	TRequestStatus* iListen;	// 0x8000005 listener, NULL when not armed
	TBool iReplyQueued;			// a message arrived while no listener was armed
	TUint32 iReply[3];			// raw payload handed back by 0x8000015
	DThread* iClient;			// thread that armed the listener (for DSP-initiated messages)
	TInt iRate, iChannels;		// raw PCM node format (from its EAP stream, see EapStreams)
	TInt iEapStream;			// ctl 4 config word c[2]: the EAP stream the node belongs to
	TBool iRaw;					// raw PCM node (commands without the 0x20000000 shared form)
	TInt iKind;					// ENodeOther / ENodeRaw / ENodeMp3 / ENodeAac / ENodeEaac
	};
// codec nodes by DSP node UUID (docs/nokia-dsp-playback.md sections 1 and 6)
enum { ENodeOther, ENodeRaw, ENodeMp3, ENodeAac, ENodeEaac };
static TUint32 LastDefUuid;		// first UUID word of the last 0x400001f get-def (precedes node create)
static TInt NodeKind(TUint32 aUuid0)
	{
	switch (aUuid0)
		{
	case 0xc36a3ebc: return ENodeRaw;
	case 0xb11a1f21: return ENodeMp3;
	case 0x5c89a1f1: return ENodeAac;
	case 0x8468856b: return ENodeEaac;
		}
	return ENodeOther;
	}
// device format, stop command and "stopped" DSP message per codec node
static TInt KindFormat(TInt aKind) { return aKind == ENodeMp3 ? 2 : (aKind == ENodeAac || aKind == ENodeEaac) ? 3 : 0; }
static TUint32 KindStopCmd(TInt aKind) { return (aKind == ENodeMp3 || aKind == ENodeAac) ? 6 : 1; }
static TUint32 KindStoppedMsg(TInt aKind) { return (aKind == ENodeMp3 || aKind == ENodeAac) ? 4 : 8; }
static SNode Nodes[KMaxNodes];
static TInt NodeCount;

static SNode* Node(TAny* aToken)
	{
	TInt d = (TInt)aToken - KNodeToken;
	TInt i = d >> 8;
	return (d & 0xff) == 0 && i >= 0 && i < KMaxNodes ? &Nodes[i] : NULL;
	}	// >= baseimage.dof (155756 bytes)


// ---- playback engine ------------------------------------------------------------------------
// syborg-audio registers (word index), see syborg_audio.c
enum { KAuCtrl = 1, KAuFormat = 2, KAuRate = 3, KAuChannels = 4, KAuBufAddr = 5, KAuBufLen = 6,
	   KAuSubmit = 7, KAuStatus = 8, KAuVolume = 14, KAuQueueFree = 15, KAuDone = 16 };
const TInt KSlots = 8, KSlotSize = 0x4000;		// 128 KB ring of PCM slots
const TInt KAheadSlots = 3;						// complete a client block once <= this many are queued
const TInt KMaxReq = 8;
struct SPcmReq
	{
	DThread* iClient;
	TRequestStatus* iStatus;
	TLinAddr iPtr;			// payload (after the 4-byte header) in the client
	TInt iLen, iCopied;
	TBool iEos;
	SNode* iNode;
	};
static SPcmReq Req[KMaxReq];
static TInt ReqHead, ReqCount;
static TBool CancelAll;
static DPlatChunkHw* Ring;
static TPhysAddr RingPhys;
static TUint8* RingLin;
static TInt SlotNext, Inflight;
static TUint32 LastDone;
static TBool Playing;
static SNode* EosNode;			// waiting for the device's end of stream, then {8,0,0}
static SNode* StopNode;			// stopped by the client (cmd 1): answer {8,0,0} after the cancel
static TDfc* PumpDfc;
static NTimer* PumpTimer;

static void Au(TInt aReg, TUint32 aVal) { WriteReg(KHwSVPAudioDevice, aReg, aVal); }
static TUint32 AuR(TInt aReg) { return ReadReg(KHwSVPAudioDevice, aReg); }

// EAP streams (docs/research/dsp-pcm-format.md): before a raw PCM node is created, eapcontrolmodule
// sends EAP message 0 (16 bytes, eapgwc_ord8) {0, stream id, mode, rate code, stereo, port count,
// ports...} to the EAP control node; the raw node's create config word c[2] is that stream id.
// The rate code is the N95's own DSP numbering, eapcontrolmodule FUN_80a0f7c4 (switch on the
// AudioServer rate enum): 3 = 48000 Hz ... 11 = 8000 Hz, 14 = 48000 Hz (second 48 kHz mode),
// 15 = not supported.
const TInt KMaxEapStreams = 16;
struct SEapStream { TInt iId; TInt iRate; TInt iChannels; };
static SEapStream EapStreams[KMaxEapStreams];
static TInt EapStreamNext;

static TInt EapRateHz(TInt aCode)
	{
	static const TInt KHz[] = { 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };	// codes 3..11
	if (aCode >= 3 && aCode <= 11)
		return KHz[aCode - 3];
	return aCode == 14 ? 48000 : 0;
	}

static void EapStreamSeen(const TUint16* aMsg)
	{
	TInt id = aMsg[1], rate = EapRateHz(aMsg[3]), ch = aMsg[4] ? 2 : 1;
	TInt slot = -1;
	for (TInt i = 0; i < KMaxEapStreams; ++i)
		if (EapStreams[i].iId == id && EapStreams[i].iRate)
			slot = i;
	if (slot < 0)
		{
		slot = EapStreamNext;
		EapStreamNext = (EapStreamNext + 1) % KMaxEapStreams;
		}
	EapStreams[slot].iId = id;
	EapStreams[slot].iRate = rate;
	EapStreams[slot].iChannels = ch;
	Kern::Printf("N95STUB: Hwa EAP stream %d: rate code %d -> %d Hz, %s, %d port(s)", id, aMsg[3], rate,
				 ch == 2 ? "stereo" : "mono", aMsg[5]);
	}

static const SEapStream* EapStream(TInt aId)
	{
	for (TInt i = 0; i < KMaxEapStreams; ++i)
		if (EapStreams[i].iId == aId && EapStreams[i].iRate)
			return &EapStreams[i];
	return NULL;
	}

static TInt MapRate(TInt aMask)
	{
	static const TInt KRates[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000,
								   12000, 24000, 64000 };
	for (TInt i = 0; i < 12; ++i)
		if (aMask & (1 << i))
			return KRates[i];
	return 8000;
	}

static void DspMessage(SNode* aNode, TUint32 aCmd)
	{
	// a DSP-initiated node message: delivered through the armed listener / 0x8000015
	aNode->iReply[0] = aCmd;
	aNode->iReply[1] = 0;
	aNode->iReply[2] = 0;
	if (aNode->iListen && aNode->iClient)
		Kern::RequestComplete(aNode->iClient, aNode->iListen, KErrCompletion);
	else
		aNode->iReplyQueued = ETrue;
	}

static void Pump(TAny*)
	{
	if (!Ring)
		return;
	TUint32 done = AuR(KAuDone);
	Inflight -= (TInt)(done - LastDone);
	if (Inflight < 0)
		Inflight = 0;
	LastDone = done;
	if (CancelAll)
		{
		CancelAll = EFalse;
		Au(KAuCtrl, 2);					// stop: drop the device queue
		Playing = EFalse;
		Inflight = 0;
		LastDone = AuR(KAuDone);
		EosNode = NULL;
		while (ReqCount)
			{
			SPcmReq& r = Req[ReqHead];
			Kern::RequestComplete(r.iClient, r.iStatus, KErrCancel);
			ReqHead = (ReqHead + 1) % KMaxReq;
			--ReqCount;
			}
		}
	if (StopNode)
		{
		Kern::Printf("N95STUB: Hwa play stopped");
		DspMessage(StopNode, KindStoppedMsg(StopNode->iKind));	// stopped node: {8} raw/eAAC+, {4} MP3/AAC
		StopNode = NULL;
		}
	while (ReqCount)
		{
		SPcmReq& r = Req[ReqHead];
		if (!Playing)
			{
			Au(KAuFormat, KindFormat(r.iNode->iKind));
			Au(KAuRate, r.iNode->iRate);
			Au(KAuChannels, r.iNode->iChannels);
			Au(KAuCtrl, 1);
			Playing = ETrue;
			Kern::Printf("N95STUB: Hwa play kind %d format %d (%d Hz %d ch)", r.iNode->iKind,
						 KindFormat(r.iNode->iKind), r.iNode->iRate, r.iNode->iChannels);
			}
		while (r.iCopied < r.iLen && Inflight < KSlots)
			{
			TInt n = Min(KSlotSize, r.iLen - r.iCopied);
			TUint8* slot = RingLin + SlotNext * KSlotSize;
			if (Kern::ThreadRawRead(r.iClient, (const TAny*)(r.iPtr + r.iCopied), slot, n) != KErrNone)
				{
				r.iCopied = r.iLen;
				break;
				}
			Au(KAuBufAddr, RingPhys + SlotNext * KSlotSize);
			Au(KAuBufLen, n);
			Au(KAuSubmit, 1);
			SlotNext = (SlotNext + 1) % KSlots;
			++Inflight;
			r.iCopied += n;
			}
		if (r.iCopied < r.iLen || Inflight > KAheadSlots)
			break;						// wait for the device to play some of it
		if (r.iEos)
			{
			Au(KAuCtrl, 4);				// end: report once everything queued has played
			EosNode = r.iNode;
			Kern::Printf("N95STUB: Hwa end of stream queued (inflight %d)", Inflight);
			}
		Kern::RequestComplete(r.iClient, r.iStatus, KErrNone);	// "buffer consumed"
		ReqHead = (ReqHead + 1) % KMaxReq;
		--ReqCount;
		}
	if (EosNode && (AuR(KAuStatus) & 2))
		{
		Kern::Printf("N95STUB: Hwa play complete");
		DspMessage(EosNode, KindStoppedMsg(EosNode->iKind));	// play complete: {8} raw/eAAC+, {4} MP3/AAC
		EosNode = NULL;
		Playing = EFalse;
		}
	if (ReqCount || Inflight || EosNode)
		PumpTimer->OneShot(NKern::TimerTicks(10), *PumpDfc);
	}

static TInt PlaybackInit()
	{
	if (Ring)
		return KErrNone;
	NKern::ThreadEnterCS();
	TInt r = Epoc::AllocPhysicalRam(KSlots * KSlotSize, RingPhys, 12);
	if (r == KErrNone)
		r = DPlatChunkHw::New(Ring, RingPhys, KSlots * KSlotSize, EMapAttrSupRw | EMapAttrFullyBlocking);
	if (r == KErrNone)
		{
		PumpDfc = new TDfc(Pump, NULL, Kern::DfcQue0(), 1);
		PumpTimer = new NTimer;
		if (!PumpDfc || !PumpTimer)
			r = KErrNoMemory;
		}
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		{
		Kern::Printf("N95STUB: Hwa playback init failed %d", r);
		Ring = NULL;
		return r;
		}
	RingLin = (TUint8*)Ring->LinearAddress();
	Au(KAuVolume, 200);
	LastDone = AuR(KAuDone);
	Kern::Printf("N95STUB: Hwa playback ring %d KB at phys %08x, audio device id %08x",
				 KSlots * KSlotSize / 1024, RingPhys, AuR(0));
	return KErrNone;
	}

static void Kick()
	{
	PumpDfc->Enque();
	}

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
		LastDefUuid = uuid[0] | (uuid[1] << 8) | (uuid[2] << 16) | (uuid[3] << 24);
		TInt k = Dcd ? DcdFind(uuid, type) : KErrNotFound;
		if (k < 0)
			return KErrArgument;
		TPtrC8 bytes((const TUint8*)Dcd[k].iPath, Dcd[k].iPathLen);	// length in characters (shift 1)
		return Kern::ThreadDesWrite(aClient, (TUint8*)a1 + 0x14, bytes, 0, KChunkShiftBy1, aClient);
		}
	case 0x4000002:	// PROC load: chunk for the base image
		{
		if (a2)		// N73 (older bridgeapi, OMAP1710): a1 = the whole baseimage.dof already in a user
			{		// buffer, a2 = &size; PROC_Load takes any non-zero result as a failure
					// (docs/devices/nokia-n73-rm132/dspbridge-n73.md). The N95 passes a2 = 0.
			TInt size = 0;
			Kern::ThreadRawRead(aClient, a2, &size, sizeof(size));
			Kern::Printf("N95STUB: Hwa base image %d bytes in the client's buffer %08x (loaded)", size, a1);
			return KErrNone;
			}
		TLinAddr base = 0;
		TInt size = (TInt)w[0] > KLoadChunkSize ? (TInt)w[0] : KLoadChunkSize;
		TInt h = NewChunk(aClient, size, LoadChunk, base);
		Kern::Printf("N95STUB: Hwa image chunk %d bytes at %08x -> handle %08x", size, base, h);
		return h;
		}
	case 0x400000a:	// the N73's number for it (older bridgeapi; never issued by the N95's), same layout
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
		TInt i = NodeCount < KMaxNodes ? NodeCount++ : KMaxNodes - 1;	// reuse the last slot if full
		Nodes[i].iListen = NULL;
		Nodes[i].iReplyQueued = EFalse;
		Nodes[i].iRaw = EFalse;
		Nodes[i].iKind = NodeKind(LastDefUuid);
		// a1 -> TBuf8<0x14>{p1 (rate mask), p0, node struct*, p2 (encoding), p3 (channels)}
		TUint32 c[7] = {0, 0, 0, 0, 0, 0, 0};
		Kern::ThreadRawRead(aClient, a1, c, sizeof(c));
		Nodes[i].iRate = MapRate(c[2]);
		Nodes[i].iChannels = c[6] == 2 ? 2 : 1;
		Nodes[i].iEapStream = c[2];
		if (Nodes[i].iKind == ENodeRaw)
			{
			// the raw PCM node's format is its EAP stream's (EAP message 0 above); cmd 7 later gives
			// the channel layout again
			const SEapStream* s = EapStream(c[2]);
			Nodes[i].iRate = s ? s->iRate : 44100;
			Nodes[i].iChannels = s ? s->iChannels : 2;
			if (!s)
				Kern::Printf("N95STUB: Hwa raw node %d: no EAP stream %d seen, assuming 44100 Hz stereo", i, c[2]);
			}
		Kern::Printf("N95STUB: Hwa node %d kind %d (uuid %08x) config %08x %08x %08x %08x %08x -> %d Hz %d ch",
					 i, Nodes[i].iKind, LastDefUuid, c[2], c[3], c[4], c[5], c[6], Nodes[i].iRate, Nodes[i].iChannels);
		TInt tok = KNodeToken + (i << 8);
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
	case 0x8000005:	// Bal node message listener: a2 -> { TRequestStatus*, 0 }; pending until a reply
		{
		TUint32 l[2] = {0, 0};
		Kern::ThreadRawRead(aClient, a2, l, sizeof(l));
		SNode* n = Node(a1);
		if (!n)
			return KErrArgument;
		n->iListen = (TRequestStatus*)l[0];
		n->iClient = aClient;
		if (n->iReplyQueued && n->iListen)
			{
			n->iReplyQueued = EFalse;
			Kern::RequestComplete(aClient, n->iListen, KErrCompletion);	// "message waiting"
			}
		return KErrNone;
		}
	case 0x8000014:	// Bal send: a2 -> { TRequestStatus* send, TBuf8<12>* msg {cmd, arg1, arg2} }
		{
		// The fake DSP task acks every message: send completes 0, the listener completes with
		// KErrCompletion, and 0x8000015 hands back a raw 12-byte payload {reply cmd, 0, 0}
		// (eapgwc wait loop FUN_80a0cd64; docs/nokia-dspbridge.md section 5)
		TUint32 b[2] = {0, 0}, m[5] = {0, 0, 0, 0, 0};
		Kern::ThreadRawRead(aClient, a2, b, sizeof(b));
		Kern::ThreadRawRead(aClient, (TAny*)b[1], m, sizeof(m));	// len, max, cmd, arg1, arg2
		TUint16 eapCmd = 0;
		if (m[2] == 0x20000000 && m[3])
			Kern::ThreadRawRead(aClient, (TAny*)m[3], &eapCmd, 2);	// payload u16[0]
		Kern::Printf("N95STUB: Hwa node send cmd %08x ptr %08x len %d eap %d", m[2], m[3], m[4], eapCmd);
		if (m[2] == 0x20000000 && m[3] && m[4] && eapCmd != 1)
			{
			TUint16 pl[40];
			TInt nb = Min((TInt)m[4], (TInt)sizeof(pl));
			Kern::ThreadRawRead(aClient, (TAny*)m[3], pl, nb);
			if (eapCmd == 0 && m[4] == 16)		// EAP 0 "create stream": the PCM format
				EapStreamSeen(pl);
			for (TInt j = 0; j < nb / 2; j += 10)
				Kern::Printf("N95STUB: Hwa eap %d +%02d: %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x", eapCmd, j * 2,
							 pl[j], pl[j+1], pl[j+2], pl[j+3], pl[j+4], pl[j+5], pl[j+6], pl[j+7], pl[j+8], pl[j+9]);
			}
		SNode* n = Node(a1);
		if (!n)
			return KErrArgument;
		if (m[2] != 0x20000000)
			{
			// raw PCM node command (7 config, 3 start, 4 pause, 1 stop, 2 record): no DSP reply
			n->iRaw = ETrue;
			if (m[2] == 7 && n->iKind == ENodeRaw)
				{
				// RawGwc config: arg2 = channel layout (RawGwc_ord2: 1 mono, 2 interleaved stereo,
				// 3 non-interleaved); the rate is the EAP stream's
				const SEapStream* s = EapStream(n->iEapStream);
				if (s)
					n->iRate = s->iRate;
				n->iChannels = m[4] == 1 ? 1 : 2;
				Kern::Printf("N95STUB: Hwa raw node format: EAP stream %d, layout %d -> %d Hz %d ch%s", n->iEapStream,
							 m[4], n->iRate, n->iChannels, m[4] == 3 ? " (non-interleaved: played as interleaved)" : "");
				}
			TRequestStatus* send = (TRequestStatus*)b[0];
			if (send)
				Kern::RequestComplete(aClient, send, KErrNone);
			if (m[2] == KindStopCmd(n->iKind) && Ring)
				{
				StopNode = n;
				CancelAll = ETrue;
				Kick();
				}
			return KErrNone;
			}
		n->iReply[0] = (eapCmd == 4) ? 4 : 0;	// u16[0] = reply command (0 = ack, 4 = query answer)
		n->iReply[1] = 0;
		n->iReply[2] = 0;
		TRequestStatus* send = (TRequestStatus*)b[0];
		if (send)
			Kern::RequestComplete(aClient, send, KErrNone);
		if (n->iListen)
			Kern::RequestComplete(aClient, n->iListen, KErrCompletion);	// nulls iListen
		else
			n->iReplyQueued = ETrue;
		return KErrNone;
		}
	case 0x8000006:	// Bal cancel node listener: complete the armed listener with KErrCancel
		{
		SNode* n = Node(a1);
		if (n)
			{
			if (n->iListen)
				Kern::RequestComplete(aClient, n->iListen, KErrCancel);
			n->iReplyQueued = EFalse;
			}
		}
		return KErrNone;
	case 0x800000e:	// PCM block: a2 -> {TPtr8 type|len, max, ptr, TRequestStatus*, len, &out, stream}
		{
		TUint32 b[7] = {0, 0, 0, 0, 0, 0, 0};
		Kern::ThreadRawRead(aClient, a2, b, sizeof(b));
		SNode* n = Node(a1);
		TRequestStatus* st = (TRequestStatus*)b[3];
		TInt len = b[0] & 0x0fffffff;
		TUint8 hdr[4] = {0, 0, 0, 0};
		if (len >= 4)
			Kern::ThreadRawRead(aClient, (const TAny*)b[2], hdr, 4);
		if (!n || PlaybackInit() != KErrNone || ReqCount == KMaxReq || len < 4)
			{
			if (st)
				Kern::RequestComplete(aClient, st, KErrNone);
			return KErrNone;
			}
		NKern::Lock();
		SPcmReq& r = Req[(ReqHead + ReqCount) % KMaxReq];
		r.iClient = aClient;
		r.iStatus = st;
		r.iPtr = b[2] + 4;
		r.iLen = len - 4;
		r.iCopied = 0;
		r.iEos = n->iKind == ENodeAac ? hdr[2] == 1 : (hdr[3] & 0x10) != 0;
		if (n->iKind == ENodeAac && hdr[2] == 2)
			r.iLen = 0;					// AAC concealment (header-only) block: nothing to play
		if (r.iEos || len <= 4)
			Kern::Printf("N95STUB: Hwa PCM block len %d hdr %02x %02x %02x %02x words %08x %08x %08x", len,
						 hdr[0], hdr[1], hdr[2], hdr[3], b[4], b[5], b[6]);
		r.iNode = n;
		++ReqCount;
		NKern::Unlock();
		Kick();
		return KErrNone;
		}
	case 0x800000f:	// cancel PCM issue: pending blocks complete KErrCancel, device stops
		if (Ring)
			{
			CancelAll = ETrue;
			Kick();
			}
		return KErrNone;
	case 0x8000015:	// Bal receive: a2 -> { TRequestStatus* rx, TBuf8<12>* out }
		{
		TUint32 b[2] = {0, 0};
		Kern::ThreadRawRead(aClient, a2, b, sizeof(b));
		SNode* n = Node(a1);
		static const TUint32 KAck[3] = {0, 0, 0};
		TInt r = Kern::ThreadRawWrite(aClient, (TUint8*)b[1] + 8, n ? n->iReply : KAck, sizeof(KAck));
		TRequestStatus* rx = (TRequestStatus*)b[0];
		if (rx)
			Kern::RequestComplete(aClient, rx, r);
		return KErrNone;
		}
		}
	return KStubNotHandled;
	}

NO_CONTROLS
NOKIA_STUB_LDD_CTLHOOK("Hwa", KCtl, ETrue, HwaCtl)
