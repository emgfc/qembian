# Connectivity for qembian phones: Bluetooth, WLAN, cellular, and the network path to the host

Research and design study, 2026-11-10. No code changed. Marks: [V] verified in this tree (file
lists, binaries' strings, EPL source, our docs/code), [I] inferred (Symbian/BB5 knowledge, naming
references such as oFono headers, or a plausible reading not checked in a binary). Budget reached at
about 32 tool calls; every "to verify" is a trace to run, not a guess to trust.

## Recommendation (short)

1. **Network path: Ethernet on a Syborg mailbox NIC -> slirp.** Reuse the EPL ethernet LDD
   (`enet.ldd`, "Ethernet", `kernel/eka/drivers/ethernet`) with a small PDD of ours (the baseport's
   virtio PDD as a template), a QEMU device in the style of `syborg_modem.c` (TX/RX windows + IRQ)
   registered as a QEMU NIC on `-netdev user` (slirp, DHCP 10.0.2.x). Guest side: the firmware's
   `EthInt.nif` on Symbian^3/Belle (N8, 701, 808 have it [V]) plus a LAN IAP in CommsDat. Missing
   pieces: the packet driver `EtherPkt.drv` (not in any firmware [V]) and, on 9.x phones (N95, 5800,
   N73), `EthInt.nif` itself [V]; both are small EPL networking sources to build (not in
   `build/epocroot`, which has only kernelhwsrv + graphics + adapt/qemu [V]). Effort: QEMU device
   1-2 days, PDD 1 day, CommsDat records + packet driver 2-3 days, 9.x ethint build 2-4 days.
   Apps see a normal IAP (LANService bearer) [I: standard CommsDat; the S60 destination/SNAP
   membership must be added for apps that pick "Internet").
2. **Cellular as the second path**, on the host-side modem that already exists (`syborg_modem.c`):
   every firmware generation has the whole packet-data user side (TSY, GPRS/UMTS agent, `rawip.nif`
   or `rawip.dll`, `DATAPORT.CSY`, `spud.nif` on 9.x) [V], so nothing new goes into the guest; the
   host modem must learn PN_GPDS (attach, context create/configure/activate), PN_PIPE (DATAPORT's
   pipes) and a "SIM present + registered" model, and bridge the pipe's IP packets into slirp. Effort
   2-4 weeks, mostly trace-and-answer loops; the SIM-present model touches start-up on every phone.
3. **Bluetooth: fake HCI controller on the host behind a `bc02` stand-in.** Every phone's HCI
   transport bottoms out in `BC02.LDD` + `bc02_uart1.PDD` (CSR BlueCore 02 naming; PDD units
   "BC02.Helen_2" on 9.x, "BC02.Rap" on Belle) [V]. One stand-in LDD forwards bytes to a QEMU char
   device; a host controller answers Reset/version/address/scan/inquiry with zero devices. Transport
   framing differs per generation (9.x `Hci.dll` monolith, probably BCSP; Symbian^3/Belle HCI v2
   framework `h4plushciv2_*` = H4 with a power byte) [I]. Effort: 1 week for the first generation
   once a trace shows the LDD controls and the first bytes; +2-3 days per further framing.
4. **WLAN: do not emulate.** Nokia's stack is TI WiLink LMAC firmware over SPI (`wlanpdd.pdd`
   gen2.5, `wlanhwinit.dll` loads NVS/PDA data) under a 124 KB UMAC LDD with the WHA/WSA command
   set, then `WlanPkt.drv`/`WlanNif.nif`/`WlmServer` [V names, I roles]. A stand-in must fake either
   the LMAC command protocol (below the UMAC) or the UMAC's management+data API (above it); both are
   large proprietary interfaces: 4-8 weeks, high risk, for the same IP connectivity Ethernet gives in
   days. Keep the WlmServer death (kill -5 at 8 s, tolerated on 9.2 [V oracle table]) or make the UI
   honest with `hardware.wlan = false` where the profile allows it.
5. **701/808 audio <-> Bluetooth**: found and already patched (H-fmradio-none). The chain is
   !AudioServer -> Lac.dll -> fmradioaudiocontrol.dll -> radioadaptationserver.exe -> radiohalserver.dll
   `CRadioHci` (`MHciFunctions`, `MHCIEventObserver`, `MHciChipStatusObserver` [V strings]) ->
   `hciaccess.dll` (HCI Direct Access: `CHCIEventReceiveHandler`, `CHCIHardResetListener` [V]) ->
   the Bluetooth stack over ESock (fn 190 -> -1 without a stack) [V sym102 README]. So the audio
   server waits for the FM radio HAL, which waits for a Bluetooth HCI direct-access session to the
   combo chip (FM tuner driven by vendor HCI commands) [I: chip]. With a BT stand-in, the patch can
   go only if the fake controller answers the FM vendor commands (OGF 0x3F) with success.

6. **USB (section 6)**: backup/restore, transfer and install are already covered by sydb's drive
   images, snapshots and `sis install`; a USB gadget (EPL `eusbc.ldd` + a PSL stand-in + a QEMU
   device exported to the host with usbip) costs 3-4 weeks and root on the host; do it after
   Ethernet and Bluetooth, Mass Storage/MTP first, never PC Suite.

Order: Ethernet NIC (Symbian^3/Belle first, EthInt.nif already in ROM) -> Bluetooth controller
(unblocks FM radio, SDP/FaxModem deaths, "BT off" UI) -> cellular packet data (universal, needed for
9.x phones unless ethint is built) -> WLAN never, unless the UI's WLAN wizard is itself the goal.

## 1. Bluetooth

### What the firmware has [V]

| Generation | Transport pieces | Stack |
|---|---|---|
| 9.1/9.2/9.3/9.4 (N73, N95, 5730, 5800) | `BC02.LDD` (classes `DLogicalDeviceBC02`, `DLogicalChannelBC02`, `DBC02PowerHandler`), `bc02_uart1.PDD` ("BC02.Helen_2": PDD unit of the OMAP2 "Helen" UART), `Hci.dll` (35 KB, ROFS, compressed: strings unreadable), `hciproxy.dll` | `bt.prt` (260 KB), `bluetooth.dll`, `btmanserver.exe`, `BTServer.exe`, `sdpserver` and `FaxModem` die at 13 s without it (oracle table, `docs/real-phone-tracing.md`) |
| Symbian^3 / Belle (N8, 701, 808) | `BC02.LDD` (`DBtPowerHandler`), `bc02_uart1.PDD` ("BC02.Rap": RAPU UART), HCI v2 framework: `corehci_base.dll`, `hctl_base.dll`, `h4plushciv2_hctl.dll` (`CHCTLUart`, `CHCTLUartPort`, `CHCTLUartSender/Receiver`, `CUartSupervisionTimer`, `MHCTLPowerInterface`), `h4plushciv2_initialisor.dll` (44 KB: vendor init), `h4plushciv2_qdp.dll`, `h4plushciv2_corehci.dll`, `hcicmdq.dll`, `hciaccess.dll`, `btpowercontrol.dll`, `bt_v2.prt` | ESock BT worker (`esock_bt.cmi`, our H-bt-ondemand keeps it off until asked: at boot it panicked KERN-EXEC 0 on a transport channel it never opened and took C32Start down) |

Reading [I]: the LDD name is "BC02" and the PDD names follow Symbian's "<ldd>.<unit>" convention,
so `RBusLogicalChannel::Open` with unit 0 and the PDD "BC02.Helen_2" / "BC02.Rap". The channel is
almost certainly RBusDevComm-shaped (read/write/config/caps + power-state controls: the PDD class is
`DPhysicalDeviceBC02` with a `DBC02Helen_2` deriving from a UART PDD); the exact control numbers are
not in strings. 9.x `Hci.dll` is Nokia's monolithic HCI layer; CSR BlueCore chips default to BCSP
over UART, Nokia's H4+ ("H4 plus": H4 with a one-byte power-management channel/wake handshake)
arrived with the HCI v2 framework; which framing `Hci.dll` uses on the N95 is [I] (BCSP most likely:
CSR, 2007). The HCI v2 framework's ini files (`hctl_h4plus.ini` or similar in `private/101f7989`)
were not located in the 701 file lists; the initialisor may read PSKEY/baud data from them [I].

### Proposed design (dull, extendable)

Guest (stand-in, `src/standins/bc02/`): one LDD named "BC02" + a PDD stub named per device
(`[[standins]] config = { pdd_unit = "Helen_2" }` / `"Rap"`), or a single LDD that registers both
names (the PDD can be a load-only stub as `lightpdd` is). The channel keeps an RX FIFO and forwards
TX bytes to a QEMU device; power controls return success and raise no events. Controls to expect
[I]: Config/SetConfig (baud, flow), Caps, Read/Write (+Cancel), BreakOn/Off (never), power on/off
(the H4+ hctl's `MHCTLPowerInterface`), maybe "chip reset". First step before coding: boot a trace
build with the LDD-trace hook on "BC02" and record the control numbers and the first TX bytes (a
BCSP link-establishment `sync` packet 0xDA 0xDC 0xED 0xED, or an H4 0x01 command packet, tells the
framing).

Host (QEMU `syborg_bt.c`, same mailbox shape as `syborg_modem.c` or a plain char-device front end so
the controller can also be an external process): a fake controller with a BD_ADDR from the board
config, HCI version 4.0 (LMP 6) or 2.1 for 9.x, and these answers:

| Command (OGF/OCF) | Reply |
|---|---|
| Reset 03/0003 | Command Complete, status 0 |
| Read_Local_Version 04/0001 | HCI ver, HCI rev, LMP ver, manufacturer (CSR = 0x000A), LMP subversion |
| Read_BD_ADDR 04/0009 | status 0 + 6-byte address |
| Read_Buffer_Size 04/0005 | ACL 1021/8, SCO 64/8 |
| Read_Local_Supported_Features 04/0003, Commands 04/0002, Extended_Features 04/0004 | plausible masks, page 0 only |
| Set_Event_Mask 03/0001, Write_Scan_Enable 03/001A, Write_Local_Name 03/0013, Read_Local_Name 04/0014, Write_Class_of_Device 03/0024, Write_Page_Timeout 03/0018, Write_Inquiry_Mode 03/0045, Write_Simple_Pairing_Mode 03/0056, Host_Buffer_Size 03/0033, Write_Page/Inquiry_Scan_Activity, Write_Voice_Setting, Write_Inquiry_Scan_Type, Set_Event_Filter | Command Complete, status 0 (echo parameters where the reply carries them) |
| Inquiry 01/0001 | Command Status 0, then Inquiry Complete (status 0) after N seconds, no Inquiry Result events |
| Create_Connection 01/0005, Remote_Name_Request 01/0019 | Command Status 0, then Connection Complete / Remote Name Request Complete with status 0x04 (page timeout) |
| Vendor OGF 0x3F (CSR BCCMD: PSKEY get/set, warm reset; Nokia FM radio control) | Command Complete status 0 with the request's varid/seqno echoed; BCCMD GETREQ returns zeros [I] |
| Anything else | Command Complete / Status with 0x01 Unknown HCI Command |

Framing layers in the host, selectable per device profile: H4 (packet type byte 0x01/0x02/0x04),
H4+ (H4 plus wake bytes [I]), BCSP (SLIP 0xC0 framing, 4-byte header with seq/ack/CRC, channel 1
link establishment `sync/sync-resp/conf/conf-resp`, channel 5 HCI command/event, channel 6 ACL,
reliable-packet ack timer). BCSP is the only non-trivial piece (about 400 lines).

Firmware pieces that stay: everything above the LDD (Hci.dll/hciproxy on 9.x, the HCI v2 DLLs, the
stacks, servers, UI). The H-bt-ondemand edit goes away once the transport channel opens. The
`missing_drivers` lists drop bc02.ldd/bc02_uart1.pdd on every profile.

Unverified traps: the 9.x `Hci.dll` may open the LDD only after a Nokia power-control step through a
different driver (`DBC02PowerHandler` suggests the LDD owns it); the HCI v2 initialisor may require
a specific chip identity in Read_Local_Version to pick its init script and leave otherwise.

## 2. WLAN

What the firmware has [V names]: kernel `wlanpdd.pdd` (TI, "WlanSpiProtocol", "wlan.phys",
gen2.5 source paths `ncp_sw\spp_core3.0\wlanpdd_gen2.5\wlan_symbian`, "LMAC"), `wlanldd.ldd`
(UMAC: `WlanDot11*` states Idle/Roam/IbssMode/MacError/SoftReset, `WlanWsa*` commands Initialize/
Configure/Scan/Join/AddKey/ReadMib/WriteMib/SetPsMode/Disconnect/Release, `WlanWhaConfigureAc`,
`DWlanPowerHandler`), 5800 also `pltwlanpdd.pdd`; user side `wlanhwinit.dll` (`CWlanHwInitPdaParser`,
`CWlanHwInitPermParser`: NVS calibration data and firmware blobs), `WlanPkt.drv` (packet driver),
`WlanNif.nif`, `WlanAgt.agt`, `WlmServerExe.exe` + `WlmServer.dll` (engine), `wlmplatform.dll`,
`wlanagthotspotimpl` (5800), `wlancontrolimpl.dll` (701). The LDD dies at boot today: `WlmServerExe
WlanServer kill -5` at 8.2 s (driver missing) [V].

Depth of a stand-in [I]: (a) below the UMAC: a PDD that speaks TI's SPI transport and the LMAC
command/event protocol (WHA = Wireless Hardware Abstraction, TI firmware blocks: Initialize,
Configure with MIBs, Scan with result events, Join, keys, data path with TX/RX descriptors); the
UMAC's WHA interface is a C++ class vtable (`N10WlanHalApi3WhaE`), not a message protocol, so the
PDD must implement that class; the firmware/NVS loading by `wlanhwinit.dll` must be satisfied too
(it feeds blobs through the LDD). (b) replacing the UMAC LDD: implement what `WlanPkt.drv` and
`WlmServer` need: management requests (scan -> scan-result list in UMAC format, connect with
dot11 parameters, keys/EAPOL state, RSSI/MIB reads, power-save, indications) and the data path
(802.3/EtherType frames with a WLAN header struct the packet driver expects). Both interfaces are
Nokia-internal, undocumented, and the data path is only Ethernet frames at the end: once faked they
could be bridged to slirp/tap exactly as the Ethernet NIC, which is why the Ethernet NIC wins.
Effort 4-8 weeks, high risk of never passing the engine's state machine (association, keys, DHCP
timing). Only reason to do it: the WLAN wizard/UI itself.

## 3. Cellular packet data on the host modem

Firmware pieces [V]: `NOKIATSY`/`PHONETSY.TSY`/`SIM.TSY`, `GPRSUMTSAgent.agt` (9.x) / Symbian^3
`pdp` layer via `rawip.dll`, `rawip.nif` + `spud.nif` (9.x secondary-context multiplexer),
`DATAPORT.CSY` (ISC channels 0x2E/0x2F for ports 0/1, 0x1F for ports 2/3, `PNS_NAME_ADD_REQ`
with name 0x000A01D9 / 0x00010DD9, `PN_PIPE` 0xD9 responses with status at [11] in {1, 0x0D},
ctl 22 buffer size; `docs/devices/nokia-n95-8gb-rm320/n95-isc-isi-protocol-2.md` 3.4), the GPDS
object in NOKIATSY (`PNS_NAME_QUERY` for name 0x31 -> dev 0, obj 0x33; `31 19 STATUS_REQ`,
`31 30 CONFIGURATION_INFO_REQ` sent at boot, unanswered today), `isi-unanswered.md` on the N8 lists
GPDS 31 19/31 30 too.

Flow [I, oFono `gpds.h`/`gprs-context.c` naming reference, no code]: the NIF (`rawip`) opens
`DATAPORT::0` through C32 and writes/reads raw IP datagrams; DATAPORT (a PEP) creates a Phonet pipe
to GPDS (`PNS_PIPE_CREATE_REQ` on resource 0xD9: pipe type, PEP1 = DATAPORT's device/object, PEP2 =
GPDS 0x33, -> `PNS_PIPE_CREATE_RESP` with the pipe handle; `PNS_PIPE_ENABLE_REQ/RESP`,
`PNS_PEP_CTRL_REQ`, `PNS_PIPE_REMOVE_REQ`) and the TSY ties the context to the pipe:
`GPDS_CONTEXT_ID_CREATE_REQ` 0x02/RESP 0x03 (context id), `GPDS_LL_CONFIGURE_REQ` 0x00/0x01
(context id, pipe handle, link type PLAIN), `GPDS_CONTEXT_CONFIGURE_REQ` 0x06/0x07 (PDP type IPv4
0x21, context type normal, sub-blocks APN `GPDS_APN_INFO`, PDP address, QoS), `GPDS_ATTACH_REQ`
0x13/RESP 0x14/IND 0x15, `GPDS_CONTEXT_ACTIVATE_REQ` 0x08/RESP 0x09 then
`GPDS_CONTEXT_ACTIVATE_IND` 0x0A with sub-blocks PDP address (`GPDS_PDP_ADDRESS_INFO`), primary/
secondary DNS (`GPDS_PDNS_ADDRESS_INFO`/`GPDS_SDNS_ADDRESS_INFO`), `GPDS_CONTEXT_DEACTIVATE_REQ`
0x0B/0x0C/IND 0x0D, status indications `GPDS_CONTEXT_STATUS_IND`, `GPDS_STATUS_RESP` 0x1A (attach
status, transfer status), `GPDS_CONFIGURATION_INFO_RESP` 0x31. Data is `PNS_PIPE_DATA` 0x20
messages carrying one IP packet each on the DATAPORT channel [I]; segmentation if the ISC frame is
smaller than the MTU (1500) is DATAPORT's job and was not read.

Host modem design: a GPDS handler (contexts table, attach state) and a pipe handler in
`syborg_modem.c`; the pipe's data frames become IP packets, wrapped in an Ethernet header (fake
MACs, ARP answered by the device) and pushed through a `NetClientState` on `-netdev user` (slirp);
slirp's replies are unwrapped and sent back as `PNS_PIPE_DATA` on the pipe's channel. PDP address =
slirp's guest address (10.0.2.15), DNS 10.0.2.3. Every reply layout above is [I] until a trace of
a real context activation (connect from the browser once the IAP exists) shows the request bytes;
the loop is the existing one: `sydb strace`/ISI log -> `platform/isi/replies-bb5.txt` or code in
the host modem.

Minimum SIM model for "registered with a SIM" [I; today every profile boots as "no SIM"]: PN_SIM
0x09 `SIM_STATUS` -> OK (`SIM_SERV_OK` 0x01 instead of 0x06), `SIM_IND` state OK, `READ_IMSI` 0x1D
-> IMSI BCD (15 digits), ICCID read, service table (`SIM_SERV_PROV_READ`-style requests) with data,
language/SPN/MSISDN reads, PIN status (PN_SECURITY 0x08: PIN disabled/verified), phonebook
(`SIM_PB` read -> empty), SMS storage read -> empty, PN_NETWORK 0x0A: `NET_REG_STATUS_GET` ->
home (1), `NET_REG_STATUS_IND` the same, `NET_RSSI_IND` and `NET_OPER_NAME_READ` for the
operator name, `NET_RAT` 3G; Symbian^3 `PN_MODEM_NETWORK` 0xC8 equivalents. Risk: the no-SIM
answers today are what keep dozens of SIM-dependent servers quiet (contacts SIM store, SMS centre,
cell broadcast, operator logo, SIM ATK); each flips to "ask more" with a SIM present. Expect 1-2
weeks of trace-and-answer across phones, on top of GPDS.

Whether apps use it: a packet-data IAP (the firmware's default "Internet" access point, operator
settings) is what 9.x apps prefer by default; on Symbian^3 the "Internet" destination lists it.
Nothing new in CommsDat is needed [I].

## 4. Shortcut: Ethernet NIF on a Syborg network device

Inventory [V]:
- EPL kernel LDD `enet.ldd` (`kernel/eka/drivers/ethernet/d_ethernet.cpp`, LDD name "Ethernet",
  `RBusDevEthernet` in `d32ethernet.h`: requests Read 0 / Write 1 (+cancels), controls Config,
  SetConfig, SetMac, Caps; `TEthernetConfigV01` with `iEthAddress[6]`).
- EPL Syborg PDD `adapt/qemu/baseport/syborg/ethernet/pdd` (`ethernet.pdd`, name "Ethernet.Syborg",
  `EthernetDevice : DEthernet` with Start/Stop/Send/ReceiveFrame/MacConfigure/DisableIrqs/RestoreIrqs,
  IRQ `EIntNet0`, legacy Syborg virtio at 0xc600c000 (phys 0xc000c000) with the word-index register
  layout ID/DEVTYPE/HOST_FEATURES/GUEST_FEATURES/QUEUE_BASE/QUEUE_NUM/QUEUE_SEL/QUEUE_NOTIFY/STATUS/
  INT_ENABLE/INT_STATUS, MAC at +0x100, vring descriptors, RX queue 0 / TX queue 1, `virtio_net_hdr`).
  The device tree node `net@0 compatible = "syborg,virtio-net", reg 0xc000c000, irq 9` is in
  `syborg.dts`; our board does not model it ("virtio ... not modelled, read as zero", `syborg.c`).
- Firmware: `EthInt.nif` in the N8, 701, 808 cores; absent on N95, 5800, N73 (they have
  `genericnif`, `ppp.nif`, `rawip.nif`, `spud.nif`, `tunnelnif.nif`, `WlanNif.nif`). `EtherPkt.drv`
  (the packet driver ethint loads by the LANBearer record) is in no firmware; `NullAgt.agt`,
  `DhcpServ.exe`, `NetCfgExtnDhcp`, `commsdat.dll`, `ipproto`/`iptransport` (Symbian^3) are present
  everywhere.
- The `symbian-qemu-0.9.1-12` tree in adapt/qemu has the original QEMU `net.c` (old slirp) only as a
  reference.

QEMU device, two options:
(a) Model the legacy Syborg virtio-net as the EPL PDD expects (pre-1.0 virtio with Syborg's own
    register map: not QEMU 11's `virtio-mmio`, whose offsets and QueuePFN semantics differ), reusing
    the PDD unchanged. Needs vring parsing in our device (QEMU's `VirtQueue` cannot be bolted to a
    foreign transport cheaply): about 600 lines.
(b) A mailbox NIC in the `syborg_modem.c` shape and a 300-line PDD of ours derived from the EPL
    one (keep the `DEthernet` glue, replace the ring code): registers at a free slot (0xc000c000,
    irq 9 as the dts says): `ID`, `TX_LEN` (w), `TX_GO` (w: sends the TX window as one frame),
    `RX_COUNT` (r), `RX_LEN` (r), `RX_POP` (w), `INT_ENABLE`, `MAC_LO/MAC_HI` (r: from `-nic`
    `mac=`), windows TX 0x1000..0x17ff and RX 0x1800..0x1fff (frames up to 1514+4 bytes), RX FIFO
    of 32 frames in the device, drop when full. QEMU side: `qemu_new_nic()` with a `receive`
    callback queuing frames and raising the IRQ, `qemu_send_packet()` on TX_GO; `-netdev
    user,id=n0,hostfwd=...` and `-device syborg-net,netdev=n0` through `tools/device.py qemu-args`
    (`[hardware] ethernet = true` or a `[network] backend = "user"` key). Snapshot-friendly
    (vmstate of the FIFO), no DMA, no rings: recommended.

Guest side:
- Stand-ins `enet` (EPL LDD, built as-is: it compiles against our kernel) and `syborg-ethernet`
  (our PDD) in `[[standins]]`; add to the ROM obey as `enet.ldd` / `ethernet.pdd`.
- `EtherPkt.drv`: build from EPL networkingsrv (`linklayerprotocols/ethernetnif` or the `ethernet`
  packet driver component; small, [I] as to size and 9.x ABI needs) with the S60 SDK of the phone;
  one binary per OS generation (9.2 for the N95 line, Symbian^3 for N8+).
- `EthInt.nif` for 9.x phones: the same source tree; 9.2 builds need the 9.2 `nifman` headers (S60
  3rd Ed FP1 SDK has them) [I]. Alternative for 9.x: skip Ethernet and use cellular there.
- CommsDat (`cccccc00.cre` on C:, or a boot-time tool run through `sydb app_run` that uses the
  CommsDat API) [I, standard Symbian emulator records]: `LANBearer` {Agent "nullagt.agt", IfName
  "ethint", LDDFilename "enet", LDDName "Ethernet", PDDFilename "ethernet", PDDName
  "Ethernet.Syborg", PacketDriverName "EtherPkt.drv"}, `LANService` {IpAddrFromServer 1, IpDNSAddrFromServer
  1, ConfigDaemonManagerName "NetCfgExtnDhcp", ConfigDaemonName "!DhcpServ"}, `IAP` {IAPServiceType
  "LANService", IAPBearerType "LANBearer", Network, Location}, `ConnectionPreferences` ranking 1 (9.x)
  and, on S60 3.2+/Symbian^3, membership of the "Internet" destination (`cmmanager` SNAP records,
  `DN_IAP` + `DestinationNetwork`) so that "connect to Internet" picks it without a prompt. A `wlan`
  bearer type is not needed; the IAP shows as a LAN access point in the connection dialog
  (apps that enumerate only WLAN/GPRS bearers, e.g. some games' bearer filters, may hide it [I]).
- Symbian^3 note: `EthInt.nif` there is the 399-architecture flow; the legacy `LANService`/`IAP`
  records still drive it through `ipproto`/`iptransport` (no `ethmcpr` in the ROM [V]) [I: to verify
  with the first connect; if ethint on Symbian^3 needs its own MCPR, the Belle phones fall back to
  cellular as well].

WINS emulator comparison: this is exactly the emulator's `ethernet.wins` + `EthInt.nif` +
`EtherPkt.drv` + LAN IAP set-up (`cccccc00.cfg`), ported to a Syborg device [I]. USB networking
(RNDIS/PC Suite) needs the USB stack that dies today and a PC-side driver: not a shortcut.

## 5. Order, effort, risks

| Step | Effort | Main risk |
|---|---|---|
| Syborg mailbox NIC (QEMU) + PDD + `enet.ldd` as stand-ins | 2-3 days | none technical; `tools/device.py qemu-args` plumbing |
| `EtherPkt.drv` build (+ `EthInt.nif` for 9.x) | 2-4 days per OS generation | SDK/toolchain fit; EPL source outside the tree must be fetched (EPL licence, fine) |
| CommsDat LAN IAP + destination membership; first browser page over slirp | 2-3 days | Symbian^3 ethint needing its own MCPR; DHCP against slirp (works for Linux guests, unverified here) |
| Bluetooth: bc02 stand-in + host controller (H4/H4+ first: N8/701/808) | 1 week + 2-3 days for BCSP on 9.x | framing/power handshake guessed wrong; initialisor leaving on an unexpected chip version; FM vendor commands |
| Cellular: GPDS + PIPE in the host modem + slirp bridge | 2 weeks | the pipe data encapsulation and DATAPORT segmentation are unread |
| SIM-present model | 1-2 weeks, every phone | regressions in start-up (SIM-dependent servers wake up); keep "no SIM" as the default profile until each phone is traced |
| WLAN stand-in | 4-8 weeks | proprietary UMAC/LMAC interfaces; may never associate |

Not done in this study (budget): decompressing the ROFS binaries (`Hci.dll`, `WlanNif.nif`,
`rawip.nif`) to read their import and string tables, the LDD control numbers of `BC02.LDD` (a
trace build gives them in an hour), and the Symbian^3 ethint flow/MCPR dependency.

## 6. USB (device side): what it is, what it would take, whether it is worth it

Follow-up study, same marks. Short answer: the owner's backup/restore, file-transfer and install
use cases are already covered without USB (`sydb drive get/put/ls`, `sydb snap save/restore/fork`,
`sydb sis install`, the `drives/*.img` of a session are byte-exact copies of C:/E:). A USB stand-in
buys authenticity (USB mode query, "Connected", charging state) and real host tools (libmtp,
obexftp, a mounted E:), at 3-4 weeks and a root-only host end. Recommended: not before Ethernet and
Bluetooth; if done, Mass Storage/MTP over a usbip export, never PC Suite.

### 6.1 The firmware's USB stack per generation [V file lists, strings; roles I where noted]

| Layer | 9.1/9.2 (N73, N95) | 9.3/9.4 (5730, 5800) | Symbian^3/Belle (N8, 701, 808) |
|---|---|---|---|
| Controller PSL (kernel extension) | `usbcc.dll`: Nokia OMAP2420 client controller, `DUsbClientController` subclass (`TOmapUsbcc`, `OmapUsbcAssp24xx`, `OmapUsbcVariantH4`, `OmapUsbOtgAssp`, `OmapUsbEpZero*`, `OmapUsbEndpoint*`, DMA helpers, `DUsbPowerHandler`) | same shape, OMAP2430/3430 [I] | same shape, OMAP3630 + OTG |
| Client LDD | `eusbc.ldd` (Symbian generic `DLddUsbcC`, `RDevUsbcClient`, d32usbc.h) | `eusbc.ldd` | `eusbc.ldd`, `eusbcsc.ldd` (shared-chunk variant, 701) + `usbcsc_bil.dll`; host/OTG: `otgdi.ldd`, `usbdi.ldd`, `usbhubdriver.ldd`, `usbhost*.dll`, `usbhostmssrv.exe` (OTG mass-storage host), `usbhidclassdriver.exe`; `usbaudiodriver.ldd` (USB audio device class) |
| Nokia link LDD | `USBISA.LDD`: "USB Phonet Driver" (`DUsbPNLDD`, `DUsbPNChannel`, `DUsbPNIsaUsbEndpoint`/`DUsbPNUsbIsaEndpoint`, `DUsbPNDataInterface`, source `casw_adaptation\usb_phonet_link`): bridges ISI (ISA) to USB endpoints = the PC Suite "Phonet over USB" interface Nokia's PC software uses to talk to the modem/phone [I role] | `usbpnserver.exe`/`usbpnclient.dll` + the same LDD [I] | same + `usbostrouter.exe`/`usbostcomm.dll` (OST trace over USB) |
| usbman | `usbsvr.exe` (`!usbman`), `usbman.dll`, `usbclasscontroller.dll`, `usbext.dll`, `UsbWatcher.exe` (cable/personality watcher), `usbclasschangeui.exe` + plugin (the "USB mode" query), `Usblcifclient`/`UsbLCIF.EXE`, `CfmwUsbConfServer.exe` | + personality framework: `UsbPersonality.dll`, `usbbasicpersonality`, `usbmscpersonality`, `usbremotepersonality` (PC Suite), `usbdevcon.exe`, `usbosdescriptorhandler` (MS OS descriptors), `usblocodplugin`, `usbmanextensionplugin` | + `usbdescriptors.dll`, `usb_charging_service`/`usb_charging_adaptation`, `usbwatcher.dll` |
| Class controllers | `acmclasscontroller` + `acmserver` + `Ecacm.csy` (CDC ACM: PC Suite serial/AT), `usbobexclasscontroller` + `obexusbtransportcontroller` + `SrcsUSB` (OBEX over USB), `usbpnclasscontroller` (Phonet), `mtpclasscontroller` + `mtpsrv.exe` + `wmdrmmtp` (Media Transfer, `MsMtpStub.SIS`), mass storage (`usbms.rsc`; `msfs.fsy` not in the grep pattern [I present]) | + `ptpclasscontroller`/`ptpserver` (PictBridge/image transfer), `dunusb.dll` (dial-up), `obexserviceman` | + `mtpserver.exe` with data providers (`mtpfiledp`, `mtpimagedp`, `mtpdevicedp`, `mtpproxydp`...), `mtpusbsiccc`, `rndisclasscontroller` + `rndisagt.agt` + `rndisproviders` (N8: USB tethering as an RNDIS network, firmware side only), `usbmscfileclasscontroller` + `usbmscfileserver.exe` (mass storage) |
| PC Suite servers | `SConPCConnServer.exe` + `sconpcconnplugin` (Symbian Connect PC-connectivity server over OBEX: backup/restore via the Secure Backup Engine, file browsing, install), SyncML DS/DM stacks (`nsml*`) | same | `sconpcconnclient`/`sconpcconnplugin` |

Only `usbman.rsc`, `usbms.rsc`, `usbclasschangeui_reg.rsc` were found as USB configuration files on
the N95; personality definitions live in resource/CenRep, not ini files [V names, I location].

### 6.2 Why usbsvr and UsbWatcher die today

Serial log of the N95 session [V]: `UsbWatcher.exe::UsbWatcher Killed -1` at 12.0, 12.2, 12.3 s
(Starter's three restarts), `usbsvr.exe::!usbman Killed -1` at 13.1 s; no panic. Our ROM carries
our kernel side: Nokia's `usbcc.dll` extension (the controller PSL) is not loaded, and on the
N8/701/808 `eusbc.ldd` and the OTG/host LDDs are in `missing_drivers` [V]. Without a PSL the generic
LDD cannot find a controller (`DUsbClientController::UsbcControllerPointer()` is NULL), so
`RDevUsbcClient::Open`/`User::LoadLogicalDevice("eusbc")` fail with -1 and usbman gives up; on
Symbian^3 usbman also opens the OTG driver first [I]. The oracle shows `usbsvr` running on the real
phone, so these are counted as qembian deaths to fix [V `docs/real-phone-tracing.md`].

### 6.3 What a stand-in needs

Guest (kernel side, all buildable from EPL in our tree):
- `eusbc.ldd` from `kernel/eka/drivers/usbc` (PIL `usbcc_lib` from `drivers/usbcc`: chapter 9,
  descriptors, request queues are generic) [V source present]; later `eusbcsc.ldd` for the 701's
  shared-chunk clients.
- A PSL kernel extension `src/standins/usbcc/` deriving `DUsbClientController` and implementing its
  pure virtuals [V list from `include/drivers/usbcshared.h`]: SetDeviceAddress, Configure/
  DeConfigureEndpoint, Allocate/DeAllocate/QueryEndpointResource, SetupEndpointRead/Write +
  cancels, SetupEndpointZeroRead/Write, SendEp0ZeroByteStatusPacket, Stall/ClearStall/
  EndpointStallStatus/EndpointErrorStatus, ResetDataToggle, SynchFrameNumber, StartUdc/StopUdc,
  UdcConnect/UdcDisconnect, UsbConnectionStatus/UsbPowerStatus/DeviceSelfPowered,
  DeviceEndpointCaps/DeviceTotalEndpoints, SoftConnectCaps/DeviceStateChangeCaps; plus the
  PIL callbacks on events (Ep0 setup packet received, data in/out complete, bus reset/suspend/
  resume, VBUS). About 1200 lines against a mailbox device; the endpoint table (ep0 + 5 bulk/
  interrupt in/out pairs, 64/512-byte packets) is what the Nokia controller offered [I].
- `USBISA.LDD` stand-in only if the PC Suite personality is wanted (ISI to the PC): skip; choose
  personalities without the Phonet class (Mass Storage, Media Transfer, PictBridge).
- Symbian^3: stub `otgdi.ldd` returning "no OTG, B-device, VBUS present" so usbman starts [I].

QEMU device `syborg_usbc.c` (sysbus, mailbox shape of `syborg_modem.c`): registers for device
state (CONNECT, ADDRESS, SPEED, VBUS), per-endpoint TX/RX windows with length + GO/POP, a setup-
packet FIFO, interrupt status bits (SETUP, EPn_DONE, RESET, SUSPEND, RESUME, VBUS). The device
is a gadget; the host end is where the cost is:
1. **usbip export** (recommended if USB is done): the QEMU device, or a sidecar process on a
   socket, speaks the usbip protocol (`OP_REQ_DEVLIST`/`OP_REQ_IMPORT`, `USBIP_CMD_SUBMIT` ->
   `USBIP_RET_SUBMIT`, `USBIP_CMD_UNLINK`); the Linux host attaches it with `vhci_hcd` (`modprobe
   vhci-hcd`, `usbip attach -r 127.0.0.1 -b 1-1`, root once). The host then sees a real USB device
   (VID 0x0421 Nokia) and stock tools work: `libmtp`/gvfs-mtp for Media Transfer (N8+), the kernel
   `usb-storage` for Mass Storage (E: mounted; the phone dismounts E: meanwhile, as on hardware,
   and `sydb drive` must not touch the image then), `obexftp -u` for the OBEX interface. 300-500
   lines; needs root/modules on the host [I protocol detail from memory, V that QEMU 11 has no
   built-in "gadget to host" export: usb-redir/usb-host/usbip in QEMU all import host devices].
2. Host-side class terminators inside QEMU (no root): Mass Storage -> write the SCSI image to a
   file (pointless: the drive images exist), OBEX client -> `sydb usb get/put` (redundant with
   `sydb drive`/agent), MTP initiator (1-2 weeks, redundant). Only "USB connected + charging"
   feedback has value here, and that needs just the device-state registers (0.5 day on top of the
   PSL).
3. Windows-only ends (Nokia PC Suite / Ovi Suite): need Nokia's Windows cable driver; Wine has no
   USB stack, so only a Windows VM with the usbip device attached: not worth it. `.nbu/.nbf` are
   PC Suite's own containers (NbuExplorer reads them); the phone-side data behind them is the
   Secure Backup Engine (`sbengine`/`SBEClient`) reached through `SConPCConnServer` over OBEX.
   A guest-side `sbeclient` call from `devagent` would give the same backup without USB [I].
   gnokii/gammu drive S60 phones through AT over ACM (handled by the modem: our fake modem has no
   AT parser) or `gnapplet` over Bluetooth: neither fits.

### 6.4 Cheapest useful target, effort, risks

- Already covered: backup/restore, transfer, install = sydb drive images, snapshots, `sis install`
  [V `docs/sydb.md`]. No USB work is needed for these.
- Cheapest real USB: PSL + `eusbc.ldd` + QEMU gadget with state registers (usbman starts, the
  USB mode query and "charging" appear, two oracle deaths gone): 1.5-2 weeks. Then usbip export:
  +1 week; Mass Storage first (no class code on our side, stock `usb-storage` on the host), MTP
  on N8+ via libmtp, OBEX via obexftp.
- Risks: PIL/PSL callback ordering (Ep0 setup -> data -> status, `Ep0ReadSetupPktProceed`,
  ZLPs, data toggles) is where real ports spend their time; personality CenRep entries that pull
  `usbpnclasscontroller` (needs `USBISA.LDD`) into the default mode, making usbman fail the
  personality; Symbian^3 usbman's OTG dependency; the usbip end needs root and a kernel module;
  a mounted E: and the session's `drives/` image fighting over the same bytes.
