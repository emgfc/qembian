# N95 (S60 3.1, RM-320 35.0.001): why our OMA CP push is accepted but never stored

Evidence: `build/instances/n95-test/serial.log` (push injected at 78.318 s: 156 + 53 byte TPDUs), headless
Ghidra of the core ROM modules (`build/firmware/RM-320/35.0.001/13/ghidra/ProvisioningHandler.dll.c`,
`ProvisioningEngine.dll.c`, `NOKIATSY.DLL.c`, `pushwatcher.dll.c`, `pushsecurity.dll.c`, `pushapphandler.dll.c`),
the Symbian Foundation `omaprovisioning/provisioning` source and `build/epocroot/.../platform/etelmmcs.h`.

## What the phone did (78.35–78.53 s) [verified]

1. ESock_SmsWap hands the reassembled push to watcher.exe; pushwatcher asks ECOM for a content handler with
   match string `application/vnd.wap.connectivity-wbxml` (ECOM 7 at 78.368) and loads `ProvisioningHandler.dll`
   (its static deps ProvisioningEngine/Parser/BC come with it). So WSP headers, content type, the SEC/MAC
   parameters and the port/concatenation are all fine: the right handler is chosen.
2. The handler opens MsvServer (M:-1, 11, 37 r:0, 51 r:2 = session/notify set-up), scans the BIF database
   (`resource\messaging\Bif\*.r01`, ~78.38–78.52 s; that is `CBIODatabase::NewLC`, see `imp_biodb_` in the
   handler) and then builds the phone object:
   `CWPMessage::ProcessL` = FUN_82f6b99c: `if (!iPhone) iPhone = WPPhoneFactory::CreateL()` (ProvisioningEngine
   ord30 -> ord31 -> `CWPPhone::ConstructL` = FUN_82f63a18), **not trapped**, before `CWPEngine::NewLC` (ord69),
   `ImportDocumentL` (ord62) and `PopulateL` (ord75).
3. `CWPPhone::ConstructL`: RTelServer::Connect, LoadPhoneModule("PhoneTsy") (M:57), RPhone::Open("DefaultPhone")
   (M:0), Initialise (M:75 r:0), then
   - etelmm ord462 = `RMobilePhone::GetSubscriberId` = IPC **22017** -> r:0 (our modem's READ_IMSI reply is
     parsed by NOKIATSY FUN_808c6c30 into "001010123456789"; fine), `LeaveIfError` passes;
   - etelmm ord455 = `RMobilePhone::GetHomeNetwork` = IPC **22004** -> **r:-1 (KErrNotFound)**, completed by the
     TSY in the same millisecond (no ISI round trip: answered from its cache), then `User::LeaveIfError(status)`.
   IPC numbers from etelmmcs.h: 22004 = EMobilePhoneGetHomeNetwork, 22017 = EMobilePhoneGetSubscriberId
   (EKA2L1 `etel/common.h` agrees). The N8 doc's "22004 = GetSubscriberId" is a mislabel: it was GetHomeNetwork
   there too, and the Belle handler swallows it; the 3.1 engine leaves.
4. The leave unwinds `ProcessL` -> handler RunError -> `CPluginKiller::KillPushPlugin`; the watcher closes the
   MsvServer session (M:12 at 78.530, session delete at 78.531). No ECOM adapter load, no CenRep, no
   `CMsvEntry::CreateL` ever happens. The document is never parsed, so nothing in `tools/sms_provision.py` is
   at fault for the N95 (same bytes as the N8 accepts; sender/PIN/MAC never reached).

## Fix: answer GetHomeNetwork on the N95 TSY (modem side, not the SMS)

- NOKIATSY completes 22004 from its cached home-PLMN info; the cache is filled at start-up from the SIM server
  (resource 0x09, ch 7 at 3.29 s): `2D {04 00}` (SIM_NETWORK_INFO_REQ, service 04 = read HPLMN) and `BC {01 00}`
  twice, which we answer `BD 01 24` (data not available). [inferred: which of the two feeds GetHomeNetwork;
  0x2D/HPLMN is the ofono-named one]. Answer them with the virtual SIM's MCC/MNC (`sim.mcc`/`sim.mnc` already
  exist in syborg_modem.c) in `syborg_modem.c` / `platform/isi/replies-bb5.txt`, status 01 (OK), like the
  READ_IMSI case at `syborg_modem.c:450`. Expect the N95 (and N73/5800-era TSYs) to need the same.
- Cheapest confirmations (one sydb run each, no SMS variants needed):
  1. `sydb strace EtelServer` while sending `sydb sms-provision`: 22004 must return 0 after the fix.
  2. hw breakpoint at the `LeaveIfError` after ord455 in FUN_82f63a18 (ROM, fixed address 0x82f63a18+; check
     r0 == -1 before the fix), then at FUN_82f6b99c's ord102 (`ItemCount`) and the `User::Leave` after it: the
     N8-style "zero items -> KErrCorrupt" test comes next and is the only remaining drop path for the current
     document (the N8 fix already made ItemCount > 0 with this layout).
  3. If 22004 still fails after answering 0x2D: `sydb log --grep 'ch 7 .* 00 09 '` and compare which SIM
     request still gets status 24; also check `NET 0x0A` home-network name reads (`0A 36/37` at 3.257 is RAT).
- Not the cause (ruled out): content-type encoding, X-Wap-Application-Id, WSP form, PROV public id, ports,
  SEC/MAC/PIN (authentication runs only after the phone object exists), concatenation. Leave sms_provision.py.
