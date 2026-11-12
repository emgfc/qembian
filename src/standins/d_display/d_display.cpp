// N95 d_display.ldd stand-in (see nokiastub.h): channel "DisplayDriver" used by the N95's
// r_display.dll (CDisplayDriver): scdv.dll (WSERV's screen driver) and, through
// power_save_display_mode.dll, the screensaver. The real driver is documented in
// docs/devices/nokia-n95-8gb-rm320/nokia-d_display.md (section 4 = what this stand-in follows). Geometry and the frame buffer
// come from HAL (our lcd_n95 driver); the Syborg frame buffer is always live, so screen updates
// (ctl 8/9) need no copy.
//
//   ctl 3 {start,end,fullColour}  partial display (screensaver): only panel rows start..end lit.
//                                 Panel rows are rows of the native landscape panel, so in the
//                                 picture they are rows at rotation index 0/2 and columns at 1/3
//                                 (WSERV switches rotation with req 0x8003; the screensaver passes
//                                 its UI Y range at 0/2 and X range at 1/3 accordingly).
//                                 Programmed into the Syborg fb (PARTIAL_* registers)
//   ctl 4                         cancel partial display
//   ctl 5 -> TInt                 partial display active
//   ctl 10 (screen by value)      select screen: only 0
//   ctl 11 -> 26 words            capabilities (table below; 23 words on the N73, whose
//                                 CDisplayDriver keeps 0x5c bytes for them: board config d_display.caps)
//   ctl 26 -> TInt                current rotation index
//   ctl 31 -> TInt                scan-line bytes
//   req 0x8001 (sub)              panel self-test: sub 0 only
//   req 0x8002 (mode index)       colour mode: 1 = EColor16MU only (our fb is 32 bpp)
//   req 0x8003 (rotation index)   0, 2, 3 (90 degrees is not supported by the panel). ScDv then
//                                 draws the picture in that orientation (240x320 at 3, 320x240 at
//                                 0/2) into the frame buffer, line pitch always 1280 bytes (ctl 31,
//                                 as on the device); the display controller (the Syborg fb,
//                                 ORIENTATION register) rotates it onto the portrait screen
//   req 0x23                      display event notification: stays pending (as on the device)
//   req 0x7f01                    ours (not on the device): complete at the next vsync. Used by the
//                                 sygl blob's eglSwapBuffers (src/n95gl, hack H-vsync-wait)
#include "nokiastub.h"
#include "syborg.h"
#include "boardconfig.h"

// Per device (board config from the profile's [board.display], tools/device.py board_config; the
// defaults are the N95's): caps block size, native panel orientation, scan-line bytes (ctl 31 and the
// LCD HAL's stride), panel rows (partial display), and which rotation indexes the panel accepts.
const TInt KMaxCapsWords = 28;		// KCaps below
static TInt CapsWords = 26;			// d_display.caps
static TBool NativeLandscape = ETrue;	// d_display.native=landscape|portrait
static TInt FbPitch = 1280;			// d_display.pitch
static TUint PanelRows = 240;			// d_display.rows
static TBool PartialCtls = ETrue;		// d_display.partial: controls 3..5 are the partial display mode
static void ReadDisplayConfig()
	{
	char b[16];
	CapsWords = BoardConfigInt("d_display.caps", 26);
	if (CapsWords < 1 || CapsWords > KMaxCapsWords)
		CapsWords = 26;
	if (BoardConfig("d_display.native", b, sizeof(b)) > 0)
		NativeLandscape = b[0] == 'l';
	FbPitch = BoardConfigInt("d_display.pitch", 1280);
	PanelRows = BoardConfigInt("d_display.rows", NativeLandscape ? 240 : 320);
	// the N95/5800 driver's partial (power save) display mode; the N8's driver has none, and Belle's
	// TV-out behaviour plugin uses control 3 for something else (args 1,0,1,1; it panics on an error)
	PartialCtls = BoardConfigInt("d_display.partial", 1) != 0;
	Kern::Printf("SYBSTUB: DisplayDriver %s panel, %d caps words, stride %d, %d panel rows",
				 NativeLandscape ? "landscape" : "portrait", CapsWords, FbPitch, PanelRows);
	}

static TUint32 KCaps[KMaxCapsWords] =		// N95 26 words, N73 23, 5730 27 (d_display.caps); words 26.. unknown, 0
	{
	1,				// +0  display present
	0,				// +4  caller is the window server (ScDv ignores it)
	1,				// +8  DSB buffers supported (as on the device): CDirectScreenBitmap uses ctl 23/24/25 +
					//     req 0x2003/0x2004, paced by vsync (with 0 ScDv draws on heap memory and its
					//     EndUpdate completes at once: no frame pacing)
	0,				// +12
	0, 0,			// +16 partial-width updates, +20 update retry strategy (0 on the N95)
	0, 0,			// +24/+28 current height/width (real 240/320, swapped after rotation; ScDv
					//         does not need them, left 0 as before)
	240,			// +32 max partial-display lines (power_save_display_mode reports it; = d_display.rows)
	0, 1, 1,		// +36/+40/+44 rotation 90/180/270 (the N95 panel cannot do 90; a portrait panel
					//             gets 1, 1, 1 in Install)
	0,				// +48 EColor4K
	0,				// +52 EColor64K: real 1; off because our frame buffer is 32 bpp only
	0,				// +56 EColor16M
	1,				// +60 EColor16MU (N95 wsini.ini asks for it)
	0,				// +64 EColor16MA
	0, 0, 0, 0, 0,	// +68..+84
	1, 1,			// +88/+92 constants on the device (meaning unknown)
	0, 0,			// +96/+100
	0, 0			// +104/+108 (5730's 27th word; meaning unknown)
	};

// partial-display state (one panel) and the current rotation index (req 0x8003)
static TInt PartialActive;
static TUint PartialStart, PartialEnd;
static TBool PartialFullColour;
static TUint Rotation = 0;

// Syborg fb registers (src/qemu-syborg/hw/arm/syborg_fb.c)
enum { KFbOrientation = 4, KFbPartialStart = 14, KFbPartialEnd = 15, KFbPartialMode = 16 };

// The phone is held portrait: the portrait UI (rotation index 3) is upright on the screen. The
// controller turns the picture by (3 - index) quarter turns, so at the native index 0 (the
// screensaver) the landscape picture's top is at the screen's left edge (inferred from the
// screensaver passing its portrait X range as panel rows unreversed at index 3; confirmed by the
// owner against the real N95 8GB).
// A native portrait panel (N73) shows rotation index 0 upright, so the controller turns the picture
// by index quarter turns the other way [I: no landscape use seen yet on such a phone].
static void SetFbOrientation()
	{
	WriteReg(KHwBaseClcd, KFbOrientation, (NativeLandscape ? 3 - Rotation : 4 - Rotation) & 3);
	}

static void SetPartial(TBool aOn, TUint aStart, TUint aEnd, TBool aFullColour)
	{
	PartialActive = aOn;
	PartialStart = aStart;
	PartialEnd = aEnd;
	PartialFullColour = aFullColour;
	TBool rows = (Rotation & 1) == 0;	// panel rows are picture rows unless rotated by 90/270
	if (Rotation == 2)					// upside-down landscape: panel row r = picture row 239-r
		{
		TUint s = aStart;
		aStart = PanelRows - 1 - aEnd;
		aEnd = PanelRows - 1 - s;
		}
	WriteReg(KHwBaseClcd, KFbPartialStart, aStart);
	WriteReg(KHwBaseClcd, KFbPartialEnd, aEnd);
	WriteReg(KHwBaseClcd, KFbPartialMode, (aOn ? 1 : 0) | (aFullColour ? 2 : 0) | (rows ? 4 : 0));
	}

// Host GL backend (ours, not on the device): the sygl blob (src/n95gl/hw.c) sends its rasteriser's
// command stream through these controls to the syborg-gl QEMU device (syborg_gl.c,
// syborg_gl_proto.h). The stream is copied into contiguous RAM the device reads; read-backs come back
// the same way. Hack H-gles-host.
//   ctl 0x7f10 -> 2 words          {1 = device present and host GL ready (0 = software), caller's thread id}
//   ctl 0x7f11 {cmd, len, out, outLen}  run a command buffer; out/outLen: where SYGL_READ results go
//   ctl 0x7f12 a1 = SyglKick*        run the buffers it names in place (the device reads and writes
//                                    the caller's memory through its page tables): no copies
//   ctl 0x7f13 -> 1 word             the device's register page mapped user-writable (or 0): the
//                                    caller writes SyglKick addresses to KICK itself, no kernel call;
//                                    its address space is bound to it (0 when it cannot be: shared)
// Host objects (surfaces, textures) belong to a process (the owner id ctl 0x7f10 returns); when a
// process's last DisplayDriver channel closes (exit, kill, panic) the stand-in sends
// SYGL_OWNER_FREE for it, so the device's tables do not fill up with the dead.
// Authority (docs/gpu-boundary-roadmap.md phase 1): the device reaches guest memory with the user's
// permissions only, and lets user mode write KICK alone. The kernel says whose a run is: PID/FLAGS before
// each run it makes (under GlLock), and BIND at ctl 0x7f13, which ties the caller's user address space to
// its process for its own kicks. The window server (SID 0x10003B20) gets the compositor right.
const TUint KHwBaseGl = KHwBasePeripherals + 0x0f * KHwLinSeparation;
enum { KGlId = 0, KGlCaps = 1, KGlBuf = 2, KGlLen = 3, KGlExec = 4, KGlOut = 5, KGlOutLen = 6, KGlKick = 7,
	   KGlPid = 8, KGlFlags = 9, KGlBind = 10 };
const TUint32 KGlRightKernel = 1, KGlRightCompositor = 2;
const TUint32 KWservSid = 0x10003B20;
const TUint32 KGlDeviceId = 0xc51d0011;
const TInt KGlBufSize = 1 << 20;		// commands; and as much again for read-backs (bands: hw.c)
static DPlatChunkHw* GlChunk;
static TPhysAddr GlPhys;
static TInt GlState;					// 0 unknown, 1 ready, -1 none
static DMutex* GlLock;
static DPlatChunkHw* GlUserRegs;		// the register page, user-writable (ctl 0x7f13)
static TInt GlUserRegsState;			// 0 not tried, 1 mapped, -1 failed

static TInt GlReady()
	{
	if (GlState == 0)
		{
		GlState = -1;
		if (ReadReg(KHwBaseGl, KGlId) == KGlDeviceId && ReadReg(KHwBaseGl, KGlCaps) == 1)
			{
			NKern::ThreadEnterCS();
			TInt r = Kern::MutexCreate(GlLock, _L("SyGl"), KMutexOrdGeneral0);
			if (r == KErrNone)
				r = Epoc::AllocPhysicalRam(2 * KGlBufSize, GlPhys, 12);
			if (r == KErrNone)
				r = DPlatChunkHw::New(GlChunk, GlPhys, 2 * KGlBufSize, EMapAttrSupRw | EMapAttrFullyBlocking);
			NKern::ThreadLeaveCS();
			GlState = r == KErrNone ? 1 : -1;
			Kern::Printf("SYBSTUB: DisplayDriver host GL %s (%d)", GlState > 0 ? "ready" : "unavailable", r);
			}
		else
			Kern::Printf("SYBSTUB: DisplayDriver no host GL device: software rendering");
		}
	return GlState > 0;
	}

// a command buffer from the kernel itself (OWNER_FREE at channel close)
static void GlRunKernel(const TUint32* aWords, TInt aCount)
	{
	if (!GlReady())
		return;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*GlLock);
	memcpy((TUint8*)GlChunk->LinearAddress(), aWords, aCount * 4);
	WriteReg(KHwBaseGl, KGlPid, 0);
	WriteReg(KHwBaseGl, KGlFlags, KGlRightKernel);
	WriteReg(KHwBaseGl, KGlBuf, GlPhys);
	WriteReg(KHwBaseGl, KGlLen, aCount * 4);
	WriteReg(KHwBaseGl, KGlOut, GlPhys + KGlBufSize);
	WriteReg(KHwBaseGl, KGlOutLen, 0);
	WriteReg(KHwBaseGl, KGlExec, 1);
	Kern::MutexSignal(*GlLock);
	NKern::ThreadLeaveCS();
	}

// open DisplayDriver channels per process: the host objects go with the last one
const TInt KGlOwners = 256;			// processes with a DisplayDriver channel; beyond that: no host GL
static struct { TUint32 iPid; TInt iCount; } GlOwners[KGlOwners];
static NFastMutex GlOwnersLock;

static void GlOwnerOpened(TUint32 aPid)
	{
	NKern::FMWait(&GlOwnersLock);
	TInt freeSlot = -1;
	for (TInt i = 0; i < KGlOwners; ++i)
		{
		if (GlOwners[i].iCount && GlOwners[i].iPid == aPid)
			{
			GlOwners[i].iCount++;
			NKern::FMSignal(&GlOwnersLock);
			return;
			}
		if (!GlOwners[i].iCount && freeSlot < 0)
			freeSlot = i;
		}
	if (freeSlot >= 0)				// table full: the process gets no host GL (GlOwnerTracked)
		{
		GlOwners[freeSlot].iPid = aPid;
		GlOwners[freeSlot].iCount = 1;
		}
	NKern::FMSignal(&GlOwnersLock);
	}

// host GL only for a process whose objects will be freed at its exit
static TBool GlOwnerTracked(TUint32 aPid)
	{
	TBool found = EFalse;
	NKern::FMWait(&GlOwnersLock);
	for (TInt i = 0; i < KGlOwners && !found; ++i)
		found = GlOwners[i].iCount && GlOwners[i].iPid == aPid;
	NKern::FMSignal(&GlOwnersLock);
	return found;
	}

static TUint32 GlRights(DThread* aClient)
	{
	return Kern::ProcessSecureId(aClient->iOwningProcess) == KWservSid ? KGlRightCompositor : 0;
	}

static void GlOwnerClosed(TUint32 aPid)
	{
	TBool last = EFalse;
	NKern::FMWait(&GlOwnersLock);
	for (TInt i = 0; i < KGlOwners; ++i)
		if (GlOwners[i].iCount && GlOwners[i].iPid == aPid)
			{
			last = --GlOwners[i].iCount == 0;
			break;
			}
	NKern::FMSignal(&GlOwnersLock);
	if (last && GlState > 0)
		{
		const TUint32 cmd[3] = { 9 /* SYGL_OWNER_FREE */, 3, aPid };
		GlRunKernel(cmd, 3);
		}
	}

static TInt GlSubmit(DThread* aClient, const TUint32* aW)
	{
	if (!GlReady())
		return KErrNotSupported;
	TUint len = aW[1], outLen = aW[3];
	if (len < 8 || len > (TUint)KGlBufSize || outLen > (TUint)KGlBufSize)
		return KErrArgument;
	TUint8* in = (TUint8*)GlChunk->LinearAddress();
	TUint8* out = in + KGlBufSize;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*GlLock);
	TInt r = Kern::ThreadRawRead(aClient, (TAny*)aW[0], in, len);
	if (r == KErrNone)
		{
		WriteReg(KHwBaseGl, KGlPid, aClient->iOwningProcess->iId);
		WriteReg(KHwBaseGl, KGlFlags, GlRights(aClient));
		WriteReg(KHwBaseGl, KGlBuf, GlPhys);
		WriteReg(KHwBaseGl, KGlLen, len);
		WriteReg(KHwBaseGl, KGlOut, GlPhys + KGlBufSize);
		WriteReg(KHwBaseGl, KGlOutLen, outLen);
		WriteReg(KHwBaseGl, KGlExec, 1);			// runs synchronously in the device
		TUint32 res = ReadReg(KHwBaseGl, KGlExec);
		if (res)
			r = KErrGeneral;
		if (r == KErrNone && outLen)
			r = Kern::ThreadRawWrite(aClient, (TAny*)aW[2], out, outLen);
		}
	Kern::MutexSignal(*GlLock);
	NKern::ThreadLeaveCS();
	return r;
	}

static TInt DisplayCtl(DThread* aClient, TInt aFn, TAny* a1, TAny*, const TUint32* aWords)
	{
	if (!PartialCtls && aFn >= 3 && aFn <= 5)
		return KStubNotHandled;		// not this driver's partial mode: the table / generic answer
	if (aFn >= 0x7f11 && aFn <= 0x7f13 && !GlOwnerTracked(aClient->iOwningProcess->iId))
		return KErrNotSupported;
	switch (aFn)
		{
	case 0x7f10:	// {ready, owner}: owner = the caller's process id (hw.c's surface ids and texture
		{			// keys are addresses, unique in one process, and any of its threads may use them)
		TUint32 pid = aClient->iOwningProcess->iId;
		TUint32 q[2] = { (TUint32)(GlReady() && GlOwnerTracked(pid)), pid };
		return Kern::ThreadRawWrite(aClient, a1, q, sizeof(q));
		}
	case 0x7f11:
		return GlSubmit(aClient, aWords);
	case 0x7f12:	// in place: the device translates the caller's addresses while this write runs
		{
		if (!GlReady())
			return KErrNotSupported;
		NKern::ThreadEnterCS();
		Kern::MutexWait(*GlLock);			// PID/FLAGS and the kick together
		WriteReg(KHwBaseGl, KGlPid, aClient->iOwningProcess->iId);
		WriteReg(KHwBaseGl, KGlFlags, GlRights(aClient));
		WriteReg(KHwBaseGl, KGlKick, (TUint32)a1);
		TUint32 res = ReadReg(KHwBaseGl, KGlKick);
		Kern::MutexSignal(*GlLock);
		NKern::ThreadLeaveCS();
		return res == 0 ? KErrNone : res == 0xffffffffu ? KErrBadDescriptor : KErrGeneral;
		}
	case 0x7f13:
		{
		TUint32 lin = 0;
		if (GlReady())
			{
			NKern::ThreadEnterCS();
			Kern::MutexWait(*GlLock);
			if (GlUserRegsState == 0)
				{
				TPhysAddr phys = Epoc::LinearToPhysical(KHwBaseGl);
				TInt r = phys == KPhysAddrInvalid ? KErrNotFound
						: DPlatChunkHw::New(GlUserRegs, phys, 0x1000, EMapAttrUserRw | EMapAttrFullyBlocking);
				GlUserRegsState = r == KErrNone ? 1 : -1;
				Kern::Printf("SYBSTUB: DisplayDriver host GL registers for user mode: %d (phys %08x lin %08x)", r,
							 phys, r == KErrNone ? GlUserRegs->LinearAddress() : 0);
				}
			TUint32 bound = 0;
			if (GlUserRegsState > 0)		// this process's own kicks: its address space is its identity
				{
				TUint32 rights = GlRights(aClient);
				WriteReg(KHwBaseGl, KGlFlags, rights);
				WriteReg(KHwBaseGl, KGlBind, aClient->iOwningProcess->iId);
				bound = ReadReg(KHwBaseGl, KGlBind);
				Kern::Printf("SYBSTUB: DisplayDriver host GL bind pid %d sid %08x rights %d: %d",
							 aClient->iOwningProcess->iId, (TUint)Kern::ProcessSecureId(aClient->iOwningProcess),
							 rights, bound);
				}
			Kern::MutexSignal(*GlLock);
			NKern::ThreadLeaveCS();
			if (bound)
				lin = GlUserRegs->LinearAddress();
			}
		return Kern::ThreadRawWrite(aClient, a1, &lin, sizeof(lin));
		}
	case 3:		// {startRow, endRow, fullColour}: start < end < panel rows
		if (aWords[0] >= aWords[1] || aWords[1] >= PanelRows)
			return KErrArgument;
		SetPartial(ETrue, aWords[0], aWords[1], aWords[2] != 0);
		Kern::Printf("SYBSTUB: DisplayDriver partial display panel rows %d..%d %s, rotation %d -> picture %s",
					 aWords[0], aWords[1], aWords[2] ? "full colour" : "8 colours", Rotation,
					 (Rotation & 1) ? "columns" : "rows");
		return KErrNone;
	case 4:
		SetPartial(EFalse, 0, 0, EFalse);
		return KErrNone;
	case 5:
		return Kern::ThreadRawWrite(aClient, a1, &PartialActive, sizeof(TInt));
	case 10:	// screen index by value
		return (TInt)a1 == 0 ? KErrNone : KErrNotSupported;
	case 11:	// capabilities: exactly the client's block size (d_display.caps words)
		return Kern::ThreadRawWrite(aClient, a1, KCaps, CapsWords * sizeof(TUint32));
	case 31:	// scan-line bytes (r_display export 77 on the N95): the frame buffer stride, as on the
				// device (N95: 320 x 4 at every rotation; lcd_n95 programs the same pitch)
		return Kern::ThreadRawWrite(aClient, a1, &FbPitch, sizeof(TInt));
	case 26:	// current rotation index (last req 0x8003). r_display ord59 swaps HAL's native
				// 320x240 to the current 240x320 with it (CDirectScreenBitmap checks its rect so)
		return Kern::ThreadRawWrite(aClient, a1, &Rotation, sizeof(TInt));
		}
	return KStubNotHandled;
	}

static TInt DisplayReq(DThread*, TInt aReq, const TUint32* aArgs, TInt)
	{
	switch (aReq)
		{
	case 0x8001: return aArgs[0] == 0 ? KErrNone : KErrNotSupported;
	case 0x8002: return aArgs[0] == 1 ? KErrNone : KErrNotSupported;
	case 0x8003:
		if (aArgs[0] > 3 || (aArgs[0] && !KCaps[8 + aArgs[0]]))	// caps words 9..11: 90/180/270
			return KErrNotSupported;
		Rotation = aArgs[0];
		SetFbOrientation();
		if (PartialActive)		// the panel band stays, its place in the picture moves
			SetPartial(ETrue, PartialStart, PartialEnd, PartialFullColour);
		return KErrNone;
		}
	return KStubNotHandled;
	}

static const SStubCtl KCtl[] =
	{
	{ KStubRequest | 0x23, EPendRequest, 0, 0, 0 },
	};

// ---------------------------------------------------------------------------------------------
// Direct screen bitmap (DSB): CDirectScreenBitmap in ScDv, through r_display (docs/devices/nokia-n95-8gb-rm320/nokia-d_display.md
// section 2, caps word 2 = 1). Games draw into a driver buffer and post it with EndUpdate, which
// completes only after the frame went to the panel: that is the frame pacing games rely on.
//   ctl 23 a1 -> {x | w<<16, y | h<<16}, a2 = 1 double buffer / 2 incremental (single buffer)
//          create the buffer(s): physical RAM mapped user-readable/writable at one global address
//          (like the frame buffer itself, which ScDv also writes from user side)
//   ctl 24 a1 <- {TDisplayMode, address, w, h, line bytes, pixel shift, 0}
//          = BeginUpdate. r_display would treat word 1 as a chunk handle and add word 6 only when
//          caps word 3 != 0; the N95 reports 0 there, so the address is used as it is.
//          -18 no buffer, -39 a buffer is already being drawn (begun, not posted)
//   ctl 25 release (pending EndUpdate completes with KErrCancel)
//   req 0x2003        EndUpdate, whole buffer
//   req 0x2004 a1 ->  EndUpdate for the rect {x | w<<16, y | h<<16} (screen coordinates)
// A posted buffer is copied into the Syborg frame buffer at the next vsync (fb INT_CAUSE bit 0,
// fixed rate, QEMU property syborg-fb.refresh-rate, default 60 Hz) and the request completes then,
// so EndUpdate-paced loops run at the panel rate. WSERV's own updates (ctl 8/9) are not paced:
// the frame buffer is always live. Coordinates are portrait (our frame buffer memory layout).
// ---------------------------------------------------------------------------------------------
const TInt KFbW = 240, KFbH = 320, KBpp = 4;	// stride: FbPitch (ctl 31)
enum { KFbBase = 1, KFbIntMask = 6, KFbIntCause = 7, KFbFrameCount = 17, KFbRefreshRate = 18 };
const TInt KErrDsbBusy = -39;			// value the real driver returns (FUN_8012aa20)
const TInt KDisplayModeColor16MU = 11;	// TDisplayMode EColor16MU

struct SDsb
	{
	DPlatChunkHw* iChunk;	// global mapping of the buffers (user accessible)
	TPhysAddr iPhys;
	TLinAddr iKern;			// address of buffer 0 (same for kernel and user)
	TAny* iOwner;			// channel that created it
	TInt iX, iY, iW, iH;	// screen rect (portrait)
	TInt iBufBytes, iBuffers, iBack;	// buffer size, 1 or 2 buffers, buffer handed out next
	TBool iBegun;			// ctl 24 done, EndUpdate not yet
	// posted frame waiting for the vsync
	TBool iPending;
	DThread* iReqThread;
	TRequestStatus* iReqStatus;
	TAny* iReqChannel;
	TInt iReqBuf, iRx, iRy, iRw, iRh;
	TUint iFrames;			// frames presented
	};
static SDsb Dsb;
static NFastMutex DsbLock;
static DPlatChunkHw* FbChunk;
static TUint8* FbKern;

static void VsyncDfcFn(TAny*);
static TDfc VsyncDfc(VsyncDfcFn, NULL, 1);

// threads waiting for the next vsync (req 0x7f01), protected by DsbLock
const TInt KReqVsyncWait = 0x7f01;
const TInt KMaxVsyncWaiters = 16;
struct SVsyncWaiter { DThread* iThread; TRequestStatus* iStatus; TAny* iChannel; };
static SVsyncWaiter VsyncWaiters[KMaxVsyncWaiters];
static TInt VsyncWaiterCount;

static void SetVsyncIrq(TBool aOn)
	{
	TInt irq = NKern::DisableAllInterrupts();
	TUint m = ReadReg(KHwBaseClcd, KFbIntMask);
	WriteReg(KHwBaseClcd, KFbIntMask, aOn ? (m | 1) : (m & ~1u));
	NKern::RestoreInterrupts(irq);
	}

static void VsyncIsr(TAny*)
	{
	WriteReg(KHwBaseClcd, KFbIntCause, 1);
	VsyncDfc.Add();
	}

// complete a request of another thread and drop our reference on it (DFC or client context)
static void CompleteAndRelease(DThread* aThread, TRequestStatus* aStatus, TInt aReason)
	{
	Kern::RequestComplete(aThread, aStatus, aReason);
	NKern::ThreadEnterCS();
	aThread->Close(NULL);
	NKern::ThreadLeaveCS();
	}

// complete the vsync waiters (all, or those of aChannel) with aReason
static void VsyncWaitersComplete(TAny* aChannel, TInt aReason)
	{
	SVsyncWaiter done[KMaxVsyncWaiters];
	TInt n = 0;
	NKern::FMWait(&DsbLock);
	for (TInt i = 0; i < VsyncWaiterCount; )
		if (!aChannel || VsyncWaiters[i].iChannel == aChannel)
			{
			done[n++] = VsyncWaiters[i];
			VsyncWaiters[i] = VsyncWaiters[--VsyncWaiterCount];
			}
		else
			++i;
	NKern::FMSignal(&DsbLock);
	for (TInt i = 0; i < n; ++i)
		CompleteAndRelease(done[i].iThread, done[i].iStatus, aReason);
	}

static void VsyncWait(DThread* aClient, TAny* aChannel, TRequestStatus* aStatus)
	{
	NKern::FMWait(&DsbLock);
	TBool ok = VsyncWaiterCount < KMaxVsyncWaiters;
	if (ok)
		{
		SVsyncWaiter& w = VsyncWaiters[VsyncWaiterCount++];
		w.iThread = aClient; w.iStatus = aStatus; w.iChannel = aChannel;
		aClient->Open();
		}
	NKern::FMSignal(&DsbLock);
	if (ok)
		SetVsyncIrq(ETrue);
	else
		Kern::RequestComplete(aStatus, KErrOverflow);
	}

static void VsyncDfcFn(TAny*)
	{
	VsyncWaitersComplete(NULL, KErrNone);
	NKern::FMWait(&DsbLock);
	if (!Dsb.iPending)
		{
		TBool waiters = VsyncWaiterCount > 0;		// queued after the completion above
		NKern::FMSignal(&DsbLock);
		if (!waiters)
			SetVsyncIrq(EFalse);
		return;
		}
	const TUint8* src = (const TUint8*)Dsb.iKern + Dsb.iReqBuf * Dsb.iBufBytes
						+ (Dsb.iRy - Dsb.iY) * Dsb.iW * KBpp + (Dsb.iRx - Dsb.iX) * KBpp;
	TInt rw = Dsb.iRw, rh = Dsb.iRh, rx = Dsb.iRx, ry = Dsb.iRy, pitch = Dsb.iW * KBpp;
	DThread* t = Dsb.iReqThread;
	TRequestStatus* s = Dsb.iReqStatus;
	Dsb.iPending = EFalse;
	Dsb.iReqThread = NULL;
	TUint frames = ++Dsb.iFrames;
	NKern::FMSignal(&DsbLock);
	// the frame "goes to the panel": copy into the live frame buffer
	TUint8* dst = FbKern + ry * FbPitch + rx * KBpp;
	for (TInt y = 0; y < rh; ++y, src += pitch, dst += FbPitch)
		memcpy(dst, src, rw * KBpp);
	if (frames == 1 || (frames % 600) == 0)
		Kern::Printf("SYBDSB: frame %d presented at vsync %d (%d Hz)", frames,
					 ReadReg(KHwBaseClcd, KFbFrameCount), ReadReg(KHwBaseClcd, KFbRefreshRate));
	CompleteAndRelease(t, s, KErrNone);
	}

static TBool ClipRect(TInt& aX, TInt& aY, TInt& aW, TInt& aH, TInt aX0, TInt aY0, TInt aW0, TInt aH0)
	{
	TInt x1 = Max(aX, aX0), y1 = Max(aY, aY0);
	TInt x2 = Min(aX + aW, aX0 + aW0), y2 = Min(aY + aH, aY0 + aH0);
	if (x2 <= x1 || y2 <= y1)
		return EFalse;
	aX = x1; aY = y1; aW = x2 - x1; aH = y2 - y1;
	return ETrue;
	}

// cancel a pending EndUpdate (aChannel NULL = any); called in a client thread
static void DsbCancelPending(TAny* aChannel, TInt aReason)
	{
	NKern::FMWait(&DsbLock);
	DThread* t = NULL;
	TRequestStatus* s = NULL;
	if (Dsb.iPending && (!aChannel || Dsb.iReqChannel == aChannel))
		{
		t = Dsb.iReqThread;
		s = Dsb.iReqStatus;
		Dsb.iPending = EFalse;
		Dsb.iReqThread = NULL;
		}
	NKern::FMSignal(&DsbLock);
	if (t)
		CompleteAndRelease(t, s, aReason);
	}

static TInt DsbRelease(TAny* aChannel)
	{
	if (!Dsb.iChunk)
		return KErrNotReady;
	if (aChannel && Dsb.iOwner != aChannel)
		return KErrAccessDenied;
	DsbCancelPending(NULL, KErrCancel);
	NKern::ThreadEnterCS();
	Dsb.iChunk->Close(NULL);
	Epoc::FreePhysicalRam(Dsb.iPhys, Dsb.iBufBytes * Dsb.iBuffers);
	NKern::ThreadLeaveCS();
	Kern::Printf("SYBDSB: released after %d frames", Dsb.iFrames);
	memclr(&Dsb, sizeof(Dsb));
	return KErrNone;
	}

static TInt DsbCreate(DThread* aClient, TAny* aChannel, TAny* a1, TInt aFlags)
	{
	TUint32 r[2];
	TInt e = Kern::ThreadRawRead(aClient, a1, r, sizeof(r));
	if (e != KErrNone)
		return e;
	TInt x = r[0] & 0xffff, w = r[0] >> 16, y = r[1] & 0xffff, h = r[1] >> 16;
	Kern::Printf("SYBDSB: create rect %d,%d %dx%d flags %d by %O", x, y, w, h, aFlags, aClient);
	if ((aFlags != 1 && aFlags != 2) || w <= 0 || h <= 0 || x + w > KFbW || y + h > KFbH)
		return KErrArgument;
	if (Dsb.iChunk)
		return KErrInUse;
	NKern::ThreadEnterCS();
	if (!FbChunk)
		{
		e = DPlatChunkHw::New(FbChunk, ReadReg(KHwBaseClcd, KFbBase), Kern::RoundToPageSize(FbPitch * KFbH),
							  EMapAttrSupRw | EMapAttrBufferedC);
		if (e != KErrNone)
			{
			NKern::ThreadLeaveCS();
			return e;
			}
		FbKern = (TUint8*)FbChunk->LinearAddress();
		}
	SDsb d;
	memclr(&d, sizeof(d));
	d.iX = x; d.iY = y; d.iW = w; d.iH = h;
	d.iBuffers = aFlags == 1 ? 2 : 1;
	d.iBufBytes = Kern::RoundToPageSize(w * h * KBpp);
	TInt total = d.iBufBytes * d.iBuffers;
	e = Epoc::AllocPhysicalRam(total, d.iPhys);
	if (e == KErrNone)
		{
		e = DPlatChunkHw::New(d.iChunk, d.iPhys, total, EMapAttrUserRw | EMapAttrBufferedC);
		if (e != KErrNone)
			Epoc::FreePhysicalRam(d.iPhys, total);
		}
	NKern::ThreadLeaveCS();
	if (e != KErrNone)
		return e;
	d.iKern = d.iChunk->LinearAddress();
	memclr((TAny*)d.iKern, total);
	d.iOwner = aChannel;
	Dsb = d;
	return KErrNone;
	}

static TInt DsbBegin(DThread* aClient, TAny* a1)
	{
	TUint32 info[7] = {0, 0, 0, 0, 0, 0, 0};
	TInt e = KErrNone;
	NKern::FMWait(&DsbLock);
	if (!Dsb.iChunk)
		e = KErrNotReady;
	else if (Dsb.iBegun)
		e = KErrDsbBusy;
	else
		{
		info[0] = KDisplayModeColor16MU;
		info[1] = Dsb.iKern + Dsb.iBack * Dsb.iBufBytes;
		info[2] = Dsb.iW;
		info[3] = Dsb.iH;
		info[4] = Dsb.iW * KBpp;
		info[5] = 5;			// log2(bits per pixel)
		info[6] = 0;
		Dsb.iBegun = ETrue;
		}
	NKern::FMSignal(&DsbLock);
	if (e == KErrDsbBusy)
		return e;			// the real driver leaves the block alone here
	TInt w = Kern::ThreadRawWrite(aClient, a1, info, sizeof(info));
	return e != KErrNone ? e : w;
	}

static void DsbEnd(DThread* aClient, TAny* aChannel, TRequestStatus* aStatus, TInt aReq, TUint32 aRectPtr)
	{
	TInt e = KErrNone;
	TInt x = Dsb.iX, y = Dsb.iY, w = Dsb.iW, h = Dsb.iH;
	if (aReq == 0x2004)
		{
		TUint32 r[2];
		e = Kern::ThreadRawRead(aClient, (TAny*)aRectPtr, r, sizeof(r));
		if (e == KErrNone)
			{
			x = r[0] & 0xffff; w = r[0] >> 16; y = r[1] & 0xffff; h = r[1] >> 16;
			if (!ClipRect(x, y, w, h, Dsb.iX, Dsb.iY, Dsb.iW, Dsb.iH))
				e = KErrArgument;
			}
		}
	NKern::FMWait(&DsbLock);
	if (e == KErrNone && !Dsb.iChunk)
		e = KErrNotReady;
	else if (e == KErrNone && Dsb.iPending)
		e = KErrInUse;		// one posted frame at a time
	if (e == KErrNone)
		{
		Dsb.iPending = ETrue;
		Dsb.iReqThread = aClient;
		Dsb.iReqStatus = aStatus;
		Dsb.iReqChannel = aChannel;
		Dsb.iReqBuf = Dsb.iBack;
		Dsb.iRx = x; Dsb.iRy = y; Dsb.iRw = w; Dsb.iRh = h;
		Dsb.iBegun = EFalse;
		if (Dsb.iBuffers == 2)
			Dsb.iBack ^= 1;
		aClient->Open();	// (fast mutex held: Open is a simple atomic increment)
		}
	NKern::FMSignal(&DsbLock);
	if (e != KErrNone)
		Kern::RequestComplete(aStatus, e);
	else
		SetVsyncIrq(ETrue);
	}

class DDisplayChannel : public DNokiaStubChannel
	{
public:
	DDisplayChannel(DNokiaStubFactory* aF) : DNokiaStubChannel(aF), iPid(0) {}
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer)
		{
		TInt r = DNokiaStubChannel::DoCreate(aUnit, aInfo, aVer);
		if (r == KErrNone)
			{
			iPid = Kern::CurrentThread().iOwningProcess->iId;	// = the owner id of ctl 0x7f10
			GlOwnerOpened(iPid);
			}
		return r;
		}
	virtual ~DDisplayChannel()
		{
		if (iPid)
			GlOwnerClosed(iPid);
		VsyncWaitersComplete(this, KErrCancel);
		DsbCancelPending(this, KErrCancel);
		if (Dsb.iChunk && Dsb.iOwner == this)
			DsbRelease(this);
		}
	virtual TInt Request(TInt aFn, TAny* a1, TAny* a2)
		{
		DThread* client = &Kern::CurrentThread();
		switch (aFn)
			{
		case 23: return DsbCreate(client, this, a1, (TInt)a2);
		case 24: return DsbBegin(client, a1);			// every frame: not logged
		case 25: return DsbRelease(this);
		case ~0x2003:
		case ~0x2004:
			{
			TUint32 args[2] = {0, 0};
			if (a2)
				Kern::ThreadRawRead(client, a2, args, sizeof(args));
			DsbEnd(client, this, (TRequestStatus*)a1, ~aFn, args[0]);
			return KErrNone;
			}
		case 26:							// rotation index: sygl asks every frame, not logged
			return Kern::ThreadRawWrite(client, a1, &Rotation, sizeof(TInt));
		case ~KReqVsyncWait:				// every frame: not logged
			VsyncWait(client, this, (TRequestStatus*)a1);
			return KErrNone;
		case KMaxTInt:
			VsyncWaitersComplete(this, KErrCancel);
			DsbCancelPending(this, KErrCancel);
			break;
			}
		return DNokiaStubChannel::Request(aFn, a1, a2);
		}
private:
	TUint32 iPid;		// the process that opened the channel
	};

class DDisplayFactory : public DNokiaStubFactory
	{
public:
	DDisplayFactory(const TDesC& aName)
		: DNokiaStubFactory(aName, KCtl, sizeof(KCtl) / sizeof(KCtl[0]), ETrue, ETrue, DisplayReq, DisplayCtl) {}
	virtual TInt Install()
		{
		static TBool bound = EFalse;	// later loads of the LDD create (and drop) more factories
		if (!bound)
			{
			ReadDisplayConfig();
			KCaps[8] = PanelRows;
			if (!NativeLandscape)
				KCaps[9] = KCaps[10] = KCaps[11] = 1;
			VsyncDfc.SetDfcQ(Kern::DfcQue0());
			TInt r = Interrupt::Bind(EIntFb, VsyncIsr, NULL);
			if (r == KErrNone)
				r = Interrupt::Enable(EIntFb);
			bound = r == KErrNone;
			Kern::Printf("SYBDSB: vsync interrupt %d, refresh %d Hz", r, ReadReg(KHwBaseClcd, KFbRefreshRate));
			}
		return DNokiaStubFactory::Install();
		}
	virtual TInt Create(DLogicalChannelBase*& aChannel)
		{
		aChannel = new DDisplayChannel(this);
		return aChannel ? KErrNone : KErrNoMemory;
		}
	};

// other r_display requests (synchronous DoRequest + WaitForRequest) complete at once
_LIT(KStubName, "DisplayDriver");
DECLARE_STANDARD_LDD()
	{
	return new DDisplayFactory(KStubName);
	}
