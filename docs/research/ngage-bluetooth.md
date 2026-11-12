# N-Gage QD Bluetooth: the LPRF module behind btldd.ldd

The board's Bluetooth module (`src/qemu-ngage/hw/arm/ngage_lprf.c`, glue in `ngage.c` and
`ngage_keys.c`) is a minimal HCI controller: a Bluetooth 1.1 module that is present and powered,
so the phone's own stack starts, and whose radio finds nobody. Whether Bluetooth is "on" for the
user stays the phone's own setting (BtUi). Host test: `tests/ngage_lprf_test.c`.
Marks: [V] read in the code or seen live, [I] inferred.

## Why

Before the model, btldd's writes all timed out (`BTLDD: CompleteWithError: 1 -33`,
`[HCI] CHCTLCBusSender ERROR -33`). bt.prt then answered RHostResolver::GetHostName with -44
(KErrHardwareNotAvailable), and Snakes (6R45_1) wrote through a null object in its BTGetName
error path (build/ngage/snakes-research.md).

## Hardware path [V]

| Piece | What |
|---|---|
| Module | LPRF ("low-power RF", Nokia's BT module) = CBus device 5 (CBus master 0x0C150420) |
| Registers | btldd's logical registers 37-44 = device 5 hardware registers 0, 1, 6, 8, 9, 10, 11, 16 (cbus.dll register table 0x5037e544: entries of {device, type, hw register, ?, bits}) |
| Interrupt | CTSI status 2 (0x0C150102) bit 4; cbus.dll ISR 0x5037d5bc calls btldd's ISR (0x503f0818), which writes 0x10 back to ack. Source 25 (IrqCTSI_IRQ) [I for the source; it works] |
| Other | btldd writes 0x10 to 0x0C150014 / 0x0C150016 around wake-ups (an enable mask [I]; not modelled), reads 0x0C15020C bit 6 only on the sleep path (unused) |

btldd reaches the module through the cbus.dll object's virtuals: +0x10 read(dev 5, reg, 16 bits),
+0x14 write(dev 5, reg, 16 bits, value) (cbus.dll 0x5037ce84 / 0x5037d094).

## Register protocol (btldd.ldd 0x503EFFD0)

| hw reg | btldd | Read | Write |
|---|---|---|---|
| 0 | 37 | next 2 RX bytes (first byte in bits 15-8) | next 2 TX bytes (same order; an odd last byte alone in 15-8) |
| 1 | 38 | length of the RX packet waiting | |
| 6 | 39 | interrupt status (read clears [I]) | control: 0 then 1 at channel open; 0 then 2 = wake-up request |
| 8 | 40 | acknowledge: bits 15-14 type, 13-0 length | 0x8000\|len start of a TX packet, 0xC000\|len its end |
| 9 | 41 | reset reason: bit 3 = power-up (else "Reset reason" and -36) | |
| 10 | 42 | | 0x8000: buffer request |

Interrupt status bits (btldd's interrupt DFC 0x503f0854):

| Bit | Meaning | Driver action |
|---|---|---|
| 0x0080 | reset indication | reads reg 9; bit 3 -> state 1 (start) [V] |
| 0x0040 | acknowledge in reg 8 | type 0: buffer size (>= 520 = never ask again, 0x503f105c); type 0x4000 with the written length = data accepted (state 8 -> 3, write request completes) [V] |
| 0x0010 | RX packet | reads reg 1, then reg 0 words up to 512 bytes per pass [V] |
| 0x0004 | awake | wake-up paths [V]; the model answers control 2 with it [I] |
| 0x0020, 0x0100 | fall asleep / wake-up GPIO path | never raised: the module doesn't sleep |

Driver state machine (byte at channel+0x1544): 0 wait for the reset indication (1.5 s timer ->
-33), 1 send the buffer request, 2 wait for its acknowledge, 3 idle, 4 wait awake, 5 start a write
(reg 8 = 0x8000|len), 6 wait for a buffer acknowledge (skipped when the module said >= 520),
7 data words, then reg 8 = 0xC000|len, 8 wait for the data acknowledge. One write and one read
request at a time; a 4096-byte RX ring. btldd's factory allows **one channel** (Create leaves -11
when one is open, 0x503f1edc) [V].

## HCI framing (hci.dll) [V]

H4 with the packet type padded to 16 bits: `type, 0, HCI packet` (01 command, 02 ACL, 04 event).
HCI_Reset is written as `01 00 03 0C 00`. hci.dll reads exact byte counts (2: type; event: 2 =
code + length, then the parameters; ACL: 4, then the data), so a module packet is never padded.
Details, command builders and the ioctl table: build/ngage/bt-hci-research.md (local).

## The controller

Standard BT 1.1 replies: Command Complete for everything, Command Status + Inquiry Complete (no
devices) for Inquiry, Command Status + Connection Complete "page timeout" for Create Connection,
a failed Remote Name Request. BD_ADDR 00:02:EE:12:34:56 (Nokia OUI) [I], name "N-Gage" [I],
manufacturer 1 [I]; nothing in hci.dll or bt.prt compares these [V, research agent]. Unknown
commands get status 0x01 and a log line (`ngage-bt:` in qemu.log), as does every command.

## Who owns the channel at boot [V]

btldd's factory allows one channel. SelfTestServer (Starter's normal list, entry 2) always connects
to BTTestServer (BTClient.dll, which launches it); BTTestServer's ConstructL loads hci.dll and opens
btldd at ~5 s. It frees the channel only when it exits, 1 ms after its last session closes, and
SelfTestServer closes that session only in its destructor. SelfTestServer's only exit is its ISI
handler: resource 0x59 (PN_EPOC_TEST), message 1, sub-id (byte 10) 8 (0x5036c79c: reply, then
CActiveScheduler::Stop). The board sends that message when SelfTestServer opens NOSDRIVER unit 13
(H-ngage-selftest-stop). ESock retries its HCI open on every Bluetooth socket open, so the stack
starts at the first open after that (BTServer, ~54 s): Read_Buffer_Size, Read_BD_ADDR,
the timeouts, Write_Scan_Enable 3, Read_Local_Features, Write_Local_Name, Read_Local_Version
(BTServer's ioctl 22), Read_Local_Name, Write_Current_IAC_LAP, then Write_Scan_Enable 0: the
phone's own "Bluetooth off" setting.

Result (2026-11-12): no BTLDD/HCI errors, emon prints "BT connection initialized", Snakes gets its
local name and runs (menu, single-player level).

## Open

- What really ends the boot test session on a phone (H-ngage-selftest-stop is a guess at the
  mechanism, not at the code path, which is verified).
- BTTestServer's 0x0C16(0xFFFF) now gets status 0x12 as from a real controller; BTTestServer
  ignores it.
- Multiplayer needs a radio: two boards, or the host's Bluetooth.
