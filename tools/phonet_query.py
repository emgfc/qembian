#!/usr/bin/env python3
"""Ask a real Nokia phone's modem the ISI questions qembian's fake modem answers, over USB phonet.

The phone in PC Suite mode exposes a phonet link (Linux: modules cdc_phonet + phonet, interface usbpnN).
This script sends ISI requests as phonet datagrams and prints the replies in hex, so the fake modem's
reply table (platform/isi, models/<id>/isi-replies.txt, syborg_modem.c) can be checked against the real one.

  sudo modprobe phonet cdc_phonet; sudo ip link set usbpn0 up       (interface name: ip link)
  (the script gives usbpn0 our phonet address, 0x10 by default; run it from the repo root)
  sudo python3 tools/phonet_query.py [--dev 0x00] [--iface usbpn0]   default: the safe queries below
  sudo python3 tools/phonet_query.py --raw 15 02 00 00              any request: RESOURCE MSGID DATA...
  sudo python3 tools/phonet_query.py --mtc-state 0a                 MTC_STATE_REQ {0A 00}: CHANGES the modem
                                                                     state (asked by N8 Belle at start-up)

Default queries (read-only):
  PN_SIM 0x09  SIM_STATUS_REQ 0xC0 {00 00}  and {01 00}   (N8 Belle SAServer asks both at the security check)
  PN_MTC 0x15  MTC_STATE_QUERY_REQ 0x02 {00 00}            (current modem state)
Output: one line per request and per reply ("res msgid data"), also written to
build/evidence/phonet/query-<time>.txt (raw modem data: keep out of git).
Written from the Linux phonet socket API (linux/phonet.h); no third-party code.
"""
import argparse, ctypes, ctypes.util, os, select, socket, struct, sys, time

AF_PHONET = 35
PN_PROTO_PHONET = 1           # datagram (ISI) protocol of AF_PHONET
SIOCPNADDRESOURCE = 0x89E0    # not needed for client use

libc = ctypes.CDLL(ctypes.util.find_library('c'), use_errno=True)


def sockaddr_pn(obj=0, dev=0, res=0):
    # struct sockaddr_pn { sa_family_t spn_family; u8 spn_obj; u8 spn_dev; u8 spn_resource; u8 zero[11]; }
    return struct.pack('<HBBB11x', AF_PHONET, obj, dev, res)


def set_address(iface, addr):
    """give the interface our phonet device address (RTM_NEWADDR, family AF_PHONET, IFA_LOCAL = 1 byte)"""
    idx = socket.if_nametoindex(iface)
    RTM_NEWADDR, IFA_LOCAL = 20, 2
    NLM_F_REQUEST, NLM_F_ACK, NLM_F_EXCL, NLM_F_CREATE = 1, 4, 0x200, 0x400
    rta = struct.pack('<HHB3x', 5, IFA_LOCAL, addr)                 # rtattr len 5, padded to 8
    ifa = struct.pack('<BBBBI', AF_PHONET, 0, 0, 0, idx)
    body = ifa + rta
    msg = struct.pack('<IHHII', 16 + len(body), RTM_NEWADDR, NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL, 1, 0) + body
    nl = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, socket.NETLINK_ROUTE)
    nl.send(msg)
    r = nl.recv(4096)
    nl.close()
    err = struct.unpack_from('<i', r, 16)[0] if len(r) >= 20 else 0
    if err not in (0, -17):                                          # -17 EEXIST: already set
        raise SystemExit('setting phonet address 0x%02x on %s: %s' % (addr, iface, os.strerror(-err)))
    print('phonet address 0x%02x on %s%s' % (addr, iface, ' (already set)' if err == -17 else ''))


def open_socket(iface):
    fd = libc.socket(AF_PHONET, socket.SOCK_DGRAM, PN_PROTO_PHONET)
    if fd < 0:
        raise SystemExit('socket(AF_PHONET): %s (modprobe phonet cdc_phonet; run as root)' % os.strerror(ctypes.get_errno()))
    if iface:
        b = iface.encode() + b'\0'
        SO_BINDTODEVICE = 25
        if libc.setsockopt(fd, socket.SOL_SOCKET, SO_BINDTODEVICE, b, len(b)) < 0:
            raise SystemExit('SO_BINDTODEVICE %s: %s' % (iface, os.strerror(ctypes.get_errno())))
    a = sockaddr_pn()
    if libc.bind(fd, a, len(a)) < 0:
        raise SystemExit('bind: %s' % os.strerror(ctypes.get_errno()))
    return fd


def send(fd, dev, res, payload):
    a = sockaddr_pn(0, dev, res)
    n = libc.sendto(fd, payload, len(payload), 0, a, len(a))
    if n < 0:
        raise SystemExit('sendto: %s' % os.strerror(ctypes.get_errno()))


def recv_all(fd, secs):
    out = []
    end = time.time() + secs
    buf = ctypes.create_string_buffer(4096)
    src = ctypes.create_string_buffer(16)
    slen = ctypes.c_uint(16)
    while True:
        left = end - time.time()
        if left <= 0:
            break
        r, _, _ = select.select([fd], [], [], left)
        if not r:
            break
        slen.value = 16
        n = libc.recvfrom(fd, buf, len(buf), 0, src, ctypes.byref(slen))
        if n < 0:
            break
        fam, obj, dev, res = struct.unpack_from('<HBBB', src.raw, 0)
        out.append((dev, res, buf.raw[:n]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dev', type=lambda x: int(x, 0), default=0x00, help='phonet device of the modem (default 0x00)')
    ap.add_argument('--iface', default=None, help='phonet interface, e.g. usbpn0')
    ap.add_argument('--raw', nargs='+', help='RESOURCE MSGID DATA... (hex bytes)')
    ap.add_argument('--mtc-state', help='send MTC_STATE_REQ {STATE 00} (changes the modem state)')
    ap.add_argument('--wait', type=float, default=2.0, help='seconds to collect replies per request')
    ap.add_argument('--addr', type=lambda x: int(x, 0), default=0x10,
                    help='our phonet device address, set on --iface first (default 0x10, the PC)')
    a = ap.parse_args()
    reqs = []
    if a.raw:
        b = [int(x, 16) for x in a.raw]
        reqs.append((b[0], b[1], bytes(b[2:])))
    elif a.mtc_state:
        reqs.append((0x15, 0x01, bytes([int(a.mtc_state, 16), 0])))
    else:
        reqs = [(0x09, 0xC0, b'\x00\x00'), (0x09, 0xC0, b'\x01\x00'), (0x15, 0x02, b'\x00\x00')]
    if a.iface:
        set_address(a.iface, a.addr)
    fd = open_socket(a.iface)
    os.makedirs('build/evidence/phonet', exist_ok=True)
    log = open(time.strftime('build/evidence/phonet/query-%Y%m%d-%H%M%S.txt'), 'w')
    for tid, (res, msg, data) in enumerate(reqs, 1):
        payload = bytes([tid, msg]) + data       # ISI after the phonet header: transaction id, message id, data
        line = 'req  dev %02x res %02x tid %02x msg %02x  %s' % (a.dev, res, tid, msg, data.hex(' '))
        print(line); log.write(line + '\n')
        send(fd, a.dev, res, payload)
        for dev, rres, p in recv_all(fd, a.wait):
            line = 'resp dev %02x res %02x tid %02x msg %02x  %s' % (dev, rres, p[0] if p else 0, p[1] if len(p) > 1 else 0, p[2:].hex(' '))
            print(line); log.write(line + '\n')
    log.close()


if __name__ == '__main__':
    sys.exit(main())
