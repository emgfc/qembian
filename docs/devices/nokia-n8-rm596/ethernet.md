# Ethernet on the N8 (syborg-net): status 2026-11-11

Works [V]: QEMU syborg-net (src/qemu-syborg/hw/arm/syborg_net.c) on `-nic user` (slirp) -> enet.ldd (EPL) +
sybeth.pdd (ours) -> the firmware's etherpkt.drv / ether802.dll / EthInt.nif -> DHCP (NetCfgExtnDhcp) -> TCP/IP.
`netap.exe create` writes IAP "Ethernet (host)" (LANBearer ethint/EtherPkt.drv/enet/sybeth/nullagt.agt,
LANService with DHCP); `netap.exe test` on N8 Delight: start 0, example.com resolved, HTTP/1.1 200 OK.
Tools: src/n95apps/netap (`create | list | test [IAP] [HOST] | allow [0|1] | cr UID KEY [VALUE]`), installed
with `sydb push build/n95apps/netap.exe 'c:\sys\bin\netap.exe'`; netallow.exe = the same with SysAp's SID.

Blocked: S60 applications do not use a LAN IAP [V: trace + SF source]:
- No SIM -> the phone stays in the Offline profile ("Insert SIM card"); SysAp keeps CenRep 0x101F876C key 1
  (KCoreAppUIsNetworkConnectionAllowed) at 0, writable only by SysAp's SID 0x100058F3 (netallow.exe sets it).
- The browser's connection manager (SF mw.web browserutilities/connectionmanager InternetConnectionManager.cpp)
  restricts the bearer set: connection not allowed -> WLAN only; a non-GPRS IAP as first preference -> CSD
  ("temp fix for CDMA"); only an IAP of bearer type "all bearers" gets LAN. MPM then finds no IAP: ESock
  RConnection::Start (fn 74) = -1. The browser was switched to "user defined" IAP 2 (CenRep 0x10008D39
  0x84 = 0, 0x0E = 2) for the test; still -1 (bearer set).
- No CMManager LAN plugin in the firmware: destinations and settings UIs cannot show the IAP.

So for applications the transport must look like packet data (needs a SIM-present model + PN_GPDS/PIPE in
the fake modem, research section 3) or like WLAN (offline-capable; research section 2: large). The Ethernet
path stays useful for our own tools, native test programs and as the IP bridge either of those can reuse.
