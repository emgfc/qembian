# N73 (RM-132) DSP bridge ("Hwa" 1.1.1071): what differs from the N95 stand-in

Static analysis of the N73 `Sys/Bin/bridgeapi.dll` (XIP, code `0xf8523f58`, 0x5000 bytes, Thumb) and
`Sys/Bin/Bal.dll` (code `0xf8502178`, 0xb74 bytes) in `build/roms/nokia-n73-rm132/<fw>/core-flat.img`.
Import thunks were resolved against the N73 euser/efsrv export tables + S60 3rd FP1 `.dso` names.
Mock-level only. Compare with `docs/devices/nokia-n95-8gb-rm320/nokia-dspbridge.md` (N95, "Hwa"
1.1.1540). Tags: [verified] = seen in the N73 code, [inferred] = best guess.

## 0. Bottom line

- The N73 bridge is the N95 one with a different PROC numbering and a different image upload. No
  heavy emulation needed. [verified]
- **ctl `0x4000002` must return 0 on the N73.** The client passes the whole image in a heap buffer:
  `a1` = `User::Alloc` buffer that already holds all of `baseimage.dof`, `a2` = `TInt*` file size.
  There is no RChunk in N73 `bridgeapi.dll` at all (no `RChunk` import). [verified]
  The stand-in returns a chunk handle (positive). `PROC_Load` (`ord26`, `0xf8524032`) treats
  *any* non-zero result as failure and returns it (`0xf8524186` → `0xf85241a6`). [verified]
- **Where the NULL object comes from** [verified chain, last step's log line inferred]:
  1. `Bal` load wrapper `0xf8502222` sees `ord26` ≠ 0 and calls Bal close `0xf85021e8`.
  2. Bal close calls `ord10(Bal+40)` (`0xf8524ed8`). That frees the node-manager caches and sets
     `nodeMgr[+4]` (its copy of the DCD object pointer, taken from `bridge[+12]` by `ord9` at attach,
     `0xf8524f38`) to 0.
  3. Bal close then calls `ord33(Bal+4)` = detach: `RMutex` close, `delete` DCD object,
     `bridge[+12] = 0`, `DoControl(0x4000001, bridge+16, 0)`, `RHandleBase::Close`.
  4. `0xf8502222` returns the positive value. `User::LeaveIfError` (`0xf85022e8`) does not leave on
     it, so Bal keeps going and its export at `0xf8502732` calls `bridgeapi ord2(Bal+40)`
     (`0xf8525008`).
  5. `ord2` runs `f8525a70(nodeMgr[+4] = NULL)` → `RMutex::Wait(NULL+0x2C)`: that is the crash.

  So no answer was *missing*. The wrong answer was the positive return of `0x4000002`. A
  `ctl 0x4000001` log line should appear between the image chunk line and the crash [inferred,
  check the full log].

## 1. Controls issued by N73 bridgeapi up to and after the image load

Channel: "Hwa", unit 0, version 1.1.1071 (literal `0x42f` at `0xf85286b8`), opened by `ord32`
(PROC_Attach, `0xf8524220`) via `RBusLogicalChannel::DoCreate` (`0xf85286fa`). [verified]
Control bases: `0x4000001 + n` (PROC), DCD = `0x400000b + 19 + index` (`0xf85245fc`/`0xf8524614`,
so index 0 = `0x400001e`). [verified]

| # | Ctl | N73 meaning | a1 / a2 | Must come back | Accepted | N95 stand-in today | Change? |
|---|---|---|---|---|---|---|---|
| 1 | `0x400001e` | DCD create manager (`0xf8525ae8`) | a1 → `{+0 RMutex handle (dcd+0x2C), +4 out mgr, +8 RThread::Id of 0xFFFF8001 (log: 0x5c), +12 out}`; a2 = 0 [verified] | `a1[1]` ≠ 0, else `KErrBadHandle` (−8) → dcd+0x30. `a1[3]` → dcd+0x38 (char step, 2 as on N95 [inferred: registration worked with 2]) | 0 | writes mgr token + 2 | none |
| 2 | `0x400000d` | first PROC control of `PROC_Load` (`0xf852404a`), issued once before the DCD work | a1 = 0, a2 = 0 [verified] | nothing | 0 (≠0 aborts the load) | ignored → 0 | none |
| 3 | `0x4000023` | DCD auto-register (inside `0xf8525ba4`) | a1 = mgr token by value | nothing | 0 | ignored → 0 | none |
| 4 | `0x4000020` | DCD register | as N95 (`{count, TDesC16* path, uuid[64][16], type[64]}`), a2 = mgr | remember (uuid,type)→path. N73 base image registers 4 objects (log) | 0 | as N95 | none |
| 5 | `0x4000022` | DCD enumerate (`0xf852603c` type 0 loop; `0xf8526068` type 1) | as N95, a2 = mgr | uuid of index-th entry of that type | 0. Error ends the type-0 loop (the 4th returned −1, fine). Type 1 index 0 must succeed | as N95 | none |
| 6 | `0x400001f` | DCD get object def (`0xf8526092`) | as N95 | path in TBuf16 at +0x14 | 0 | as N95 | none |
| 7 | `0x400000c` | register overlay libs, only if a type-0 node has load type 2 (`[def+0x124]==2`) | a1 → `{count,…}` (sp+48), a2 = list head [verified] | nothing | 0 | (table/ignored) → 0 | none (not reached) |
| 8 | **`0x4000002`** | **PROC load image** (`0xf8524186`) | **a1 = user buffer with the whole file (log w[0..3] = DOFF header words), a2 → `TInt` file size** [verified: RFile::Size → User::Alloc → TPtr8 → RFile::Read → DoControl → User::Free] | nothing (the buffer is freed right after) | **0** (any ≠0 → load fails → crash chain in §0) | creates a chunk, returns handle | **yes** |
| 9 | `0x4000022` + `0x400001f` | enumerate type 1 index 0 + get def (processor object) | as above | as above | 0 | ok | none |
| 10 | `0x4000003` | PROC set processor attrs (`0xf85241fe`) | a1 → 0x5c-byte struct copied from the processor def; a2 = 0 [verified] | nothing | 0 | N95 numbering calls this "commit", ignored → 0 | none |

After `PROC_Load` returns 0, Bal calls `ord9(Bal+40, Bal+4)` (node manager; user-side only)
[verified]. bridgeapi node exports `ord1..ord8` issue only DCD controls (`0x400001e + n`), no
other DoControl lies in that code [verified: all DoControl call sites are `0xf8524026..0xf8524620`
and the wrappers `0xf852867a`/`0xf85286f4`]. Node *creation* on the N73 goes through a second
channel class in bridgeapi (`ord15..ord24`). It is opened through `0xf8528680` (DoCreate "Hwa"
1.1.1071) and uses controls `0x0c000001 + n` [verified numbers, layouts inferred]:
- `0x0c000001` (ord16), `0x0c000002` (ord18): a1 = object token (`[this]`), a2 → `TRequestStatus`
  `{0x80000001, 2}`, then `User::WaitForRequest`. **Complete with 0.** [inferred]
- `0x0c000001`/`0x0c000002` variants ord15/ord17: a1 = token, a2 → `{arg, buf, len}` (sync). Return 0. [inferred]
- `0x0c000003` (ord19), `0x0c000004` (ord20): a1 = token, a2 → `TInt` (in/out). Bal calls these at
  `0xf85026e0`/`0xf85026ea`. Return 0. Write a non-zero token if the caller reads it back [inferred].
These are the N73 equivalents of the N95's Bal controls `4..9` / `0x800000x`. They were not
reached yet, so check them in the next run's log.

Shared buffer (BalBuffers `ord30`, `0xf852434e`), only for buffer types 1/2: **`0x400000a`**
(N95: `0x400000b`/`0x4000016`). a1 → `TPtr8` out, a2 → `{.., .., size}` block on the stack (size at +8,
page multiple). Fill the `TPtr8` as the N95 stand-in does. [verified number, layout inferred from N95]

## 2. Version keying: aMajor does not separate the two phones

N95 opens "Hwa" **1.1.1540**, the N73 **1.1.1071**: both major 1 [verified from the N95 doc + N73
literal]. So `aMajor == 1` is true on both. Cheapest discriminators:
- `0x4000002`: N95 passes no a2 (a2 = 0) and a1 → `TInt size`. N73 passes a2 → `TInt size`
  [verified both]. Key on `a2 != 0`. No framework change is needed.
- Or extend `TStubControlHook` with the build (`iBuild < 1540` = N73 style). Only `ehwa.cpp` and
  `custom.cpp` use control hooks.

## 3. Proposed stand-in change (src/standins/ehwa/ehwa.cpp, not applied)

```cpp
	case 0x4000002:	// PROC load
		{
		if (a2)	// N73 (Hwa 1.1.1071): a1 = user buffer holding the whole image, a2 -> TInt size;
			{	// nothing to keep, any non-zero result makes bridgeapi detach (dspbridge-n73.md)
			TInt size = 0;
			Kern::ThreadRawRead(aClient, a2, &size, 4);
			Kern::Printf("SYBSTUB: Hwa image %d bytes inline -> 0", size);
			return KErrNone;
			}
		// N95 (1.1.1540): a1 -> TInt size, returns the user handle of an upload chunk (unchanged)
		TLinAddr base = 0;
		TInt size = (TInt)w[0] > KLoadChunkSize ? (TInt)w[0] : KLoadChunkSize;
		TInt h = NewChunk(aClient, size, LoadChunk, base);
		Kern::Printf("SYBSTUB: Hwa image chunk %d bytes at %08x -> handle %08x", size, base, h);
		return h;
		}
```

Optional, when the N73 reaches them (keyed by build, which needs the hook extension below):
- `0x400000a` with build < 1540 → same body as `0x400000b` (shared buffer).
- `0x0c000001`/`0x0c000002` with a2 → TRequestStatus: complete with 0 (`Kern::RequestComplete`
  on the user status, as the N95 Bal node-start handler does).

Hook extension (only if build keying is wanted). It covers both hook users (`nokiastub.h:112`):
```cpp
typedef TInt (*TStubControlHook)(DThread* aClient, TInt aFn, TAny* a1, TAny* a2,
								 const TUint32* aWords, TInt aBuild);
// in the channel: store iBuild = aVer.iBuild next to iMajor; call iCtlHook(..., w, iBuild)
```

## 4. Not checked
- Exact layouts of the `0x0c00000x` family and which N73 DLL calls `ord15..18` (not Bal; no thunk
  for them in core, maybe ROFS).
- Whether the N73 base image's type-1 (processor) entry is among the 4 registered objects. The
  `PROC_Load` tail needs it (`0xf8526068` index 0) [inferred yes].
