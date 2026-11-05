# Nokia DSP bridge (`ehwa.ldd`, "Hwa"): what MmRmServerThread init needs

Static analysis of `Bal.dll`, `bridgeapi.dll`, `MmRmServer.dll`, `TestServerGwc.dll`, `Gwc.dll`
(Ghidra output in `build/n95/ghidra/`). Scope: only what makes `MmfAudioServer.exe::MmRmServerThread`
signal "server up" without a DSP. Tags: [verified] = seen in code/data, [inferred] = best guess.

## 0. Bottom line

- No DSP-side emulation is needed for init. [verified] Node creation in `bridgeapi.dll` is pure
  user-side bookkeeping; the only kernel traffic is a small "DCD registry" (uuid → file path), one
  chunk for the base-image upload, and three asynchronous controls that just have to be completed
  with 0. No message from the DSP is read back before the RM server signals its start semaphore.
- The current panic: `0x400001e` must return a non-zero word at `a1[1]`; because it stays 0,
  `bridgeapi` returns `KErrBadHandle`, closes the channel, and `Bal` then issues a second control on
  the closed (zero) handle → `KERN-EXEC 0` (section 3).

## 1. Init path and every control it issues

Entry: `MmRmServer.dll` thread entry `0x809307f8` → server `NewL` `FUN_8092df0c` → engine
`FUN_80930e48` → `FUN_80930cde` (Bal) → `FUN_80930c18` (TestServerGwc node) → `FUN_80930be4`
(AudioServerProxy, no bridge) → `RSemaphore::Signal` at `0x80930838` → `CActiveScheduler::Start`.
[verified] `MmfAudioServer` waits for that semaphore 30 s (`0x1c9c380` µs, `MmRmServer_ord1`
`0x80930878`) and leaves with the error otherwise.

Config read from `Z:\private\101F457f\MmRmDSPInitData.RSC` [verified, extracted from rofs1]:
base image `Z:\resource\plugins\baseimage.dof` (present in ROFS, 155756 bytes), plugin dir
`Z:\resource\plugins\`, then 61 node DLL names (`mpeg4aac_dec_dyn.dll55l`, ...), most of which
are not in ROFS (their registration is skipped user-side when `RFile::Open` fails; errors ignored).

Control numbers: `bridgeapi` uses bases `0x4000001` (PROC) and `0x400000c`; the DCD family is
`0x400001e + index` (`FUN_80a5db2a/db42`). `Bal.dll` adds plain controls 4..9 and `0x8000000 + n`
on the *same* channel (unit 0, "Hwa", version 1.1.1540). Only one channel is opened during init
(`BridgeUtil` is opened only by `DspMemMapper`, not on this path [verified]).

| # | Control | Name [inferred unless noted] | Caller | Argument block (a1 / a2) | Must come back | Accepted result |
|---|---|---|---|---|---|---|
| 1 | `0x400001e` | DCD_CreateManager | `FUN_80a5f018` (PROC_Attach, `bridgeapi_ord32`) | a1 → `{ RMutex handle "BridgeDcdSync", out mgr, thread id (0xFFFF8001 resolved), out charStep }`; a2 = 0 [verified] | `a1[1]` ≠ 0 (opaque DCD manager token, stored at dcdObj+0x30, passed back as **a2** of all other DCD controls; never used as a kernel handle [verified]). `a1[3]` = bytes per DSP char, **must be 2** [verified: `FUN_80a5ee30` steps the `.dcd_register` text by this value, and the section in `baseimage.dof` is 16-bit chars; 0 = infinite loop] | 0 |
| 2 | `0x400000e` | PROC lock/unlock (load state) | `bridgeapi_ord26` (PROC_Load) start/end | a1 → `TInt` 1 then 0; a2 = 0 | nothing | 0 |
| 3 | `0x4000023` | DCD_AutoRegister (start) | `FUN_80a5f0d4` | a1 = mgr token **as value**, a2 = 0 [verified] | nothing | 0 |
| 4 | `0x4000020` | DCD_Register | `FUN_80a5f0d4` (base image), `FUN_80a5f488` (each node DLL via `Bal_ord11`) | a1 → `struct { TInt count; const TDesC16* path; TUint8 uuid[64][16]; TInt type[64]; }` (0x508 bytes; `path` is a user `TBuf16<256>`); a2 = mgr [verified] | kernel must **remember** (uuid,type) → path. The base image registers 3 entries [verified from `.dcd_register`]: `6130D910…:0` (EAP_TASK node), `898FCE1D-29B6-454F-9E24-057F81D792ED:0` (TESTSERVER_NOKIA node), `93FA56D0-6FE5-11D4-B747-006008BDB66F:1` (processor) | 0 |
| 5 | `0x4000022` | DCD_EnumerateObject | `FUN_80a5f56c` (type 0), `FUN_80a5f598` (type 1) | a1 → `{ TInt index; TUint8 uuid[16] (out); TInt type }`; a2 = mgr [verified] | uuid of the index-th registered entry of that type | 0, or any error when exhausted (ends the loop; a type-0 miss at index 0 is fine, type 1 index 0 **must** succeed) |
| 6 | `0x400001f` | DCD_GetObjectDef | `FUN_80a5f5c2` (after each enumerate; node create) | a1 → `{ TUint8 uuid[16]; TInt type; TBuf16<256> path (out, at +0x14) }`; a2 = mgr [verified] | the registered path written as a 16-bit descriptor; the client then parses section `.<UUID>` of that file itself [verified] | 0; miss → `KErrArgument` (−6) [inferred: deplib lookup `FUN_80a5e12a` retries type 3→2 on −6] |
| 7 | `0x4000002` | PROC_Load: allocate upload chunk | `bridgeapi_ord26` | a1 → `TInt fileSize` (155756); no a2 [verified] | **return value = user handle of an RChunk** (result < 0 → treated as 0 → `RChunk::Base()`=0 → `KErrNoMemory`). Client reads the file into it then `Close()`s the handle [verified] | handle > 0 |
| 8 | `0x4000003` | PROC_Load (commit) | `bridgeapi_ord26` | none | nothing | 0 |
| 9 | `0x4000004` | PROC set processor attrs | `bridgeapi_ord26` | a1 → 0x5c-byte struct parsed from the processor object; a2 = 0 | nothing | 0 |
| 10 | `0x400000d` | PROC register overlay libs | `bridgeapi_ord26`, only if some node has load type 2 | a1 → count, a2 → list | nothing | 0 (not reached: all base-image nodes have load type 0 [verified from section text `...,iAlgName,0,...`]) |
| 11 | `0x4000016` / `0x400000c` | PROC alloc/free shared buffer (`bridgeapi_ord30/29`) | `BalBuffers.dll` from `Gwc_ord5`/`BalBufferManager_ord3` | ord30: a1 → `TPtr8` out, a2 → `TBuf8<4>{ size }` (page multiple); ord29: a1 → that `TPtr8` | [inferred] if the DSP-memory path (type 1/2) is used, the kernel must fill the `TPtr8` with user-addressable memory: `{ 0x20000000|size, size, userBase }`; the other path (`type 0`) is `User::Alloc` with no control. Which path `BalBufferManager_ord3` takes is unresolved. | 0 |
| 12 | `4` | Bal: create node channel | `FUN_80a41ddc` from `Bal_ord1` | a1 → `TBuf8<0x14>{ p1, p0, bridgeNodeStruct*, p2, p3 }`; a2 → `TInt out` [verified] | write a non-zero token to `*a2` (opaque; used as a1 of 5/6/7/0x800000x) | 0 |
| 13 | `5` | Bal: start node (async) | `FUN_80a41ddc` | a1 = node token; a2 = `TRequestStatus*` [verified: `{ KRequestPending, flags }` then `User::WaitForRequest`] | **complete the status** with 0 (non-zero → control 7 and failure) | 0 |
| 14 | `0x8000007` | Bal: wait node ready (async) | `Bal_ord28` from `TestServerGwc_ord5` | a1 = node token; a2 = `TRequestStatus*` [verified] | complete with 0 (status becomes `ord5`'s return → `LeaveIfError`; −2 = stop node) | 0 |
| 15 | `0x8000005` | Bal: listen for node messages (async) | `Bal_ord19` via `Gwc_ord22` | a1 = node token; a2 → `{ TRequestStatus*, 0 }` | **leave pending** (completing it runs the Gwc listener `RunL` on garbage) | 0 |
| 16 | `6` / `7` | Bal: stop (async, a2 = status) / delete node | teardown only | | complete 6 with 0 so teardown does not hang | 0 |

Order during init [verified]: 1, 2(1), 3, 4(base), 5(type 0, idx 0..2) each followed by 6, 7,
8, 5(type 1, idx 0), 6, 9, 2(0); then 4 ×(nodes whose DLL exists); then 6 (uuid `898FCE1D…`,
type 0), [11], 12, 13, 14, 15, Signal. `StiLdd.ldd` (`TestServerGwc_ord4`) is not in the ROM, so
its channel open fails and that path is skipped [verified].

User-side side effects to be aware of: the client parses `baseimage.dof` (DOFF) and the `.dcd_register`
/ `.<UUID>` sections itself, so the file must stay readable; `BridgeDcdSync` global mutex is created
by the client. `Bal_ord22` later calls `User::FreeLogicalDevice("Hwa")` only on failure.

## 2. DSP answers

None needed for the RM server's own init [verified]: no control reads a DSP message, notification
or node state before `Signal`. But the first client request (MmRmServerThread IPC 32 → EAP node init)
does send one message and blocks until it is answered: see section 5.

## 3. The KERN-EXEC 0

[verified] `bridgeapi_ord32` (`0x80a5d750`) → `FUN_80a5f018` → `0x400001e` returns 0 but
`a1[1]` is still 0 → `KErrBadHandle` (−8) → `bridgeapi_ord33` (`0x80a5d4fc`): `DoControl(0x4000001,
this+0x10, 0)` (the logged second control) then `RHandleBase::Close()` (handle := 0). `ord32`
returns −8 to `Bal` `FUN_80a4165c` (`0x80a4165c`), which calls `Bal_ord22` (`0x80a41608`) →
`bridgeapi_ord33` **again** → `RBusLogicalChannel::DoControl(0x4000001)` on handle 0 →
`Exec::ChannelControl` → `KERN-EXEC 0`. So: wrong handle = the closed channel handle, caused by the
missing manager token in reply 1 (not a handle the stub must return).

## 4. Stand-in design for `ehwa.cpp`

Control hook (`NOKIA_STUB_LDD_CTLHOOK`) plus per-channel state. The hook gets `aClient`, `a1`,
`a2` and the first 4 words; it reads larger blocks itself. State: a DCD table (96 entries of
`{uuid[16], type, path TBuf16<256>}`), the upload `DChunk*`, optional shared-buffer chunk. Keep
`completeRequests = ETrue` (no `DoRequest` is used by this path).

```cpp
// ehwa.cpp -- "Hwa" DSP bridge stand-in: enough for MmRmServerThread init (docs/nokia-dspbridge.md)
#include "nokiastub.h"
#include <memmodel/epoc/platform.h>   // EMapAttr*

struct SDcdEntry { TUint8 iUuid[16]; TInt iType; TBuf16<256> iPath; };
struct SDcdRegisterBlock { TInt iCount; const TAny* iPath; TUint8 iUuid[64][16]; TInt iType[64]; }; // 0x508
const TInt KMaxDcd = 96;
static SDcdEntry  Dcd[KMaxDcd];
static TInt       DcdCount;
static DChunk*    LoadChunk;          // 0x4000002 upload chunk
static DChunk*    BufChunk;           // 0x4000016 shared buffer (if that path is taken)
const TInt KMgrToken = 0x44434401;    // opaque DCD manager token
const TInt KNodeToken = 0x4e4f4445;   // opaque Bal node token
const TInt KLoadChunkSize = 0x40000;  // >= baseimage.dof (155756)

static TInt DcdFind(const TUint8* aUuid, TInt aType)
	{
	for (TInt i = 0; i < DcdCount; ++i)
		if (Dcd[i].iType == aType && memcompare(Dcd[i].iUuid, 16, aUuid, 16) == 0)
			return i;
	return KErrNotFound;
	}

static TInt NewChunk(DThread* aClient, TInt aSize, DChunk*& aChunk, TUint8*& aUserBase)
	{
	if (aChunk) { Kern::ChunkClose(aChunk); aChunk = NULL; }
	TChunkCreateInfo info;
	info.iType = TChunkCreateInfo::ESharedKernelMultiple;
	info.iMaxSize = (aSize + 0xfff) & ~0xfff;
	info.iMapAttr = EMapAttrCachedMax;
	info.iOwnsMemory = ETrue;
	TLinAddr kaddr; TUint32 attr;
	TInt r = Kern::ChunkCreate(info, aChunk, kaddr, attr);
	if (r == KErrNone) r = Kern::ChunkCommit(aChunk, 0, info.iMaxSize);
	if (r != KErrNone) { if (aChunk) Kern::ChunkClose(aChunk); aChunk = NULL; return r; }
	TInt h = Kern::MakeHandleAndOpen(aClient, aChunk);   // maps the chunk into the client process
	if (h < 0) { Kern::ChunkClose(aChunk); aChunk = NULL; return h; }
	aUserBase = Kern::ChunkUserBase(aChunk, aClient);
	return h;                                             // user-side RChunk handle
	}

static TInt HwaCtl(DThread* aClient, TInt aFn, TAny* a1, TAny* a2, const TUint32* w)
	{
	switch (aFn)
		{
	case 0x400001e:	// DCD_CreateManager: { mutexH, out mgr, threadId, out charStep }
		{
		TInt mgr = KMgrToken, step = 2;
		Kern::ThreadRawWrite(aClient, (TUint8*)a1 + 4, &mgr, 4);
		Kern::ThreadRawWrite(aClient, (TUint8*)a1 + 12, &step, 4);
		return KErrNone;
		}
	case 0x4000020:	// DCD_Register { count, path*, uuid[64][16], type[64] }
		{
		SDcdRegisterBlock* b = new SDcdRegisterBlock;   // 0x508 bytes: not on the kernel stack
		if (!b) return KErrNoMemory;
		TInt r = Kern::ThreadRawRead(aClient, a1, b, sizeof(*b));
		if (r == KErrNone)
			{
			TBuf8<512> path8;
			r = Kern::ThreadDesRead(aClient, b->iPath, path8, 0, KChunkShiftBy1);   // 16-bit descriptor
			for (TInt i = 0; r == KErrNone && i < b->iCount && i < 64; ++i)
				{
				TInt k = DcdFind(b->iUuid[i], b->iType[i]);
				if (k < 0) { if (DcdCount >= KMaxDcd) { r = KErrOverflow; break; } k = DcdCount++; }
				memcpy(Dcd[k].iUuid, b->iUuid[i], 16);
				Dcd[k].iType = b->iType[i];
				Dcd[k].iPath.Copy((const TUint16*)path8.Ptr(), path8.Length() / 2);
				}
			}
		delete b;
		return r;
		}
	case 0x4000021:	// DCD_Unregister { 1, 0, uuid, type }: forget it
		{
		TUint8 uuid[16]; TInt type;
		Kern::ThreadRawRead(aClient, (TUint8*)a1 + 8, uuid, 16);
		Kern::ThreadRawRead(aClient, (TUint8*)a1 + 0x408, &type, 4);
		TInt k = DcdFind(uuid, type);
		if (k >= 0) Dcd[k] = Dcd[--DcdCount];
		return KErrNone;
		}
	case 0x4000022:	// DCD_Enumerate { index, out uuid[16], type }
		{
		TInt index = w[0], type; TInt n = 0;
		Kern::ThreadRawRead(aClient, (TUint8*)a1 + 20, &type, 4);
		for (TInt i = 0; i < DcdCount; ++i)
			if (Dcd[i].iType == type && n++ == index)
				return Kern::ThreadRawWrite(aClient, (TUint8*)a1 + 4, Dcd[i].iUuid, 16);
		return KErrNotFound;
		}
	case 0x400001f:	// DCD_GetObjectDef { uuid[16], type, TBuf16<256> path out }
		{
		TUint8 uuid[16]; TInt type;
		Kern::ThreadRawRead(aClient, a1, uuid, 16);
		Kern::ThreadRawRead(aClient, (TUint8*)a1 + 16, &type, 4);
		TInt k = DcdFind(uuid, type);
		if (k < 0) return KErrArgument;
		TPtrC8 bytes((const TUint8*)Dcd[k].iPath.Ptr(), Dcd[k].iPath.Length() * 2);
		return Kern::ThreadDesWrite(aClient, (TUint8*)a1 + 0x14, bytes, 0, KChunkShiftBy1, aClient);
		}
	case 0x4000002:	// PROC_Load: chunk for the base image, return its user handle
		{
		TUint8* base;
		TInt size = (TInt)w[0] > KLoadChunkSize ? (TInt)w[0] : KLoadChunkSize;
		return NewChunk(aClient, size, LoadChunk, base);
		}
	case 0x4000016:	// PROC alloc shared buffer: a1 = TPtr8 out, a2 = TBuf8<4>{size} [inferred]
		{
		TInt size = 0; TUint8* base = NULL;
		Kern::ThreadRawRead(aClient, (TUint8*)a2 + 4, &size, 4);
		TInt h = NewChunk(aClient, size, BufChunk, base);
		if (h < 0) return h;
		TUint32 ptr[3] = { 0x20000000u | (TUint32)size, (TUint32)size, (TUint32)base };   // TPtr8
		return Kern::ThreadRawWrite(aClient, a1, ptr, sizeof(ptr));
		}
	case 4:			// Bal node create: a2 -> out node token
		{
		TInt tok = KNodeToken;
		return Kern::ThreadRawWrite(aClient, a2, &tok, 4);
		}
	case 5:			// Bal node start (a2 = TRequestStatus*)
	case 6:			// Bal node stop
	case 0x8000007:	// Bal wait node ready
		{
		TRequestStatus* s = (TRequestStatus*)a2;
		if (s) Kern::RequestComplete(aClient, s, KErrNone);
		return KErrNone;
		}
	case 0x8000005:	// Bal message listener: stay pending
		return KErrNone;
	default:		// 0x4000001 detach, 0x400000e, 0x4000023, 0x4000003, 0x4000004, 0x400000d, 0x400000c, 7, ...
		return KStubNotHandled;	// table/default: return 0, write nothing
		}
	}

NO_CONTROLS
NOKIA_STUB_LDD_CTLHOOK("Hwa", KCtl, ETrue, HwaCtl)
```

Notes:
- `KChunkShiftBy1` (`u32std.h`) selects a 16-bit user descriptor for `ThreadDesRead/Write`.
  `Kern::ThreadDesWrite` with a `TPtrC8` of raw bytes writes `len/2` 16-bit characters [inferred
  from the API; check the length the client sees in the `N95STUB` log].
- `Kern::MakeHandleAndOpen(DThread*, DObject*)` on a shared-kernel chunk adds it to the client
  process; the client's `RChunk::Base()` then works. The chunk is never freed by the client
  (it only closes the handle), so close it on channel destruction or on the next `0x4000002`.
- The `0x400001e` write of word 3 = 2 is mandatory, not cosmetic (section 1, row 1).
- If `BalBufferManager_ord3` turns out to use the `User::Alloc` path, `0x4000016` is never seen;
  if it uses the DSP path, the TPtr8 layout above is the guess to validate first.
- Static state is per-driver, fine for one client (MmfAudioServer); `Ivapolicyserver.exe` also
  links `bridgeapi` but is not on this path.

Residual risks after this: `FUN_80930be4` (AudioServerProxy ord70/77/75, same-process session to
the audio server) and anything the RM server does once clients connect (DevSound) were not analysed.

## 5. EAP node init message (`0x8000014` → listener → `0x8000015`)

Static analysis of `eapgwc.dll` (RVCT, Ghidra `build/n95/ghidra/eapgwc.dll.c`), `Gwc.dll` and
`eapcontrolmodule.dll`. Scope: one canned ack so that `eapgwc_ord3` returns 0; nothing else.

### 5.1 The send: control `0x8000014` (`Bal_ord23`, via `Gwc_ord27`) [verified]

- a1 = node token; a2 → `{ TRequestStatus* iSendStatus; TBuf8<12>* iMsg }`, both on `Bal_ord23`'s
  stack frame (`iSendStatus` = a2+8, matching the logged `0040bc80` / `80000001 00000002`).
- `iMsg` is the `TBuf8<12>` at Gwc object+0x30: `{ TUint len (type EBuf, 12); TUint max (12);
  TUint32 iCmd; TUint32 iArg1; TUint32 iArg2 }`. For every gateway send `iCmd = 0x20000000`
  ("payload is in the shared buffer"), `iArg1` = **user address** of the payload (`TDesC8::Ptr()` of
  the buffer obtained by `Gwc_ord4`, i.e. the `0x400000b` chunk buffer), `iArg2` = payload length.
  Pointers in the block: a2[0], a2[1], `iArg1` (user addresses of the calling thread).
- Payload of the init message (`eapgwc_ord3`, 0x84 bytes, 16-bit little-endian words):
  `u16[0] = 1` (EAP command, [inferred] "set client/preset name"), `u16[1]` = stale, `u16[2..65]` =
  the name's 8-bit chars widened, unused entries `0xffff`. The name comes from
  `eapcontrolmodule.dll` `FUN_80a0d888` → `FUN_80a10924(obj+8)` [inferred: a configured name].
- `Bal_ord23` then does `User::WaitForRequest(iSendStatus)` and returns the status value: this is
  where `!AudioServer` is stuck now. `Gwc_ord27` ignores the return value; `FUN_80a0cd64`
  (eapgwc, the synchronous send-and-wait) only continues if `Gwc_ord27` returned 0.

### 5.2 How the reply is delivered [verified]

Completing the listener alone is **not** enough; the receive control must return message words.

1. `FUN_80a0cd64` (`0x80a0cd64`): `obj+0x108 = 0`; `Gwc_ord27` (send); then loop
   `{ obj+0x104 = 0xffff; User::WaitForRequest(listener.iStatus); listener->RunL(); r = obj+0x104;
   break if r ∈ {0xffff, 0, 1, 4}; }`; returns `KErrCompletion` (−17) if `r == 0xffff` else
   `obj+0x108`. `listener` is the `CActive` at Gwc+0x60 whose `iStatus` is the `TRequestStatus*`
   passed in `0x8000005` (`0x0090b50c` in the log). The RunL is called by hand, not by the scheduler.
2. Listener `RunL` (`Gwc.dll` `0x80a43a54`, vtable `0x80a44254` slot +0x10, missing from the
   Ghidra listing) checks the completion code:
   - `KErrCompletion` (−17) → message available: `Bal_ord21` → control **`0x8000015`** with a1 =
     node token, a2 → `{ TRequestStatus* iRxStatus; TBuf8<12>* iRx }` (`iRx` at listener+0x40, its
     three words at +0x48/+0x4c/+0x50); synchronous (`User::WaitForRequest(iRxStatus)`, result =
     status value; non-zero → observer error callback(−2)). Then the payload descriptor is built:
     `iRx.iCmd == 0x20000000` → `TPtr8(iRx.iArg1, iRx.iArg2)` (`iArg1` used unchanged as a user
     pointer); any other `iCmd` → `TPtr8(&iRx.iCmd, 12)` (the three raw words are the payload).
     The listener is re-armed (`Bal_ord19` → a new `0x8000005`, same status address) and then
     the observer's `MessageReceived(payload)` runs (thunk `0x80a0d2ae` → eapgwc `FUN_80a0d24a`).
   - `KErrAbort` (−39) → re-arm + error callback(−39).
   - anything else, including 0 → re-arm only; `FUN_80a0cd64` then returns −17 to the caller.
3. `FUN_80a0d24a` (`0x80a0d24a`): `obj+0x104 = payload.u16[0]` (reply command), then
   `__ARM_switch8` (table `09 | 11 18 11 11 06 11 11 11 20 | 20`):
   - 0, 2, 3, 5, 6, 7 → client callback (client vtable+0x14)(gwc, payload), `obj+0x108` unchanged;
   - 1 → `obj+0x108 = payload.u32[1] ? payload.u32[1] : KErrGeneral`, then callback (an error reply);
   - 4 → halfword-swap `payload.u32[1]` and `u32[2]`, then callback (reply to the cmd-4 query);
   - 8 and ≥ 9 → `obj+0x104 = 0xffff`, no callback (ignored).

So for the init message the gateway expects: send status 0; listener status **−17**; `0x8000015`
status 0 with `iRx` words whose `u16[0] == 0` (ack). Cheapest reply: `iRx = { 0, 0, 0 }` (raw
12-byte payload, no shared-buffer write), which makes `eapgwc_ord3` return 0. The client callback
then sees a 12-byte zero payload [inferred: harmless for an ack; the caller `FUN_80a0d888` only uses
the return value].

### 5.3 Stub code for `ehwa.cpp` (replaces the `0x8000005` / `0x8000014` cases)

```cpp
static TRequestStatus* ListenStatus;	// 0x8000005 listener, NULL when not armed
static TBool   ReplyQueued;			// a send arrived while no listener was armed
static TUint32 ReplyWords[3];			// iRx words handed back by 0x8000015

	case 0x8000005:	// Bal node message listener: a2 -> { TRequestStatus*, 0 }
		{
		TUint32 l[2] = {0, 0};
		Kern::ThreadRawRead(aClient, a2, l, sizeof(l));
		ListenStatus = (TRequestStatus*)l[0];
		if (ReplyQueued && ListenStatus)
			{
			ReplyQueued = EFalse;
			Kern::RequestComplete(aClient, ListenStatus, KErrCompletion);	// -17 = "message waiting"
			ListenStatus = NULL;
			}
		return KErrNone;
		}
	case 0x8000014:	// Bal send: a2 -> { TRequestStatus* send, TBuf8<12>* msg }
		{
		TUint32 b[2] = {0, 0}, m[5] = {0};
		Kern::ThreadRawRead(aClient, a2, b, sizeof(b));
		Kern::ThreadRawRead(aClient, (TAny*)b[1], m, sizeof(m));	// len, max, cmd, arg1, arg2
		TUint16 eapCmd = 0;
		if (m[2] == 0x20000000 && m[3])
			Kern::ThreadRawRead(aClient, (TAny*)m[3], &eapCmd, 2);	// payload u16[0]
		Kern::Printf("N95STUB: Hwa send cmd %08x ptr %08x len %d eap %d", m[2], m[3], m[4], eapCmd);
		ReplyWords[0] = (eapCmd == 4) ? 4 : 0;	// raw 12-byte payload: u16[0] = reply command (0 = ack)
		ReplyWords[1] = 0; ReplyWords[2] = 0;
		if (b[0])
			Kern::RequestComplete(aClient, (TRequestStatus*)b[0], KErrNone);	// unblocks Bal_ord23
		if (ListenStatus)
			{
			Kern::RequestComplete(aClient, ListenStatus, KErrCompletion);	// RunL -> 0x8000015
			ListenStatus = NULL;
			}
		else
			ReplyQueued = ETrue;
		return KErrNone;
		}
	case 0x8000015:	// Bal receive: a2 -> { TRequestStatus* rx, TBuf8<12>* out }
		{
		TUint32 b[2] = {0, 0};
		Kern::ThreadRawRead(aClient, a2, b, sizeof(b));
		TInt r = Kern::ThreadRawWrite(aClient, (TUint8*)b[1] + 8, ReplyWords, sizeof(ReplyWords));
		if (b[0])
			Kern::RequestComplete(aClient, (TRequestStatus*)b[0], r);
		return KErrNone;
		}
```

Notes: `Kern::RequestComplete(DThread*, TRequestStatus*&, TInt)` nulls its second argument, hence
the locals. The `TBuf8<12>` length word of `iRx` is already 12 (set by its constructor), so only
the three data words are written. Static state is fine: one node, one client thread.

### 5.4 Further messages before IPC request 32 completes

Only one send (EAP cmd 1) was seen before the hang; what the `eapcontrolmodule` caller does after
`eapgwc_ord3` returns 0 was not traced [inferred: more sends are possible]. All other gateway sends
go through the same `FUN_80a0cd64`, so the code above answers them too:

| eapgwc export | payload `u16[0]` | size | reply that satisfies the wait loop |
|---|---|---|---|
| `ord8` | 0 (set parameter: id, value, type, 2 shorts, name) | 0x10 | ack `u16[0] = 0` |
| `ord2` | 5 (one short argument) | 4 | ack |
| `ord5` | 4 (query, one short) | 4 | `u16[0] = 4`, data words `u32[1..2]` (halfword-swapped by the handler); the stub returns zeros |
| `ord7` | 7 (table, from `FUN_80a0ced6`) | 0x50 | ack |
| `ord6` | 8 (table, from `FUN_80a0d09c`) | 0x30 | ack |

Reply commands 2, 3, 5, 6, 7 are asynchronous notifications (callback only, the wait loop keeps
waiting), so never send them from the stub.
