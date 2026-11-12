// netap.exe: the LAN access point for QEMU's syborg-net (enet.ldd + sybeth.pdd -> etherpkt.drv -> ethint), and a
// connection test. Run from sydb: `sydb run 'c:\sys\bin\netap.exe' ARG`; output on RDebug ("NETAP: ...").
//   create   LANBearer + LANService (DHCP) + Network + IAP "Ethernet (host)", unless an IAP of that name exists
//   allow [0|1]  CenRep 0x101F876C key 1 (KCoreAppUIsNetworkConnectionAllowed): 1 lets apps connect in the
//            Offline profile (no SIM: the phone cannot leave it); the browser checks it before ESock
//   cr UID KEY [VALUE]  read (or set) an integer CenRep key, hex UID and KEY
//   gprs [APN]  a packet-data IAP "Internet", added to the first destination, (OutgoingGPRS, APN default "internet", IPv4, address and DNS from
//            the network) on the firmware's "GPRS Modem" bearer (spud-rawip over DATAPORT), unless one exists
//   usage [0|1|2]  cellular data usage at home and abroad (CMManager general settings): 0 ask, 1 automatic
//            (default), 2 disabled; "ask" denies connections that must not prompt (-46)
//   dest IAP [SNAP]  add connection method IAP to destination SNAP (default: the first one, "Internet")
//            through CMManager, so applications that use destinations (the browser) pick it
//   avail    the IAPs ConnMon reports available (what MPM picks from for a destination)
//   snap SNAP  start a connection on a destination (as the browser does), then resolve example.com
//   reg      ETel: network registration, packet service registration and status (what ConnMon judges)
//   list     every IAP: id, name, service and bearer type
//   test [IAP] [HOST]  start that IAP (default: ours), resolve HOST (default example.com), HTTP GET /, print the
//            status line and the byte count
// CommsDat through the legacy CommDb API (text column names); ESock for the test.
#include <e32base.h>
#include <e32debug.h>
#include <commdb.h>
#include <cdbcols.h>
#include <commdbconnpref.h>
#include <es_sock.h>
#include <in_sock.h>
#include <centralrepository.h>
#include <cmmanager.h>
#include <cmgenconnsettings.h>
#include <cmdestination.h>
#include <cmconnectionmethod.h>
#include <rconnmon.h>
#include <connpref.h>
#include <etelmm.h>
#include <etelpckt.h>

_LIT(KApName, "Ethernet (host)");

static void Log(TRefByValue<const TDesC> aFmt, ...)
	{
	VA_LIST l;
	VA_START(l, aFmt);
	TBuf<256> b;
	b.AppendFormatList(aFmt, l);
	RDebug::Print(_L("NETAP: %S"), &b);
	}

static TUint32 FindIapL(CCommsDatabase& aDb, const TDesC& aName)
	{
	CCommsDbTableView* v = aDb.OpenViewMatchingTextLC(TPtrC(IAP), TPtrC(COMMDB_NAME), aName);
	TUint32 id = 0;
	if (v->GotoFirstRecord() == KErrNone)
		v->ReadUintL(TPtrC(COMMDB_ID), id);
	CleanupStack::PopAndDestroy(v);
	return id;
	}

static TUint32 InsertL(CCommsDbTableView& aV)
	{
	TUint32 id;
	User::LeaveIfError(aV.InsertRecord(id));
	return id;
	}

static void CreateL()
	{
	CCommsDatabase* db = CCommsDatabase::NewL(EDatabaseTypeIAP);
	CleanupStack::PushL(db);
	TUint32 have = FindIapL(*db, KApName);
	if (have)
		{
		Log(_L("IAP %d \"%S\" exists"), have, &KApName);
		CleanupStack::PopAndDestroy(db);
		return;
		}
	User::LeaveIfError(db->BeginTransaction());

	CCommsDbTableView* v = db->OpenTableLC(TPtrC(NETWORK));
	TUint32 net = InsertL(*v);
	v->WriteTextL(TPtrC(COMMDB_NAME), KApName);
	User::LeaveIfError(v->PutRecordChanges());
	CleanupStack::PopAndDestroy(v);

	v = db->OpenTableLC(TPtrC(LAN_BEARER));
	TUint32 bearer = InsertL(*v);
	v->WriteTextL(TPtrC(COMMDB_NAME), _L("Ethernet (host) bearer"));
	v->WriteTextL(TPtrC(LAN_BEARER_AGENT), _L("nullagt.agt"));
	v->WriteTextL(TPtrC(LAN_BEARER_NIF_NAME), _L("ethint"));
	v->WriteTextL(TPtrC(LAN_BEARER_LDD_FILENAME), _L("enet"));
	v->WriteTextL(TPtrC(LAN_BEARER_LDD_NAME), _L("Ethernet"));
	v->WriteTextL(TPtrC(LAN_BEARER_PDD_FILENAME), _L("sybeth"));
	v->WriteTextL(TPtrC(LAN_BEARER_PDD_NAME), _L("Ethernet.Syborg"));
	v->WriteTextL(TPtrC(LAN_BEARER_PACKET_DRIVER_NAME), _L("EtherPkt.drv"));
	v->WriteUintL(TPtrC(LAST_SOCKET_ACTIVITY_TIMEOUT), (TUint32)-1);
	v->WriteUintL(TPtrC(LAST_SESSION_CLOSED_TIMEOUT), (TUint32)-1);
	v->WriteUintL(TPtrC(LAST_SOCKET_CLOSED_TIMEOUT), (TUint32)-1);
	User::LeaveIfError(v->PutRecordChanges());
	CleanupStack::PopAndDestroy(v);

	v = db->OpenTableLC(TPtrC(LAN_SERVICE));
	TUint32 service = InsertL(*v);
	v->WriteTextL(TPtrC(COMMDB_NAME), _L("Ethernet (host) service"));
	v->WriteTextL(TPtrC(LAN_IF_NETWORKS), _L("ip"));
	v->WriteBoolL(TPtrC(LAN_IP_ADDR_FROM_SERVER), ETrue);
	v->WriteBoolL(TPtrC(LAN_IP_DNS_ADDR_FROM_SERVER), ETrue);
	v->WriteTextL(TPtrC(LAN_CONFIG_DAEMON_MANAGER_NAME), _L("NetCfgExtnDhcp"));
	v->WriteTextL(TPtrC(LAN_CONFIG_DAEMON_NAME), _L("!DhcpServ"));
	User::LeaveIfError(v->PutRecordChanges());
	CleanupStack::PopAndDestroy(v);

	v = db->OpenTableLC(TPtrC(IAP));
	TUint32 iap = InsertL(*v);
	v->WriteTextL(TPtrC(COMMDB_NAME), KApName);
	v->WriteUintL(TPtrC(IAP_SERVICE), service);
	v->WriteTextL(TPtrC(IAP_SERVICE_TYPE), TPtrC(LAN_SERVICE));
	v->WriteUintL(TPtrC(IAP_BEARER), bearer);
	v->WriteTextL(TPtrC(IAP_BEARER_TYPE), TPtrC(LAN_BEARER));
	v->WriteUintL(TPtrC(IAP_NETWORK), net);
	v->WriteUintL(TPtrC(IAP_NETWORK_WEIGHTING), 0);
	v->WriteUintL(TPtrC(IAP_LOCATION), 2);		// the "Mobile" location record that S60's IAPs use [I]
	User::LeaveIfError(v->PutRecordChanges());
	CleanupStack::PopAndDestroy(v);

	User::LeaveIfError(db->CommitTransaction());
	Log(_L("created IAP %d (bearer %d, service %d, network %d)"), iap, bearer, service, net);
	CleanupStack::PopAndDestroy(db);
	}

static void DestL(TUint32 aIap, TUint32 aSnap);
_LIT(KGprsName, "Internet");

static void GprsL(const TDesC& aApn)
	{
	CCommsDatabase* db = CCommsDatabase::NewL(EDatabaseTypeIAP);
	CleanupStack::PushL(db);
	TUint32 have = FindIapL(*db, KGprsName);
	if (have)
		{
		Log(_L("IAP %d \"%S\" exists"), have, &KGprsName);
		CleanupStack::PopAndDestroy(db);
		return;
		}
	TUint32 bearer = 0;
	CCommsDbTableView* v = db->OpenTableLC(TPtrC(MODEM_BEARER));
	for (TInt r = v->GotoFirstRecord(); r == KErrNone; r = v->GotoNextRecord())
		{
		TBuf<64> name, nif;
		v->ReadTextL(TPtrC(COMMDB_NAME), name);
		TRAP_IGNORE(v->ReadTextL(TPtrC(MODEM_NIF_NAME), nif));
		TUint32 id;
		v->ReadUintL(TPtrC(COMMDB_ID), id);
		Log(_L("ModemBearer %d \"%S\" nif %S"), id, &name, &nif);
		if (!bearer && nif.FindF(_L("rawip")) >= 0)
			bearer = id;
		}
	CleanupStack::PopAndDestroy(v);
	if (!bearer)
		{
		Log(_L("no packet-data ModemBearer"));
		User::Leave(KErrNotFound);
		}
	User::LeaveIfError(db->BeginTransaction());
	v = db->OpenTableLC(TPtrC(NETWORK));
	TUint32 net = InsertL(*v);
	v->WriteTextL(TPtrC(COMMDB_NAME), KGprsName);
	User::LeaveIfError(v->PutRecordChanges());
	CleanupStack::PopAndDestroy(v);

	v = db->OpenTableLC(TPtrC(OUTGOING_GPRS));
	TUint32 service = InsertL(*v);
	v->WriteTextL(TPtrC(COMMDB_NAME), KGprsName);
	v->WriteLongTextL(TPtrC(GPRS_APN), aApn);
	v->WriteUintL(TPtrC(GPRS_PDP_TYPE), 0);			// IPv4
	v->WriteTextL(TPtrC(GPRS_IF_NETWORKS), _L("ip"));
	v->WriteBoolL(TPtrC(GPRS_IF_PROMPT_FOR_AUTH), EFalse);
	v->WriteBoolL(TPtrC(GPRS_IP_ADDR_FROM_SERVER), ETrue);
	v->WriteBoolL(TPtrC(GPRS_IP_DNS_ADDR_FROM_SERVER), ETrue);
	v->WriteBoolL(TPtrC(GPRS_DISABLE_PLAIN_TEXT_AUTH), EFalse);
	v->WriteUintL(TPtrC(GPRS_AP_TYPE), 2);			// [I] internet + WAP
	User::LeaveIfError(v->PutRecordChanges());
	CleanupStack::PopAndDestroy(v);

	v = db->OpenTableLC(TPtrC(IAP));
	TUint32 iap = InsertL(*v);
	v->WriteTextL(TPtrC(COMMDB_NAME), KGprsName);
	v->WriteUintL(TPtrC(IAP_SERVICE), service);
	v->WriteTextL(TPtrC(IAP_SERVICE_TYPE), TPtrC(OUTGOING_GPRS));
	v->WriteUintL(TPtrC(IAP_BEARER), bearer);
	v->WriteTextL(TPtrC(IAP_BEARER_TYPE), TPtrC(MODEM_BEARER));
	v->WriteUintL(TPtrC(IAP_NETWORK), net);
	v->WriteUintL(TPtrC(IAP_NETWORK_WEIGHTING), 0);
	v->WriteUintL(TPtrC(IAP_LOCATION), 2);
	User::LeaveIfError(v->PutRecordChanges());
	CleanupStack::PopAndDestroy(v);
	User::LeaveIfError(db->CommitTransaction());
	Log(_L("created IAP %d \"%S\" (APN %S, bearer %d, service %d)"), iap, &KGprsName, &aApn, bearer, service);
	CleanupStack::PopAndDestroy(db);
	DestL(iap, 0);			// into the first destination ("Internet"), where the browser looks
	}

static void UsageL(TInt aUsage)
	{
	RCmManager cm;
	cm.OpenL();
	TCmGenConnSettings g;
	cm.ReadGenConnSettingsL(g);
	Log(_L("cellular data usage home %d visitor %d, wlan %d -> %d"), g.iCellularDataUsageHome,
		g.iCellularDataUsageVisitor, g.iUsageOfWlan, aUsage);
	g.iCellularDataUsageHome = (TCmCellularDataUsage)aUsage;
	g.iCellularDataUsageVisitor = (TCmCellularDataUsage)aUsage;
	cm.WriteGenConnSettingsL(g);
	cm.Close();
	}

static void DestL(TUint32 aIap, TUint32 aSnap)
	{
	RCmManager cm;
	cm.OpenL();
	RArray<TUint32> dests;
	cm.AllDestinationsL(dests);
	for (TInt i = 0; i < dests.Count(); i++)
		{
		RCmDestination d = cm.DestinationL(dests[i]);
		HBufC* name = d.NameLC();
		Log(_L("destination %d \"%S\": %d methods"), dests[i], name, d.ConnectionMethodCount());
		CleanupStack::PopAndDestroy(name);
		d.Close();
		}
	if (!aSnap && dests.Count())
		aSnap = dests[0];
	dests.Close();
	RCmDestination d = cm.DestinationL(aSnap);
	RCmConnectionMethod m = cm.ConnectionMethodL(aIap);
	TInt idx = d.AddConnectionMethodL(m);
	d.UpdateL();
	Log(_L("IAP %d added to destination %d at %d"), aIap, aSnap, idx);
	m.Close();
	d.Close();
	cm.Close();
	}

static void AvailL()
	{
	RConnectionMonitor cm;
	cm.ConnectL();
	TConnMonIapInfoBuf buf;
	TRequestStatus st;
	cm.GetPckgAttribute(EBearerIdAll, 0, KIapAvailability, buf, st);
	User::WaitForRequest(st);
	Log(_L("available IAPs: %d (status %d)"), buf().iCount, st.Int());
	for (TUint i = 0; i < buf().iCount && i < KConnMonMaxIAPCount; i++)
		Log(_L("  IAP %d"), buf().iIap[i].iIapId);
	cm.Close();
	}

static void SnapL(TUint32 aSnap)
	{
	RSocketServ ss;
	User::LeaveIfError(ss.Connect());
	RConnection conn;
	User::LeaveIfError(conn.Open(ss));
	TConnSnapPref pref(aSnap);
	TRequestStatus st;
	conn.Start(pref, st);
	User::WaitForRequest(st);
	Log(_L("start SNAP %d: %d"), aSnap, st.Int());
	if (st.Int() == KErrNone)
		{
		RHostResolver hr;
		if (hr.Open(ss, KAfInet, KProtocolInetUdp, conn) == KErrNone)
			{
			TNameEntry ne;
			TInt r = hr.GetByName(_L("example.com"), ne);
			Log(_L("resolve example.com: %d"), r);
			hr.Close();
			}
		}
	conn.Close();
	ss.Close();
	}

static void RegL()
	{
	RTelServer ts;
	User::LeaveIfError(ts.Connect());
	ts.LoadPhoneModule(_L("phonetsy"));
	RTelServer::TPhoneInfo pi;
	ts.GetPhoneInfo(0, pi);
	RMobilePhone ph;
	User::LeaveIfError(ph.Open(ts, pi.iName));
	TRequestStatus st;
	RMobilePhone::TMobilePhoneRegistrationStatus reg;
	ph.GetNetworkRegistrationStatus(st, reg);
	User::WaitForRequest(st);
	Log(_L("phone registration %d (%d)"), reg, st.Int());
	RPacketService ps;
	TInt r = ps.Open(ph);
	if (r == KErrNone)
		{
		RPacketService::TRegistrationStatus preg;
		ps.GetNtwkRegStatus(st, preg);
		User::WaitForRequest(st);
		RPacketService::TStatus pst;
		TInt r2 = ps.GetStatus(pst);
		Log(_L("packet registration %d (%d), packet status %d (%d)"), preg, st.Int(), pst, r2);
		ps.Close();
		}
	else
		Log(_L("packet service open %d"), r);
	ph.Close();
	ts.Close();
	}

static void ListL()
	{
	CCommsDatabase* db = CCommsDatabase::NewL(EDatabaseTypeIAP);
	CleanupStack::PushL(db);
	CCommsDbTableView* v = db->OpenTableLC(TPtrC(IAP));
	for (TInt r = v->GotoFirstRecord(); r == KErrNone; r = v->GotoNextRecord())
		{
		TUint32 id = 0;
		TBuf<64> name, st, bt;
		v->ReadUintL(TPtrC(COMMDB_ID), id);
		v->ReadTextL(TPtrC(COMMDB_NAME), name);
		v->ReadTextL(TPtrC(IAP_SERVICE_TYPE), st);
		v->ReadTextL(TPtrC(IAP_BEARER_TYPE), bt);
		Log(_L("IAP %d \"%S\" %S / %S"), id, &name, &st, &bt);
		}
	CleanupStack::PopAndDestroy(2, db);
	}

static void TestL(TUint32 aIap, const TDesC& aHost)
	{
	if (!aIap)
		{
		CCommsDatabase* db = CCommsDatabase::NewL(EDatabaseTypeIAP);
		CleanupStack::PushL(db);
		aIap = FindIapL(*db, KApName);
		CleanupStack::PopAndDestroy(db);
		}
	RSocketServ ss;
	User::LeaveIfError(ss.Connect());
	CleanupClosePushL(ss);
	RConnection conn;
	User::LeaveIfError(conn.Open(ss));
	CleanupClosePushL(conn);
	TCommDbConnPref pref;
	pref.SetIapId(aIap);
	pref.SetDialogPreference(ECommDbDialogPrefDoNotPrompt);
	TRequestStatus st;
	conn.Start(pref, st);
	User::WaitForRequest(st);
	Log(_L("start IAP %d: %d"), aIap, st.Int());
	User::LeaveIfError(st.Int());

	RHostResolver hr;
	User::LeaveIfError(hr.Open(ss, KAfInet, KProtocolInetUdp, conn));
	CleanupClosePushL(hr);
	TNameEntry ne;
	TInt r = hr.GetByName(aHost, ne);
	TInetAddr addr = TInetAddr::Cast(ne().iAddr);
	TBuf<64> a;
	addr.Output(a);
	Log(_L("resolve %S: %d %S"), &aHost, r, &a);
	User::LeaveIfError(r);

	RSocket s;
	User::LeaveIfError(s.Open(ss, KAfInet, KSockStream, KProtocolInetTcp, conn));
	CleanupClosePushL(s);
	addr.SetPort(80);
	s.Connect(addr, st);
	User::WaitForRequest(st);
	Log(_L("connect: %d"), st.Int());
	User::LeaveIfError(st.Int());
	TBuf8<256> req;
	req.Copy(_L8("GET / HTTP/1.0\r\nHost: "));
	TBuf8<128> h;
	h.Copy(aHost);
	req.Append(h);
	req.Append(_L8("\r\nConnection: close\r\n\r\n"));
	s.Write(req, st);
	User::WaitForRequest(st);
	TBuf8<1024> buf;
	TSockXfrLength len;
	TInt total = 0;
	TBuf<80> first;
	for (;;)
		{
		s.RecvOneOrMore(buf, 0, st, len);
		User::WaitForRequest(st);
		if (st.Int() != KErrNone)
			break;
		if (!total)
			{
			TInt e = buf.Find(_L8("\r\n"));
			first.Copy(buf.Left(e > 0 ? Min(e, 80) : Min(buf.Length(), 80)));
			}
		total += buf.Length();
		}
	Log(_L("HTTP: \"%S\", %d bytes (end %d)"), &first, total, st.Int());
	CleanupStack::PopAndDestroy(4, &ss);
	}

static void AllowL(TInt aValue)
	{
	CRepository* rep = CRepository::NewL(TUid::Uid(0x101F876C));
	TInt old = -1;
	rep->Get(1, old);
	TInt r = rep->Set(1, aValue);
	Log(_L("network connection allowed: %d -> %d (%d)"), old, aValue, r);
	delete rep;
	}

static void CrL(TUint32 aUid, TUint32 aKey, TBool aSet, TInt aValue)
	{
	CRepository* rep = CRepository::NewL(TUid::Uid(aUid));
	TInt v = 0;
	TInt r = rep->Get(aKey, v);
	Log(_L("cr %08x/%x = %d (%d)"), aUid, aKey, v, r);
	if (aSet)
		{
		r = rep->Set(aKey, aValue);
		Log(_L("cr %08x/%x := %d (%d)"), aUid, aKey, aValue, r);
		}
	delete rep;
	}

static void MainL()
	{
	TBuf<128> cmd;
	User::CommandLine(cmd);
	TLex lex(cmd);
	TPtrC verb = lex.NextToken();
	if (verb == _L("create"))
		CreateL();
	else if (verb == _L("allow"))
		{
		TInt v = 1;
		TLex n(lex.NextToken());
		n.Val(v);
		AllowL(v);
		}
	else if (verb == _L("cr"))
		{
		TUint32 uid = 0, key = 0;
		TLex a(lex.NextToken()), b(lex.NextToken());
		a.Val(uid, EHex);
		b.Val(key, EHex);
		TPtrC t = lex.NextToken();
		TInt v = 0;
		if (t.Length())
			{
			TLex c(t);
			c.Val(v);
			}
		CrL(uid, key, t.Length() > 0, v);
		}
	else if (verb == _L("gprs"))
		{
		TPtrC apn = lex.NextToken();
		GprsL(apn.Length() ? apn : TPtrC(_L("internet")));
		}
	else if (verb == _L("usage"))
		{
		TInt v = 1;
		TLex n(lex.NextToken());
		n.Val(v);
		UsageL(v);
		}
	else if (verb == _L("dest"))
		{
		TUint32 iap = 0, snap = 0;
		TLex a(lex.NextToken()), b(lex.NextToken());
		a.Val(iap, EDecimal);
		b.Val(snap, EDecimal);
		DestL(iap, snap);
		}
	else if (verb == _L("reg"))
		RegL();
	else if (verb == _L("avail"))
		AvailL();
	else if (verb == _L("snap"))
		{
		TUint32 snap = 4097;
		TLex a(lex.NextToken());
		a.Val(snap, EDecimal);
		SnapL(snap);
		}
	else if (verb == _L("list"))
		ListL();
	else if (verb == _L("test"))
		{
		TUint32 iap = 0;
		TPtrC t = lex.NextToken();
		TPtrC host(_L("example.com"));
		if (t.Length())
			{
			TLex n(t);
			if (n.Val(iap, EDecimal) != KErrNone)
				{
				iap = 0;
				host.Set(t);
				}
			else
				{
				TPtrC h2 = lex.NextToken();
				if (h2.Length())
					host.Set(h2);
				}
			}
		TestL(iap, host);
		}
	else
		Log(_L("usage: netap create | list | test [IAP] [HOST]"));
	}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* tc = CTrapCleanup::New();
	TRAPD(r, MainL());
	if (r != KErrNone)
		Log(_L("failed: %d"), r);
	delete tc;
	return r;
	}

// ---- process start-up for devagent/startup.s (as eglprobe.cpp): heap, C++ runtime, E32Main ----
struct SDaThreadCreateInfo
	{
	TAny* iHandle;
	TInt iType;
	TThreadFunction iFunction;
	TAny* iPtr;
	};
struct SStdEpocThreadCreateInfo;
IMPORT_C TInt DaSetupThreadHeap(TBool, SStdEpocThreadCreateInfo&) asm("_ZN8UserHeap15SetupThreadHeapEiR24SStdEpocThreadCreateInfo");
IMPORT_C void DaExceptionsGlobals(TAny* aThis) asm("_ZN23TCppRTExceptionsGlobalsC1Ev");

extern "C" void DaRunThread(TBool aNotFirst, SDaThreadCreateInfo& aInfo)
	{
	TUint64 exceptionGlobals[64];
	TInt r = DaSetupThreadHeap(aNotFirst, (SStdEpocThreadCreateInfo&)aInfo);
	if (r == KErrNone)
		{
		DaExceptionsGlobals(exceptionGlobals);		// after the heap: its Dll::SetTls allocates
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
