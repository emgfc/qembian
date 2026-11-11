// AAMdaifDriver.ldd stand-in (5730, RAPIDO audio): AAMdaifApi.dll opens "MdaifDriver" (1.0.1) and
// uses only DoControl. Audio is silent: no DSP memory, the mapping controls are accepted and dropped.
// Protocol: docs/devices/nokia-5730-rm465/rapido-audio.md.
#include "nokiastub.h"
#include <platform.h>
#include <kernel/kern_priv.h>
#include "boardconfig.h"

// ctl 2 table read by user code (AAMdaifApi ord 1/5): +4 u16 count = 0 -> every id looks up as 0.
// Kept in ROM (XIP LDD) so any client can read it; a non-zero marker keeps it out of .bss.
static const TUint32 KDspInfo[0x50 / 4] = { 0x4644444d /* "MDDF" marker, not read */ };

// One shared user-writable buffer for every "write" mapping (silent audio: nothing reads it): a
// shared kernel chunk, opened in each client process and returned at that process's address. (A
// DPlatChunkHw with EMapAttrUserRw is global on the multiple memory model but not user-accessible on
// the flexible one: the N8's AudioProcessingEngine faulted writing to it.)
const TInt KWritableSize = 0x100000;	// 1 MB: the 5800 audio server writes 0x4b00-byte blocks past 64 KB (DSP memory window); the N8 engine writes at the end of any size (RAPIDO shared-memory state not modelled)
static DChunk* WritableChunk;
static DProcess* MappedIn[4];			// processes holding a handle (one per process is enough)
static TLinAddr MappedAt[4];
static TAny* WritableBuffer(DThread* aClient)
	{
	NKern::ThreadEnterCS();
	if (!WritableChunk)
		{
		TChunkCreateInfo info;
		info.iType = TChunkCreateInfo::ESharedKernelMultiple;
		info.iMaxSize = KWritableSize;
		info.iMapAttr = EMapAttrCachedMax;
		info.iOwnsMemory = ETrue;
		info.iDestroyedDfc = NULL;
		TLinAddr kaddr;
		TUint32 attr;
		TInt r = Kern::ChunkCreate(info, WritableChunk, kaddr, attr);
		if (r == KErrNone)
			r = Kern::ChunkCommit(WritableChunk, 0, KWritableSize);
		if (r != KErrNone)
			{
			Kern::Printf("SYBSTUB: MdaifDriver writable buffer: %d", r);
			NKern::ThreadLeaveCS();
			return NULL;
			}
		memclr((TAny*)kaddr, KWritableSize);
		}
	DProcess* proc = aClient->iOwningProcess;
	TAny* ret = NULL;
	for (TInt i = 0; i < 4 && !ret; i++)
		if (MappedIn[i] == proc)
			ret = (TAny*)MappedAt[i];
	if (!ret)
		{
		TInt h = Kern::MakeHandleAndOpen(aClient, WritableChunk);	// maps the chunk into the client's process
		if (h >= 0)
			{
			ret = (TAny*)Kern::ChunkUserBase(WritableChunk, aClient);
			for (TInt i = 0; i < 4; i++)
				if (!MappedIn[i])
					{
					MappedIn[i] = proc;
					MappedAt[i] = (TLinAddr)ret;
					break;
					}
			}
		Kern::Printf("SYBSTUB: MdaifDriver writable buffer %d bytes at %08x for %O (handle %d)", KWritableSize, ret, aClient, h);
		}
	NKern::ThreadLeaveCS();
	return ret;
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
		static TInt rings;	// each stream its own 128 KB region of the window (8 regions in 1 MB)
		TUint32 unit = w[0] == 0 ? 1920 : w[0] == 1 ? 960 : 320;	// bytes per 10 ms [verified: cdsp]
		TUint32 size = units * unit;
		TUint32 offset = (rings++ & 7) * 0x20000;
		TUint32 x = 0;
		TInt r = Kern::ThreadRawWrite(aClient, (TAny*)w[1], &size, sizeof(size));
		if (r == KErrNone)
			r = Kern::ThreadRawWrite(aClient, (TAny*)w[2], &offset, sizeof(offset));
		if (r == KErrNone && w[3])
			r = Kern::ThreadRawWrite(aClient, (TAny*)w[3], &x, sizeof(x));
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
