// Hosted EKA2: the syborg keyboard (QEMU hw/syborg_keyboard.c registers) for EKEYB.DLL
// (baseport syborg/keyboard): ID, DATA (pops the FIFO), FIFO_COUNT, INT_ENABLE; interrupt EIntKeyboard while
// the FIFO holds codes and the interrupt is enabled. Codes are pushed by the host side (HostedKeybPush: the
// phone window's key ring, hdisplay.cpp). A code is what QEMU's device delivers: bit 31 = released, bit 30 =
// already a Symbian scan code in bits 0..7 (syborg-pg keyboard patch), else a PC set-1 code. Original work.
#include <kernel/kern_priv.h>

extern TInt HostedIoAttach(TUint32 aBase, TUint32 aSize, TUint32 (*aRead)(TAny*, TUint32),
                           void (*aWrite)(TAny*, TUint32, TUint32), TAny* aCtx);
extern void HostedRaiseIrq(TInt aIrq);
extern void HostedLowerIrq(TInt aIrq);

static const TUint32 KKeybIo = 0xC6000000u + 0x03 * 0x1000;		// KHwBaseKmiKeyboard
static const TInt KKeybIrq = 2;										// EIntKeyboard
static const TUint32 KKeybId = 0xc51d0002u;							// SYBORG_ID_KEYBOARD
enum { RId = 0, RData = 1, RCount = 2, RIntEnable = 3 };
static const TInt KFifo = 64;

static TUint32 Fifo[KFifo];
static TInt Head, Count;
static TUint32 IntEnable;
static volatile TInt Lock;
TInt HostedKeybDropped;

static void Acquire() { while (__atomic_exchange_n(&Lock, 1, __ATOMIC_ACQUIRE)) hsys(SYS_sched_yield); }
static void Release() { __atomic_store_n(&Lock, 0, __ATOMIC_RELEASE); }
// as hsnd.cpp: a nanothread holds the lock with interrupts disabled (EKEYB's ISR reads these registers)
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
	if (Count && IntEnable)
		HostedRaiseIrq(KKeybIrq);
	else
		HostedLowerIrq(KKeybIrq);
	}

static TUint32 KeybRead(TAny*, TUint32 aOff)
	{
	TInt irq = Enter();
	TUint32 v = 0;
	switch (aOff >> 2)
		{
	case RId: v = KKeybId; break;
	case RData:
		if (Count)
			{
			v = Fifo[Head];
			Head = (Head + 1) % KFifo;
			--Count;
			}
		else
			v = 0xffffffffu;			// as QEMU: an empty FIFO reads -1
		UpdateIrq();
		break;
	case RCount: v = Count; break;
	case RIntEnable: v = IntEnable; break;
	case 4: v = KFifo; break;					// FIFO size (QEMU register 4)
		}
	Leave(irq);
	return v;
	}

static void KeybWrite(TAny*, TUint32 aOff, TUint32 aValue)
	{
	TInt irq = Enter();
	if ((aOff >> 2) == RIntEnable)
		{
		IntEnable = aValue & 1;
		UpdateIrq();
		}
	Leave(irq);
	}

// a key event from the host (any thread): queued, interrupt raised; a full FIFO drops it (as QEMU)
void HostedKeybPush(TUint32 aCode)
	{
	TInt irq = Enter();
	if (Count < KFifo)
		{
		Fifo[(Head + Count) % KFifo] = aCode;
		++Count;
		}
	else
		++HostedKeybDropped;
	UpdateIrq();
	Leave(irq);
	}

TInt HostedKeybAttach()
	{
	return HostedIoAttach(KKeybIo, 0x1000, &KeybRead, &KeybWrite, NULL);
	}
