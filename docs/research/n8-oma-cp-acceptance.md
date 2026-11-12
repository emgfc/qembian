# N8 Delight 6.7: why our OMA CP push is accepted by the stack but never stored

Static analysis of the ROFS modules (`build/evidence/n8-omacp/`: ELFs relocated to the `SYBCODE:` bases of
`build/instances/n8-sim/serial.log`, Ghidra output in `ghidra/`) checked against the Symbian Foundation source
of the same component (`~/work/symbian/oss.FCL.sf.mw.remotemgmt/omaprovisioning/provisioning`; the export
ordinals of ProvisioningEngine.dll match its `eabi/ProvisioningEngineu.def` one to one).

## Decision path (provisioninghandler.dll at 7bb6c000, ProvisioningEngine.dll at 7bb60000)

1. watcher -> pushapphandler picks the handler by content type (BIO database `IsBioMessageL`); the handler DLL
   loaded at 45.98 s, so the WSP header/content type and the X-Wap-Application-Id are fine. [verified]
2. `CWPMessage::ProcessL` = FUN_7bb6ca20. Order (matches `CWPMessage.cpp` plus three Belle CenRep keys):
   - `WPPhoneFactory::CreateL` (ord30): `RMobilePhone::GetSubscriberId` = CTelServer M:22004 r:-1 at 46.19 s;
     the error is swallowed, IMSI = "". It only matters for NETWPIN/USERNETWPIN. [verified]
   - CenRep 0x101F87AA `Set(4,0)`, `Set(5,0)` (log 46.19: M:0, M:6, M:6, M:35). [verified]
   - `CWPEngine::NewLC` (ord69), `ImportDocumentL(body)` (ord62), `PopulateL` (ord75: adapters loaded
     46.27–46.80 s, so the WBXML parsed without a leave), `ContextExistsL(originator)` (ord56). [verified]
   - `CWPBootstrap::NewL(IMSI)` (ord7), `BootstrapL(msg, engine, KNullDesC)` (ord6) -> `DoAuthenticateL` ->
     `CWPPushMessage::AuthenticateL` = export_21 @ 7bb61fbc: reads key 1 (ROM `101f87aa.cre`: key0=1,
     key1=15 "all mechanisms", key2=0, key3="", keys 4..7=0); SEC=USERPIN (iSec=1) with an empty PIN ->
     `Set(4,1)` and result KWPAuthResultPinRequired -> EPinRequired (2). Log 46.989 Get, 46.990 Set. [verified]
     No "trusted sender", operator or DM-domain check exists on this path; DmDomainServer at 53.3 s is the
     watcher's housekeeping after `CPluginKiller::KillPushPlugin`. [verified]
   - Handler reads keys 5, 4, 7 (log 47.003: M:0, M:5, M:5, M:5, M:35), `haveSettings = CWPEngine::ItemCount()
     > 0` (ord102 = `CWPAdapterManager::iItemCount`, the sum of the adapters' ItemCount). [verified]
   - switch @ 7bb6cb1a (switch8 table at 7bb6cb1e), enum 0 ENoBootstrap, 1 ENotAuthenticated, 2 EPinRequired,
     3 EAuthenticationFailed, 4 EBootstrapExists, 5 ESucceeded: [verified]
     - 0 and 2 -> 7bb6cbea: `haveSettings ? (key7!=0 && key4!=0 && ItemCount()==key5 ? silent return :
       StoreMsgL @ 7bb6c7fe) : User::Leave(KErrCorrupt)` (7bb6cc3c, r0 = -20).
     - 1 -> 7bb6cb40: like above when haveSettings, else StoreMsgL("authentication failed", class 0 variant
       7bb6c900 if key2==1, else 7bb6c6e8) then Leave(KErrAccessDenied).
     - 3 -> 7bb6cbf0: StoreMsgL("authentication failed") then Leave(KErrAccessDenied): still visible in Inbox.
     - 4 -> 7bb6cc2c: StoreMsgL(), or StoreMsgL("bootstrap exists") + Leave(KErrAlreadyExists).
     - 5 -> 7bb6cc42: StoreMsgL(), or StoreMsgL("bootstrapped").
   - StoreMsgL creates the Inbox entry (`CMsvSession::GetEntryL(0x1002)`, `CMsvEntry::CreateL` = MsvServer
     opcode EMsvCreateEntry, `EditStoreL`, `CWPPushMessage::StoreL`, `ChangeL`). [verified]

## Why ours is dropped

The watcher never sent EMsvCreateEntry: its MsvServer traffic is two session opens (M:-1, 11, 37, 51 at
45.99 and 46.73 s) and the closes (M:12 = cancel of the pending M:11 notify) at 47.02/47.10/47.11 s. So no
StoreMsgL variant ran. With key7 = 0 the silent-dedupe branch cannot fire; the only remaining non-storing
branch for EPinRequired is `haveSettings == false` -> `User::Leave(KErrCorrupt)` at 7bb6cc3c ->
`CWPHandler::RunError` -> message dropped without any UI. [verified by exclusion]
So the engine's adapters counted zero items for our document: wpapadapter's VisitL (FUN_7bb58906) creates a
CWPAPNapdef for type 0x55 (NAPDEF) and only counts it as an item when its index (+0x204, FUN_7bb59b0c) is set,
which requires NAPID (parm 0x11 -> flag 0x800) and an accepted bearer/address; the APPLICATION branch (0x155)
sets the "browser" flag (+0x84) only when APPID == "w2" and feature 0x16f is on, and that flag is passed into
the NAPDEF created *after* it. [inferred: ItemCount itself not located, budget reached]

## What to change (cheapest first)

1. Measure, not guess (one sydb run): hw breakpoints at 7bb6cb0a (r0 = ItemCount after ord102), 7bb6cc3c
   (Leave, r0 = reason) and 7bb6cc46 (StoreMsgL). Bases from the run's `SYBCODE:` lines (they were stable).
2. Document (tools/sms_provision.py): encode BEARER as value token 0xAB (GSM-GPRS) and NAP-ADDRTYPE as 0x89
   (APN) instead of inline strings; add `name=INTERNET` (0x14, no value) to NAPDEF; emit the APPLICATION
   (APPID w2, TO-NAPID) characteristic *before* NAPDEF. These are what real operator CP messages carry.
   [inferred]
3. Not needed: NETWPIN/IMSI. With a working GetSubscriberId and MAC = HMAC-SHA1(ConvertIMSIL(IMSI), WBXML)
   the result is still ENoBootstrap (no BOOTSTRAP/PROVIDER-ID, iTPS empty) -> the same haveSettings test;
   it only spares the user the PIN prompt. A BOOTSTRAP characteristic with NETWPIN would yield
   ESucceeded/StoreMsgL("bootstrapped") even with zero items, but needs the IMSI path first. [verified]
4. Modem: 22004 (GetSubscriberId) answering -1 is harmless here but should return the SIM's IMSI (a second
   ETel session at 46.98 s already reads SIM_IMSI over ISI, resource 0x09/0x1d) once NETWPIN is wanted.
