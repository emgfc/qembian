// Copyright (c) 2026 qembian contributors. Licensed under the Eclipse Public License v1.0.
//
// The ISC/ISA channel driver in front of the fake modem. The modem itself (every ISI answer, indications,
// delayed replies, power off) is QEMU's syborg-modem device (src/qemu-syborg/hw/arm/syborg_modem.c,
// reply table generated at launch by tools/device.py qemu-args): this driver forwards what IscApi sends
// (ISI messages, subscriptions, channel open/close) to its port and delivers what comes back, so a modem
// change needs no ROM rebuild.
// Built as nokiaiscdriver.ldd ("NokiaISCDriver", channels = units 0..31, modem port 0),
// nokiaiscdriverextension.ldd ("NokiaISCDriverExtension", channels 32..63, unit = channel - 32, port 1)
// and the Symbian^3 isaaccessldd.ldd (channels 0..63, port 0).
//
// Protocol: docs/devices/nokia-n95-8gb-rm320/n95-isc-isi-protocol.md and -2.md (IscApi controls, ISI framing, boot replies).
//   ctl 0  {status*}                     modem link init          -> complete KErrNone
//   ctl 1  {status*, channel, TDesC8*}   open channel             -> complete KErrNone
//   ctl 4  {status*, TDes8*, TPtr8*}     receive next message     -> deliver or park
//   ctl 7  {status*, TPtr8*}             notify connection status -> park
//   ctl 14 {status*}                     async close              -> complete KErrNone
//   ctl 17 {TDesC8*}                     send ISI message         -> answer per reply table
//   ctl 21 {channel, TDes8*}             channel info             -> "6C <channel>"
//   ctl 23 {TDesC8*}                     subscribe (res, msgid) pairs
//   ctl 22                               max data size (return value) -> 4096
//   ctl 33 / 35                          cancel receive / connection notify
// ISI frame: [0]media [1]rdev [2]sdev [3]resource [4..5]len-6 (LE) [6]robj [7]sobj [8]tid [9]msgid [10..]
// The modem answers like one in NORMAL state with no SIM. Every send and delivery is logged here
// ("ISCPEER: ch N send/deliver"), the modem's own notes come back as log frames ("ISCPEER: ...").

#include <kernel/kernel.h>
#include "syborg.h"

#ifndef ISC_CHANNEL_BASE
#define ISC_CHANNEL_BASE 0
#endif
#ifndef ISC_LDD_NAME
#define ISC_LDD_NAME "NokiaISCDriver"
#endif
_LIT(KLddName, ISC_LDD_NAME);
// ISC_ISA_ACCESS: the Symbian^3 "ISA Access" front end (N8 isaaccessldd.ldd, logical device "IADDriver"):
// one device for channels 0..63, the channel in DoCreate's info descriptor, other control numbers,
// cancels through DoCancel (docs/devices/nokia-n8-rm596/isa-access.md). The ISI content is the same.
#ifdef ISC_ISA_ACCESS
const TInt KMaxChannels = 64;
#else
const TInt KMaxChannels = 32;
#endif

// A channel object, not a channel number, is what the modem talks to: several clients open the same ISC
// channel (the 701's audio server, DSP controller and audio engine all on 36), each with its own replies,
// subscriptions and open/close. Each object takes a handle 1..KMaxHandles-1; the port sees
// handle << 8 | channel number and answers the same value.
const TInt KMaxHandles = 64;
const TInt KMaxMsg = 256;
const TInt KQueueLen = 16;
const TInt KMaxSubs = 128;
// syborg-modem: port 0 at KHwBaseModem (IRQ EIntModem0), port 1 0x2000 further (EIntModem1); per port
// registers 0x00..0x24, TX buffer +0x1000, RX buffer (head frame) +0x1800 (syborg_modem.c)
const TInt KModemPort = ISC_CHANNEL_BASE >= 32 ? 1 : 0;
const TUint KModemBase = KHwBaseModem + KModemPort * 0x2000;
const TInt KModemIrq = KModemPort ? EIntModem1 : EIntModem0;
enum { KMTxChannel = 0x04, KMTxLen = 0x08, KMTxGo = 0x0C, KMRxCount = 0x10, KMRxChannel = 0x14, KMRxLen = 0x18,
	KMRxKind = 0x1C, KMRxPop = 0x20, KMIntEnable = 0x24, KMTxBuf = 0x1000, KMRxBuf = 0x1800 };
enum { KMSend = 1, KMSubscribe = 2, KMOpen = 3, KMClose = 4 };		// TX kinds
enum { KMDeliver = 1, KMLog = 2 };									// RX kinds
static inline volatile TUint32& ModemReg(TInt aOff) { return *(volatile TUint32*)(KModemBase + aOff); }

class DIscPeerChannel;

class DIscPeerFactory;
class DIscPeerFactory : public DLogicalDevice
	{
public:
	DIscPeerFactory() : iRxDfc(RxDfcFn, this, 1)
		{
		iVersion = TVersion(255, 255, 0xffff);
#ifdef ISC_ISA_ACCESS
		iParseMask = KDeviceAllowInfo;			// unit KNullUnit, channel in the info descriptor
#else
		iParseMask = KDeviceAllowUnit | KDeviceAllowInfo;
		iUnitsMask = 0xffffffffu;
#endif
		}
	virtual TInt Install()
		{
		TInt r = Kern::MutexCreate(iLock, KLddName, KMutexOrdGeneral0);
		if (r != KErrNone)
			return r;
		iRxDfc.SetDfcQ(Kern::DfcQue1());
		r = Interrupt::Bind(KModemIrq, Isr, this);
		if (r != KErrNone)
			return r;
		ModemReg(KMIntEnable) = 1;
		Interrupt::Enable(KModemIrq);
		Kern::Printf("ISCPEER: install %S (channels %d..%d, QEMU modem port %d)", &KLddName, ISC_CHANNEL_BASE, ISC_CHANNEL_BASE + KMaxChannels - 1, KModemPort);
		return SetName(&KLddName);
		}
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DLogicalChannelBase*& aChannel);
public:
	void Drain();					// deliver what the modem queued (caller holds iLock)
	static void Isr(TAny* aPtr);
	static void RxDfcFn(TAny* aPtr);
	DMutex* iLock;
	DIscPeerChannel* iChannels[KMaxHandles];	// by handle (0 unused)
	TDfc iRxDfc;
	};

struct SMsg { TInt iLen; TUint8 iData[KMaxMsg]; };


class DIscPeerChannel : public DLogicalChannelBase
	{
public:
	DIscPeerChannel(DIscPeerFactory* aF) : iF(aF) {}
	virtual ~DIscPeerChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aFn, TAny* a1, TAny* a2);
	void Queue(const TUint8* aMsg, TInt aLen);
	TInt Channel() const { return iChannel; }
private:
	TInt Control(TInt aFn, TAny* a1);
#ifdef ISC_ISA_ACCESS
	TInt ControlIsa(TInt aFn, TAny* a1);
	void CancelIsa(TInt aReq);
#endif
	void Forward(TInt aKind, const TUint8* aData, TInt aLen);
	void TryDeliver();
	void HandleSend(const TUint8* m, TInt aLen);
	void Log(const char* aWhat, const TUint8* m, TInt aLen) const;
	TInt ReadArgs(TAny* a1, TUint32* aW, TInt aWords);
private:
	DIscPeerFactory* iF;
	DThread* iClient;
	TInt iUnit;
	TInt iChannel;
	SMsg iQ[KQueueLen];
	TInt iQHead, iQCount;
	TRequestStatus* iRxStatus;		// pending Receive
	TAny* iRxBuf;					// TDes8* in client
	TAny* iRxNeeded;				// TPtr8* (2 bytes) in client
	TRequestStatus* iConnStatus;	// pending NotifyConnectionStatus
	TRequestStatus* iFlowStatus;	// pending NotifyFlowControlStatus
	TUint8 iSubs[KMaxSubs];
	TInt iSubsLen;
	TInt iSlot;				// handle: index in iF->iChannels (-1: none)
	TBool iNeededRaw;		// ISA: the receive "needed length" is a raw TUint16*, ISC: a TPtr8*
	TRequestStatus* iDataRxStatus;		// ISA DataReceive: parked
	TRequestStatus* iCustomStatus[3];	// ISA async custom functions: parked
	};

TInt DIscPeerFactory::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DIscPeerChannel(this);
	return aChannel ? KErrNone : KErrNoMemory;
	}

TInt DIscPeerChannel::DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion&)
	{
	iClient = &Kern::CurrentThread();	// a channel never outlives its owning thread
	iUnit = aUnit;
#ifdef ISC_ISA_ACCESS
	TBuf8<1> ch;						// IscApi: TBuf8<1>{channel} in user memory
	if (!aInfo || Kern::ThreadDesRead(iClient, aInfo, ch, 0) != KErrNone || ch.Length() != 1 || ch[0] >= KMaxChannels)
		return KErrArgument;
	iChannel = ch[0];
#else
	(void)aInfo;
	iChannel = ISC_CHANNEL_BASE + aUnit;
#endif
	iSlot = -1;
	if (iChannel - ISC_CHANNEL_BASE >= 0 && iChannel - ISC_CHANNEL_BASE < KMaxChannels)
		{
		NKern::ThreadEnterCS();
		Kern::MutexWait(*iF->iLock);
		for (TInt h = 1; h < KMaxHandles && iSlot < 0; ++h)
			if (!iF->iChannels[h])
				iSlot = h;
		if (iSlot > 0)
			{
			iF->iChannels[iSlot] = this;
			Forward(KMOpen, NULL, 0);		// the modem starts this object's state afresh
			}
		Kern::MutexSignal(*iF->iLock);
		NKern::ThreadLeaveCS();
		}
	if (iSlot < 0)
		Kern::Printf("ISCPEER: channel %d: no free handle, the modem will not answer it", iChannel);
	Kern::Printf("ISCPEER: channel %d opened by %O (handle %d)", iChannel, iClient, iSlot);
	return KErrNone;
	}

DIscPeerChannel::~DIscPeerChannel()
	{
	NKern::ThreadEnterCS();
	Kern::MutexWait(*iF->iLock);
	if (iSlot > 0 && iF->iChannels[iSlot] == this)
		{
		iF->iChannels[iSlot] = NULL;
		Forward(KMClose, NULL, 0);
		}
	Kern::MutexSignal(*iF->iLock);
	NKern::ThreadLeaveCS();
	Kern::Printf("ISCPEER: channel %d closed", iChannel);
	}

TInt DIscPeerChannel::ReadArgs(TAny* a1, TUint32* aW, TInt aWords)
	{
	memclr(aW, aWords * 4);
	if (!a1)
		return KErrArgument;
	return Kern::ThreadRawRead(iClient, a1, aW, aWords * 4);
	}

void DIscPeerChannel::Log(const char* aWhat, const TUint8* m, TInt aLen) const
	{
	static const char KHex[] = "0123456789abcdef";
	TBuf8<200> hex;
	for (TInt i = 0; i < aLen && hex.Length() < 190; ++i)
		{
		hex.Append(KHex[m[i] >> 4]);
		hex.Append(KHex[m[i] & 15]);
		hex.Append(' ');
		}
	Kern::Printf("ISCPEER: ch %d %s (%d): %S", iChannel, aWhat, aLen, &hex);
	}

TInt DIscPeerChannel::Request(TInt aFn, TAny* a1, TAny* a2)
	{
#ifdef ISC_ISA_ACCESS
	if (aFn == (TInt)KMaxTInt)			// RBusLogicalChannel::DoCancel(n): a1 = the request number
		{
		NKern::ThreadEnterCS();
		Kern::MutexWait(*iF->iLock);
		CancelIsa((TInt)a1);
		Kern::MutexSignal(*iF->iLock);
		NKern::ThreadLeaveCS();
		return KErrNone;
		}
#endif
	if (aFn < 0 || aFn == (TInt)KMaxTInt)
		{
		// IscApi never uses DoRequest/DoCancel; log and complete requests so nothing hangs.
		Kern::Printf("ISCPEER: ch %d unexpected request %d a1=%08x a2=%08x", iChannel, aFn, a1, a2);
		if (aFn < 0 && aFn != (TInt)KMaxTInt && a1)
			{
			TRequestStatus* s = (TRequestStatus*)a1;
			Kern::RequestComplete(iClient, s, KErrNotSupported);
			}
		return KErrNone;
		}
	NKern::ThreadEnterCS();				// DMutex use requires a critical section
	Kern::MutexWait(*iF->iLock);
#ifdef ISC_ISA_ACCESS
	TInt r = ControlIsa(aFn, a1);
#else
	TInt r = Control(aFn, a1);
#endif
	Kern::MutexSignal(*iF->iLock);
	NKern::ThreadLeaveCS();
	return r;
	}

#ifdef ISC_ISA_ACCESS
static void DoneIsa(DThread* aT, TRequestStatus*& aS, TInt aR)
	{
	if (aS)
		Kern::RequestComplete(aT, aS, aR);	// sets aS = NULL
	}

// ISA control numbers (iscapi.dll export -> DoControl n, a1 -> struct of words); where an ISC control
// does the job, forwarded to Control() with the ISC number (same argument word positions)
TInt DIscPeerChannel::ControlIsa(TInt aFn, TAny* a1)
	{
	TUint32 w[3];
	switch (aFn)
		{
	case 0:		// Open {status*, TUint16* iChannel, resources}: the client waits for it
		{
		ReadArgs(a1, w, 3);
		TUint16 ch = (TUint16)iChannel;
		if (w[1])
			Kern::ThreadRawWrite(iClient, (TAny*)w[1], &ch, 2);	// RIscApi::iChannel (0xFFFF before)
		return Control(1, a1);			// ISC open: logs the resources, completes w[0]
		}
	case 29:	// synchronous Open {TUint16* iChannel}
		{
		ReadArgs(a1, w, 1);
		TUint16 ch = (TUint16)iChannel;
		return w[0] ? Kern::ThreadRawWrite(iClient, (TAny*)w[0], &ch, 2) : KErrArgument;
		}
	case 1:		// Close(TRequestStatus&) {status*}
		return Control(14, a1);
	case 12:	// Close(): non-zero panics IADUserApiPanic 4
	case 30:	// close of a synchronous channel: non-zero panics 30
		return KErrNone;
	case 13:	// Send {const TDesC8*}
		return Control(17, a1);
	case 2:		// Send(TRequestStatus&, msg) {status*, const TDesC8*}
		{
		ReadArgs(a1, w, 2);
		TBuf8<KMaxMsg> m;
		TInt r = Kern::ThreadDesRead(iClient, (TAny*)w[1], m, 0);
		if (r == KErrNone)
			HandleSend(m.Ptr(), m.Length());
		TRequestStatus* s = (TRequestStatus*)w[0];
		DoneIsa(iClient, s, r);
		return KErrNone;
		}
	case 14:	// DataSend {const TDesC8*}: data channels go nowhere
		return KErrNone;
	case 4:		// DataSend(TRequestStatus&, data) {status*, const TDesC8*}
		{
		ReadArgs(a1, w, 1);
		TRequestStatus* s = (TRequestStatus*)w[0];
		DoneIsa(iClient, s, KErrNone);
		return KErrNone;
		}
	case 3:		// Receive {status*, TDes8*, TUint16* needed}
		iNeededRaw = ETrue;
		return Control(4, a1);
	case 5:		// DataReceive {status*, ...}: park
		ReadArgs(a1, w, 1);
		if (iDataRxStatus)
			{
			TRequestStatus* s = (TRequestStatus*)w[0];
			DoneIsa(iClient, s, KErrInUse);
			}
		else
			iDataRxStatus = (TRequestStatus*)w[0];
		return KErrNone;
	case 6:		// NotifyConnectionStatus {status*, TInt*}: park
		return Control(7, a1);
	case 7:		// NotifyFlowControlStatus {status*, TInt*}: park
		return Control(8, a1);
	case 8: case 9: case 10:	// async custom functions {status*, arg}: park
		ReadArgs(a1, w, 1);
		iCustomStatus[aFn - 8] = (TRequestStatus*)w[0];
		return KErrNone;
	case 15:	// ConnectionStatus [inferred]: 0 = connected
	case 16:	// FlowControlStatus [inferred]: 0 = off
	case 22:	// ResetBuffers [inferred]
		return KErrNone;
	case 18:	// MaximumDataSize
		return Control(22, a1);
	case 19:	// custom function 23: subscribe to indications {const TDesC8* (res, msg) pairs}
		return Control(23, a1);
	default:
		Kern::Printf("ISCPEER: ch %d ISA ctl %d a1=%08x ignored", iChannel, aFn, a1);
		return KErrNone;
		}
	}

// DoCancel(n): n = the control number of the request to cancel, 0 = all
void DIscPeerChannel::CancelIsa(TInt aReq)
	{
	if (aReq == 0 || aReq == 3) DoneIsa(iClient, iRxStatus, KErrCancel);
	if (aReq == 0 || aReq == 5) DoneIsa(iClient, iDataRxStatus, KErrCancel);
	if (aReq == 0 || aReq == 6) DoneIsa(iClient, iConnStatus, KErrCancel);
	if (aReq == 0 || aReq == 7) DoneIsa(iClient, iFlowStatus, KErrCancel);
	for (TInt i = 0; i < 3; ++i)
		if (aReq == 0 || aReq == 8 + i) DoneIsa(iClient, iCustomStatus[i], KErrCancel);
	}
#endif

TInt DIscPeerChannel::Control(TInt aFn, TAny* a1)
	{
	TUint32 w[3];
	switch (aFn)
		{
	case 0:		// modem link init {status*}
	case 14:	// async close {status*}
		{
		if (ReadArgs(a1, w, 1) == KErrNone && w[0])
			{
			TRequestStatus* s = (TRequestStatus*)w[0];
			Kern::RequestComplete(iClient, s, KErrNone);
			}
		Kern::Printf("ISCPEER: ch %d ctl %d completed", iChannel, aFn);
		return KErrNone;
		}
	case 1:		// open {status*, channel, const TDesC8* resources}
		{
		ReadArgs(a1, w, 3);
		TBuf8<32> res;
		if (w[2])
			Kern::ThreadDesRead(iClient, (TAny*)w[2], res, 0);
		Log("open, serves resources", res.Ptr(), res.Length());
		if (w[0])
			{
			TRequestStatus* s = (TRequestStatus*)w[0];
			Kern::RequestComplete(iClient, s, KErrNone);
			}
		return KErrNone;
		}
	case 4:		// receive {status*, TDes8* buf, TPtr8* needed}
		{
		ReadArgs(a1, w, 3);
		if (iRxStatus)
			{
			TRequestStatus* s = (TRequestStatus*)w[0];
			Kern::RequestComplete(iClient, s, KErrInUse);
			return KErrNone;
			}
		iRxStatus = (TRequestStatus*)w[0];
		iRxBuf = (TAny*)w[1];
		iRxNeeded = (TAny*)w[2];
		TryDeliver();
		return KErrNone;
		}
	case 8:		// notify flow control status {status*, TPtr8*}: flow never changes (park)
		ReadArgs(a1, w, 1);
		iFlowStatus = (TRequestStatus*)w[0];
		return KErrNone;
	case 7:		// notify connection status {status*, TPtr8*}: modem link never changes
		ReadArgs(a1, w, 1);
		iConnStatus = (TRequestStatus*)w[0];
		return KErrNone;
	case 17:	// send {const TDesC8* msg}
		{
		ReadArgs(a1, w, 1);
		TBuf8<KMaxMsg> m;
		TInt r = Kern::ThreadDesRead(iClient, (TAny*)w[0], m, 0);
		if (r != KErrNone)
			return r;
		HandleSend(m.Ptr(), m.Length());
		return KErrNone;
		}
	case 21:	// channel info {channel, TDes8* out}: 16-bit big-endian object id
		{
		// Aamm.DLL (0x808803dc) reads the two bytes as a big-endian value and requires <= 255
		// (User::Invariant at 0x80881e8a); mc_isiserver uses only the low byte. So {0, object}.
		ReadArgs(a1, w, 2);
		TBuf8<2> info;
		info.Append(0);
		info.Append((TUint8)w[0]);
		return Kern::ThreadDesWrite(iClient, (TAny*)w[1], info, 0);
		}
	case 23:	// subscribe to indications {const TDesC8* pairs}
		{
		ReadArgs(a1, w, 1);
		TPtr8 subs(iSubs, KMaxSubs);
		Kern::ThreadDesRead(iClient, (TAny*)w[0], subs, 0);
		iSubsLen = subs.Length();
		Log("subscribe (res,msgid pairs)", iSubs, iSubsLen);
		Forward(KMSubscribe, iSubs, iSubsLen);		// the modem sends the indications it owes this channel
		return KErrNone;
		}
	case 22:	// IscApi export 10: maximum data transfer size of this channel (return value).
				// DATAPORT.CSY sizes its reads with it; 0 makes it read 0 bytes -> KErrArgument leave.
		return 4096;
	case 33:	// receive cancel
		if (iRxStatus)
			{
			Kern::RequestComplete(iClient, iRxStatus, KErrCancel);
			iRxStatus = NULL;
			}
		return KErrNone;
	case 35:	// connection status notify cancel
		if (iConnStatus)
			{
			Kern::RequestComplete(iClient, iConnStatus, KErrCancel);
			iConnStatus = NULL;
			}
		return KErrNone;
	default:
		Kern::Printf("ISCPEER: ch %d ctl %d a1=%08x ignored", iChannel, aFn, a1);
		return KErrNone;
		}
	}

void DIscPeerChannel::Queue(const TUint8* aMsg, TInt aLen)
	{
	if (iQCount == KQueueLen || aLen > KMaxMsg)
		{
		Kern::Printf("ISCPEER: ch %d queue full, message dropped", iChannel);
		return;
		}
	SMsg& q = iQ[(iQHead + iQCount) % KQueueLen];
	memcpy(q.iData, aMsg, aLen);
	q.iLen = aLen;
	++iQCount;
	TryDeliver();
	}

// to the modem: one frame through the port's TX buffer, then whatever it answered at once (caller holds iLock)
void DIscPeerChannel::Forward(TInt aKind, const TUint8* aData, TInt aLen)
	{
	if (aLen > 0x800)
		aLen = 0x800;
	volatile TUint32* tx = (volatile TUint32*)(KModemBase + KMTxBuf);
	for (TInt i = 0; i < aLen; i += 4)
		{
		TUint32 w = 0;
		for (TInt k = 0; k < 4 && i + k < aLen; ++k)
			w |= (TUint32)aData[i + k] << (8 * k);
		tx[i >> 2] = w;
		}
	if (iSlot <= 0)
		return;
	ModemReg(KMTxChannel) = (TUint32)(iSlot << 8 | iChannel);
	ModemReg(KMTxLen) = (TUint32)aLen;
	ModemReg(KMTxGo) = (TUint32)aKind;
	iF->Drain();
	}

void DIscPeerChannel::HandleSend(const TUint8* m, TInt aLen)
	{
	Log("send", m, aLen);
	Forward(KMSend, m, aLen);
	}

void DIscPeerFactory::Drain()
	{
	while (ModemReg(KMRxCount))
		{
		TInt kind = ModemReg(KMRxKind), ch = ModemReg(KMRxChannel), len = ModemReg(KMRxLen);
		TUint8 m[KMaxMsg + 1];
		if (len > KMaxMsg)
			len = KMaxMsg;
		volatile TUint32* rx = (volatile TUint32*)(KModemBase + KMRxBuf);
		for (TInt i = 0; i < len; i += 4)
			{
			TUint32 w = rx[i >> 2];
			for (TInt k = 0; k < 4 && i + k < len; ++k)
				m[i + k] = (TUint8)(w >> (8 * k));
			}
		ModemReg(KMRxPop) = 1;
		if (kind == KMLog)
			{
			m[len] = 0;
			Kern::Printf("ISCPEER: %s", m);
			continue;
			}
		TInt h = ch >> 8;
		DIscPeerChannel* c = h > 0 && h < KMaxHandles ? iChannels[h] : NULL;
		if (c && c->Channel() == (ch & 0xff))
			c->Queue(m, len);
		else
			Kern::Printf("ISCPEER: ch %d (handle %d) closed, modem message dropped", ch & 0xff, h);
		}
	}

void DIscPeerFactory::Isr(TAny* aPtr)		// a delayed reply or indication is waiting
	{
	DIscPeerFactory* f = (DIscPeerFactory*)aPtr;
	Interrupt::Disable(KModemIrq);
	f->iRxDfc.Add();
	}

void DIscPeerFactory::RxDfcFn(TAny* aPtr)	// DfcThread1: the factory mutex as Request() takes it
	{
	DIscPeerFactory* f = (DIscPeerFactory*)aPtr;
	NKern::ThreadEnterCS();
	Kern::MutexWait(*f->iLock);
	f->Drain();
	Kern::MutexSignal(*f->iLock);
	NKern::ThreadLeaveCS();
	Interrupt::Enable(KModemIrq);
	}

void DIscPeerChannel::TryDeliver()
	{
	if (!iRxStatus || !iQCount)
		return;
	SMsg& q = iQ[iQHead];
	TInt max = Kern::ThreadGetDesMaxLength(iClient, iRxBuf);
	if (q.iLen > max)
		{
		TUint16 need = (TUint16)q.iLen;
		if (iNeededRaw)
			Kern::ThreadRawWrite(iClient, iRxNeeded, &need, 2);		// ISA: a TUint16*
		else
			{
			TPtrC8 n((const TUint8*)&need, 2);
			Kern::ThreadDesWrite(iClient, iRxNeeded, n, 0);		// ISC: a TPtr8* over 2 bytes
			}
		Kern::Printf("ISCPEER: ch %d message %d > buffer %d, KErrOverflow", iChannel, q.iLen, max);
		Kern::RequestComplete(iClient, iRxStatus, KErrOverflow);
		iRxStatus = NULL;
		return;
		}
	TPtrC8 msg(q.iData, q.iLen);
	TInt r = Kern::ThreadDesWrite(iClient, iRxBuf, msg, 0);
	Log("deliver", q.iData, q.iLen);
	iQHead = (iQHead + 1) % KQueueLen;
	--iQCount;
	Kern::RequestComplete(iClient, iRxStatus, r);
	iRxStatus = NULL;
	}

DECLARE_STANDARD_LDD()
	{
	return new DIscPeerFactory;
	}
