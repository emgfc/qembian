# N-Gage QD: why the idle softkeys do nothing (static analysis, 2026-11-12)

Scope: Phone.app (ROM 0x50C9CFFC header, code 0x50C9CEE4.., Thumb; disassembly helper
`build/evidence/ngage-idle/dis.py START END` names `bl` targets and literals). Marks:
[V] read in the code, [I] inferred.

## The key path in Phone.app (all [V])

1. `CAknAppUi::HandleWsEventL` 0x5064D2D8: if `[this+0x50]==0` calls `CEikAppUi::HandleWsEventL`
   0x50605800 -> `CCoeAppUi::HandleWsEventL` 0x5090818C. Key events (types 1..3) go to
   `CCoeControlStack::OfferKeyL` 0x50907EA4 (`iStack=[appui+12]`): each stacked control, top
   down, skipping flag bit 0 (RefuseAllKeys), gets `OfferKeyEventL` (vtable+12); stop on 1.
   Not consumed -> virtual `HandleKeyEventL` (vtable index 6), which Phone does NOT override
   (vtable 0x50CC78FC holds Cone's default 0x509082B4, `return 0`).
2. Phone has no `AddToStackL` import: only the framework CBA (`CAknAppUi::Cba()`, 0x50CC25E0;
   `SetCommandSetL` 0x50CC1D04 from the CBA helper 0x50CBF7B8) and dialogs are on the stack.
   Phone's own key catcher 0x50CAC5B4 (`OfferKeyEventL`) only acts on scan 0xC5 (End),
   long press of EKeyDevice1 0xF843 (-> command 0x454) and scan 0xAA; a short right softkey
   falls through with 0 (EKeyWasNotConsumed, 0x50CAC6E6).
3. CBA -> `MEikCommandObserver::ProcessCommandL` = thunk 0x50CC454C -> Phone 0x50CA0510 ->
   `CAknAppUi::ProcessCommandL` -> Phone `HandleCommandL` 0x50CA0538:
   0x50CA06E0 handles 0x100 (exit), 0x451..0x459 (End key, help, ...); anything else goes to
   the current state object `[appui+0x7C]`, vtable method 10 (+0x30).
4. Idle state `HandleCommandL` 0x50CAD450: 0x47E = left idle softkey, 0x47F = right. The slot
   (`this+0x2C` left, `this+0x40` right) was filled at construction (0x50CAD29C) from SharedData
   strings `LeftIdle` / `RightIdle` (and `GameLaunch`, `IdleTxt`). Slot layout: +4 type,
   +8 message, +0xC app uid, +0x10 view uid. Dispatch: view type -> 0x50C9E5D8
   (`CCoeAppUi::ActivateViewL` with message); type 3 -> 0x50CAD794; type 0x22 -> 0x50CADB30;
   uid == -1 -> NOTHING (silent); view == -1: type 0x24 -> 0x50CBA2FC (GameLaunch),
   else 0x50C9E520 (`TApaTaskList::FindApp` + `BringToForeground`, or
   `RApaLsSession::StartApp` 0x50C9E570); else 0x50C9E5BC (`ActivateViewL(uid, view)`).
   No start-up / SharedData `state.val` / System Agent test anywhere on this path.

So the launch APIs the main session watched are the END of a chain whose first Phone-owned
step is `ProcessCommandL` 0x50CA0510. Nothing in Phone gates on Startup's state here; the
`0x10005943` (`state.val`) literals in Phone (0x50CC55BC..0x50CC76F8) belong to the SetState
(213) code and the start-up watcher, not to key handling [V for the idle handler, I for the rest].

## Two candidates that fit "events arrive, nothing follows"

- (A) [I] The key never reaches the CBA: a control higher on the stack consumes it (a dialog or
  note left behind by the first-boot queries) or the CBA entry has RefuseAllKeys. The clock
  dialogs worked because the dialog itself was on top then.
- (B) [I] The CBA delivers 0x47E/0x47F but the slot's uid is -1 (SharedData `LeftIdle` /
  `RightIdle` not readable: no operator ini on C:, or another value format), so 0x50CAD4C4
  returns silently. The labels can still come from the resource.

## Concrete change to try (live bisection, four breakpoints, one key press)

```
0x50907EA4  CCoeControlStack::OfferKeyL   r0=stack: [r0+4]=count; entries via At(): +0 ctrl, +8 flags, +12 prio
0x5060E1B0  CEikButtonGroupContainer::OfferKeyEventL   (CBA reached?)  r1->TKeyEvent, r2=type
0x50CA0510  Phone ProcessCommandL   r1 = command (expect 0x47E left / 0x47F right)
0x50CAD450  idle HandleCommandL     r1 = command; at 0x50CAD47A r4=slot: [r4+4] type, [r4+0xC] uid, [r4+0x10] view
```
- OfferKeyL hit but CBA not: candidate (A); dump the stack entries (control vtables tell the
  class) and remove / dismiss the top one.
- 0x50CAD450 hit with uid -1: candidate (B); read SharedData `LeftIdle`/`RightIdle` (file
  under C:\System\SharedData, operator.rsc at 0x50CC64xx) and give them values.
- Neither OfferKeyL nor CEikAppUi 0x50605800 for type 1: `[appui+0x50] != 0` in
  CAknAppUi::HandleWsEventL (an Avkon "event blocked" object) [I].

## Menu key and Startup

- [I] In S60 1.x the Menu key is captured by EikSrv, not Phone: break at
  `CEikServAppUiBase::HandleKeyEventL` 0x506391E0 (EikSrv.dll) to see whether it arrives.
- [V] `CStartupEventListener::RecordReadResponse` is a SysAp.app string (0x50B90E50), not
  Startup.app; SysAp also owns `KeyGuard`, `RawKey`, `startup.cmd/src/rea`, `state.val/src`.
  "Read response was 0" is SysAp reading a start-up record; no link to Phone's key path was
  found [I]. Startup.app staying alive does not block keys: they reach Phone's window group.
- No ISI reply is implied by this path; nothing for `isi-replies.txt` yet.
