// Hosted EKA2: the fake modem on the hosted board. The modem itself is the core shared with the QEMU
// device (src/qemu-syborg/hw/arm/syborg_modem_core.c: ISI protocol, reply table, SIM, SMS, power); this
// file is its host: the two register windows at KHwBaseModem (port 0, interrupt 15) and +0x2000 (port 1,
// interrupt 16) on the board's I/O bus, interrupts, timers (a host thread), the log. No network (packet
// data) and no sound device yet.
// Locking: the core is used by nanothreads (register accesses) and by the timer thread. A nanothread
// takes the kernel lock first (no EKA2 reschedule while it holds the modem: a preempted holder would
// leave the next nanothread spinning on a unicore kernel), then the host spinlock; the timer thread
// takes the spinlock only. Original work for qembian.

#include <kernel/kern_priv.h>
extern "C" {
#define SYBMODEM_HOSTED
#include "syborg_modem_core.h"
}

extern TInt HostedIoAttach(TUint32 aBase, TUint32 aSize, TUint32 (*aRead)(TAny*, TUint32),
                           void (*aWrite)(TAny*, TUint32, TUint32), TAny* aCtx);
extern void HostedRaiseIrq(TInt aIrq);
extern void HostedLowerIrq(TInt aIrq);

static const TUint32 KModemIo = 0xC6000000u + 0x19 * 0x1000;	// KHwBaseModem (syborg.h)
static const TInt KModemIrq[MODEM_PORTS] = { 15, 16 };			// EIntModem0/1

static SyborgModemState Modem;
static volatile TInt ModemLock;
static TBool ModemReady;

struct SybModemTimer
	{
	void (*iFn)(void*);
	void* iArg;
	TUint64 iDue;		// ns, monotonic
	TBool iArmed;
	};
static SybModemTimer Timers[MODEM_LATE + 2];
static volatile TInt TimerSeq;
static volatile TInt TimerThreadTid;

static void Acquire() { while (__atomic_exchange_n(&ModemLock, 1, __ATOMIC_ACQUIRE)) hsys(SYS_sched_yield); }
static void Release() { __atomic_store_n(&ModemLock, 0, __ATOMIC_RELEASE); }

extern "C" void* sybmodem_host_alloc_zero(size_t n)
	{
	TAny* m = hmmap(0, (n + 0xfff) & ~0xfffu, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return HMAP_FAILED(m) ? NULL : m;		// anonymous memory reads as zeros
	}

// ---- the host side of the core
static void HostSetIrq(void*, int aPort, int aLevel)
	{
	if (aLevel)
		HostedRaiseIrq(KModemIrq[aPort]);
	else
		HostedLowerIrq(KModemIrq[aPort]);
	}

static void HostTimerMod(void*, SybModemTimer* aT, int aMs)
	{
	aT->iDue = hnow_ns() + (TUint64)(aMs > 0 ? aMs : 0) * 1000000u;
	aT->iArmed = ETrue;
	__atomic_add_fetch(&TimerSeq, 1, __ATOMIC_SEQ_CST);
	hfutex_wake(&TimerSeq, 1);
	}

static void HostTimerDel(void*, SybModemTimer* aT)
	{
	aT->iArmed = EFalse;
	}

static void HostGuestError(void*, const char* aText)
	{
	hprintf("HOSTED: %s", aText);
	}

static void HostPower(void*, int aWhat)
	{
	hprintf("HOSTED: the modem %s the phone\n", aWhat == 1 ? "switches off" : "resets");
	hexit(0);
	}

static bool HostNetPresent(void*) { return false; }
static void HostNetSend(void*, const uint8_t*, size_t) {}
static void HostNetFlush(void*) {}
static bool HostSndRingWritten(void*, int, uint32_t, uint64_t*) { return false; }
static uint32_t HostSndPending(void*, int, uint64_t) { return 0; }
static uint32_t HostSndRingSize(void*, int) { return 0; }
static void HostSndBeep(void*, uint32_t, uint32_t, uint32_t) {}

static const SybModemHost ModemHost =
	{
	HostSetIrq, HostTimerMod, HostTimerDel, HostGuestError, HostPower,
	HostNetPresent, HostNetSend, HostNetFlush,
	HostSndRingWritten, HostSndPending, HostSndRingSize, HostSndBeep
	};

// ---- timers: one host thread fires them in due order (under the modem lock)
static int TimerThread(void*)
	{
	for (;;)
		{
		TInt seq = __atomic_load_n(&TimerSeq, __ATOMIC_SEQ_CST);
		Acquire();
		TUint64 now = hnow_ns(), next = 0;
		for (TInt i = 0; i < (TInt)(sizeof(Timers) / sizeof(Timers[0])); ++i)
			{
			SybModemTimer& t = Timers[i];
			if (!t.iArmed)
				continue;
			if (t.iDue <= now)
				{
				t.iArmed = EFalse;
				t.iFn(t.iArg);		// may re-arm itself
				now = hnow_ns();
				}
			if (t.iArmed && (!next || t.iDue < next))
				next = t.iDue;
			}
		Release();
		TUint32 us = 1000000;
		if (next)
			{
			TUint64 d = next > now ? (next - now) / 1000 + 1 : 0;
			us = d > 1000000 ? 1000000 : (TUint32)d;
			}
		if (us)
			hfutex_wait_us(&TimerSeq, seq, us);
		}
	}

// ---- the register windows
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

static TBool TraceFrames;		// htest --env MODEM_TRACE=1: each frame as the guest picks it up

static TUint32 ModemRead(TAny* aPort, TUint32 aOff)
	{
	TInt irq = Enter();
	TUint32 v = sybmodem_read(&Modem, (TUint)aPort, aOff, 4);
	if (TraceFrames && aOff == 0x18 && Modem.port[(TUint)aPort].count)
		{
		const ModemFrame& f = Modem.port[(TUint)aPort].fifo[Modem.port[(TUint)aPort].head];
		char line[200];
		TInt o = hsnprintf(line, sizeof line, "HMODEM rx ch %x len %u:", f.channel, f.len);
		for (TUint32 i = 0; i < f.len && i < 40 && o < 180; ++i)
			o += hsnprintf(line + o, sizeof line - o, " %02x", f.data[i]);
		hprintf("%s\n", line);
		}
	Leave(irq);
	return v;
	}

static void ModemWrite(TAny* aPort, TUint32 aOff, TUint32 aValue)
	{
	TInt irq = Enter();
	sybmodem_write(&Modem, (TUint)aPort, aOff, aValue, 4);
	Leave(irq);
	}

// the modem on the board: config = the board config string (modem.*, sim.*), replies = the reply table
// tools/device.py generates (gen/isi-replies.tab), or NULL for built-in replies only. Before boot.
TInt HostedModemAttach(const char* aConfig, const char* aReplies)
	{
	if (ModemReady)
		return KErrAlreadyExists;
	sybmodem_init(&Modem, &ModemHost, NULL, aConfig ? aConfig : "");
	TraceFrames = hgetenv_hex("MODEM_TRACE", 0) != 0;
	hprintf("HMODEM config: %s\n", aConfig ? aConfig : "(none)");
	TInt n = 0;
	for (TInt i = 0; i < MODEM_LATE; ++i, ++n)
		{
		Timers[n].iFn = &sybmodem_late_fire;
		Timers[n].iArg = &Modem.late[i];
		Modem.late[i].timer = &Timers[n];
		}
	Timers[n].iFn = &sybmodem_power_fire; Timers[n].iArg = &Modem; Modem.power_timer = &Timers[n++];
	Timers[n].iFn = &sybmodem_sms_fire; Timers[n].iArg = &Modem; Modem.sms_timer = &Timers[n++];
	if (aReplies && aReplies[0])
		{
		TInt fd = hsys(SYS_openat, -100, aReplies, 0, 0);
		if (fd < 0)
			return KErrNotFound;
		TInt size = hsys(19 /* lseek */, fd, 0, 2);
		hsys(19, fd, 0, 0);
		char* text = (char*)sybmodem_host_alloc_zero(size + 1);
		TInt got = 0;
		while (text && got < size)
			{
			TInt r = hsys(SYS_read, fd, text + got, size - got);
			if (r <= 0)
				break;
			got += r;
			}
		hsys(SYS_close, fd);
		if (!text || got != size)
			return KErrCorrupt;
		if (sybmodem_load_replies(&Modem, text) < 0)
			return KErrNoMemory;
		}
	sybmodem_reset(&Modem);
	for (TInt p = 0; p < MODEM_PORTS; ++p)
		if (TInt r = HostedIoAttach(KModemIo + p * MODEM_WINDOW, MODEM_WINDOW, &ModemRead, &ModemWrite, (TAny*)p))
			return r;
	TAny* stack = hmmap(0, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (HMAP_FAILED(stack) || hthread(&TimerThread, NULL, stack, 0x10000, &TimerThreadTid) < 0)
		return KErrNoMemory;
	ModemReady = ETrue;
	return KErrNone;
	}

TInt HostedModemReplies()		// tests
	{
	return Modem.nreplies;
	}

const char* HostedModemSms(const char* aHex)		// an incoming SMS (tests, later a host control)
	{
	TInt irq = Enter();
	const char* r = sybmodem_sms(&Modem, aHex);
	Leave(irq);
	return r;
	}
