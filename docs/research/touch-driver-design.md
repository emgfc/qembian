# Touch path design: a classic digitiser from the host finger to the window server

Static analysis, 2026-11-11, of the whole touch path (Android app / desktop window -> QEMU
`syborg-pointer` -> `exyin` stand-in -> kernel `DDigitiser` -> kernel event queue -> window server).
Nothing was run. Every claim is marked [verified] (read in the code) or [inferred].

Files read: `android/app/src/main/java/org/qembian/phone/MainActivity.kt` (`onScreenTouch`),
`android/native/syborg_android.c` (`in_bh`, `Java_..._touch`), `tools/sydb/phone.py` (`Screen`,
`pointer_sender`), `tools/sydb/sydb.py` (`tap`), `src/qemu-syborg/hw/arm/syborg_input.c`,
`build/android/qemu-11.1.3/ui/input.c`, `src/standins/exyin/exyin.cpp`, `exyin.mmp`,
`build/epocroot/sf/os/kernelhwsrv/kernel/eka/drivers/xyin/d_xyin.cpp`, `include/drivers/xyin.h`,
`kernel/eventq.cpp`, `kernel/sinit.cpp`, `kernel/stimer.cpp`, `euser/us_evnt.cpp`, `include/e32event.h`,
`include/e32hal.h`, `include/u32hal.h`, `halservices/hal/src/userhal.cpp`, `nkern/nk_timer.cpp`.

## 1. How the path works today

1. Host. Android: one `Native.touch(0, x, y, down)` per `MotionEvent` with the *current* sample only
   (`getX(i)`/`getY(i)`; the batched `getHistoricalX/Y` samples are not sent) [verified]. Each call
   allocates an `InEv` and schedules one main-loop bottom half that does `qemu_input_queue_abs` x,
   `qemu_input_queue_abs` y, `qemu_input_queue_btn` left, `qemu_input_event_sync` [verified].
   `qemu_input_queue_abs` is not a queue: it calls the handler's `.event` at once; `sync` calls `.sync`
   [verified, ui/input.c:515-528]. Desktop `phone.py`: a sender thread drains its queue and merges all
   queued moves into the newest one, then one QMP `input-send-event` per remaining item [verified,
   phone.py:340-370]; a QMP call delivers its events and then one sync [verified, ui/input.c:155-230].
2. Device `syborg-pointer`: 16-entry FIFO of *states* {x, y, z, buttons}; one entry per sync; when
   `count >= size-2` a move with unchanged buttons replaces the newest queued move; a full FIFO
   overwrites the newest entry [verified, syborg_input.c:316-337]. Level IRQ while `count && int_enabled`.
   No timestamp, no pointer id.
3. Guest `exyin`: the ISR drains the *whole* FIFO keeping only the last position and button state,
   latching an up (`iUpPending`) and a press (`iPressSamples = 2`) [verified, exyin.cpp:138-167]. A
   10 ms `NTimer` (own DFC thread, priority 27) samples `iCurX/iCurY` into `DDigitiser::iX[0]/iY[0]` and
   calls `RawSampleValid()`; the up is delivered by `PenUp()` at the next sample [verified, 179-222].
4. Kernel `DDigitiser` (`iPenDownDiscard 0`, `iPenUpDiscard 1`, `iNumXYSamples 1`): sample 1 of a press
   goes into the one-slot delay line (`EBufferFilling`), sample 2 pushes it out and issues
   `EButton1Down` at sample 1's position; every later sample issues `EPointerMove` at the *previous*
   sample's position; the sample still in the delay line when the pen goes up is never issued;
   `EButton1Up` is issued at `iLastPos` = the last issued move [verified, d_xyin.cpp:110-198, 258-288].
   `Kern::AddEvent` returns `KErrOverflow` when the 40-entry kernel event queue is full
   (`KMaxEventQueue = 40`, sinit.cpp:48; eventq.cpp:104-130) and `IssuePen*Event` ignore the result
   [verified, d_xyin.cpp:274-296].
5. Window server: the only consumer of the kernel queue (one `RequestEvent` at a time,
   `K::TryDeliverEvent`) [verified, eventq.cpp:138-152]. Belle's wserv/AknTouch/homescreen gesture
   code is proprietary and was not read; everything about it below is [inferred].

Timestamps: `TRawEvent::Set` stamps `iTicks = Kern::TickCount()` at issue time (eventq.cpp:338); the
system tick period is 15625 us (stimer.cpp:143); `TRawEvent` has no setter for `iTicks` [verified,
e32event.h]. So a driver cannot carry hardware timestamps into the raw event: the event's time is
*when the driver issued it*, with 15.6 ms granularity. The nanokernel tick used by `NTimer` is 1 ms
[inferred from the bsptemplate; `Syborg::MsTickPeriod` exists].

## 2. Root causes per symptom

### S1 "a weird pressed step": a press sticks or lingers

- R1 [verified mechanism, inferred that it is hit]: **the kernel event queue overflows and the pen-up
  is dropped.** The sampler issues a move every 10 ms for as long as the finger moves >= 1 screen px
  (`iAccThreshold 1`; 12-bit raw on 360 px means jitter crosses pixel edges constantly), i.e. 100
  events/s for the whole swipe. The queue holds 40. The window server consumes one raw event per
  round trip and, on one TCG vCPU shared with the homescreen's per-drag redraw and the GL forwarding,
  it falls behind on long swipes; the entries that arrive while it is full are silently lost
  (`KErrOverflow` ignored), and the last one of a swipe is the up. A lost up = the UI keeps the pressed
  state and the drag. This single cause explains all three of the owner's observations: short
  aggressive swipes are 10-30 events (never overflow), long swipes are 100-200, soft long swipes are
  100 Hz jitter moves for a second or more.
- R2 [verified]: even without overflow the up is late and displaced: the release is delivered at the
  next 10 ms sample, then the generic driver issues it at the position of the *last issued move*, and
  the final sample (up to 10 ms of motion plus one host batch) is thrown away by the delay line. The
  pen-down is also issued one sample late at sample 1's position (two samples after the press). On the
  homescreen the pressed highlight therefore appears late and ends late: the "step".
- R3 [verified]: a tap whose release arrives between sample 1 and sample 2 produces no event at all
  (`ProcessPenUp` in `EBufferFilling` -> `EIdle`), while one whose release precedes sample 1 produces a
  full down/up (`iPressSamples`): tap registration depends on phase, not on the finger.

### S2 "sequential swiping to the right swipes left then right"

- R1 again [inferred]: the up of swipe 1 was dropped (queue overflow) or joined (before 37ed561) so the
  UI is still dragging when swipe 2 presses at the left edge: the jump from the end of swipe 1 (right)
  to the start of swipe 2 (left) is a leftward drag, then the real rightward drag follows.
- R4 [verified]: when a release and the next press are drained in one ISR pass, the stand-in keeps only
  the newest position, so swipe 2's pen-down is issued at wherever the finger already is by the first
  sample, not where it touched: swipe 2 starts short. Not a reversal by itself, but it eats the first
  10-20 ms of every fast swipe.

### S3 "long aggressive or soft swipes get fucked; short aggressive ones work"

- R1 (overflow is proportional to swipe length), R2 (the flick's last segment is dropped: a UI that
  estimates velocity from the last two or three drags sees it stop before the up [inferred]),
  R5 [verified]: all 120-240 Hz Android samples between two 10 ms samples are collapsed to one; three
  consecutive samples can carry the same 15.6 ms `iTicks`; a velocity from two such events is
  quantised to 0, 1 or 2 ticks (+-50 %). Whether Belle's gesture code uses `TRawEvent::Ticks()`, wserv's
  own `TWsEvent::Time()` or its own clock is [inferred]; the issue *delay* and the dropped tail hurt in
  every case.

### Earlier today: the finger-up lost on fast back-and-forth swipes, pinch crashed Gallery

- The FIFO overwrite (fixed 8a413f1) and the join (fixed 37ed561) were two more ways to lose the up;
  R1 is the remaining one [verified that it is not handled anywhere].
- Pinch: `MainActivity` follows one finger and ignores the rest [verified]; the Gallery crash is an app
  reaction to whatever it got (a single pointer that jumps, or the up from `ACTION_POINTER_UP`), not
  something the driver path can explain statically. Multitouch readiness is in section 4.

Two residual details in the current stand-in, for completeness: the ISR and the sampler run
concurrently so `iCurX`/`iCurY` can be read torn (one axis new, one old) [verified]; `NTimer::OneShot`
on a pending timer returns `KErrInUse` and does not re-arm (nk_timer.cpp:86) [verified], which is why
the double start (ISR DFC + `WaitForPenDown` timer) is harmless.

## 3. The design: a classic path, every sample in order, no resampling

Principles (owner's rules): the host carries everything, the guest only forwards; typed events (down,
move, up) with a pointer slot and a timestamp from the device; no state machine with a delay line in
the guest; a button change is never dropped anywhere; overload is handled by coalescing *moves*
explicitly at each stage; multi-pointer in the format now, single pointer delivered until the
profile asks for more.

### 3.1 Device: `syborg-pointer` v2

Registers 0..8 keep their meaning (the N95's baseport `EPOINTER` mouse driver reads them; `BUTTONS`
stays the state after the event). New registers, all read-only and valid after a `LATCH`:

| reg | name | meaning |
|---|---|---|
| 9 | VERSION | 2 |
| 10 | CAPS | bit 0 timestamps, bit 1 typed events + slots, bits 8..15 slot count (8) |
| 11 | SLOT | pointer slot 0..7 of the latched event |
| 12 | TS_LO / 13 TS_HI | `qemu_clock_get_us(QEMU_CLOCK_VIRTUAL)` when the event was queued |
| 14 | FLAGS | bit 0 DOWN, 1 UP, 2 MOVE, 3 COALESCED (older moves of this slot were merged into it), 4 CANCEL |
| 15 | STATS | low 16 bits: events dropped since reset (should stay 0), high 16: coalesced |

FIFO: 256 typed entries `{x, y, z, buttons, slot, flags, ts}`. Policy on push:
- a MOVE when `count >= size - 8` and the newest entry is a MOVE of the same slot: replace it (keep
  the newest position, set COALESCED). The 8 spare slots are for button changes.
- otherwise append; if the FIFO is full of button changes (not reachable from fingers): overwrite the
  newest and count a drop in STATS.

Input sources: the single-touch path (`ABS` x/y + `BTN` left) maps to slot 0: a left press at sync =
DOWN, a release = UP, otherwise MOVE (also while hovering, buttons 0, for the mouse driver). The
multi-touch path (`INPUT_EVENT_KIND_MTT`: `begin`/`update`/`end`/`cancel` with `slot` and
`tracking-id`, plus `mtt_abs` x/y per slot; QEMU 11 `qemu_input_queue_mtt*`, QMP `input-send-event`
type `mtt`) [verified in ui/input.c:531-560 and qapi/ui.json:1079-1166] assembles per-slot pending
state and emits one entry per touched slot at sync. The handler mask becomes
`BTN | ABS | MTT`. `-global syborg-pointer.trace=on` logs each pushed entry on stderr (host-side
half of the test plan).

```c
/* syborg_input.c, pointer part (sketch; replaces SyborgPointerEvent/ptr_event/ptr_sync/ptr_read) */
#define PTR_FIFO_MAX 256
#define PTR_SLOTS 8
enum { PTR_DOWN = 1, PTR_UP = 2, PTR_MOVE = 4, PTR_COALESCED = 8, PTR_CANCEL = 16 };

typedef struct SyborgPointerEvent {
    uint32_t x, y, z, buttons, slot, flags;
    uint64_t ts;                   /* QEMU_CLOCK_VIRTUAL, us */
} SyborgPointerEvent;

typedef struct { uint32_t x, y; bool down, began, ended, cancel, moved; int32_t tid; } PtrSlot;

struct SyborgPointerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t fifo_size;            /* default 256 */
    bool absolute, input, trace;
    uint32_t int_enabled, count, head;
    uint32_t buttons;              /* single-touch button state (slot 0) */
    uint32_t dropped, coalesced;
    int32_t dz;
    SyborgPointerEvent cur;
    PtrSlot slot[PTR_SLOTS];
    SyborgPointerEvent fifo[PTR_FIFO_MAX];
    QemuInputHandlerState *hs;
};

static void ptr_push(SyborgPointerState *s, const SyborgPointerEvent *e)
{
    SyborgPointerEvent *last = s->count ? &s->fifo[(s->head + s->count - 1) % s->fifo_size] : NULL;
    if ((e->flags & PTR_MOVE) && last && (last->flags & PTR_MOVE) && last->slot == e->slot
        && s->count >= s->fifo_size - 8) {
        *last = *e; last->flags |= PTR_COALESCED; s->coalesced++;
    } else if (s->count < s->fifo_size) {
        s->fifo[(s->head + s->count) % s->fifo_size] = *e; s->count++;
    } else if (last) {
        *last = *e; s->dropped++;  /* 256 queued button changes: not reachable from fingers */
    }
    if (s->trace) {
        fprintf(stderr, "syborg-pointer: slot %u flags %02x %u,%u buttons %u ts %" PRIu64 " count %u\n",
                e->slot, e->flags, e->x, e->y, e->buttons, e->ts, s->count);
    }
    ptr_update(s);
}

static void ptr_event(DeviceState *dev, QemuConsole *src, QemuInputEvent *evt)
{
    SyborgPointerState *s = SYBORG_POINTER(dev);
    PtrSlot *p;
    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:                    /* single touch / mouse: slot 0 */
        p = &s->slot[0];
        if (evt->abs.axis == INPUT_AXIS_X) p->x = evt->abs.value;
        else if (evt->abs.axis == INPUT_AXIS_Y) p->y = evt->abs.value;
        p->moved = true;
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->btn.button == INPUT_BUTTON_WHEEL_UP && evt->btn.down) s->dz--;
        else if (evt->btn.button == INPUT_BUTTON_WHEEL_DOWN && evt->btn.down) s->dz++;
        else {
            uint32_t bit = evt->btn.button == INPUT_BUTTON_LEFT ? 1 :
                           evt->btn.button == INPUT_BUTTON_RIGHT ? 2 :
                           evt->btn.button == INPUT_BUTTON_MIDDLE ? 4 : 0;
            if (bit == 1) { if (evt->btn.down) s->slot[0].began = true; else s->slot[0].ended = true; }
            if (evt->btn.down) s->buttons |= bit; else s->buttons &= ~bit;
        }
        break;
    case INPUT_EVENT_KIND_MTT:                    /* multi-touch: one slot per finger */
        if (evt->mtt.slot < 0 || evt->mtt.slot >= PTR_SLOTS) break;
        p = &s->slot[evt->mtt.slot];
        switch (evt->mtt.type) {
        case INPUT_MULTI_TOUCH_TYPE_BEGIN:  p->began = true; p->tid = evt->mtt.tracking_id; break;
        case INPUT_MULTI_TOUCH_TYPE_END:    p->ended = true; break;
        case INPUT_MULTI_TOUCH_TYPE_CANCEL: p->ended = p->cancel = true; break;
        case INPUT_MULTI_TOUCH_TYPE_DATA:
            if (evt->mtt.axis == INPUT_AXIS_X) p->x = evt->mtt.value;
            else if (evt->mtt.axis == INPUT_AXIS_Y) p->y = evt->mtt.value;
            p->moved = true; break;
        default: break;
        }
        break;
    default: break;
    }
}

static void ptr_sync(DeviceState *dev)
{
    SyborgPointerState *s = SYBORG_POINTER(dev);
    uint64_t now = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
    for (int i = 0; i < PTR_SLOTS; i++) {
        PtrSlot *p = &s->slot[i];
        if (!(p->began || p->ended || p->moved) && !(i == 0 && s->dz)) continue;
        SyborgPointerEvent e = { .x = p->x, .y = p->y, .z = i == 0 ? (uint32_t)s->dz : 0,
                                 .buttons = i == 0 ? s->buttons : (p->down || p->began) && !p->ended,
                                 .slot = i, .ts = now };
        if (p->began && p->ended) {              /* a tap inside one sync: two entries, in order */
            e.flags = PTR_DOWN; ptr_push(s, &e); e.flags = PTR_UP | (p->cancel ? PTR_CANCEL : 0);
        } else if (p->began) e.flags = PTR_DOWN;
        else if (p->ended) e.flags = PTR_UP | (p->cancel ? PTR_CANCEL : 0);
        else e.flags = PTR_MOVE;
        ptr_push(s, &e);
        p->down = (p->began || p->down) && !p->ended;
        p->began = p->ended = p->cancel = p->moved = false;
    }
    s->dz = 0;
}
/* ptr_read: 9 -> 2; 10 -> 3 | (PTR_SLOTS << 8); 11 -> cur.slot; 12/13 -> cur.ts lo/hi; 14 -> cur.flags;
   15 -> dropped | coalesced << 16.  fifo-size default 256; property "trace".  Handler mask
   INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_MTT. */
```

### 3.2 Guest stand-in: `exyin` v2 (forwarding only)

The stand-in keeps deriving from `DDigitiser` for what it gives for free (the `EHalGroupDigitiser`
HAL entry, calibration, orientation, power, `iDfcQ`) but never feeds its state machine:
`RawSampleValid()`/`PenUp()` are never called, `WaitForPen*`, `FilterPenMove`, `ResetPenMoveFilter`
are empty, `iCfg.iPenUpDiscard = 1` only so `Create()` allocates its (unused) delay line
[verified, d_xyin.cpp:76-79]. Events go straight to `Kern::AddEvent` in a DFC thread.

Algorithm:
1. ISR: drain the device FIFO into a 256-entry guest ring (x, y, slot, flags, ts) in order; if the
   ring is full apply the same rule as the device (replace the newest move of the slot, never a button
   change); queue the delivery DFC (`TDfc::Add` is idempotent and legal from an ISR).
2. Delivery DFC (own thread `ExyinDfc`, priority 27 as today): pop entries in order, rotate the raw
   point (`exyin.rotate`), convert with the calibration matrix (`DigitiserToScreen`), and issue:
   - DOWN: if this slot is already down (an up was lost upstream), issue `EButton1Up` at the slot's
     last position first; then `EButton1Down` at the point.
   - MOVE: only while down and only if the screen point changed: `EPointerMove`.
   - UP / CANCEL: `EButton1Up` at the *release* point (the final position reaches the UI).
   - slots > 0 carry `SetPointerNumber(slot)`; slots >= the number of pointers the UI asked for
     (`EDigitiserHalSetNumberOfPointers`, default 1) are ignored, so today only slot 0 is delivered.
3. `Kern::AddEvent` result handling (the fix for R1): `KErrOverflow` on a DOWN/UP parks the entry and
   arms a 2 ms retry timer; nothing behind it is delivered before it succeeds (order). `KErrOverflow`
   on a MOVE drops that move (a newer one follows; the park/retry applies only when it is the last
   entry in the ring, so the finger's resting position is never lost). Overflows are counted and
   printed once per press in the existing `SYBSTUB: exyin press ...` line.
4. Optional pacing `exyin.min_move_us` (default 0 = every sample): a move closer than this to the
   previous *issued* move of the slot is merged into the next one. Keep 0 unless the window server
   measurably falls behind with 240 Hz moves (section 6).
5. `exyin.trace = 1` prints every issued event: `SYBSTUB: exyin ev D|M|U slot x,y ts=<us> tick=<n>`
   (the guest half of the test plan).

```cpp
// exyin.cpp v2 (sketch of the parts that change; calibration, ScreenSize, DoCreate's iCal/iCfg stay)
enum { KRegId = 0, KRegLatch = 1, KRegFifoCount = 2, KRegX = 3, KRegY = 4, KRegZ = 5, KRegButtons = 6,
       KRegIntEnable = 7, KRegFifoSize = 8, KRegVersion = 9, KRegCaps = 10, KRegSlot = 11,
       KRegTsLo = 12, KRegTsHi = 13, KRegFlags = 14, KRegStats = 15 };
enum { KFlagDown = 1, KFlagUp = 2, KFlagMove = 4, KFlagCoalesced = 8, KFlagCancel = 16 };
const TInt KRing = 256;
const TInt KSlots = 8;
const TInt KRetryMs = 2;

struct TSample { TInt iX, iY; TUint32 iTsLo, iTsHi; TUint8 iSlot, iFlags; };
struct TPointerState { TPoint iLast; TBool iDown; };

NONSHARABLE_CLASS(DSybDigitiser) : public DDigitiser
	{
public:
	// ... DDigitiser virtuals as today; WaitForPenDown/Up/UpDebounce, FilterPenMove, ResetPenMoveFilter: empty
	virtual TInt HalFunction(TInt aFunction, TAny* a1, TAny* a2);
	static void Isr(TAny* aPtr);
	static void DeliverDfcFn(TAny* aPtr) { ((DSybDigitiser*)aPtr)->Deliver(); }
	static void RetryExpired(TAny* aPtr) { ((DSybDigitiser*)aPtr)->iDeliverDfc.Add(); }
	void Push(const TSample& aS);			// ISR side
	TBool Pop(TSample& aS);					// DFC side
	void Deliver();
	TInt Issue(const TSample& aS);
	void Trace(char aKind, const TSample& aS, const TPoint& aP);
public:
	TDfc iDeliverDfc;
	NTimer iRetry;
	TSample iRing[KRing];
	volatile TInt iHead, iCount;			// ISR writes iCount up, DFC writes it down under NKern::Lock
	TSample iParked; TBool iHaveParked;
	TPointerState iPtr[KSlots];
	TInt iMaxPointers, iPointers;			// exyin.max_pointers (1), EDigitiserHalSetNumberOfPointers
	TInt iMinMoveUs, iTrace, iRotate, iOverflows, iPressEvents;
	TCal iCal, iCalSaved, iCalFactory;
	};

// DoCreate additions (after the DFC queue and calibration set-up of today):
//	iDeliverDfc.SetDfcQ(iDfcQ);
//	iMaxPointers = BoardConfigInt("exyin.max_pointers", 1); iPointers = 1;
//	iMinMoveUs = BoardConfigInt("exyin.min_move_us", 0); iTrace = BoardConfigInt("exyin.trace", 0);
//	version check: if (Reg(KRegVersion) < 2) Kern::Printf("SYBSTUB: exyin: pointer device v1, no typed events");
//	(a v1 device can be driven by deriving flags from button edges in the ISR: keep that fallback for one release)

void DSybDigitiser::Isr(TAny* aPtr)
	{
	DSybDigitiser& d = *(DSybDigitiser*)aPtr;
	while (Reg(KRegFifoCount))
		{
		WriteReg(KHwBaseKmiPointer, KRegLatch, 1);
		TSample s;
		s.iX = (TInt)Reg(KRegX) >> (15 - KRawBits);
		s.iY = (TInt)Reg(KRegY) >> (15 - KRawBits);
		s.iSlot = (TUint8)Reg(KRegSlot);
		s.iFlags = (TUint8)Reg(KRegFlags);
		s.iTsLo = Reg(KRegTsLo); s.iTsHi = Reg(KRegTsHi);
		d.Push(s);
		}
	Interrupt::Clear(EIntPointer);
	d.iDeliverDfc.Add();
	}

void DSybDigitiser::Push(const TSample& aS)
	{
	if (iCount == KRing)
		{
		TSample& last = iRing[(iHead + iCount - 1) % KRing];
		if ((aS.iFlags & KFlagMove) && (last.iFlags & KFlagMove) && last.iSlot == aS.iSlot)
			{ last = aS; last.iFlags |= KFlagCoalesced; return; }
		// a button change into a full ring: drop the newest move of that slot instead (scan back);
		// if there is none the ring is 256 button changes: overwrite the newest (not reachable from fingers)
		}
	iRing[(iHead + iCount) % KRing] = aS;
	++iCount;
	}

void DSybDigitiser::Deliver()
	{
	if (iHaveParked)
		{
		if (Issue(iParked) != KErrNone) { iRetry.OneShot(NKern::TimerTicks(KRetryMs)); return; }
		iHaveParked = EFalse;
		}
	TSample s;
	while (Pop(s))
		{
		if (s.iSlot >= iPointers)
			continue;							// a finger the UI did not ask for (single touch today)
		if (Issue(s) == KErrNone)
			continue;
		++iOverflows;
		if ((s.iFlags & KFlagMove) && iCount)
			continue;							// a newer move follows: this one is expendable
		iParked = s; iHaveParked = ETrue;		// a down, an up, or the last move: never dropped
		iRetry.OneShot(NKern::TimerTicks(KRetryMs));
		return;
		}
	}

TInt DSybDigitiser::Issue(const TSample& aS)
	{
	TInt x = aS.iX, y = aS.iY, t;
	switch (iRotate)
		{
	case 90:  t = x; x = KRawMax - y; y = t; break;
	case 180: x = KRawMax - x; y = KRawMax - y; break;
	case 270: t = x; x = y; y = KRawMax - t; break;
		}
	TPoint raw(Min(Max(x, 0), KRawMax), Min(Max(y, 0), KRawMax)), p;
	DigitiserToScreen(raw, p);
	TPointerState& ptr = iPtr[aS.iSlot];
	TRawEvent e;
	if (aS.iFlags & KFlagDown)
		{
		if (ptr.iDown)							// an up lost upstream: close the old press first
			{
			e.Set(TRawEvent::EButton1Up, ptr.iLast.iX, ptr.iLast.iY);
			if (aS.iSlot) e.SetPointerNumber(aS.iSlot);
			if (Kern::AddEvent(e) != KErrNone) return KErrOverflow;
			ptr.iDown = EFalse;
			}
		e.Set(TRawEvent::EButton1Down, p.iX, p.iY);
		}
	else if (aS.iFlags & (KFlagUp | KFlagCancel))
		{
		if (!ptr.iDown) return KErrNone;
		e.Set(TRawEvent::EButton1Up, p.iX, p.iY);
		}
	else
		{
		if (!ptr.iDown || p == ptr.iLast) return KErrNone;	// hover, or no pixel change
		e.Set(TRawEvent::EPointerMove, p.iX, p.iY);
		}
	if (aS.iSlot) e.SetPointerNumber(aS.iSlot);
	TInt r = Kern::AddEvent(e);					// a DFC thread, kernel unlocked: AddEvent takes a fast mutex
	if (r != KErrNone) return r;
	ptr.iLast = p;
	if (aS.iFlags & KFlagDown) ptr.iDown = ETrue;
	if (aS.iFlags & (KFlagUp | KFlagCancel)) ptr.iDown = EFalse;
	if (iTrace) Trace(aS.iFlags & KFlagDown ? 'D' : aS.iFlags & (KFlagUp | KFlagCancel) ? 'U' : 'M', aS, p);
	return KErrNone;
	}

// Pop: NKern::Lock(); if (!iCount) { unlock; return EFalse; } aS = iRing[iHead]; iHead = (iHead+1)%KRing; --iCount; NKern::Unlock()
// (the ISR only ever increments iCount / writes beyond head+count, so a short kernel lock on the DFC side is enough)

TInt DSybDigitiser::HalFunction(TInt aFunction, TAny* a1, TAny* a2)
	{
	switch (aFunction)
		{
	case EDigitiserHal3DInfo:						// HAL::EPointerMaxPointers and friends read this (userhal.cpp:812)
		{
		TPckgBuf<TDigitiserInfoV02> b;
		TDigitiserInfoV02& i = b();
		DigitiserInfo(i);							// the V01 part
		i.iZRange = 0;								// e32hal.h:208-225 [verified]: these ten members
		i.iThetaSupported = i.iPhiSupported = i.iAlphaSupported = i.iPressureSupported = 0;
		i.iProximityStep = 0; i.iMaxPressure = 0; i.iPressureStep = 0;
		i.iMaxPointers = (TUint8)iMaxPointers;
		i.iNumberOfPointers = (TUint8)iPointers;
		Kern::InfoCopy(*(TDes8*)a1, b);
		return KErrNone;
		}
	case EDigitiserHalSetNumberOfPointers:
		{
		TInt n = (TInt)a1;
		if (n < 1 || n > iMaxPointers) return KErrArgument;
		iPointers = n;
		return KErrNone;
		}
	default:
		return DDigitiser::HalFunction(aFunction, a1, a2);
		}
	}
```

Why not drop `DDigitiser` entirely: it costs nothing (the mmp already compiles `d_xyin.cpp`) and keeps
the seven calibration/state HAL functions and the extension entry; the state machine is simply never
entered (`Create()` calls `WaitForPenDown()` once, a no-op). If `HalFunction` were not virtual this
would not work; it is (`xyin.h:165`) [verified].

Profile: `[[standins]] name = "exyin" config = { rotate = 0, max_pointers = 1 }` for N8/808 now;
`max_pointers = 2` when multitouch is switched on (the 808/N8 panels are 2-finger Synaptics panels
[inferred]). The pointer device needs `fifo-size = 256` from `qemu-args` (or the new default).

### 3.3 Host contract

Both hosts deliver *every* sample, in order, each as its own device entry (one `sync` per sample),
and never merge button changes. Moves may be merged only when the sender is demonstrably behind, and
then the merged sample keeps the newest position.

Android (`MainActivity.kt` + `syborg_android.c`):
- one JNI call per `MotionEvent` with all its samples: the historical ones
  (`getHistoricalX(i, h)`, `getHistoricalY(i, h)`, `getHistoricalEventTime(h)`, `h < historySize`)
  then the current one; packed into a preallocated `IntArray` `[slot, kind, x, y, tMs] * n`;
  kind 0 move, 1 down, 2 up, 3 cancel; every pointer gets its own slot (`getPointerId` -> slot, the
  followed-finger logic goes away once the guest delivers more than slot 0; until then the native
  side forwards slot 0 only, the rest are `mtt` events the device queues and the guest ignores).
- no per-event allocation: the native side has a fixed SPSC ring (`TouchSample ring[1024]`, atomic
  write/read indexes), one `QEMUBH` created at start, `qemu_bh_schedule` (idempotent) per JNI call.
  The BH drains the ring and, per sample, emits `qemu_input_queue_mtt(begin|end|cancel)` /
  `qemu_input_queue_mtt_abs` x, y / `qemu_input_event_sync` (slot 0 may keep the `abs`+`btn` form as a
  transition). Key events go through the same ring so order between touch and keys is kept.

```kotlin
// MainActivity.kt: all samples of the event, in time order, one JNI call
private val tbuf = IntArray(5 * 128)
private fun onScreenTouch(v: View, e: MotionEvent): Boolean {
    val sc = Math.min(v.width.toFloat() / scrW, v.height.toFloat() / scrH)
    val dw = scrW * sc; val dh = scrH * sc; val dx = (v.width - dw) / 2; val dy = (v.height - dh) / 2
    var n = 0
    fun put(slot: Int, kind: Int, px: Float, py: Float, t: Long) {
        if (n * 5 + 5 > tbuf.size) return
        tbuf[n * 5] = slot; tbuf[n * 5 + 1] = kind
        tbuf[n * 5 + 2] = (((px - dx) / dw).coerceIn(0f, 1f) * 0x7fff).toInt()
        tbuf[n * 5 + 3] = (((py - dy) / dh).coerceIn(0f, 1f) * 0x7fff).toInt()
        tbuf[n * 5 + 4] = (t and 0x7fffffff).toInt(); n++
    }
    val a = e.actionIndex
    when (e.actionMasked) {
        MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN ->
            put(e.getPointerId(a), 1, e.getX(a), e.getY(a), e.eventTime)
        MotionEvent.ACTION_MOVE -> {
            for (h in 0 until e.historySize) for (i in 0 until e.pointerCount)
                put(e.getPointerId(i), 0, e.getHistoricalX(i, h), e.getHistoricalY(i, h), e.getHistoricalEventTime(h))
            for (i in 0 until e.pointerCount) put(e.getPointerId(i), 0, e.getX(i), e.getY(i), e.eventTime)
        }
        MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP ->
            put(e.getPointerId(a), 2, e.getX(a), e.getY(a), e.eventTime)
        MotionEvent.ACTION_CANCEL -> for (i in 0 until e.pointerCount) put(e.getPointerId(i), 3, e.getX(i), e.getY(i), e.eventTime)
    }
    if (n > 0) Native.touchBatch(tbuf, n)
    return true
}
```

```c
/* syborg_android.c: fixed ring, one bottom half, no allocation per event */
typedef struct { int slot, kind, x, y, t; } TouchSample;
#define TOUCH_RING 1024
static TouchSample touch_ring[TOUCH_RING];
static unsigned touch_w, touch_r;      /* written by the UI thread / the main loop; qatomic_* access */
static QEMUBH *touch_bh;

static void touch_bh_fn(void *opaque)
{
    unsigned w = qatomic_load_acquire(&touch_w);
    while (touch_r != w) {
        TouchSample *s = &touch_ring[touch_r % TOUCH_RING];
        if (s->slot == 0) {                       /* single-touch form until the guest takes slots > 0 */
            qemu_input_queue_abs(NULL, INPUT_AXIS_X, s->x, 0, 0x7fff);
            qemu_input_queue_abs(NULL, INPUT_AXIS_Y, s->y, 0, 0x7fff);
            if (s->kind == 1) qemu_input_queue_btn(NULL, INPUT_BUTTON_LEFT, true);
            else if (s->kind >= 2) qemu_input_queue_btn(NULL, INPUT_BUTTON_LEFT, false);
        } else {
            if (s->kind == 1) qemu_input_queue_mtt(NULL, INPUT_MULTI_TOUCH_TYPE_BEGIN, s->slot, s->slot);
            qemu_input_queue_mtt_abs(NULL, INPUT_AXIS_X, s->x, 0, 0x7fff, s->slot, s->slot);
            qemu_input_queue_mtt_abs(NULL, INPUT_AXIS_Y, s->y, 0, 0x7fff, s->slot, s->slot);
            if (s->kind == 2) qemu_input_queue_mtt(NULL, INPUT_MULTI_TOUCH_TYPE_END, s->slot, s->slot);
            if (s->kind == 3) qemu_input_queue_mtt(NULL, INPUT_MULTI_TOUCH_TYPE_CANCEL, s->slot, s->slot);
        }
        qemu_input_event_sync();                  /* one device entry per sample */
        qatomic_store_release(&touch_r, touch_r + 1);
    }
}

JNIEXPORT void JNICALL Java_org_qembian_phone_Native_touchBatch(JNIEnv *env, jclass cls, jintArray arr, jint n)
{
    jint tmp[5 * 128];
    if (n > 128) n = 128;
    (*env)->GetIntArrayRegion(env, arr, 0, n * 5, tmp);
    unsigned w = touch_w, r = qatomic_load_acquire(&touch_r);
    for (int i = 0; i < n; i++) {
        if (w - r == TOUCH_RING) break;           /* the main loop is 1024 samples behind: drop the rest, never a button change before a move */
        touch_ring[w % TOUCH_RING] = (TouchSample){ tmp[i*5], tmp[i*5+1], tmp[i*5+2], tmp[i*5+3], tmp[i*5+4] };
        w++;
    }
    qatomic_store_release(&touch_w, w);
    qemu_bh_schedule(touch_bh);                   /* touch_bh = qemu_bh_new(touch_bh_fn, NULL) once at start */
}
```
(The `qemu_input_queue_mtt*` signatures are `(src, type, slot, tracking_id)` and
`(src, axis, value, min, max, slot, tracking_id)` [verified, include/ui/input.h:83-86].)

Desktop `phone.py`: the sender keeps its queue but merges moves only when more than 4 items are
waiting (the daemon round trip is the only reason to merge), never a press or release; `sydb` gains
`swipe X0 Y0 X1 Y1 [--ms 150] [--steps 12] [--hold 0]` (one `input-send-event` per step, spaced in
time) next to `tap`, and `tap` sends press and release in two calls as it does.

Timestamps: the guest cannot put the device time into `TRawEvent` (no setter) [verified], so `ts` is
for diagnostics (host vs guest latency, the test plan) and for an optional later "replay at original
spacing" after a host stall. Do not build the replay now.

## 4. Multitouch readiness (not switched on)

- Device: slots 0..7, `tracking-id`, typed entries; QMP `mtt` events and the Android batch carry
  every finger already.
- Guest: `SetPointerNumber(slot)` on every event of slot > 0 [verified API, e32event.h:379]; the
  per-slot down/last state; `EDigitiserHal3DInfo` reports `iMaxPointers` from `exyin.max_pointers`
  and `EDigitiserHalSetNumberOfPointers` records what the UI asks for (userhal.cpp:805-890 maps
  `HAL::EPointerMaxPointers` / `EPointerNumberOfPointers` to exactly these two HAL functions)
  [verified for the HAL layer]. The window server on Symbian^3 reads `EPointerMaxPointers` at start
  and enables multi-pointer handling only when it is > 1 [inferred: the EPL wserv does; Belle's is
  proprietary], so `max_pointers = 1` keeps today's behaviour and `2` is the one-line switch to test
  the pinch once the rest of the path is in.
- Until then the Gallery pinch: slot 0 only, exactly as today (the second finger is a separate slot
  the guest ignores instead of teleporting the first one).

## 5. Test plan (no interactive reproduction)

1. Host unit test of the FIFO policy: move `ptr_push`/`ptr_sync` into `syborg_pointer_fifo.h` (as
   `syborg_gl2_precision.h` is tested by `tests/gl2_precision_test.c`) and add
   `tests/syborg_pointer_test.c`: (a) 300 moves then up -> count <= 256, order kept, the up is the
   last entry, COALESCED set only on moves, `dropped == 0`; (b) 100 x (down, move, up) -> 300
   entries intact; (c) a down and an up inside one sync -> two entries, down first; (d) two slots
   interleaved -> each slot's order kept.
2. Guest path, scripted (`tests/touch-path.sh ID`, an isolated `sydb --session build/sydb-touch
   start --seed ...` with `exyin.trace = 1` in a test variant and `-global syborg-pointer.trace=on`):
   inject through QMP (`sydb tap`, the new `sydb swipe`, raw `input-send-event` for bursts):
   - tap 100 ms -> trace has exactly `D` then `U`, both at the tap point (+- 1 px after calibration);
   - swipe right 12 steps / 150 ms -> `D`, >= 10 `M` with monotonically increasing x, `U` at the last
     injected point, in that order;
   - two swipes right with 5 ms between the release and the next press -> `D M.. U D M.. U`, never
     `D` twice without a `U`, the second `D` at the second swipe's first point (R4 gone);
   - a soft 2 s swipe of 400 steps -> one `U`, `U` after the last `M`, `exyin press` line reports
     `overflow N` and the up still delivered (R1 handled; N > 0 proves the queue does fill, which is
     the measurement for `exyin.min_move_us`);
   - a burst: 64 `input-send-event` calls as fast as the daemon allows, then up -> no lost up, host
     trace shows COALESCED only when `count` neared 248;
   - tap 10 ms -> `D` and `U` (R3 gone).
   Assertions are greps over `sydb log --grep 'exyin ev'` and the QEMU stderr; no screenshots needed.
   Optionally `sydb ui_screenshot` before/after the swipes to see the homescreen page change.
3. Regression: `tests/device-smoke.sh` for N8, 808 and 5800 (the 5800 single-touch panel runs the
   same stand-in) and an N95 boot (the pointer device with `input=off`: registers 0..8 unchanged).
4. Android: `adb logcat` with the device trace on shows `count` staying small on the S23 while
   swiping; the per-press `exyin press` line reports `overflow 0` or the chosen `min_move_us`.

## 6. Risks

- **Event rate into the window server** (the riskiest part): delivering every 120-240 Hz Android sample
  instead of 100 Hz resampled ones means more raw events, each a wserv wake-up and a client IPC on one
  TCG vCPU. wserv coalesces consecutive drags per client queue [inferred], but the kernel queue is
  40 deep and the retry path only protects correctness, not smoothness. Mitigation: the measurement in
  test 2 (overflow count per press) and `exyin.min_move_us` (8000 = ~120 Hz) if needed; the profile
  decides, not the code.
- `DDigitiser::Create` still calls `WaitForPenDown()` and allocates the delay line; the HAL `XYState`
  functions still toggle `DigitiserOn/Off` through the generic message queue: harmless, but
  `EDigitiserHalSetXYState` off must stop delivery (one flag in `Deliver`).
- The 12-bit raw grid (0..4095 on 640 px) is kept; moves are suppressed only when the *screen* point
  is unchanged, so soft swipes no longer emit 100 Hz jitter moves from sub-pixel noise (the host's
  samples are already integers on 0..0x7fff; two samples that map to the same pixel produce nothing).
- `vmstate` of the pointer device changes (entry layout, 256 entries, slots): bump `version_id` with
  a v1 loader or accept that snapshots taken before the change do not resume; the owner's `sydb snap`
  files are affected either way: say so in the commit.
- `TDigitiserInfoV02` (`e32hal.h:208-225`) adds exactly `iZRange`, `iTheta/Phi/Alpha/PressureSupported`,
  `iProximityStep`, `iMaxPressure`, `iPressureStep`, `iMaxPointers`, `iNumberOfPointers` to V01; the
  sketch sets them all [verified]. `TPckgBuf` zero-fills nothing: fill every member.
- A v1 device with a v2 guest (or the reverse) must not hang: the guest checks `VERSION` (reg 9 reads
  0 on v1) and falls back to deriving DOWN/UP from button edges; a v1 guest on a v2 device keeps
  working because registers 0..8 are unchanged.
- The window server's own drag/flick rules on Belle are unread; if the homescreen still misbehaves
  with a clean `D M.. U` trace the problem is above the driver, and the trace is the evidence.
- Not addressed: pinch in Gallery (needs `max_pointers = 2` and a real two-finger test), the
  15.6 ms `iTicks` granularity (kernel-wide, not a driver matter).
