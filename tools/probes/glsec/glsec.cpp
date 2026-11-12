// glsec: hostile tests of the syborg-gl boundary (docs/gpu-boundary-roadmap.md phase 1), run as an
// ordinary app (no capabilities). Each test prints "GLSEC: <name> PASS|FAIL (...)"; the device logs every
// refusal as "syborg-gl: refused ..." in QEMU's stderr. A second instance ("glsec.exe b") is the other
// process of the scan-out test.
#include <e32base.h>
#include <e32debug.h>
#include "../../../src/qemu-syborg/hw/arm/syborg_gl_proto.h"

class RGlChan : public RBusLogicalChannel
	{
public:
	TInt Open() { return DoCreate(_L("DisplayDriver"), TVersion(1, 1, 1540), KNullUnit, NULL, NULL, EOwnerProcess); }
	TInt Ctl(TInt aFn, TAny* a1) { return DoControl(aFn, a1); }
	};

static RGlChan gChan;
static volatile TUint32* gRegs;
static TUint32 gPid;
static TUint32* gBuf;           // command words (our own chunk)
static TUint8* gOut;
static TInt gLen;
static SyglKick gKick;
static TInt gFails;

static void Emit(TUint32 aOp, TInt aN, const TUint32* aArgs)
	{
	gBuf[gLen] = aOp; gBuf[gLen + 1] = aN + 2;
	for (TInt i = 0; i < aN; i++)
		gBuf[gLen + 2 + i] = aArgs[i];
	gLen += aN + 2;
	}

static void Owner(TUint32 aOwner) { Emit(SYGL_OWNER, 1, &aOwner); }

// a user-mode kick of what is in gBuf, output at aOut; the device's result word
static TUint32 Kick(TUint32 aOut, TUint32 aOutLen, TUint32 aBuf = 0)
	{
	gKick.buf = aBuf ? aBuf : (TUint32)gBuf; gKick.len = gLen * 4; gKick.out = aOut; gKick.outlen = aOutLen;
	gKick.result = 0x12345678;
	__asm__ volatile("" ::: "memory");
	gRegs[SYGL_REG_KICK] = (TUint32)&gKick;
	__asm__ volatile("" ::: "memory");
	gLen = 0;
	return gKick.result;
	}

static void Check(const char* aName, TBool aPass, TUint32 aV)
	{
	RDebug::Printf("GLSEC: %s %s (%08x)", aName, aPass ? "PASS" : "FAIL", aV);
	if (!aPass)
		gFails++;
	}

static void Surf(TUint32 aId)
	{
	const TUint32 a[4] = { aId, 4, 4, 0 };
	Emit(SYGL_SURF, 4, a);
	}

static void Present(TUint32 aSurf, const TUint32* aSid)
	{
	const TUint32 a[5] = { aSurf, aSid[0], aSid[1], aSid[2], aSid[3] };
	Emit(SYGL_VG_PRESENT, 5, a);
	}

static const TUint32 KSid[4] = { 0x6c657367, 0x00000001, 0x00000002, 0x00000003 };	// not a real GCE id

static TInt Setup()
	{
	TUint32 q[2] = { 0, 0 };
	TInt r = gChan.Open();
	if (r == KErrNone)
		r = gChan.Ctl(0x7f10, q);
	gPid = q[1];
	TUint32 lin = 0;
	if (r == KErrNone && q[0])
		r = gChan.Ctl(0x7f13, &lin);
	gRegs = (volatile TUint32*)lin;
	static RChunk chunk;
	if (r == KErrNone)
		r = chunk.CreateLocal(0x20000, 0x20000);
	if (r == KErrNone)
		{
		gBuf = (TUint32*)chunk.Base();
		gOut = chunk.Base() + 0x10000;
		}
	RDebug::Printf("GLSEC: setup %d ready %d pid %d regs %08x", r, q[0], gPid, lin);
	return r == KErrNone && q[0] && lin ? KErrNone : KErrNotSupported;
	}

static void SecondProcess()
	{
	// the other process: present into the GCE surface id the first one presented
	Owner(gPid); Surf(1); Present(1, KSid);
	TUint32 r = Kick((TUint32)gOut, 0);
	Check("S3 present into another process's GCE surface refused", r == 10, r);	// owner 3 + surface 6 words: the present is at word 9
	Owner(gPid); { TUint32 a = 1; Emit(SYGL_SURF_FREE, 1, &a); } Kick((TUint32)gOut, 0);
	}

GLDEF_C TInt E32Main()
	{
	if (Setup() != KErrNone)
		{
		RDebug::Printf("GLSEC: no host GL with user kicks: nothing tested");
		return 0;
		}
	TBuf<8> cmd;
	User::CommandLine(cmd);
	if (cmd.Length() && cmd[0] == 'b')
		{
		SecondProcess();
		return 0;
		}

	// control: a good batch of our own
	Owner(gPid); Surf(1);
	{ const TUint32 a[4] = { 1, 0, 0, 4 }; Emit(SYGL_READ, 4, a); }
	TUint32 r = Kick((TUint32)gOut, 64);
	Check("control: own surface, read-back into own memory", r == 0, r);

	// S2: user writes to the kernel's registers are ignored; user reads of them give 0
	gRegs[SYGL_REG_BUF] = (TUint32)gBuf;
	gRegs[SYGL_REG_LEN] = 8;
	gRegs[SYGL_REG_PID] = 1;
	gRegs[SYGL_REG_FLAGS] = SYGL_RIGHT_KERNEL | SYGL_RIGHT_COMPOSITOR;
	TUint32 rb = gRegs[SYGL_REG_BUF], rf = gRegs[SYGL_REG_FLAGS];
	Check("S2 user cannot read/write BUF, FLAGS", rb == 0 && rf == 0, rb | rf);
	gRegs[SYGL_REG_EXEC] = 1;
	Check("S2 user EXEC ignored (no result)", gRegs[SYGL_REG_EXEC] == 0, 0);

	// S1: outputs where the app may not write
	Owner(gPid);
	{ const TUint32 a[4] = { 1, 0, 0, 4 }; Emit(SYGL_READ, 4, a); }
	r = Kick(0xC8000000, 64);			// kernel data (supervisor only)
	Check("S1 read-back into kernel memory refused", r != 0, r);
	Owner(gPid);
	{ const TUint32 a[4] = { 1, 0, 0, 4 }; Emit(SYGL_READ, 4, a); }
	r = Kick((TUint32)&E32Main & ~3u, 64);	// our own code: read-only
	Check("S1 read-back into read-only code refused", r != 0, r);
	Owner(gPid);
	r = Kick((TUint32)gOut, 0, 0x00001000);	// command words from an unmapped page
	Check("S1 commands from an unmapped page refused", r == 0xffffffffu, r);
	Owner(gPid);
	r = Kick((TUint32)gOut, 0, 0xC8000000);	// command words from kernel data
	Check("S1 commands from kernel memory refused", r == 0xffffffffu, r);

	// S3: another process's identity and rights
	Owner(gPid + 1);
	r = Kick((TUint32)gOut, 0);
	Check("S3 OWNER of another process refused", r == 1, r);
	Owner(gPid); { TUint32 a = gPid - 1; Emit(SYGL_OWNER_FREE, 1, &a); }
	r = Kick((TUint32)gOut, 0);
	Check("S3 OWNER_FREE of another process refused", r == 4, r);
	Owner(gPid); { TUint32 a = ~0u; Emit(SYGL_OWNER_FREE, 1, &a); }
	r = Kick((TUint32)gOut, 0);
	Check("S3 OWNER_FREE of everyone refused", r == 4, r);
	Owner(gPid); { const TUint32 a[5] = { 0, 0, 0, 0, 0 }; Emit(SYGL_COMPOSE, 5, a); }
	r = Kick((TUint32)gOut, 0);
	Check("S3 COMPOSE from an app refused", r == 4, r);
	Owner(gPid); Emit(SYGL_SCANOUT_FREE, 4, KSid);
	r = Kick((TUint32)gOut, 0);
	Check("S3 SCANOUT_FREE from an app refused", r == 4, r);

	// S3 through the kernel paths: the forged owner is refused there too
	Owner(gPid + 1);
	gKick.buf = (TUint32)gBuf; gKick.len = gLen * 4; gKick.out = (TUint32)gOut; gKick.outlen = 0;
	TInt e = gChan.Ctl(0x7f12, &gKick);
	gLen = 0;
	Check("S3 forged OWNER through ctl 0x7f12 refused", e != KErrNone, (TUint32)e);
	Owner(gPid + 1);
	TUint32 sub[4] = { (TUint32)gBuf, (TUint32)gLen * 4, (TUint32)gOut, 0 };
	e = gChan.Ctl(0x7f11, sub);
	gLen = 0;
	Check("S3 forged OWNER through ctl 0x7f11 refused", e != KErrNone, (TUint32)e);

	// S3 scan-out images: ours first, then the second process tries the same surface id
	Owner(gPid); Present(1, KSid);
	r = Kick((TUint32)gOut, 0);
	Check("control: present into a new GCE surface id", r == 0, r);
	RProcess p;
	e = p.Create(_L("glsec.exe"), _L("b"));
	if (e == KErrNone)
		{
		TRequestStatus st;
		p.Logon(st);
		p.Resume();
		User::WaitForRequest(st);
		p.Close();
		}
	RDebug::Printf("GLSEC: second process run %d", e);

	// review H1: one process claims at most 16 scan-out images (the table is everyone's); we hold one already
	TInt refusedAt = -1;
	for (TInt k = 1; k <= 20 && refusedAt < 0; k++)
		{
		const TUint32 sid[4] = { 0x6c657367, 0x100 + k, 2, 3 };
		Owner(gPid); Present(1, sid);
		if (Kick((TUint32)gOut, 0) != 0)
			refusedAt = k;
		}
	Check("H1 at most 16 GCE surfaces per process", refusedAt == 16, refusedAt);

	Owner(gPid); { TUint32 a = 1; Emit(SYGL_SURF_FREE, 1, &a); } Kick((TUint32)gOut, 0);
	RDebug::Printf("GLSEC: done, %d failed", gFails);
	return 0;
	}

struct SDaThreadCreateInfo
	{
	TAny* iHandle;
	TInt iType;
	TThreadFunction iFunction;
	TAny* iPtr;
	};
struct SStdEpocThreadCreateInfo;
IMPORT_C TInt DaSetupThreadHeap(TBool, SStdEpocThreadCreateInfo&) asm("_ZN8UserHeap15SetupThreadHeapEiR24SStdEpocThreadCreateInfo");

extern "C" void DaRunThread(TBool aNotFirst, SDaThreadCreateInfo& aInfo)
	{
	TInt r = DaSetupThreadHeap(aNotFirst, (SStdEpocThreadCreateInfo&)aInfo);
	if (r == KErrNone)
		{
		if (aNotFirst)
			r = (*aInfo.iFunction)(aInfo.iPtr);
		else
			{
			User::InitProcess();
			r = E32Main();
			}
		}
	User::Exit(r);
	}
extern "C" void DaInvariant() { User::Invariant(); }
extern "C" void DaHandleException(TAny* aInfo) { User::HandleException(aInfo); }
