// devagent: the on-phone half of sydb (tools/sydb). Plain EXE, all capabilities, no window group,
// no active scheduler. Talks a line protocol on serial unit 1 (RBusDevComm on ECOMM/EUART; the
// Syborg board's second UART, a unix socket on the host). serial0 stays the kernel debug port.
//
// Protocol (8-bit text, '\n' terminated; fields are k=v, values %XX-escaped):
//   host:  <tag> <cmd> [args...]          (args %XX-escaped, separated by single spaces)
//   agent: <tag> . k=v k=v ...            one list item (0..n)
//          <tag> ok [k=v ...]             success, ends the reply
//          <tag> err code=<n> msg=<text>  failure, ends the reply
//          * <event> k=v ...              unsolicited: hello (agent started), exit (watched process)
//   put:   <tag> put <path> <size> <crc32hex>  -> "<tag> go", host sends <size> raw bytes -> ok/err
//   get:   <tag> get <path>  -> "<tag> data size=N crc=C" + N raw bytes + "<tag> ok"
// No C++ exception runtime in this toolchain: only non-leaving APIs (or ones that leave on OOM).
#include <e32base.h>
#include <e32debug.h>
#include <e32property.h>
#include <e32svr.h>
#include <f32file.h>
#include <d32comm.h>
#include <w32std.h>
#include <apgcli.h>
#include <apgtask.h>
#include <apacmdln.h>
#include <aknkeylock.h>

#define P(fmt, ...) RDebug::Printf("DEVAGENT: " fmt, ##__VA_ARGS__)

static const char KVersion[] = "4";
static const TInt KUnit = 1;
static const TInt KMaxWatch = 32;
static const TInt KChunk = 4096;
static const TInt KAckChunk = 2048;		// put flow control window (< the LDD receive buffer)

// ------------------------------------------------------------------------------------------------
// helpers

static TUint32 gCrcTable[256];
static void CrcInit()
	{
	for (TUint32 i = 0; i < 256; i++)
		{
		TUint32 c = i;
		for (TInt k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		gCrcTable[i] = c;
		}
	}
static TUint32 Crc(TUint32 aCrc, const TUint8* aP, TInt aLen)
	{
	TUint32 c = ~aCrc;
	while (aLen-- > 0)
		c = gCrcTable[(c ^ *aP++) & 0xff] ^ (c >> 8);
	return ~c;
	}

static TInt HexVal(TUint8 c)
	{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
	}

// %XX-decode in place
static void Unescape(TDes8& aS)
	{
	TInt o = 0;
	for (TInt i = 0; i < aS.Length(); i++)
		{
		TUint8 c = aS[i];
		if (c == '%' && i + 2 < aS.Length() + 0 && HexVal(aS[i + 1]) >= 0 && HexVal(aS[i + 2]) >= 0)
			{
			c = (TUint8)(HexVal(aS[i + 1]) * 16 + HexVal(aS[i + 2]));
			i += 2;
			}
		aS[o++] = c;
		}
	aS.SetLength(o);
	}

// \sys\ and \resource\ need TCB, which this exe cannot have (it links WSERV/apparc/avkon)
static TBool NeedsTcb(const TDesC& aPath)
	{
	TParsePtrC pp(aPath);
	TPtrC d = pp.Path();
	return d.Left(5).CompareF(_L("\\sys\\")) == 0 || d.Left(10).CompareF(_L("\\resource\\")) == 0;
	}

// run c:\sys\bin\dafs.exe OP A1 [A2] and return its exit reason
static TInt Dafs(const TDesC& aOp, const TDesC& aA1, const TDesC& aA2)
	{
	TBuf<600> cl;
	cl.Append(aOp); cl.Append(' '); cl.Append(aA1);
	if (aA2.Length()) { cl.Append(' '); cl.Append(aA2); }
	RProcess p;
	TInt r = p.Create(_L("c:\\sys\\bin\\dafs.exe"), cl);
	if (r != KErrNone)
		return r;
	TRequestStatus s;
	p.Logon(s);
	p.Resume();
	User::WaitForRequest(s);
	r = p.ExitType() == EExitKill ? p.ExitReason() : KErrGeneral;
	p.Close();
	return r;
	}

static TBool ParseUint(const TDesC8& aS, TUint32& aV, TRadix aRadix = EDecimal)
	{
	TLex8 l(aS);
	if (aS.Length() > 2 && aS[0] == '0' && (aS[1] == 'x' || aS[1] == 'X'))
		{
		l = TLex8(aS.Mid(2));
		aRadix = EHex;
		}
	return l.Val(aV, aRadix) == KErrNone && l.Eos();
	}

static TBool ParseInt(const TDesC8& aS, TInt& aV)
	{
	if (aS.Length() > 2 && aS[0] == '0' && (aS[1] == 'x' || aS[1] == 'X'))
		{
		TUint32 u;
		if (!ParseUint(aS, u)) return EFalse;
		aV = (TInt)u;
		return ETrue;
		}
	TLex8 l(aS);
	return l.Val(aV) == KErrNone && l.Eos();
	}

// ------------------------------------------------------------------------------------------------
// output line builder

class TOut
	{
public:
	TOut() {}
	void Begin(const TDesC8& aTag, const char* aKind)
		{
		iB.Zero();
		iB.Append(aTag);
		iB.Append(' ');
		iB.Append(TPtrC8((const TUint8*)aKind));
		}
	void Field(const char* aKey, const TDesC8& aVal)
		{
		if (iB.Length() + 8 > iB.MaxLength()) return;
		iB.Append(' ');
		iB.Append(TPtrC8((const TUint8*)aKey));
		iB.Append('=');
		for (TInt i = 0; i < aVal.Length() && iB.Length() + 4 < iB.MaxLength(); i++)
			{
			TUint8 c = aVal[i];
			if (c <= ' ' || c == '%' || c == '=' || c >= 0x7f)
				iB.AppendFormat(_L8("%%%02X"), c);
			else
				iB.Append(c);
			}
		}
	void Field(const char* aKey, const TDesC16& aVal)
		{
		TBuf8<512> b;
		for (TInt i = 0; i < aVal.Length() && i < b.MaxLength(); i++)
			{
			TUint c = aVal[i];
			b.Append(c < 0x100 ? (TUint8)c : (TUint8)'?');
			}
		Field(aKey, b);
		}
	void Int(const char* aKey, TInt aV) { TBuf8<16> b; b.Num(aV); Field(aKey, b); }
	void Hex(const char* aKey, TUint aV) { TBuf8<16> b; b.Append(_L8("0x")); b.AppendNum(aV, EHex); Field(aKey, b); }
	void Str(const char* aKey, const char* aV) { Field(aKey, TPtrC8((const TUint8*)aV)); }
	TBuf8<1536> iB;
	};

// ------------------------------------------------------------------------------------------------
// the agent

struct SWatch
	{
	RProcess iProc;
	TRequestStatus iStatus;
	TUint iPid;
	TBool iUsed;
	};

class CAgent
	{
public:
	TInt Start();
	void Loop();
private:
	TInt OpenLink();
	void Send(const TDesC8& aLine);
	void SendRaw(const TDesC8& aData);
	void End(TOut& aO) { Send(aO.iB); }
	void Ok(const TDesC8& aTag) { TOut o; o.Begin(aTag, "ok"); End(o); }
	void Err(const TDesC8& aTag, TInt aCode, const char* aMsg);
	void HandleLine(TDes8& aLine);
	void HandleBytes(const TDesC8& aData);
	void IssueRead();
	void Watch(RProcess& aP);
	void CheckWatches();
	void KeepAwakeTick();
	void Tick();
	TInt iTicks;
	TInt Ws();
	// commands
	void CmdHello(const TDesC8& aTag);
	void CmdLs(const TDesC8& aTag, const TDesC8& aPath);
	void CmdPut(const TDesC8& aTag, const TDesC8& aPath, const TDesC8& aSize, const TDesC8& aCrc);
	void CmdGet(const TDesC8& aTag, const TDesC8& aPath);
	void CmdRm(const TDesC8& aTag, const TDesC8& aPath);
	void CmdMkdir(const TDesC8& aTag, const TDesC8& aPath);
	void CmdRun(const TDesC8& aTag, const TDesC8& aPath, const TDesC8& aArgs);
	void CmdKill(const TDesC8& aTag, const TDesC8& aPid, const TDesC8& aReason);
	void CmdPs(const TDesC8& aTag, const TDesC8& aMatch);
	void CmdThreads(const TDesC8& aTag, const TDesC8& aMatch);
	void CmdApps(const TDesC8& aTag);
	void CmdLaunch(const TDesC8& aTag, const TDesC8& aWhat);
	void CmdWg(const TDesC8& aTag);
	void CmdFg(const TDesC8& aTag, const TDesC8& aWg);
	void CmdKeylock(const TDesC8& aTag, const TDesC8& aOp);
	void CmdKeepAwake(const TDesC8& aTag, const TDesC8& aOp);
	void CmdProp(const TDesC8& aTag, TBool aSet, const TDesC8& aCat, const TDesC8& aKey, const TDesC8& aVal);
	void CmdMem(const TDesC8& aTag);
	void ToPath(const TDesC8& aIn, TDes& aOut) { aOut.Copy(aIn.Left(aOut.MaxLength())); }

	RBusDevComm iComm;
	RFs iFs;
	RWsSession iWs;
	TBool iWsOpen;
	TRequestStatus iReadStatus;
	TBuf8<512> iReadBuf;
	TBuf8<1024> iLine;
	// put state
	TBool iInPut;
	RFile iPutFile;
	TInt iPutLeft, iPutSize;
	TUint32 iPutCrc, iPutWant;
	TBuf8<32> iPutTag;
	TFileName iPutStage, iPutDest;
	TInt iPutSinceAck;
	TUint iPutLastTick;
	TBool iPutFailed;
	// process watches
	SWatch iWatch[KMaxWatch];
	// keep-awake
	RTimer iTimer;
	TRequestStatus iTimerStatus;
	TBool iKeepAwake, iKeepUnlocked;
	TBool iQuit;
	};

TInt CAgent::OpenLink()
	{
	TInt r = User::LoadPhysicalDevice(_L("EUART"));
	TInt rl = User::LoadLogicalDevice(_L("ECOMM"));
	if ((r != KErrNone && r != KErrAlreadyExists) || (rl != KErrNone && rl != KErrAlreadyExists))
		{
		static TInt said;
		if (!said++)
			P("LoadPhysicalDevice EUART -> %d, LoadLogicalDevice ECOMM -> %d (retrying)", r, rl);
		}
	r = iComm.Open(KUnit);
	if (r != KErrNone)
		return r;
	TCommConfig cfg;
	iComm.Config(cfg);
	cfg().iRate = EBps115200;
	cfg().iDataBits = EData8;
	cfg().iStopBits = EStop1;
	cfg().iParity = EParityNone;
	cfg().iHandshake = 0;
	cfg().iTerminatorCount = 0;
	r = iComm.SetConfig(cfg);
	if (r != KErrNone)
		P("SetConfig -> %d", r);
	iComm.SetReceiveBufferLength(16384);
	return KErrNone;
	}

TInt CAgent::Start()
	{
	CrcInit();
	TInt r = iFs.Connect();
	if (r != KErrNone)
		return r;
	// started very early (EStart, before the composite Z: is mounted): drivers may not load yet
	for (TInt i = 0; i < 120; i++)
		{
		r = OpenLink();
		if (r == KErrNone)
			break;
		User::After(500000);
		}
	P("serial unit %d open -> %d", KUnit, r);
	if (r != KErrNone)
		return r;
	iTimer.CreateLocal();
	iTimer.After(iTimerStatus, 1000000);
	TOut o;
	o.Begin(_L8("*"), "hello");
	o.Str("version", KVersion);
	RProcess me;
	o.Hex("pid", (TUint)me.Id());
	End(o);
	IssueRead();
	return KErrNone;
	}

void CAgent::Send(const TDesC8& aLine)
	{
	TBuf8<1600> b;
	b.Copy(aLine.Left(b.MaxLength() - 1));
	b.Append('\n');
	SendRaw(b);
	}

void CAgent::SendRaw(const TDesC8& aData)
	{
	TRequestStatus s;
	iComm.Write(s, aData);
	User::WaitForRequest(s);
	}

void CAgent::Err(const TDesC8& aTag, TInt aCode, const char* aMsg)
	{
	TOut o;
	o.Begin(aTag, "err");
	o.Int("code", aCode);
	o.Str("msg", aMsg);
	End(o);
	}

void CAgent::IssueRead()
	{
	iReadBuf.Zero();
	iComm.ReadOneOrMore(iReadStatus, iReadBuf);
	}

void CAgent::Loop()
	{
	while (!iQuit)
		{
		User::WaitForAnyRequest();
		if (iReadStatus != KRequestPending)
			{
			TInt r = iReadStatus.Int();
			if (r == KErrNone || iReadBuf.Length() > 0)
				HandleBytes(iReadBuf);
			if (r != KErrNone && r != KErrCommsFrame && r != KErrCommsOverrun && r != KErrCommsParity)
				P("read -> %d", r);
			if (!iQuit)
				IssueRead();
			}
		if (iTimerStatus != KRequestPending)
			{
			Tick();
			iTimer.After(iTimerStatus, 1000000);
			}
		CheckWatches();
		}
	}

void CAgent::HandleBytes(const TDesC8& aData)
	{
	TInt i = 0;
	while (i < aData.Length())
		{
		if (iInPut)
			{
			TInt n = Min(iPutLeft, aData.Length() - i);
			TPtrC8 part = aData.Mid(i, n);
			if (!iPutFailed && iPutFile.Write(part) != KErrNone)
				iPutFailed = ETrue;
			iPutCrc = Crc(iPutCrc, part.Ptr(), n);
			iPutLeft -= n;
			i += n;
			iPutSinceAck += n;
			iPutLastTick = User::NTickCount();
			if (iPutSinceAck >= KAckChunk && iPutLeft > 0)
				{
				// flow control: the host sends KAckChunk bytes, then waits for this
				iPutSinceAck = 0;
				TOut a;
				a.Begin(iPutTag, "ack");
				a.Int("got", iPutSize - iPutLeft);
				End(a);
				}
			if (iPutLeft == 0)
				{
				iInPut = EFalse;
				iPutFile.Close();
				if (iPutFailed)
					Err(iPutTag, KErrWrite, "write failed");
				else if (iPutCrc != iPutWant)
					Err(iPutTag, KErrCorrupt, "crc mismatch");
				else if (iPutStage != iPutDest && Dafs(_L("mv"), iPutStage, iPutDest) != KErrNone)
					Err(iPutTag, KErrAccessDenied, "dafs mv failed");
				else
					{
					TOut o;
					o.Begin(iPutTag, "ok");
					o.Int("size", iPutSize);
					o.Hex("crc", iPutCrc);
					End(o);
					}
				}
			continue;
			}
		TUint8 c = aData[i++];
		if (c == '\n')
			{
			HandleLine(iLine);
			iLine.Zero();
			}
		else if (c != '\r' && iLine.Length() < iLine.MaxLength())
			iLine.Append(c);
		}
	}

// split by single spaces into at most aMax parts; the last part keeps the rest of the line
static TInt Split(const TDesC8& aLine, TPtrC8* aParts, TInt aMax)
	{
	TInt n = 0, start = 0;
	for (TInt i = 0; i <= aLine.Length() && n < aMax; i++)
		{
		if (i == aLine.Length() || (aLine[i] == ' ' && n < aMax - 1))
			{
			if (i > start)
				aParts[n++].Set(aLine.Mid(start, i - start));
			start = i + 1;
			}
		}
	return n;
	}

void CAgent::HandleLine(TDes8& aLine)
	{
	TPtrC8 part[5];
	TInt n = Split(aLine, part, 5);
	if (n < 2)
		return;
	TBuf8<32> tag;
	tag.Copy(part[0].Left(32));
	TBuf8<512> a[3];
	for (TInt i = 0; i < 3; i++)
		{
		if (2 + i < n)
			{
			a[i].Copy(part[2 + i].Left(512));
			if (!(i == 2 && part[1] == _L8("run")))		// run keeps the raw argument string
				Unescape(a[i]);
			}
		}
	const TDesC8& cmd = part[1];
	if (cmd == _L8("hello")) CmdHello(tag);
	else if (cmd == _L8("ls")) CmdLs(tag, a[0]);
	else if (cmd == _L8("put")) CmdPut(tag, a[0], a[1], a[2]);
	else if (cmd == _L8("get")) CmdGet(tag, a[0]);
	else if (cmd == _L8("rm")) CmdRm(tag, a[0]);
	else if (cmd == _L8("mkdir")) CmdMkdir(tag, a[0]);
	else if (cmd == _L8("run"))
		{
		// run <path> [raw args...]: everything after the path is the command line
		TPtrC8 rest;
		if (n >= 4)
			rest.Set(aLine.Mid(part[3].Ptr() - aLine.Ptr()));
		TBuf8<512> args(rest.Left(512));
		Unescape(args);
		CmdRun(tag, a[0], args);
		}
	else if (cmd == _L8("kill")) CmdKill(tag, a[0], a[1]);
	else if (cmd == _L8("ps")) CmdPs(tag, a[0]);
	else if (cmd == _L8("threads")) CmdThreads(tag, a[0]);
	else if (cmd == _L8("apps")) CmdApps(tag);
	else if (cmd == _L8("launch")) CmdLaunch(tag, a[0]);
	else if (cmd == _L8("wg")) CmdWg(tag);
	else if (cmd == _L8("fg")) CmdFg(tag, a[0]);
	else if (cmd == _L8("keylock")) CmdKeylock(tag, a[0]);
	else if (cmd == _L8("keepawake")) CmdKeepAwake(tag, a[0]);
	else if (cmd == _L8("pget")) CmdProp(tag, EFalse, a[0], a[1], KNullDesC8);
	else if (cmd == _L8("pset")) CmdProp(tag, ETrue, a[0], a[1], a[2]);
	else if (cmd == _L8("mem")) CmdMem(tag);
	else if (cmd == _L8("dmask"))
		{
		// dmask [INDEX] | dmask set HEX [INDEX]: kernel debug trace mask word (KIPC = bit 11 of word 0)
		TInt idx = 0;
		TUint32 v = 0;
		TBool set = a[0] == _L8("set");
		if (set && !ParseUint(a[1], v, EHex)) { Err(tag, KErrArgument, "usage: dmask set HEX [index]"); return; }
		ParseInt(set ? a[2] : a[0], idx);
		if (set) User::SetDebugMask(v, idx);
		TOut o;
		o.Begin(tag, "ok");
		o.Int("index", idx);
		o.Hex("mask", UserSvr::DebugMask(idx));
		End(o);
		}
	else if (cmd == _L8("quit")) { Ok(tag); iQuit = ETrue; }
	else if (cmd == _L8("upgrade"))
		{
		// upgrade <staged exe>: dafs waits for us to exit, swaps the binary, starts the new agent
		TFileName src;
		ToPath(a[0], src);
		TBuf<600> cl;
		cl.Append(_L("upgrade "));
		RProcess me;
		cl.AppendNum((TUint)me.Id(), EHex);
		cl.Append(' ');
		cl.Append(src);
		RProcess d;
		TInt r = d.Create(_L("c:\\sys\\bin\\dafs.exe"), cl);
		if (r != KErrNone) { Err(tag, r, "dafs start failed"); return; }
		d.Resume();
		d.Close();
		Ok(tag);
		iQuit = ETrue;
		}
	else Err(tag, KErrNotSupported, "unknown command");
	}

// ------------------------------------------------------------------------------------------------

void CAgent::CmdHello(const TDesC8& aTag)
	{
	TOut o;
	o.Begin(aTag, "ok");
	o.Str("version", KVersion);
	RProcess me;
	o.Hex("pid", (TUint)me.Id());
	TInt w = 0;
	for (TInt i = 0; i < KMaxWatch; i++)
		if (iWatch[i].iUsed) w++;
	o.Int("watched", w);
	o.Int("keepawake", iKeepAwake);
	End(o);
	}

void CAgent::CmdLs(const TDesC8& aTag, const TDesC8& aPath)
	{
	TFileName p;
	ToPath(aPath, p);
	if (p.Length() && p[p.Length() - 1] != '\\')
		{
		TEntry e;
		if (iFs.Entry(p, e) == KErrNone && !e.IsDir())
			{
			TOut o;
			o.Begin(aTag, ".");
			o.Field("name", e.iName);
			o.Int("size", e.iSize);
			o.Hex("att", e.iAtt);
			End(o);
			Ok(aTag);
			return;
			}
		p.Append('\\');
		}
	CDir* dir = NULL;
	TInt r = iFs.GetDir(p, KEntryAttMaskSupported, ESortByName | EDirsFirst, dir);
	if (r != KErrNone)
		{
		Err(aTag, r, "GetDir failed");
		return;
		}
	for (TInt i = 0; i < dir->Count(); i++)
		{
		const TEntry& e = (*dir)[i];
		TOut o;
		o.Begin(aTag, ".");
		o.Field("name", e.iName);
		o.Int("size", e.iSize);
		o.Str("type", e.IsDir() ? "dir" : "file");
		TDateTime dt = e.iModified.DateTime();
		TBuf8<24> t;
		t.Format(_L8("%04d-%02d-%02dT%02d:%02d:%02d"), dt.Year(), dt.Month() + 1, dt.Day() + 1,
				 dt.Hour(), dt.Minute(), dt.Second());
		o.Field("mtime", t);
		End(o);
		}
	TOut o;
	o.Begin(aTag, "ok");
	o.Int("count", dir->Count());
	End(o);
	delete dir;
	}

void CAgent::CmdPut(const TDesC8& aTag, const TDesC8& aPath, const TDesC8& aSize, const TDesC8& aCrc)
	{
	TUint32 size, crc;
	if (!ParseUint(aSize, size) || !ParseUint(aCrc, crc, EHex))
		{
		Err(aTag, KErrArgument, "usage: put path size crc32hex");
		return;
		}
	TFileName p;
	ToPath(aPath, p);
	iPutDest = p;
	if (NeedsTcb(p))
		{
		// stage in our private dir, dafs.exe moves it into place when complete
		iFs.CreatePrivatePath(EDriveC);
		iFs.PrivatePath(p);
		p.Insert(0, _L("C:"));
		p.Append(_L("incoming.tmp"));
		}
	iPutStage = p;
	iFs.MkDirAll(p);
	TInt r = iPutFile.Replace(iFs, p, EFileWrite | EFileShareExclusive);
	if (r != KErrNone)
		{
		Err(aTag, r, "Replace failed");
		return;
		}
	iPutTag.Copy(aTag);
	iPutSize = iPutLeft = (TInt)size;
	iPutWant = crc;
	iPutCrc = 0;
	iPutFailed = EFalse;
	iPutSinceAck = 0;
	iPutLastTick = User::NTickCount();
	TOut o;
	o.Begin(aTag, "go");
	End(o);
	if (size == 0)
		{
		iPutFile.Close();
		TOut k;
		k.Begin(aTag, "ok");
		k.Int("size", 0);
		k.Hex("crc", 0);
		End(k);
		return;
		}
	iInPut = ETrue;
	}

void CAgent::CmdGet(const TDesC8& aTag, const TDesC8& aPath)
	{
	TFileName p;
	ToPath(aPath, p);
	RFile f;
	TInt r = f.Open(iFs, p, EFileRead | EFileShareReadersOnly);
	if (r != KErrNone)
		{
		Err(aTag, r, "Open failed");
		return;
		}
	TInt size = 0;
	f.Size(size);
	// crc first (one pass), then the data (second pass)
	HBufC8* buf = HBufC8::New(KChunk);
	if (!buf)
		{
		f.Close();
		Err(aTag, KErrNoMemory, "no memory");
		return;
		}
	TPtr8 b = buf->Des();
	TUint32 crc = 0;
	TInt pos = 0;
	while (pos < size)
		{
		f.Read(b);
		if (b.Length() == 0) break;
		crc = Crc(crc, b.Ptr(), b.Length());
		pos += b.Length();
		}
	TOut o;
	o.Begin(aTag, "data");
	o.Int("size", pos);
	o.Hex("crc", crc);
	End(o);
	TInt seek = 0;
	f.Seek(ESeekStart, seek);
	TInt left = pos;
	while (left > 0)
		{
		f.Read(b, Min(KChunk, left));
		if (b.Length() == 0) break;
		SendRaw(b);
		left -= b.Length();
		}
	delete buf;
	f.Close();
	Ok(aTag);
	}

void CAgent::CmdRm(const TDesC8& aTag, const TDesC8& aPath)
	{
	TFileName p;
	ToPath(aPath, p);
	TInt r = NeedsTcb(p) ? Dafs(_L("rm"), p, KNullDesC) : iFs.Delete(p);
	if (r == KErrNone) Ok(aTag); else Err(aTag, r, "Delete failed");
	}

void CAgent::CmdMkdir(const TDesC8& aTag, const TDesC8& aPath)
	{
	TFileName p;
	ToPath(aPath, p);
	if (p.Length() && p[p.Length() - 1] != '\\') p.Append('\\');
	TInt r = iFs.MkDirAll(p);
	if (r == KErrNone || r == KErrAlreadyExists) Ok(aTag); else Err(aTag, r, "MkDirAll failed");
	}

void CAgent::Watch(RProcess& aP)
	{
	for (TInt i = 0; i < KMaxWatch; i++)
		{
		SWatch& w = iWatch[i];
		if (!w.iUsed)
			{
			w.iUsed = ETrue;
			w.iProc = aP;		// takes over the handle
			w.iPid = (TUint)aP.Id();
			w.iProc.Logon(w.iStatus);
			return;
			}
		}
	aP.Close();
	}

static const char* ExitTypeName(TExitType aT)
	{
	switch (aT)
		{
	case EExitKill: return "kill";
	case EExitTerminate: return "terminate";
	case EExitPanic: return "panic";
	default: return "pending";
		}
	}

void CAgent::CheckWatches()
	{
	for (TInt i = 0; i < KMaxWatch; i++)
		{
		SWatch& w = iWatch[i];
		if (w.iUsed && w.iStatus != KRequestPending)
			{
			TOut o;
			o.Begin(_L8("*"), "exit");
			o.Hex("pid", w.iPid);
			o.Field("name", w.iProc.Name());
			o.Str("type", ExitTypeName(w.iProc.ExitType()));
			o.Int("reason", w.iProc.ExitReason());
			o.Field("category", w.iProc.ExitCategory());
			End(o);
			w.iProc.Close();
			w.iUsed = EFalse;
			}
		}
	}

void CAgent::CmdRun(const TDesC8& aTag, const TDesC8& aPath, const TDesC8& aArgs)
	{
	TFileName p;
	ToPath(aPath, p);
	TBuf<512> args;
	args.Copy(aArgs);
	RProcess proc;
	TInt r = proc.Create(p, args);
	if (r != KErrNone)
		{
		Err(aTag, r, "RProcess::Create failed");
		return;
		}
	TOut o;
	o.Begin(aTag, "ok");
	o.Hex("pid", (TUint)proc.Id());
	o.Field("name", proc.Name());
	Watch(proc);
	// resume after the logon is in place
	for (TInt i = 0; i < KMaxWatch; i++)
		if (iWatch[i].iUsed && iWatch[i].iPid == (TUint)proc.Id())
			iWatch[i].iProc.Resume();
	End(o);
	}

void CAgent::CmdKill(const TDesC8& aTag, const TDesC8& aPid, const TDesC8& aReason)
	{
	RProcess proc;
	TInt r;
	TUint32 id;
	if (ParseUint(aPid, id, EHex) && aPid.Length() <= 8)
		r = proc.Open(TProcessId(id));
	else
		{
		// by name pattern
		TFullName name;
		TBuf<128> pat;
		pat.Copy(aPid);
		pat.Append('*');
		TFindProcess fp(pat);
		r = fp.Next(name);
		if (r == KErrNone)
			r = proc.Open(fp);
		}
	if (r != KErrNone)
		{
		Err(aTag, r, "open process failed");
		return;
		}
	TInt reason = 0;
	if (aReason.Length()) ParseInt(aReason, reason);
	TOut o;
	o.Begin(aTag, "ok");
	o.Hex("pid", (TUint)proc.Id());
	o.Field("name", proc.Name());
	proc.Kill(reason);
	proc.Close();
	End(o);
	}

void CAgent::CmdPs(const TDesC8& aTag, const TDesC8& aMatch)
	{
	TBuf<128> pat;
	if (aMatch.Length()) { pat.Copy(aMatch); pat.Append('*'); } else pat.Copy(_L("*"));
	TFindProcess fp(pat);
	TFullName name;
	TInt n = 0;
	while (fp.Next(name) == KErrNone)
		{
		RProcess p;
		TOut o;
		o.Begin(aTag, ".");
		if (p.Open(fp) == KErrNone)
			{
			o.Hex("pid", (TUint)p.Id());
			o.Field("name", name);
			o.Str("exit", ExitTypeName(p.ExitType()));
			if (p.ExitType() != EExitPending)
				{
				o.Int("reason", p.ExitReason());
				o.Field("category", p.ExitCategory());
				}
			o.Hex("uid3", p.SecureId().iId);
			o.Int("prio", p.Priority());
			p.Close();
			}
		else
			o.Field("name", name);
		End(o);
		n++;
		}
	TOut o;
	o.Begin(aTag, "ok");
	o.Int("count", n);
	End(o);
	}

static const char* PrioName(TThreadPriority aP)
	{
	switch (aP)
		{
	case EPriorityMuchLess: return "muchless";
	case EPriorityLess: return "less";
	case EPriorityNormal: return "normal";
	case EPriorityMore: return "more";
	case EPriorityMuchMore: return "muchmore";
	case EPriorityRealTime: return "realtime";
	default: return "abs";
		}
	}

void CAgent::CmdThreads(const TDesC8& aTag, const TDesC8& aMatch)
	{
	TBuf<128> pat;
	if (aMatch.Length()) { pat.Copy(aMatch); pat.Append('*'); } else pat.Copy(_L("*"));
	TFindThread ft(pat);
	TFullName name;
	TInt n = 0;
	while (ft.Next(name) == KErrNone)
		{
		RThread t;
		TOut o;
		o.Begin(aTag, ".");
		o.Field("name", name);
		if (t.Open(ft) == KErrNone)
			{
			o.Hex("tid", (TUint)t.Id());
			o.Str("exit", ExitTypeName(t.ExitType()));
			if (t.ExitType() != EExitPending)
				{
				o.Int("reason", t.ExitReason());
				o.Field("category", t.ExitCategory());
				}
			o.Str("prio", PrioName(t.Priority()));
			t.Close();
			}
		End(o);
		n++;
		}
	TOut o;
	o.Begin(aTag, "ok");
	o.Int("count", n);
	End(o);
	}

void CAgent::CmdApps(const TDesC8& aTag)
	{
	RApaLsSession ls;
	TInt r = ls.Connect();
	if (r != KErrNone)
		{
		Err(aTag, r, "apparc connect failed");
		return;
		}
	r = ls.GetAllApps();
	// the app list may still be building right after boot
	for (TInt tries = 0; r == RApaLsSession::EAppListInvalid && tries < 20; tries++)
		{
		User::After(250000);
		r = ls.GetAllApps();
		}
	if (r != KErrNone)
		{
		ls.Close();
		Err(aTag, r, "GetAllApps failed");
		return;
		}
	TApaAppInfo info;
	TInt n = 0;
	while ((r = ls.GetNextApp(info)) == KErrNone)
		{
		TOut o;
		o.Begin(aTag, ".");
		o.Hex("uid", info.iUid.iUid);
		o.Field("caption", info.iCaption);
		o.Field("path", info.iFullName);
		End(o);
		n++;
		}
	ls.Close();
	TOut o;
	o.Begin(aTag, "ok");
	o.Int("count", n);
	End(o);
	}

void CAgent::CmdLaunch(const TDesC8& aTag, const TDesC8& aWhat)
	{
	RApaLsSession ls;
	TInt r = ls.Connect();
	if (r != KErrNone)
		{
		Err(aTag, r, "apparc connect failed");
		return;
		}
	TFileName exe;
	TUint32 uid = 0;
	if (ParseUint(aWhat, uid, EHex))
		{
		TApaAppInfo info;
		r = ls.GetAppInfo(info, TUid::Uid(uid));
		if (r != KErrNone)
			{
			ls.Close();
			Err(aTag, r, "GetAppInfo failed");
			return;
			}
		exe = info.iFullName;
		}
	else
		{
		// by caption (case-insensitive prefix)
		TBuf<128> want;
		want.Copy(aWhat);
		ls.GetAllApps();
		TApaAppInfo info;
		r = KErrNotFound;
		while (ls.GetNextApp(info) == KErrNone)
			if (info.iCaption.Left(want.Length()).CompareF(want) == 0)
				{
				exe = info.iFullName;
				uid = info.iUid.iUid;
				r = KErrNone;
				break;
				}
		if (r != KErrNone)
			{
			ls.Close();
			Err(aTag, r, "no app with that caption");
			return;
			}
		}
	CApaCommandLine* cl = CApaCommandLine::NewL();	// leaves only on OOM
	cl->SetExecutableNameL(exe);
	cl->SetCommandL(EApaCommandRun);
	TThreadId tid;
	r = ls.StartApp(*cl, tid);
	delete cl;
	ls.Close();
	if (r != KErrNone)
		{
		Err(aTag, r, "StartApp failed");
		return;
		}
	TOut o;
	o.Begin(aTag, "ok");
	o.Hex("uid", uid);
	o.Field("path", exe);
	RThread t;
	if (t.Open(tid) == KErrNone)
		{
		RProcess p;
		if (t.Process(p) == KErrNone)
			{
			o.Hex("pid", (TUint)p.Id());
			Watch(p);
			}
		t.Close();
		}
	End(o);
	}

TInt CAgent::Ws()
	{
	if (iWsOpen) return KErrNone;
	TInt r = iWs.Connect();
	if (r == KErrNone) iWsOpen = ETrue;
	return r;
	}

static void WgName(RWsSession& aWs, TInt aId, TDes& aOut)
	{
	aOut.Zero();
	if (aWs.GetWindowGroupNameFromIdentifier(aId, aOut) != KErrNone)
		aOut.Zero();
	for (TInt i = 0; i < aOut.Length(); i++)
		if (aOut[i] == 0) aOut[i] = '|';
	}

void CAgent::CmdWg(const TDesC8& aTag)
	{
	TInt r = Ws();
	if (r != KErrNone)
		{
		Err(aTag, r, "wserv connect failed");
		return;
		}
	CArrayFixFlat<TInt>* list = new CArrayFixFlat<TInt>(16);
	TInt focus = iWs.GetFocusWindowGroup();
	r = list ? iWs.WindowGroupList(list) : KErrNoMemory;
	if (r != KErrNone)
		{
		delete list;
		Err(aTag, r, "WindowGroupList failed");
		return;
		}
	TBuf<256> name;
	for (TInt i = 0; i < list->Count(); i++)
		{
		TInt id = (*list)[i];
		WgName(iWs, id, name);
		TOut o;
		o.Begin(aTag, ".");
		o.Int("pos", i);
		o.Int("wgid", id);
		o.Int("prio", iWs.GetWindowGroupOrdinalPriority(id));
		o.Field("name", name);
		TThreadId tid;
		if (iWs.GetWindowGroupClientThreadId(id, tid) == KErrNone)
			o.Hex("tid", (TUint)tid);
		if (id == focus) o.Int("focus", 1);
		End(o);
		}
	TOut o;
	o.Begin(aTag, "ok");
	o.Int("focus", focus);
	WgName(iWs, focus, name);
	o.Field("focus_name", name);
	o.Int("count", list->Count());
	End(o);
	delete list;
	}

void CAgent::CmdFg(const TDesC8& aTag, const TDesC8& aWg)
	{
	TInt r = Ws();
	TInt wg = 0;
	if (r == KErrNone && !ParseInt(aWg, wg))
		r = KErrArgument;
	if (r == KErrNone)
		{
		TApaTask task(iWs);
		task.SetWgId(wg);
		task.BringToForeground();
		Ok(aTag);
		}
	else
		Err(aTag, r, "fg failed");
	}

void CAgent::CmdKeylock(const TDesC8& aTag, const TDesC8& aOp)
	{
	RAknKeyLock kl;
	TInt r = kl.Connect();
	if (r != KErrNone)
		{
		Err(aTag, r, "keylock connect failed");
		return;
		}
	if (aOp == _L8("off")) kl.DisableWithoutNote();
	else if (aOp == _L8("on")) kl.EnableWithoutNote();
	TOut o;
	o.Begin(aTag, "ok");
	o.Int("locked", kl.IsKeyLockEnabled() ? 1 : 0);
	kl.Close();
	End(o);
	}

void CAgent::Tick()
	{
	// a put that stalls (lost bytes, host gone) must not swallow later commands
	if (iInPut && User::NTickCount() - iPutLastTick > 10000)
		{
		iInPut = EFalse;
		iPutFile.Close();
		iFs.Delete(iPutStage);
		Err(iPutTag, KErrTimedOut, "put stalled");
		}
	if (++iTicks % 5 == 0)
		KeepAwakeTick();
	}

void CAgent::KeepAwakeTick()
	{
	if (iKeepAwake)
		User::ResetInactivityTime();
	if (iKeepUnlocked)
		{
		RAknKeyLock kl;
		if (kl.Connect() == KErrNone)
			{
			if (kl.IsKeyLockEnabled())
				kl.DisableWithoutNote();
			kl.Close();
			}
		}
	}

void CAgent::CmdKeepAwake(const TDesC8& aTag, const TDesC8& aOp)
	{
	// keepawake on|off|unlocked (= on + keep the keypad unlocked)
	iKeepAwake = aOp != _L8("off");
	iKeepUnlocked = aOp == _L8("unlocked");
	KeepAwakeTick();
	TOut o;
	o.Begin(aTag, "ok");
	o.Int("keepawake", iKeepAwake);
	o.Int("unlocked", iKeepUnlocked);
	End(o);
	}

void CAgent::CmdProp(const TDesC8& aTag, TBool aSet, const TDesC8& aCat, const TDesC8& aKey, const TDesC8& aVal)
	{
	TUint32 cat, key;
	if (!ParseUint(aCat, cat, EHex) || !ParseUint(aKey, key, EHex))
		{
		Err(aTag, KErrArgument, "usage: pget|pset cat key [value] (hex)");
		return;
		}
	TInt r;
	TInt v = 0;
	if (aSet)
		{
		if (!ParseInt(aVal, v))
			{
			Err(aTag, KErrArgument, "bad value");
			return;
			}
		r = RProperty::Set(TUid::Uid(cat), key, v);
		}
	else
		{
		r = RProperty::Get(TUid::Uid(cat), key, v);
		if (r == KErrArgument)		// not an int: return bytes
			{
			TBuf8<256> b;
			r = RProperty::Get(TUid::Uid(cat), key, b);
			if (r == KErrNone)
				{
				TOut o;
				o.Begin(aTag, "ok");
				o.Field("bytes", b);
				End(o);
				return;
				}
			}
		}
	if (r != KErrNone)
		{
		Err(aTag, r, aSet ? "RProperty::Set failed" : "RProperty::Get failed");
		return;
		}
	TOut o;
	o.Begin(aTag, "ok");
	o.Int("value", v);
	End(o);
	}

void CAgent::CmdMem(const TDesC8& aTag)
	{
	TMemoryInfoV1Buf mi;
	UserHal::MemoryInfo(mi);
	TOut o;
	o.Begin(aTag, "ok");
	o.Int("ram_kb", mi().iTotalRamInBytes >> 10);
	o.Int("free_kb", mi().iFreeRamInBytes >> 10);
	o.Int("rom_kb", mi().iTotalRomInBytes >> 10);
	End(o);
	}

// ------------------------------------------------------------------------------------------------

GLDEF_C TInt E32Main()
	{
	P("start");
	CTrapCleanup* cleanup = CTrapCleanup::New();
	TAny* mark = User::MarkCleanupStack();		// NewL/SetExecutableNameL push on the cleanup stack
	CAgent* a = new CAgent;
	TInt r = a ? a->Start() : KErrNoMemory;
	P("Start -> %d", r);
	if (r == KErrNone)
		a->Loop();
	User::UnMarkCleanupStack((TTrapHandler*)mark);
	delete cleanup;
	P("exit");
	return r;
	}

// Startup glue called from startup.s (see src/n95apps/dsbtest/dsbtest.cpp).
struct SDaThreadCreateInfo
	{
	TAny* iHandle;
	TInt iType;
	TThreadFunction iFunction;
	TAny* iPtr;
	};
struct SStdEpocThreadCreateInfo;
IMPORT_C TInt DaSetupThreadHeap(TBool, SStdEpocThreadCreateInfo&) asm("_ZN8UserHeap15SetupThreadHeapEiR24SStdEpocThreadCreateInfo");

extern "C" void DaRunThread(TBool aNotFirst, SDaThreadCreateInfo& aInfo)
	{
	TInt r = DaSetupThreadHeap(aNotFirst, (SStdEpocThreadCreateInfo&)aInfo);
	if (r == KErrNone)
		{
		if (aNotFirst)
			r = (*aInfo.iFunction)(aInfo.iPtr);
		else
			{
			User::InitProcess();
			r = E32Main();
			}
		}
	User::Exit(r);
	}

extern "C" void DaInvariant() { User::Invariant(); }
extern "C" void DaHandleException(TAny* aInfo) { User::HandleException(aInfo); }
