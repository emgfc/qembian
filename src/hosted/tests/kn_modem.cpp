// kn_modem: the fake modem on the hosted board without a phone ROM. The modem core is shared with the
// QEMU device (src/qemu-syborg/hw/arm/syborg_modem_core.c); this drives it through the board's I/O bus
// as the ISC stand-ins (iscpeer.inl) do: reply table parsing, request -> reply framing, unknown requests,
// bad channels, no interrupt for frames of the guest's own send, RX pop, channel close, SMS input checks.
#include <kernel/kern_priv.h>

extern TInt HostedBoardInit(const char* aBoardConfig, const char* aRofsImage);
extern TInt HostedModemAttach(const char* aConfig, const char* aReplies);
extern TInt HostedModemReplies();
extern const char* HostedModemSms(const char* aHex);
extern TUint32 HostedIoRead(TUint32 aAddr);
extern void HostedIoWrite(TUint32 aAddr, TUint32 aValue);
extern TBool HostedIrqRaised(TInt aIrq);

static const TUint32 KBase = 0xC6000000u + 0x19 * 0x1000;		// port 0
enum { KTxChannel = 0x04, KTxLen = 0x08, KTxGo = 0x0C, KRxCount = 0x10, KRxChannel = 0x14, KRxLen = 0x18,
	KRxKind = 0x1C, KRxPop = 0x20, KIntEnable = 0x24, KTxBuf = 0x1000, KRxBuf = 0x1800 };

static const char* strstr_(const char* h, const char* n)
	{
	TInt l = strlen(n);
	for (; h && *h; ++h)
		if (!memcmp(h, n, l)) return h;
	return 0;
	}
#define strstr strstr_

static TUint32 Rd(TUint32 aOff) { return HostedIoRead(KBase + aOff); }
static void Wr(TUint32 aOff, TUint32 aV) { HostedIoWrite(KBase + aOff, aV); }

static void Send(TUint32 aChannel, TUint32 aKind, const TUint8* aData, TInt aLen)
	{
	for (TInt i = 0; i < aLen; i += 4)
		{
		TUint32 w = 0;
		for (TInt k = 0; k < 4 && i + k < aLen; ++k)
			w |= (TUint32)aData[i + k] << (8 * k);
		Wr(KTxBuf + i, w);
		}
	Wr(KTxChannel, aChannel);
	Wr(KTxLen, aLen);
	Wr(KTxGo, aKind);
	}

static TInt Frame(TUint8* aOut, TUint32& aKind, TUint32& aChannel)
	{
	if (!Rd(KRxCount))
		return -1;
	aKind = Rd(KRxKind);
	aChannel = Rd(KRxChannel);
	TInt len = Rd(KRxLen);
	for (TInt i = 0; i < len; i += 4)
		{
		TUint32 w = Rd(KRxBuf + i);
		for (TInt k = 0; k < 4 && i + k < len; ++k)
			aOut[i + k] = (TUint8)(w >> (8 * k));
		}
	Wr(KRxPop, 1);
	return len;
	}

int main(int, char**)
	{
	// a reply table with the cases the parser must take: comments, blank and CRLF lines, short lines
	// (ignored), lengths over 64 (capped)
	const char* path = "/tmp/kn_modem.tab";
	const char* table =
		"# test table\n"
		"7E 01 02 03 AA BB CC   # a reply\r\n"
		"\n"
		"7E 05 06\n"
		"   7d 10 11 01 5a\n"
		"7C 01 02 FF 01\n";
	TInt fd = hsys(SYS_openat, -100, path, 0x41 | 0x200, 0644);
	hsys(SYS_write, fd, table, strlen(table));
	hsys(SYS_close, fd);

	// no LxInit: the modem is used from a plain host thread (bug 2026-11-12: LxSelf() was the stale
	// TPIDRURO here and the register access crashed, one CPU)
	HCHECK(HostedBoardInit(NULL, NULL) == KErrNone, "board");
	HCHECK(HostedModemAttach("modem.imei=490154203237518;modem.power_off_ms=3000;modem.hplmn=1;modem.sms=pp-routing;modem.simst=01:01;d_display.native=landscape;d_display.pitch=1280", path) == KErrNone, "modem attached (the N95 board config)");
	HCHECK(HostedModemAttach(NULL, NULL) == KErrAlreadyExists, "a second attach is refused");
	HCHECK(HostedModemReplies() == 3, "3 replies parsed (comments, blanks, short lines skipped): %d", HostedModemReplies());
	HCHECK(Rd(0) == 0xc51d0020u, "ID register: SYBORG_ID_MODEM (%08x)", Rd(0));
	HCHECK(Rd(KRxCount) == 0, "nothing queued at reset");

	Wr(KIntEnable, 1);
	HCHECK(Rd(KIntEnable) == 1, "interrupt enable reads back");
	TUint32 ch = 1 << 8 | 7;		// handle 1, ISC channel 7
	Send(ch, 3, NULL, 0);			// opened
	TUint8 req[12] = { 0x00, 0x00, 0x00, 0x7E, 0x06, 0x00, 0x00, 0x00, 0x42, 0x01, 0x00, 0x00 };
	Send(ch, 1, req, sizeof req);
	HCHECK(!HostedIrqRaised(15), "frames of the guest's own send raise no interrupt");
	TUint8 m[2048];
	TUint32 kind = 0, rch = 0;
	TInt n = Frame(m, kind, rch);
	HCHECK(n == 13 && kind == 1 && rch == ch, "reply frame: kind %u channel %x length %d", kind, rch, n);
	HCHECK(m[1] == 0x6c && m[3] == 0x7E && m[6] == 7 && m[8] == 0x42 && m[9] == 0x02,
	       "ISI header: to the application side, resource, receiver object, tid echoed, reply id");
	HCHECK(m[10] == 0xAA && m[11] == 0xBB && m[12] == 0xCC, "payload from the table");
	HCHECK(Rd(KRxCount) == 0, "RX_POP removed it");

	req[9] = 0x10; req[3] = 0x7d;
	Send(ch, 1, req, sizeof req);
	n = Frame(m, kind, rch);
	HCHECK(n == 11 && m[9] == 0x11 && m[10] == 0x5a, "an indented table line works (%d)", n);
	req[3] = 0x7C; req[9] = 0x01;
	Send(ch, 1, req, sizeof req);
	n = Frame(m, kind, rch);
	HCHECK(n == 10 + 64, "a table length over 64 is capped at 64 (%d)", n);

	req[3] = 0x7E; req[9] = 0x09;	// not in the table
	Send(ch, 1, req, sizeof req);
	n = Frame(m, kind, rch);
	m[n > 0 ? n : 0] = 0;
	HCHECK(n > 0 && kind == 2 && strstr((const char*)m, "no reply for resource 7e msg 09"),
	       "an unknown request gives a log line: %s", m);

	Send(0 << 8 | 7, 1, req, sizeof req);		// handle 0: invalid
	HCHECK(Rd(KRxCount) == 0, "a send on handle 0 is refused (no frame)");
	Send(1 << 8 | 64, 1, req, sizeof req);	// channel number 64: invalid
	HCHECK(Rd(KRxCount) == 0, "a send on channel 64 is refused");
	Wr(KRxPop, 1);
	HCHECK(Rd(KRxCount) == 0 && Rd(KRxLen) == 0 && Rd(KRxKind) == 0, "RX_POP on an empty queue is harmless");

	// close purges what was queued for the channel object
	req[3] = 0x7E; req[9] = 0x01;
	Send(ch, 1, req, sizeof req);
	HCHECK(Rd(KRxCount) == 1, "one reply queued");
	Send(ch, 4, NULL, 0);			// closed
	HCHECK(Rd(KRxCount) == 0, "closing the channel drops its queued reply");

	// INFO_SERIAL_NUMBER_READ (1B 00): IMEI in sub-block 41 {len 0x14, 15 digits} (H-imei)
	TUint8 imeiReq[11] = { 0x00, 0x00, 0x00, 0x1b, 0x05, 0x00, 0x00, 0x00, 0x06, 0x00, 0x41 };
	Send(ch, 3, NULL, 0);
	Send(ch, 1, imeiReq, sizeof imeiReq);
	n = Frame(m, kind, rch);
	htest_note("IMEI reply (%d): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", n, m[8], m[9],
	           m[10], m[11], m[12], m[13], m[14], m[15], m[16], m[17], m[18], m[19], m[20], m[21]);
	HCHECK(n == 32 && m[9] == 0x01 && m[10] == 0x00 && m[11] == 0x01 && m[12] == 0x41 && m[13] == 0x14 && m[14] == 0x00 &&
	       m[15] == 0x0f && !memcmp(m + 16, "490154203237518", 15), "IMEI reply: sub-block 41, 15 digits from the config");
	HCHECK(Rd(0x100) == 0, "an unknown register reads 0");
	HCHECK(HostedModemSms("123") && strstr(HostedModemSms("123"), "odd"), "SMS: odd hex refused");
	HCHECK(HostedModemSms("zz") && strstr(HostedModemSms("zz"), "not hex"), "SMS: non-hex refused");
	HCHECK(HostedModemSms("0400") && strstr(HostedModemSms("0400"), "no SIM"), "SMS: no SIM in this config");
	return htest_done();
	}
