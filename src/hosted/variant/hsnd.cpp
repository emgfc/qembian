// Hosted EKA2: syborg-snd on the hosted board, as a sink without output for now. The register map, status
// bits and interrupt are QEMU's (src/qemu-syborg/hw/arm/syborg_audio.c): 16 slots, each a guest ring the
// device consumes; a guest pacing on READ_POS (the ehwa stand-in, the N95's audio) runs at real time.
// Here a host thread consumes each running ring at its playback rate (rate x frame bytes; encoded MP3/AAC
// rings at a nominal byte rate) without playing anything, and raises the watermark, end-of-stream and
// underrun events as the QEMU device does. Bug 2026-11-12: without a model the page was plain memory, READ_POS
// never moved, the start-up tone stopped after 140 buffers and Startup never finished (no idle screen).
// Original work for qembian.

#include <kernel/kern_priv.h>

extern TInt HostedIoAttach(TUint32 aBase, TUint32 aSize, TUint32 (*aRead)(TAny*, TUint32),
                           void (*aWrite)(TAny*, TUint32, TUint32), TAny* aCtx);
extern void HostedRaiseIrq(TInt aIrq);
extern void HostedLowerIrq(TInt aIrq);

static const TUint32 KSndIo = 0xC6000000u + 0x0e * 0x1000;	// KHwSVPAudioDevice
static const TInt KSndIrq = 13;
static const TInt KSlots = 16;
static const TUint32 KId = 0xc51d0012u;
enum { ST_RUN = 1, ST_EOS = 2, ST_UNDER = 4, ST_ERR = 8, ST_WM = 16, ST_PAUSE = 32 };
enum { F_S16 = 0, F_S8 = 1, F_ALAW = 2, F_MP3 = 3, F_AAC = 4, F_S24_32 = 5, F_S32 = 6 };
static const TUint32 KEncodedBytesPerSec = 16000;	// MP3/AAC without a decoder: ~128 kbit/s

struct SSlot
	{
	TUint32 iFormat, iSwap16, iRate, iChannels, iRingAddr, iRingSize, iWatermark, iGain, iClass, iStatus, iPlanar;
	TUint64 iWpos, iRpos, iFrames;
	TBool iEosSet, iWmArmed;
	TUint64 iDebt;			// consumption owed, in bytes * 1e9 (sub-byte remainder between steps)
	};
static SSlot Slots[KSlots];
static TUint32 IntStatus, IntEnable, Master = 256, Route;
static TUint64 Clock;		// 48 kHz frames since start
static volatile TInt Lock;
static volatile TInt PumpSeq;
static volatile TInt PumpTid;
static TUint64 LastNs;

static void Acquire() { while (__atomic_exchange_n(&Lock, 1, __ATOMIC_ACQUIRE)) hsys(SYS_sched_yield); }
static void Release() { __atomic_store_n(&Lock, 0, __ATOMIC_RELEASE); }
// A nanothread holds the device lock with interrupts disabled: an interrupt suspends the running nanothread,
// and an ISR that reads this device would then spin for ever on the lock it holds. Bug 2026-11-12: ehwa's
// sound ISR reads the status register; start-up deadlocked at the start-up tone (kn_efile hang dump).
// ISRs and the host helper threads (timers, pump) only take the spinlock.
static TInt Enter()
	{
	TInt irq = -1;
	if (LxSelf() && LxSelf()->iNThread && NKern::CurrentContext() != NKern::EInterrupt)
		irq = NKern::DisableAllInterrupts();
	Acquire();
	return irq;
	}

static void Leave(TInt aIrq)
	{
	Release();
	if (aIrq >= 0)
		NKern::RestoreInterrupts(aIrq);
	}



static void UpdateIrq()
	{
	if (IntStatus & IntEnable)
		HostedRaiseIrq(KSndIrq);
	else
		HostedLowerIrq(KSndIrq);
	}

static void Raise(TInt k, TUint32 aBits)
	{
	Slots[k].iStatus |= aBits;
	IntStatus |= 1u << k;
	UpdateIrq();
	}

static TBool Encoded(const SSlot& s) { return s.iFormat == F_MP3 || s.iFormat == F_AAC; }
static TUint32 FrameBytes(const SSlot& s)
	{
	return (s.iFormat == F_S16 ? 2 : s.iFormat >= F_S24_32 ? 4 : 1) * (s.iChannels == 2 ? 2 : 1);
	}
static TUint32 Unit(const SSlot& s)
	{
	if (Encoded(s))
		return 1;
	return s.iPlanar && s.iChannels == 2 ? s.iPlanar * FrameBytes(s) : FrameBytes(s);
	}
static TUint32 Queued(const SSlot& s)
	{
	TUint64 q = s.iWpos > s.iRpos ? s.iWpos - s.iRpos : 0;
	return q > s.iRingSize ? s.iRingSize : (TUint32)q;
	}
static TUint32 BytesPerSec(const SSlot& s)
	{
	return Encoded(s) ? KEncodedBytesPerSec : s.iRate * FrameBytes(s);
	}

static void Start(TInt k)
	{
	SSlot& s = Slots[k];
	s.iStatus &= ~(ST_RUN | ST_PAUSE);
	s.iEosSet = EFalse;
	s.iWpos = s.iRpos = s.iFrames = 0;
	s.iDebt = 0;
	s.iWmArmed = ETrue;
	TBool ok = s.iRingSize >= 16 && s.iRingSize <= 16 * 1024 * 1024 && s.iFormat <= F_S32 &&
	           (Encoded(s) || (s.iRate && s.iRate <= 192000));
	if (!ok)
		{
		s.iStatus = ST_ERR;
		Raise(k, ST_ERR);
		return;
		}
	s.iStatus = ST_RUN;
	}

// consume what the elapsed time has played; events as syborg_audio.c snd_slot_events
static void Pump()
	{
	TUint64 now = hnow_ns();
	TUint64 dt = LastNs ? now - LastNs : 0;
	LastNs = now;
	if (dt > 100000000ull)
		dt = 100000000ull;			// a stopped VM: do not drain everything at once
	Clock += dt * 48000 / 1000000000ull;
	for (TInt k = 0; k < KSlots; ++k)
		{
		SSlot& s = Slots[k];
		if (!(s.iStatus & ST_RUN) || (s.iStatus & ST_PAUSE))
			continue;
		s.iDebt += dt * BytesPerSec(s);
		TUint64 want = s.iDebt / 1000000000ull;
		TUint32 unit = Unit(s);
		TUint64 can = Queued(s) / unit * unit;
		TUint64 take = want < can ? want / unit * unit : can;
		s.iRpos += take;
		s.iDebt -= take * 1000000000ull;
		if (take < want)
			s.iDebt = 0;				// nothing to play: no credit carried into the next write
		if (!Encoded(s))
			s.iFrames += take / FrameBytes(s);
		TUint32 freeBytes = s.iRingSize - Queued(s);
		if (s.iWmArmed && s.iWatermark && freeBytes >= s.iWatermark)
			{
			s.iWmArmed = EFalse;
			Raise(k, ST_WM);
			}
		if (take >= want)
			continue;
		if (s.iEosSet && Queued(s) < unit)
			{
			s.iRpos = s.iWpos;
			s.iEosSet = EFalse;
			s.iStatus &= ~ST_RUN;
			Raise(k, ST_EOS);
			}
		else if (!s.iEosSet)
			s.iStatus |= ST_UNDER;
		}
	}

static TBool AnyRunning()
	{
	for (TInt k = 0; k < KSlots; ++k)
		if ((Slots[k].iStatus & ST_RUN) && !(Slots[k].iStatus & ST_PAUSE))
			return ETrue;
	return EFalse;
	}

static int PumpThread(void*)
	{
	for (;;)
		{
		TInt seq = __atomic_load_n(&PumpSeq, __ATOMIC_SEQ_CST);
		Acquire();
		Pump();
		TBool busy = AnyRunning();
		if (!busy)
			LastNs = 0;
		Release();
		if (busy)
			hsleep_us(5000);			// 5 ms steps while something plays
		else
			hfutex_wait(&PumpSeq, seq);	// idle until a slot starts
		}
	}

static void Wake()
	{
	__atomic_add_fetch(&PumpSeq, 1, __ATOMIC_SEQ_CST);
	hfutex_wake(&PumpSeq, 1);
	}

static TUint32 SndRead(TAny*, TUint32 aOff)
	{
	TInt irq = Enter();
	Pump();
	TUint32 v = 0;
	if (aOff >= 0x100)
		{
		TInt k = (aOff - 0x100) / 0x40;
		if (k < KSlots)
			{
			SSlot& s = Slots[k];
			switch ((aOff & 0x3f) >> 2)
				{
			case 1: v = s.iFormat | s.iSwap16 << 8; break;
			case 2: v = s.iRate; break;
			case 3: v = s.iChannels; break;
			case 4: v = s.iRingAddr; break;
			case 5: v = s.iRingSize; break;
			case 6: v = (TUint32)s.iWpos; break;
			case 7: v = (TUint32)s.iRpos; break;
			case 8: v = s.iWatermark; break;
			case 9: v = s.iGain; break;
			case 10: v = s.iClass; break;
			case 11: v = s.iStatus; s.iStatus &= ~ST_UNDER; break;
			case 12: v = (TUint32)s.iFrames; break;
			case 13: v = (TUint32)(s.iFrames >> 32); break;
			case 14: v = s.iPlanar; break;
			default: break;
				}
			}
		}
	else
		{
		switch (aOff >> 2)
			{
		case 0: v = KId; break;
		case 1: v = KSlots << 8 | 1 | 2 | 4; break;	// "MP3/AAC decoders": encoded streams drain silently
		                                        	// at a nominal rate (ehwa refuses those nodes otherwise)
		case 2: v = (TUint32)Clock; break;
		case 3: v = (TUint32)(Clock >> 32); break;
		case 4: v = Master; break;
		case 5: v = Route; break;
		case 6: v = IntStatus; break;
		case 7: v = IntEnable; break;
		case 8: v = KSlots - 1; break;
		default: break;
			}
		}
	Leave(irq);
	return v;
	}

static void SndWrite(TAny*, TUint32 aOff, TUint32 v)
	{
	TInt irq = Enter();
	Pump();
	if (aOff >= 0x100)
		{
		TInt k = (aOff - 0x100) / 0x40;
		if (k < KSlots)
			{
			SSlot& s = Slots[k];
			TBool running = s.iStatus & ST_RUN;
			TInt r = (aOff & 0x3f) >> 2;
			if (r == 0)
				{
				if (v & 2) { s.iStatus &= ~(ST_RUN | ST_PAUSE); s.iEosSet = EFalse; }
				if (v & 1) Start(k);
				if (v & 4) s.iStatus |= ST_PAUSE;
				if (v & 8) s.iStatus &= ~ST_PAUSE;
				if (v & 16) { s.iEosSet = ETrue; s.iStatus &= ~ST_EOS; }
				Wake();
				}
			else if (r == 6)
				{
				if (running)
					{
					TUint32 d = v - (TUint32)s.iWpos;
					if (d <= s.iRingSize - Queued(s))
						{
						s.iWpos += d;
						s.iWmArmed = ETrue;
						s.iStatus &= ~ST_WM;
						}
					}
				}
			else if (r == 8) { s.iWatermark = v; s.iWmArmed = ETrue; }
			else if (r == 9) s.iGain = v < 1024 ? v : 1024;
			else if (r == 10) s.iClass = v;
			else if (!running)
				{
				if (r == 1) { s.iFormat = v & 0xff; s.iSwap16 = (v >> 8) & 1; }
				else if (r == 2) s.iRate = v;
				else if (r == 3) s.iChannels = v == 1 ? 1 : 2;
				else if (r == 4) s.iRingAddr = v;
				else if (r == 5) s.iRingSize = v;
				else if (r == 14) s.iPlanar = v;
				}
			}
		}
	else
		{
		switch (aOff >> 2)
			{
		case 4: Master = v < 1024 ? v : 1024; break;
		case 5: Route = v; break;
		case 6: IntStatus &= ~v; UpdateIrq(); break;
		case 7: IntEnable = v; UpdateIrq(); break;
		default: break;
			}
		}
	Leave(irq);
	}

// the sound device on the board (before boot)
TInt HostedSndAttach()
	{
	for (TInt k = 0; k < KSlots; ++k)
		Slots[k].iGain = 256;
	TAny* stack = hmmap(0, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (HMAP_FAILED(stack) || hthread(&PumpThread, NULL, stack, 0x10000, &PumpTid) < 0)
		return KErrNoMemory;
	return HostedIoAttach(KSndIo, 0x1000, &SndRead, &SndWrite, NULL);
	}
