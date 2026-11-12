// AAMdaifDriver.ldd stand-in (5730, RAPIDO audio): AAMdaifApi.dll opens "MdaifDriver" (1.0.1) and
// uses only DoControl. Sound: each stream type's ring in the DSP window is played by syborg-snd (slot =
// stream type), paced by the fake modem's F3 0x35 replies on the host's playback clock.
// Protocol: docs/devices/nokia-5730-rm465/rapido-audio.md.
#include "nokiastub.h"
#include <platform.h>
#include <kernel/kern_priv.h>
#include <memmodel/epoc/platform.h>
#include "boardconfig.h"
#include "sybsnd.h"

// ctl 2 table read by user code (AAMdaifApi ord 1/5): +4 u16 count = 0 -> every id looks up as 0.
// Kept in ROM (XIP LDD) so any client can read it; a non-zero marker keeps it out of .bss.
static const TUint32 KDspInfo[0x50 / 4] = { 0x4644444d /* "MDDF" marker, not read */ };

// The DSP window: one shared user-writable buffer for every "write" mapping, a shared kernel chunk over
// physically contiguous RAM, opened in each client process and returned at that process's address. (A
// DPlatChunkHw with EMapAttrUserRw is global on the multiple memory model but not user-accessible on
// the flexible one: the N8's AudioProcessingEngine faulted writing to it.) Contiguous, so that each
// stream's ring inside it is one syborg-snd slot ring (host reads it by physical address).
const TInt KWritableSize = 0x100000;	// 1 MB: the 5800 audio server writes 0x4b00-byte blocks past 64 KB (DSP memory window); the N8 engine writes at the end of any size (RAPIDO shared-memory state not modelled)
static DChunk* WritableChunk;
static TPhysAddr WindowPhys;
const TInt KMaxMapped = 8;
static TUint MappedId[KMaxMapped];		// processes (by id: a dead process's object address can be reused) holding a
static TLinAddr MappedAt[KMaxMapped];	// handle to the window, and where it is mapped there

static TInt EnsureWindow()
	{
	if (WritableChunk)
		return KErrNone;
	NKern::ThreadEnterCS();
	TInt r = Epoc::AllocPhysicalRam(KWritableSize, WindowPhys, 16);
	if (r == KErrNone)
		{
		TChunkCreateInfo info;
		info.iType = TChunkCreateInfo::ESharedKernelMultiple;
		info.iMaxSize = KWritableSize;
		info.iMapAttr = EMapAttrCachedMax;
		info.iOwnsMemory = EFalse;
		info.iDestroyedDfc = NULL;
		TLinAddr kaddr;
		TUint32 attr;
		r = Kern::ChunkCreate(info, WritableChunk, kaddr, attr);
		if (r == KErrNone)
			r = Kern::ChunkCommitPhysical(WritableChunk, 0, KWritableSize, WindowPhys);
		if (r == KErrNone)
			memclr((TAny*)kaddr, KWritableSize);
		else
			{
			if (WritableChunk)
				{
				Kern::ChunkClose(WritableChunk);
				WritableChunk = NULL;
				}
			Epoc::FreePhysicalRam(WindowPhys, KWritableSize);	// the chunk does not own it (iOwnsMemory false)
			}
		}
	NKern::ThreadLeaveCS();
	Kern::Printf("SYBSTUB: MdaifDriver DSP window %d bytes at phys %08x: %d", KWritableSize, WindowPhys, r);
	return r;
	}

static TAny* WritableBuffer(DThread* aClient)
	{
	if (EnsureWindow() != KErrNone)
		return NULL;
	NKern::ThreadEnterCS();
	TUint pid = aClient->iOwningProcess->iId;
	TAny* ret = NULL;
	for (TInt i = 0; i < KMaxMapped && !ret; i++)
		if (MappedId[i] == pid)
			ret = (TAny*)MappedAt[i];
	if (!ret)
		{
		TInt h = Kern::MakeHandleAndOpen(aClient, WritableChunk);	// maps the chunk into the client's process
		if (h >= 0)
			{
			ret = (TAny*)Kern::ChunkUserBase(WritableChunk, aClient);
			static TInt next;
			TInt i = next++ % KMaxMapped;			// round robin: a full table forgets the oldest process
			MappedId[i] = pid;
			MappedAt[i] = (TLinAddr)ret;
			}
		Kern::Printf("SYBSTUB: MdaifDriver writable buffer %d bytes at %08x for %O (handle %d)", KWritableSize, ret, aClient, h);
		}
	NKern::ThreadLeaveCS();
	return ret;
	}

// The PCM format of each stream type, declared by the profile ([board.audio] streams -> "audio.streams",
// "RATE/CHANNELS/BITS[/PLANAR]" per type: "48000/2/24/240,..."; 24 = 24-bit samples in 32-bit words, PLANAR = stereo
// in chunks of N left then N right samples, as the N8 engine writes stream type 0 [V: 2026-11-12 ring dump, 1920
// bytes = 240 stereo frames]; other types [I]
static TBool StreamFormat(TInt aType, TInt& aRate, TInt& aCh, TInt& aFmt, TInt& aPlanar)
	{
	char b[64];
	TInt n = BoardConfig("audio.streams", b, sizeof(b));
	aRate = 0, aCh = 2, aFmt = SybSnd::FS16, aPlanar = 0;
	TInt field = 0, k = 0, f[4] = {0, 0, 0, 0};
	for (TInt i = 0; i <= n && n > 0; i++)
		{
		char c = i < n ? b[i] : ',';
		if (c >= '0' && c <= '9')
			f[k] = f[k] * 10 + (c - '0');
		else if (c == '/' && k < 3)
			k++;
		else if (c == ',')
			{
			if (field == aType && f[0] > 0)
				{
				aRate = f[0];
				aCh = f[1] == 2 ? 2 : 1;
				aFmt = f[2] == 24 ? SybSnd::FS24in32 : f[2] == 32 ? SybSnd::FS32 : SybSnd::FS16;
				aPlanar = f[3];
				}
			field++, k = 0, f[0] = f[1] = f[2] = f[3] = 0;
			}
		}
	return aRate > 0;			// not declared for this phone: the ring stays unbound (silent)
	}

// what each stream type's slot was last bound to: a repeated ring query does not restart a playing stream
static TUint32 BoundOffset[3], BoundSize[3];

// stream type t's ring {window + offset, size} becomes syborg-snd slot t; the fake modem then moves its
// write position on F3 0x34 and answers 0x35 on the host's playback clock (syborg_modem.c)
static void BindSlot(TInt aType, TUint32 aOffset, TUint32 aSize)
	{
	using namespace SybSnd;
	if (aType < 0 || aType > 2 || !Present() || EnsureWindow() != KErrNone)
		return;							// stream types 0..2 only (cdspcontrolserver's set)
	TInt rate, ch, fmt, planar;
	if (!StreamFormat(aType, rate, ch, fmt, planar))
		return;
	if (BoundSize[aType] == aSize && BoundOffset[aType] == aOffset && (S(aType, SStatus) & StRun))
		return;							// already playing this ring
	BoundSize[aType] = aSize;
	BoundOffset[aType] = aOffset;
	S(aType, SCtrl) = CStop;
	S(aType, SFormat) = fmt;
	S(aType, SRate) = rate;
	S(aType, SChannels) = ch;
	S(aType, SRingAddr) = WindowPhys + aOffset;
	S(aType, SRingSize) = aSize;
	S(aType, SWatermark) = 0;
	S(aType, SGain) = 256;
	S(aType, SClass) = 4;
	S(aType, SPlanar) = planar;
	S(aType, SCtrl) = CStart;
	Kern::Printf("SYBSTUB: MdaifDriver stream %d -> syborg-snd slot %d: %d Hz %d ch format %d planar %d, ring %d bytes at +%x",
				 aType, aType, rate, ch, fmt, planar, aSize, aOffset);
	}

// Control numbers differ between client generations: the numbers below are the 5730/5800/N8 (011.012,
// Anna) ones; Belle's aamdaif renumbered them (its 1, 3 x7, 2, 6, 4, 0, 4 start-up = the old 5, 7, 6,
// 10, 8, 2, 8, same arguments). A profile declares its numbering, [[standins]] config =
// { ctl_map = "BELLE:OLD,..." } (models/nokia-n8-rm596/firmware/111.040.1511): never guessed from the arguments.
static TInt CtlMap[16];
static TInt CtlMapState;		// 0 not read, 1 read
static TInt MapCtl(TInt aFn)
	{
	if (!CtlMapState)
		{
		CtlMapState = 1;
		for (TInt i = 0; i < 16; i++)
			CtlMap[i] = -1;
		char b[96];
		TInt n = BoardConfig("aamdaifdriver.ctl_map", b, sizeof(b));
		TInt from = -1, v = -1;
		for (TInt i = 0; i <= n && n > 0; i++)
			{
			char c = i < n ? b[i] : ',';
			if (c >= '0' && c <= '9')
				v = (v < 0 ? 0 : v * 10) + (c - '0');
			else if (c == ':')
				from = v, v = -1;
			else if (c == ',')
				{
				if (from >= 0 && from < 16 && v >= 0)
					CtlMap[from] = v;
				from = v = -1;
				}
			}
		if (n > 0)
			Kern::Printf("SYBSTUB: MdaifDriver control numbering %s", b);
		}
	return aFn >= 0 && aFn < 16 && CtlMap[aFn] >= 0 ? CtlMap[aFn] : aFn;
	}

static TInt MdaifCtl(DThread* aClient, TInt aFn, TAny* a1, TAny*, const TUint32* w)
	{
	aFn = MapCtl(aFn);
	switch (aFn)
		{
	case 5:		// "DSP already set up?" out word: 0 -> client sends ISI F3 0x30 and waits for 0x31
		{
		TInt v = 0;	// 1 would skip the ISI handshake entirely (fallback if no F3 reply)
		return Kern::ThreadRawWrite(aClient, a1, &v, sizeof(v));
		}
	case 2:		// {dspAddr, size, TAny** out, dir}: dir 0 = the DSP info table the client reads (ROM),
		{		// dir 1 = a buffer the client writes (EapDsp's playback path): writable zeroed RAM
		const TAny* p = KDspInfo;
		if (w[3] == 1)
			{
			p = WritableBuffer(aClient);
			if (!p)
				return KErrNoMemory;
			}
		return Kern::ThreadRawWrite(aClient, (TAny*)w[2], &p, sizeof(p));
		}
	case 10:	// a1 = a value (0 or 1 on 011.012, Anna and Belle [V: boot and game logs]), not an out
		{		// word: writing through it faulted in the audio server and returned -38. Accepted;
				// the old "TInt out: 0" reading (ord 5 then skips ctl 12) is kept for a real pointer.
		if ((TUint32)a1 < 0x10000)
			return KErrNone;
		TInt v = 0;
		return Kern::ThreadRawWrite(aClient, a1, &v, sizeof(v));
		}
	case 8:		// {stream type, TUint* size, TUint* offset, TUint* x}: the DSP ring for one stream
		{		// (docs/devices/nokia-n8-rm596/rapido-audio.md). aamdaif.dll maps {window + offset, size}
				// with ctl 2 dir 1 and wraps its write position at size; cdspcontrolserver sends
				// size / (1920, 960, 320) as the frame count of F3 0x32. Size 0 (the default, 5730/5800
				// as tested) never wraps: the N8's engine then wrote past the end of the window.
		TInt units = BoardConfigInt("audio.ring_units", 0);	// [[standins]] config = { ring_units = N }
		if (units <= 0)
			return KErrNone;
		TUint32 unit = w[0] == 0 ? 1920 : w[0] == 1 ? 960 : 320;	// bytes per 10 ms [verified: cdsp]
		TUint32 size = units * unit;
		if (size > 0x20000)
			size = 0x20000 / unit * unit;	// the ring fits its 128 KB region of the window
		if (w[0] > 2)
			return KErrArgument;		// stream types 0..2 (cdspcontrolserver divides by 1920 / 960 / 320)
		TUint32 offset = w[0] * 0x20000;	// each stream type its own 128 KB region of the window: the engine
												// and cdspcontrolserver both ask, and must get the same ring
		TUint32 x = 0;
		TInt r = Kern::ThreadRawWrite(aClient, (TAny*)w[1], &size, sizeof(size));
		if (r == KErrNone)
			r = Kern::ThreadRawWrite(aClient, (TAny*)w[2], &offset, sizeof(offset));
		if (r == KErrNone && w[3])
			r = Kern::ThreadRawWrite(aClient, (TAny*)w[3], &x, sizeof(x));
		if (r == KErrNone)
			BindSlot(w[0], offset, size);
		return r;
		}
	case 6: case 7:	// map entry {i, size, addr, 0} / map done: accepted
	case 15:		// a client control with no old equivalent (ctl_map "N:15"): accepted
	default:
		return KErrNone;
		}
	}

NO_CONTROLS
NOKIA_STUB_LDD_CTLHOOK("MdaifDriver", KCtl, ETrue, MdaifCtl)
