// Copyright (c) 2026 syborg-pg contributors. Licensed under the Eclipse Public License v1.0.
//
// Fake modem peer behind the Nokia N95 ISC channel interface used by IscApi.dll.
// Built twice: nokiaiscdriver.ldd ("NokiaISCDriver", channels = units 0..31) and
// nokiaiscdriverextension.ldd ("NokiaISCDriverExtension", channels 32..63, unit = channel - 32).
//
// Protocol: docs/n95-isc-isi-protocol.md and -2.md (IscApi controls, ISI framing, boot replies).
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
// The peer answers like a modem in NORMAL state with no SIM. Every send and delivery is logged
// ("ISCPEER:") as the reference for completing the protocol.

#include <kernel/kernel.h>
#include "syborg.h"
#include "boardconfig.h"

#ifndef ISC_CHANNEL_BASE
#define ISC_CHANNEL_BASE 0
#endif
#ifndef ISC_LDD_NAME
#define ISC_LDD_NAME "NokiaISCDriver"
#endif
_LIT(KLddName, ISC_LDD_NAME);

const TInt KMaxMsg = 256;
const TInt KQueueLen = 16;
const TInt KMaxSubs = 128;
const TUint8 KDevApe = 0x6C;		// PN_DEV_SOS
const TUint8 KResMtc = 0x15, KResSim = 0x09, KResSec = 0x08;

class DIscPeerChannel;

class DIscPeerFactory : public DLogicalDevice
	{
public:
	DIscPeerFactory()
		{
		iVersion = TVersion(255, 255, 0xffff);
		iParseMask = KDeviceAllowUnit | KDeviceAllowInfo;
		iUnitsMask = 0xffffffffu;
		}
	virtual TInt Install()
		{
		TInt r = Kern::MutexCreate(iLock, KLddName, KMutexOrdGeneral0);
		if (r != KErrNone)
			return r;
		Kern::Printf("ISCPEER: install %S (fake modem, channels %d..%d)", &KLddName, ISC_CHANNEL_BASE, ISC_CHANNEL_BASE + 31);
		return SetName(&KLddName);
		}
	virtual void GetCaps(TDes8&) const {}
	virtual TInt Create(DLogicalChannelBase*& aChannel);
public:
	DMutex* iLock;
	DIscPeerChannel* iChannels[32];
	};

struct SMsg { TInt iLen; TUint8 iData[KMaxMsg]; };

class DIscPeerChannel : public DLogicalChannelBase
	{
public:
	DIscPeerChannel(DIscPeerFactory* aF) : iF(aF) {}
	virtual ~DIscPeerChannel();
	virtual TInt DoCreate(TInt aUnit, const TDesC8* aInfo, const TVersion& aVer);
	virtual TInt Request(TInt aFn, TAny* a1, TAny* a2);
private:
	TInt Control(TInt aFn, TAny* a1);
	void Queue(const TUint8* aMsg, TInt aLen);
	void Reply(TUint8 aRes, TUint8 aTid, TUint8 aMsgId, const TUint8* aData, TInt aLen);
	void MtcStateInd();
	void TryDeliver();
	void HandleSend(const TUint8* m, TInt aLen);
	TBool Subscribed(TUint8 aRes, TUint8 aMsgId) const;
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
	TBool iMtcIndSent;
	};

TInt DIscPeerFactory::Create(DLogicalChannelBase*& aChannel)
	{
	aChannel = new DIscPeerChannel(this);
	return aChannel ? KErrNone : KErrNoMemory;
	}

TInt DIscPeerChannel::DoCreate(TInt aUnit, const TDesC8*, const TVersion&)
	{
	iClient = &Kern::CurrentThread();	// a channel never outlives its owning thread
	iUnit = aUnit;
	iChannel = ISC_CHANNEL_BASE + aUnit;
	if (aUnit >= 0 && aUnit < 32)
		{
		NKern::ThreadEnterCS();
		Kern::MutexWait(*iF->iLock);
		iF->iChannels[aUnit] = this;
		Kern::MutexSignal(*iF->iLock);
		NKern::ThreadLeaveCS();
		}
	Kern::Printf("ISCPEER: channel %d opened by %O", iChannel, iClient);
	return KErrNone;
	}

DIscPeerChannel::~DIscPeerChannel()
	{
	NKern::ThreadEnterCS();
	Kern::MutexWait(*iF->iLock);
	if (iUnit >= 0 && iUnit < 32 && iF->iChannels[iUnit] == this)
		iF->iChannels[iUnit] = NULL;
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
	TInt r = Control(aFn, a1);
	Kern::MutexSignal(*iF->iLock);
	NKern::ThreadLeaveCS();
	return r;
	}

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
		if (Subscribed(KResMtc, 0xC0) && !iMtcIndSent)
			{
			MtcStateInd();
			iMtcIndSent = ETrue;
			}
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

TBool DIscPeerChannel::Subscribed(TUint8 aRes, TUint8 aMsgId) const
	{
	for (TInt i = 0; i + 1 < iSubsLen; i += 2)
		if (iSubs[i] == aRes && iSubs[i + 1] == aMsgId)
			return ETrue;
	return EFalse;
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

#include "isireplies.h"

void DIscPeerChannel::Reply(TUint8 aRes, TUint8 aTid, TUint8 aMsgId, const TUint8* aData, TInt aLen)
	{
	TUint8 m[KMaxMsg];
	TInt len = 10 + aLen;
	m[0] = 0x00;			// media
	m[1] = KDevApe;			// receiver device
	m[2] = 0x00;			// sender device: modem (PN_DEV_HOST)
	m[3] = aRes;
	m[4] = (TUint8)((len - 6) & 0xff);
	m[5] = (TUint8)((len - 6) >> 8);
	m[6] = (TUint8)iChannel;	// receiver object
	m[7] = 0x00;			// sender object
	m[8] = aTid;
	m[9] = aMsgId;
	memcpy(m + 10, aData, aLen);
	Queue(m, len);
	}

// The BB5 modem owns the power supply: after MTC_POWER_OFF_REQ (or a reset request) it cuts power
// (or restarts the phone) while the application side is still shutting down. Our stand-in does that
// through the Syborg platform device (register 2: 1 = power off, 2 = reset), a grace period after
// the request, so the file servers' last writes land first.
static void ModemPowerFn(TAny* aWhat)
	{
	Kern::Printf("ISCPEER: modem %s the phone", aWhat == (TAny*)1 ? "switches off" : "resets");
	WriteReg(KHwSVPPlatformDevice, 2, (TUint32)aWhat);
	}
static void ModemPowerLater(TInt aWhat)	// the timer is never freed: the machine goes away
	{
	NTimer* t = new NTimer(ModemPowerFn, (TAny*)aWhat);
	if (t)
		t->OneShot(NKern::TimerTicks(BoardConfigInt("power_off_ms", 3000)));	// [modem].power_off_delay_s
	else
		ModemPowerFn((TAny*)aWhat);
	}

void DIscPeerChannel::MtcStateInd()
	{
	static const TUint8 KNormalStart[2] = { 0x01, 0x03 };	// MTC_STATE_INFO_IND: NORMAL, START
	Reply(KResMtc, 0x00, 0xC0, KNormalStart, 2);
	}

void DIscPeerChannel::HandleSend(const TUint8* m, TInt aLen)
	{
	Log("send", m, aLen);
	if (aLen < 10)
		return;
	TUint8 res = m[3], tid = m[8], id = m[9];
	static const TUint8 K00[6] = { 0, 0, 0, 0, 0, 0 };
	if (res == KResMtc)
		{
		switch (id)
			{
		case 0x02:	// MTC_STATE_QUERY_REQ -> current NORMAL, target NORMAL
			{
			static const TUint8 d[4] = { 0x01, 0x01, 0x00, 0x00 };
			Reply(res, tid, 0x65, d, 4);
			MtcStateInd();
			return;
			}
		case 0x01:	// MTC_STATE_REQ {target, 0}: e.g. 0x0A RF_INACTIVE = offline mode -> MTC_STATE_RESP OK
			Reply(res, tid, 0x64, K00, 2);
			return;
		case 0x0B: Reply(res, tid, 0x6E, K00, 2); return;	// MTC_STARTUP_SYNQ_RESP
		case 0x06:	// MTC_RESET_GENERATE_REQ -> RESP, then the modem restarts the phone
			Reply(res, tid, 0x69, K00, 2);
			ModemPowerLater(2);
			return;
		case 0x03:	// MTC_POWER_OFF_REQ -> RESP, then the modem cuts power
			Reply(res, tid, 0x66, K00, 2);
			ModemPowerLater(1);
			return;
		case 0x04: Reply(res, tid, 0x67, K00, 2); return;
		case 0x12: Reply(res, tid, 0x75, K00, 2); return;
			}
		}
	else if (res == KResSim)
		{
		switch (id)
			{
		case 0xC0:	// SIM_STATUS_REQ {service, 0} -> SIM_SERV_SIM_DISCONNECTED (no SIM)
			{
			// NokiaDSY 0x808bfb54 maps the status: 0x01/0x1C/0x4B -> "card present" (then waits for
			// SIM init forever), 0x06/0x24 -> "no card". NOKIATSY only acts on 0x01.
			TUint8 d[2] = { aLen > 10 ? m[10] : (TUint8)0, 0x06 };
			Reply(res, tid, 0xC1, d, 2);
			return;
			}
		case 0xE4: Reply(res, tid, 0xE5, K00, 2); return;
			}
		}
	else if (res == KResSec && id == 0x11)	// SEC_STATE_REQ -> SEC_STARTUP_OK (no code to ask)
		{
		// NokiaDSY 0x808bfd20 ignores SEC_CAUSE_NO_SIM (0x16); 0x05 raises the security-code
		// status that sae publishes as 0x100052F0 (Startup state 8 needs it == 1).
		static const TUint8 d[2] = { 0x05, 0x00 };
		Reply(res, tid, 0x12, d, 2);
		return;
		}
	else if (res == 0x1C && id == 0x2D)
		{
		Reply(res, tid, 0x2E, K00, 6);
		return;
		}
	else if (res == 0xDB && id == 0x05)	// PN_NAMESERVICE NAME_ADD_REQ
		{
		Reply(res, tid, 0x06, K00, 2);
		return;
		}
	// canned replies: src/n95compat/isi/isi-db.txt (generated isireplies.h, included at file scope)
	if (res == 0x1B && id == 0x00)			// INFO_SERIAL_NUMBER_READ (IMEI) -- hack H-imei, docs/hacks.md
		{
		// [status INFO_OK][nsb 1][sb 0x41 IMEI_PLAIN, len 20][0][15 chars]["490154203237518"][NUL]
		// the IMEI: [modem].imei of the device profile (board config), else the documentation example
		TUint8 d[22] = { 0x00, 0x01, 0x41, 0x14, 0x00, 0x0F,
			'4','9','0','1','5','4','2','0','3','2','3','7','5','1','8', 0x00 };
		char imei[16];
		if (BoardConfig("imei", imei, sizeof(imei)) == 15)
			memcpy(d + 6, imei, 15);
		Reply(res, tid, 0x01, d, sizeof(d));
		return;
		}
	if (res == 0x3A && aLen > 10)			// PN_LIGHT (SALightsService) -- hack H-light, docs/hacks.md
		{
		// 0x22 target info: reply 0x23 [status 0][count = req[11]][the request's 0x16 subblocks echoed]
		// (the parser matches the target names KBD/KBD2/MMKEYS/DISPLAY/... at subblock+4).
		// Other light requests: reply id+1 echoing the request data with the first byte = 0 (OK).
		TUint8 d[128];
		TInt n = aLen - 10 > (TInt)sizeof(d) ? (TInt)sizeof(d) : aLen - 10;
		memcpy(d, m + 10, n);
		d[0] = 0;
		Reply(res, tid, id + 1, d, n);
		return;
		}
	if (res == 0x1B && id == 0x29)			// PP read: echo the request's subblocks (values 0)
		{
		Reply(res, tid, 0x2A, m + 10, aLen - 10);
		return;
		}
	if (res == KResSim && (id == 0x1F || id == 0x1D))	// SIM language pref / READ_IMSI: "data not available"
		{
		TUint8 d[2] = { aLen > 10 ? m[10] : (TUint8)0, 0x24 };
		Reply(res, tid, id + 1, d, 2);
		return;
		}
	for (TUint i = 0; i < sizeof(KIsiReplies) / sizeof(KIsiReplies[0]); ++i)
		if (KIsiReplies[i].iRes == res && KIsiReplies[i].iReq == id)
			{
			Reply(res, tid, KIsiReplies[i].iResp, KIsiReplies[i].iData, KIsiReplies[i].iLen);
			return;
			}
	Kern::Printf("ISCPEER: ch %d no reply for resource %02x msg %02x", iChannel, res, id);
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
		TPtrC8 n((const TUint8*)&need, 2);
		Kern::ThreadDesWrite(iClient, iRxNeeded, n, 0);
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
