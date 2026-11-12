#!/usr/bin/env python3
"""Host-only regression tests for the fake modem's packet and SMS helpers (no QEMU, no phone).

- The TCP MSS clamp of syborg_modem.c (data_clamp_mss and its checksum helpers, compiled from the source file as they
  are, with data_mtu replaced by a constant): MSS at an even and an odd option offset, checksum checked against an
  independent full recompute (review 2026-11-12 N4).
- tools/sms_provision.py: TPDU sizes, UDH port addressing and concatenation, payload reassembly, fresh references.

  python3 tests/modem_unit_test.py
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import sms_provision  # noqa: E402

MODEM = os.path.join(ROOT, 'src/qemu-syborg/hw/arm/syborg_modem.c')


def extract(src, name):
    """the text of one top-level C function (static ...name(...) { ... })"""
    m = re.search(r'^static [^\n;]*\b%s\(' % name, src, re.M)
    if not m:
        raise AssertionError('function %s not found' % name)
    i = src.index('{', m.end())
    depth = 0
    for j in range(i, len(src)):
        depth += {'{': 1, '}': -1}.get(src[j], 0)
        if depth == 0:
            return src[m.start():j + 1]
    raise AssertionError('unbalanced %s' % name)


def csum(data):
    if len(data) % 2:
        data += b'\0'
    s = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return s


def tcp_ok(pkt):
    ihl = (pkt[0] & 15) * 4
    tcp = pkt[ihl:]
    pseudo = pkt[12:20] + struct.pack('!BBH', 0, 6, len(tcp))
    return csum(pseudo + tcp) == 0xffff


def make_syn(options):
    tcp = bytearray(struct.pack('!HHIIBBHHH', 0x1234, 80, 1, 0, 0, 0x02, 0xffff, 0, 0)) + options
    while len(tcp) % 4:
        tcp += b'\x01'
    tcp[12] = (len(tcp) // 4) << 4
    ip = bytearray(struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(tcp), 0, 0x4000, 64, 6, 0,
                               bytes([10, 0, 2, 15]), bytes([54, 172, 45, 38])))
    pkt = ip + tcp
    pseudo = pkt[12:20] + struct.pack('!BBH', 0, 6, len(tcp))
    c = 0xffff - csum(pseudo + bytes(tcp))
    pkt[20 + 16:20 + 18] = struct.pack('!H', c)
    assert tcp_ok(pkt)
    return pkt


class MssClamp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = open(MODEM).read()
        body = '\n'.join(extract(src, f) for f in ('csum_add', 'csum_fold', 'tcp_checksum', 'data_clamp_mss'))
        body = body.replace('SyborgModemState *s, ', '').replace('data_mtu(s)', '1400')
        prog = '#include <stdint.h>\n#include <stdio.h>\n#include <stdbool.h>\n' + body + r'''
int main(void) {
    uint8_t d[128]; int n = 0, c;
    while (n < 128 && (c = getchar()) != EOF) d[n++] = c;
    data_clamp_mss(d, n);
    fwrite(d, 1, n, stdout);
    return 0;
}'''
        cls.tmp = tempfile.mkdtemp()
        cfile, cls.exe = os.path.join(cls.tmp, 'mss.c'), os.path.join(cls.tmp, 'mss')
        with open(cfile, 'w') as f:
            f.write(prog)
        subprocess.run(['gcc', '-O1', '-Wall', '-o', cls.exe, cfile], check=True)

    def run_clamp(self, pkt):
        return subprocess.run([self.exe], input=bytes(pkt), capture_output=True, check=True).stdout

    def mss_of(self, pkt):
        tcp = pkt[20:]
        o = 20
        while o < (tcp[12] >> 4) * 4:
            if tcp[o] == 1:
                o += 1
                continue
            if tcp[o] == 2:
                return struct.unpack('!H', tcp[o + 2:o + 4])[0]
            o += tcp[o + 1]
        return None

    def test_even_offset(self):
        out = self.run_clamp(make_syn(b'\x02\x04\x05\xb4'))
        self.assertEqual(self.mss_of(out), 1360)
        self.assertTrue(tcp_ok(out))

    def test_odd_offset(self):
        out = self.run_clamp(make_syn(b'\x01\x02\x04\x05\xb4'))      # NOP, MSS: the value at an odd offset
        self.assertEqual(self.mss_of(out), 1360)
        self.assertTrue(tcp_ok(out))

    def test_small_mss_untouched(self):
        pkt = make_syn(b'\x02\x04\x02\x00')
        self.assertEqual(self.run_clamp(pkt), bytes(pkt))

    def test_not_syn_untouched(self):
        pkt = make_syn(b'\x02\x04\x05\xb4')
        pkt[20 + 13] = 0x10                                            # ACK only (checksum now wrong: untouched)
        self.assertEqual(self.run_clamp(pkt), bytes(pkt))


class SmsProvision(unittest.TestCase):
    def parse(self, tpdu):
        self.assertLessEqual(len(tpdu), 164)
        self.assertEqual(tpdu[0] & 0x43, 0x40)                        # SMS-DELIVER with UDHI
        oalen = tpdu[1]
        o = 3 + (oalen + 1) // 2
        o += 2 + 7                                                     # PID, DCS, SCTS
        udl = tpdu[o]
        ud = tpdu[o + 1:]
        self.assertEqual(len(ud), udl)
        self.assertLessEqual(udl, 140)
        udhl = ud[0]
        udh, payload = ud[1:1 + udhl], ud[1 + udhl:]
        ies, i = {}, 0
        while i < len(udh):
            ies[udh[i]] = udh[i + 2:i + 2 + udh[i + 1]]
            i += 2 + udh[i + 1]
        self.assertEqual(ies[0x05][:2], b'\x0b\x84')                   # destination port 2948
        return ies.get(0x00), payload

    def message(self, pin):
        body = sms_provision.wsp_push(sms_provision.provisioning_doc('internet', 'Internet'), pin)
        return body, sms_provision.sms_deliver_tpdus(body, '12345')

    def test_reassembly(self):
        for pin in (None, '1234'):
            body, tpdus = self.message(pin)
            parts = [self.parse(t) for t in tpdus]
            if len(tpdus) > 1:
                refs = {c[0] for c, _ in parts}
                self.assertEqual(len(refs), 1)
                self.assertEqual([c[2] for c, _ in parts], list(range(1, len(tpdus) + 1)))
            self.assertEqual(b''.join(p for _, p in parts), body)

    def test_reference(self):
        body = bytes(300)
        a = [self.parse(t)[0][0] for t in sms_provision.sms_deliver_tpdus(body, '1', ref=7)]
        self.assertEqual(set(a), {7})                                  # one reference for all parts of a message
        seen = {self.parse(sms_provision.sms_deliver_tpdus(body, '1')[0])[0][0] for _ in range(20)}
        self.assertGreater(len(seen), 1)                               # a fresh one per message by default

    def test_rejects(self):
        with self.assertRaises(ValueError):
            sms_provision.sms_deliver_tpdus(b'x', '12a')
        with self.assertRaises(ValueError):
            sms_provision.sms_deliver_tpdus(bytes(130 * 256), '1')
        with self.assertRaises(ValueError):
            sms_provision.istr('a\x00b')


if __name__ == '__main__':
    unittest.main()
