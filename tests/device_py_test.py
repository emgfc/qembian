#!/usr/bin/env python3
"""Unit tests for tools/device.py (plain unittest): tests/device_py_test.py"""
import os, re, sys, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as D

HEADER = os.path.join(ROOT, 'build', 'epocroot', 'sf', 'adapt', 'qemu', 'baseport', 'syborg', 'keyboard', 'syborg_keyboard.h')


class DeviceTest(unittest.TestCase):
    def test_profiles_check(self):
        for dev in D.device_ids():
            errs, _ = D.check(D.load(dev))
            self.assertEqual(errs, [], dev)

    def test_extends(self):
        child = D.load('nokia-n95-8gb-rm321')
        self.assertEqual(child['_extends'], 'nokia-n95-8gb-rm320')
        self.assertFalse(D.get(child, 'hardware.wlan'))
        self.assertEqual(D.get(child, 'board.ram_mb'), 168)          # inherited
        self.assertEqual(len(child['standins']), len(D.load('nokia-n95-8gb-rm320')['standins']))

    def test_merge_arrays_by_name(self):
        m = D._merge({'s': [{'name': 'a', 'x': 1}, {'name': 'b'}]},
                     {'s': [{'name': 'a', 'x': 2}, {'name': 'c'}], 'remove': ['b']})
        self.assertEqual(sorted(x['name'] for x in m['s']), ['a', 'c'])
        self.assertEqual([x for x in m['s'] if x['name'] == 'a'][0]['x'], 2)

    def test_aliases_and_default(self):
        self.assertEqual(D.resolve('n95-8gb'), 'nokia-n95-8gb-rm320')
        self.assertEqual(D.resolve('default'), 'nokia-n95-8gb-rm320')

    def test_qemu_args(self):
        a = D.qemu_args(D.load('nokia-n95-8gb-rm320'))
        for want in ('syborg-fb.width=240', 'syborg-fb.height=320', '168M', 'arm1136-r2'):
            self.assertIn(want, a)

    @unittest.skipUnless(os.path.exists(HEADER), 'no patched baseport tree (scripts/setup-epocroot.sh)')
    def test_keymap_header_matches_patched_baseport(self):
        gen = '\n'.join(D.keymap_header(D.load('nokia-n95-8gb-rm320')))
        g = {int(a, 16): b for a, b in re.findall(r"/\*([0-9a-f]{2})\*/ ([^\n]+),", gen)}
        t = open(HEADER, newline='').read()
        p = {int(a, 16): b.strip() for a, b in re.findall(r"/\*([0-9a-f]{2})\*/\s*([^,\r\n]+),", t)}
        self.assertEqual({k: v for k, v in g.items() if p.get(k) != v}, {})


if __name__ == '__main__':
    unittest.main()
