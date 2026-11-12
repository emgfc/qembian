// N95 ehwa.ldd stand-in (see nokiastub.h). "Hwa" = TI DSP/BIOS bridge driver used by the audio
// stack (Bal.dll -> bridgeapi.dll) in MmfAudioServer's resource-manager thread. Hack H-dspbridge
// (docs/hacks.md). There is no DSP: the stand-in answers just enough for MmRmServerThread's init
// (docs/devices/nokia-n95-8gb-rm320/nokia-dspbridge.md), so MmfAudioServer starts (Phone.exe and EikSrvs' key-sound server need
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
// Playback (docs/devices/nokia-n95-8gb-rm320/nokia-dsp-playback.md): WAV, the startup tone and key tones (raw
// PCM nodes) and MP3/AAC (codec nodes) arrive as blocks on 0x800000e; each node has its own ring and its
// own syborg-snd slot (src/qemu-syborg/hw/arm/syborg_audio.c), the host mixes the slots. The slot's
// watermark interrupt paces the client (a block's status completes once it is in the ring and at most
// half the ring is queued), and the slot's EOS interrupt gives that node DSP message {8,0,0} / {4,0,0}
// = play complete. Raw-node commands (cmd 7 config,
// 3 start, 4 pause, 1 stop, 2 record) get no reply; only the send status completes.
//   others     return 0, write nothing (logged)
#include "nokiastub.h"
#include "boardconfig.h"

// The bridge protocol variant this phone's bridgeapi.dll speaks, declared in the device profile:
// [[standins]] ehwa config = { bridge = "omap1710" } -> board config "ehwa.bridge" (default "omap2420",
// the N95's). The stand-in runs exactly that variant.
static TBool Bridge1710()
	{
	static TInt v = -1;
	if (v < 0)
		{
		char b[16];
		v = BoardConfig("ehwa.bridge", b, sizeof(b)) > 0 && b[4] == '1' && b[5] == '7';	// "omap1710"
		Kern::Printf("SYBSTUB: Hwa bridge variant %s", v ? "omap1710" : "omap2420");
		}
	return v;
	}
#include <memmodel/epoc/platform.h>
#include "syborg.h"
#include "sybsnd.h"

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
const TInt KMaxNodes = 15;			// one syborg-snd slot each (slot 15 is the device's capture slot)
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
	// playback: the node's own syborg-snd slot and ring (allocated at its first block)
	DPlatChunkHw* iRing;
	TPhysAddr iRingPhys;
	TUint8* iRingLin;
	TUint32 iWpos;				// bytes written to the slot (monotonic)
	TBool iRunning;				// slot started for the current stream
	TBool iEosWait;				// EOS given to the device: send the "stopped" message when it is reached
	TBool iCancel, iStopMsg;	// cancel pending blocks / then answer the stop command
	};
// codec nodes by DSP node UUID (docs/devices/nokia-n95-8gb-rm320/nokia-dsp-playback.md sections 1 and 6)
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
// codec nodes get their bitstream in the DSP's 16-bit big-endian words (MP3 frame sync arrives as fb ff) [V MP3]
static TInt KindFormat(TInt aKind) { return aKind == ENodeMp3 ? SybSnd::FMp3 | SybSnd::FSwap16 : (aKind == ENodeAac || aKind == ENodeEaac) ? SybSnd::FAac | SybSnd::FSwap16 : SybSnd::FS16; }
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
// Every Bal node that receives blocks (raw PCM: key tones, WAV, jingle; MP3/AAC/eAAC+ nodes) plays through
// its own syborg-snd slot (src/standins/_shared/sybsnd.h): a 64 KB ring per node, the host mixes the
// slots. A block's status completes once its bytes are in the ring and at most half the ring is still
// queued (the slot's watermark interrupt), the end-of-stream block arms the slot's EOS, and the slot's
// EOS interrupt sends that node its own "play complete" message. Several nodes play and end in any order.
const TInt KRingSize = 0x10000;
const TInt KMaxReq = 8;					// pending blocks per node
struct SPcmReq
	{
	DThread* iClient;
	TRequestStatus* iStatus;
	TLinAddr iPtr;			// payload (after the 4-byte header) in the client
	TInt iLen, iCopied;
	TBool iEos;
	TInt iErr;				// completion code (KErrNone unless the client's buffer could not be read)
	};
struct SNodeQueue { SPcmReq iReq[KMaxReq]; TInt iHead, iCount; };
static SNodeQueue Queues[KMaxNodes];

// Queue transactions: the client threads produce (control calls), the pump DFC consumes; reserve+publish and pop are
// each one step with the kernel locked. A queued request holds a reference on its client thread, dropped when the
// request is completed, so a client dying with blocks queued leaves no dangling pointer.
static TBool QueuePush(SNodeQueue& q, const SPcmReq& aReq)
	{
	NKern::Lock();
	TBool ok = q.iCount < KMaxReq;
	if (ok)
		{
		q.iReq[(q.iHead + q.iCount) % KMaxReq] = aReq;
		++q.iCount;
		}
	NKern::Unlock();
	return ok;
	}

static void QueuePop(SNodeQueue& q)
	{
	NKern::Lock();
	q.iHead = (q.iHead + 1) % KMaxReq;
	--q.iCount;
	NKern::Unlock();
	}

static void ReqComplete(SPcmReq& r, TInt aErr)
	{
	if (r.iStatus)
		Kern::RequestComplete(r.iClient, r.iStatus, aErr);
	NKern::ThreadEnterCS();
	r.iClient->Close(NULL);
	NKern::ThreadLeaveCS();
	r.iClient = NULL;
	}
static TBool SndInit;					// device checked, IRQ bound
static TBool SndPresent;
static TDfc* PumpDfc;
static NTimer* PumpTimer;
static volatile TUint32 IrqPending;

static TInt NodeSlot(TInt aNode) { return aNode; }

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
	Kern::Printf("SYBSTUB: Hwa EAP stream %d: rate code %d -> %d Hz, %s, %d port(s)", id, aMsg[3], rate,
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

// copy the head block(s) of node i into its ring; complete what is in and played down to half a ring
static void PumpNode(TInt i)
	{
	using namespace SybSnd;
	SNode* n = &Nodes[i];
	SNodeQueue& q = Queues[i];
	TInt k = NodeSlot(i);
	if (n->iCancel)
		{
		n->iCancel = EFalse;
		S(k, SCtrl) = CStop;
		n->iRunning = EFalse;
		n->iEosWait = EFalse;
		while (q.iCount)
			{
			ReqComplete(q.iReq[q.iHead], KErrCancel);
			QueuePop(q);
			}
		}
	if (n->iStopMsg)
		{
		n->iStopMsg = EFalse;
		Kern::Printf("SYBSTUB: Hwa node %d play stopped", i);
		DspMessage(n, KindStoppedMsg(n->iKind));	// stopped node: {8} raw/eAAC+, {4} MP3/AAC
		}
	if (n->iEosWait && (S(k, SStatus) & StEos))
		{
		n->iEosWait = EFalse;
		n->iRunning = EFalse;
		Kern::Printf("SYBSTUB: Hwa node %d play complete", i);
		DspMessage(n, KindStoppedMsg(n->iKind));	// play complete: {8} raw/eAAC+, {4} MP3/AAC
		}
	while (q.iCount && n->iRing)
		{
		SPcmReq& r = q.iReq[q.iHead];
		if (!n->iRunning || n->iEosWait)
			{
			if (n->iEosWait)
				break;					// the previous stream is still draining
			S(k, SCtrl) = CStop;
			S(k, SFormat) = KindFormat(n->iKind);
			S(k, SRate) = n->iRate;
			S(k, SChannels) = n->iChannels;
			S(k, SRingAddr) = n->iRingPhys;
			S(k, SRingSize) = KRingSize;
			S(k, SWatermark) = KRingSize / 2;
			S(k, SGain) = 256;
			S(k, SClass) = n->iKind == ENodeRaw ? 1 : 4;
			S(k, SCtrl) = CStart;
			n->iWpos = 0;
			n->iRunning = ETrue;
			Kern::Printf("SYBSTUB: Hwa node %d -> slot %d: kind %d format %d (%d Hz %d ch)", i, k, n->iKind,
						 KindFormat(n->iKind), n->iRate, n->iChannels);
			}
		TUint32 queued = n->iWpos - S(k, SReadPos);
		TInt room = KRingSize - (TInt)queued;
		while (r.iCopied < r.iLen && room > 0)
			{
			TInt at = n->iWpos % KRingSize;
			TInt m = Min(Min(r.iLen - r.iCopied, room), KRingSize - at);
			if (Kern::ThreadRawRead(r.iClient, (const TAny*)(r.iPtr + r.iCopied), n->iRingLin + at, m) != KErrNone)
				{
				r.iErr = KErrBadDescriptor;	// the client's buffer went away: what was copied plays, the error goes back
				r.iCopied = r.iLen;
				break;
				}
			r.iCopied += m;
			n->iWpos += m;
			room -= m;
			}
		S(k, SWritePos) = n->iWpos;
		queued = n->iWpos - S(k, SReadPos);
		if (r.iCopied < r.iLen || queued > (TUint32)KRingSize / 2)
			break;						// the watermark interrupt brings us back
		if (r.iEos)
			{
			S(k, SCtrl) = CEos;			// end: report once everything written has played
			n->iEosWait = ETrue;
			Kern::Printf("SYBSTUB: Hwa node %d end of stream queued (%d bytes)", i, n->iWpos);
			}
		ReqComplete(r, r.iErr);		// "buffer consumed"
		QueuePop(q);
		}
	}

static void Pump(TAny*)
	{
	TInt irq = NKern::DisableAllInterrupts();
	IrqPending = 0;
	NKern::RestoreInterrupts(irq);
	TBool busy = EFalse;
	for (TInt i = 0; i < NodeCount; ++i)
		{
		PumpNode(i);
		busy |= Queues[i].iCount || Nodes[i].iEosWait;
		}
	if (busy)
		PumpTimer->OneShot(NKern::TimerTicks(50), *PumpDfc);	// safety net; interrupts drive the pump
	}

static void SndIsr(TAny*)
	{
	using namespace SybSnd;
	TUint32 st = G(RIntStatus);
	G(RIntStatus) = st;					// level interrupt: cleared here, the DFC reads the slot status
	Interrupt::Clear(KIrq);
	IrqPending |= st;
	PumpDfc->Add();
	}

// once: is syborg-snd there (an old QEMU with the v1 device is not), bind its interrupt
static TBool PlaybackInit()
	{
	using namespace SybSnd;
	if (SndInit)
		return SndPresent;
	SndInit = ETrue;
	SndPresent = Present();
	if (!SndPresent)
		{
		Kern::Printf("SYBSTUB: Hwa no syborg-snd (id %08x): blocks complete unplayed", (TUint32)G(RId));
		return EFalse;
		}
	NKern::ThreadEnterCS();
	PumpDfc = new TDfc(Pump, NULL, Kern::DfcQue0(), 1);
	PumpTimer = new NTimer;
	NKern::ThreadLeaveCS();
	if (!PumpDfc || !PumpTimer || Interrupt::Bind(KIrq, SndIsr, NULL) < 0)
		{
		Kern::Printf("SYBSTUB: Hwa playback init failed");
		SndPresent = EFalse;
		return EFalse;
		}
	G(RIntEnable) = 0x7fff;
	Interrupt::Enable(KIrq);
	Kern::Printf("SYBSTUB: Hwa syborg-snd caps %08x", (TUint32)G(RCaps));
	return ETrue;
	}

// the node's ring, on its first block (client thread context)
static TBool NodeRing(SNode* aNode)
	{
	if (aNode->iRing)
		return ETrue;
	NKern::ThreadEnterCS();
	TInt r = Epoc::AllocPhysicalRam(KRingSize, aNode->iRingPhys, 12);
	if (r == KErrNone)
		{
		r = DPlatChunkHw::New(aNode->iRing, aNode->iRingPhys, KRingSize, EMapAttrSupRw | EMapAttrFullyBlocking);
		if (r != KErrNone)
			{
			Epoc::FreePhysicalRam(aNode->iRingPhys, KRingSize);
			aNode->iRing = NULL;
			}
		}
	NKern::ThreadLeaveCS();
	if (r != KErrNone)
		{
		Kern::Printf("SYBSTUB: Hwa node ring: %d", r);
		return EFalse;
		}
	aNode->iRingLin = (TUint8*)aNode->iRing->LinearAddress();
	return ETrue;
	}

static void Kick()
	{
	if (PumpDfc)
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
	Kern::Printf("SYBSTUB: Hwa DCD register %d objects -> %d (%d known)", n, r, DcdCount);
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
		if (Bridge1710())	// declared variant (ehwa.bridge = "omap1710", N73): a1 = the whole baseimage.dof
			{			// already in a user buffer, a2 = &size; PROC_Load takes any non-zero result as
						// a failure (docs/devices/nokia-n73-rm132/dspbridge-n73.md)
			TInt size = 0;
			Kern::ThreadRawRead(aClient, a2, &size, sizeof(size));
			Kern::Printf("SYBSTUB: Hwa base image %d bytes in the client's buffer %08x (loaded)", size, a1);
			return KErrNone;
			}
		TLinAddr base = 0;
		TInt size = (TInt)w[0] > KLoadChunkSize ? (TInt)w[0] : KLoadChunkSize;
		TInt h = NewChunk(aClient, size, LoadChunk, base);
		Kern::Printf("SYBSTUB: Hwa image chunk %d bytes at %08x -> handle %08x", size, base, h);
		return h;
		}
	case 0x400000a:	// the OMAP1710 bridge's number for it (N73; the N95's never issues it), same layout
	case 0x400000b:	// PROC shared buffer: a1 = TPtr8 out, a2 = TBuf8<4>{size}
	case 0x4000016:
		{
		TInt size = 0;
		TLinAddr base = 0;
		Kern::ThreadRawRead(aClient, (TUint8*)a2 + 8, &size, 4);	// TBuf8<4>: {typelen, maxlen, data}
		TInt h = NewChunk(aClient, size > 0 ? size : 0x1000, BufChunk, base);
		Kern::Printf("SYBSTUB: Hwa shared buffer %d bytes at %08x -> %d", size, base, h);
		if (h < 0)
			return h;
		TUint32 ptr[3] = { 0x20000000u | (TUint32)size, (TUint32)size, (TUint32)base };	// TPtr8
		return Kern::ThreadRawWrite(aClient, a1, ptr, sizeof(ptr));
		}
	case 4:			// Bal node create: *a2 = node token
		{
		// a new node, else an idle one (no stream, no queued blocks, no listener): never one that is playing
		TInt i = -1;
		if (NodeCount < KMaxNodes)
			i = NodeCount++;
		else
			{
			static TInt next;
			for (TInt t = 0; t < KMaxNodes && i < 0; ++t)
				{
				TInt c = (next + t) % KMaxNodes;
				if (!Nodes[c].iRunning && !Nodes[c].iEosWait && !Queues[c].iCount && !Nodes[c].iListen)
					i = c;
				}
			if (i < 0)
				{
				Kern::Printf("SYBSTUB: Hwa node create: all %d nodes busy", KMaxNodes);
				return KErrNoMemory;
				}
			next = i + 1;
			}
		Nodes[i].iListen = NULL;
		Nodes[i].iReplyQueued = EFalse;
		Nodes[i].iRaw = EFalse;
		Nodes[i].iEosWait = Nodes[i].iCancel = Nodes[i].iStopMsg = EFalse;
		if (Nodes[i].iRunning && SndPresent)
			SybSnd::S(NodeSlot(i), SybSnd::SCtrl) = SybSnd::CStop;
		Nodes[i].iRunning = EFalse;	// (a reused node keeps its ring)
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
				Kern::Printf("SYBSTUB: Hwa raw node %d: no EAP stream %d seen, assuming 44100 Hz stereo", i, c[2]);
			}
		Kern::Printf("SYBSTUB: Hwa node %d kind %d (uuid %08x) config %08x %08x %08x %08x %08x -> %d Hz %d ch",
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
		// (eapgwc wait loop FUN_80a0cd64; docs/devices/nokia-n95-8gb-rm320/nokia-dspbridge.md section 5)
		TUint32 b[2] = {0, 0}, m[5] = {0, 0, 0, 0, 0};
		Kern::ThreadRawRead(aClient, a2, b, sizeof(b));
		Kern::ThreadRawRead(aClient, (TAny*)b[1], m, sizeof(m));	// len, max, cmd, arg1, arg2
		TUint16 eapCmd = 0;
		if (m[2] == 0x20000000 && m[3])
			Kern::ThreadRawRead(aClient, (TAny*)m[3], &eapCmd, 2);	// payload u16[0]
		Kern::Printf("SYBSTUB: Hwa node send cmd %08x ptr %08x len %d eap %d", m[2], m[3], m[4], eapCmd);
		if (m[2] == 0x20000000 && m[3] && m[4] && eapCmd != 1)
			{
			TUint16 pl[40];
			TInt nb = Min((TInt)m[4], (TInt)sizeof(pl));
			Kern::ThreadRawRead(aClient, (TAny*)m[3], pl, nb);
			if (eapCmd == 0 && m[4] == 16)		// EAP 0 "create stream": the PCM format
				EapStreamSeen(pl);
			for (TInt j = 0; j < nb / 2; j += 10)
				Kern::Printf("SYBSTUB: Hwa eap %d +%02d: %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x", eapCmd, j * 2,
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
				Kern::Printf("SYBSTUB: Hwa raw node format: EAP stream %d, layout %d -> %d Hz %d ch%s", n->iEapStream,
							 m[4], n->iRate, n->iChannels, m[4] == 3 ? " (non-interleaved: played as interleaved)" : "");
				}
			TRequestStatus* send = (TRequestStatus*)b[0];
			if (send)
				Kern::RequestComplete(aClient, send, KErrNone);
			if (m[2] == KindStopCmd(n->iKind) && SndPresent)
				{
				n->iCancel = ETrue;		// this node only: other nodes keep playing
				n->iStopMsg = ETrue;
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
		TBool eos = n && (n->iKind == ENodeAac ? hdr[2] == 1 : (hdr[3] & 0x10) != 0);
		SNodeQueue* q = n ? &Queues[n - Nodes] : NULL;
		if (!n || len < 4 || !PlaybackInit() || !NodeRing(n))
			{
			if (st)
				Kern::RequestComplete(aClient, st, KErrNone);
			if (n && eos)
				DspMessage(n, KindStoppedMsg(n->iKind));	// nothing plays: the stream ends at once
			return KErrNone;
			}
		SPcmReq r;
		r.iErr = KErrNone;
		r.iClient = aClient;
		r.iStatus = st;
		r.iPtr = b[2] + 4;
		r.iLen = len - 4;
		r.iCopied = 0;
		r.iEos = eos;
		if (n->iKind == ENodeAac && hdr[2] == 2)
			r.iLen = 0;					// AAC concealment (header-only) block: nothing to play
		if (n->iKind != ENodeRaw && !n->iRunning && q->iCount == 0 && len >= 12)
			{
			TUint8 p[8];
			Kern::ThreadRawRead(aClient, (const TAny*)(b[2] + 4), p, 8);
			Kern::Printf("SYBSTUB: Hwa node %d first codec block len %d hdr %02x %02x %02x %02x data %02x %02x %02x %02x %02x %02x %02x %02x",
						 n - Nodes, len, hdr[0], hdr[1], hdr[2], hdr[3], p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
			}
		if (r.iEos || len <= 4)
			Kern::Printf("SYBSTUB: Hwa node %d block len %d hdr %02x %02x %02x %02x words %08x %08x %08x", n - Nodes,
						 len, hdr[0], hdr[1], hdr[2], hdr[3], b[4], b[5], b[6]);
		NKern::ThreadEnterCS();
		aClient->Open();				// held by the queued request (ReqComplete drops it)
		NKern::ThreadLeaveCS();
		if (!QueuePush(*q, r))
			{
			Kern::Printf("SYBSTUB: Hwa node %d queue full: block refused", n - Nodes);
			r.iClient = aClient;
			ReqComplete(r, KErrOverflow);
			return KErrNone;
			}
		Kick();
		return KErrNone;
		}
	case 0x800000f:	// cancel PCM issue: the node's pending blocks complete KErrCancel, its slot stops
		{
		SNode* n = Node(a1);
		for (TInt i = 0; SndPresent && i < NodeCount; ++i)
			if (!n || n == &Nodes[i])
				Nodes[i].iCancel = ETrue;
		Kick();
		return KErrNone;
		}
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
