#!/usr/bin/env python3
"""Unit tests for tools/device.py (plain unittest): tests/device_py_test.py"""
import os, re, sys, unittest
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import device as D

MAP = os.path.join(ROOT, 'build', 'epocroot', 'sf', 'adapt', 'qemu', 'baseport', 'syborg', 'keyboard', 'syborg_keyboard_map.h')


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

    def test_every_model_and_firmware_loads(self):
        for m in sorted({v[0] for v in D._index().values()}):
            for fw in D.firmware_names(m):
                prof = D.load('%s@%s' % (m, fw))
                self.assertEqual(prof['_model'], m)
                self.assertEqual(prof['_firmware'], fw)

    def test_registry_matches_profiles(self):
        reg = D.registry()
        ids = set(D.device_ids())
        for r in reg.values():
            if r.get('model'):
                self.assertIn(r['model'], ids, r['code'])
        for i in ids:
            tc = D.get(D.load(i), 'device.type_code')
            self.assertIn(tc, reg, i)
            self.assertTrue(i.startswith(reg[tc]['model']), (i, tc))   # firmware variants: <model>-<suffix>

    def test_merge_remove_table_key(self):
        m = D._merge({'files': [{'role': 'core', 'p': {'core': 'C', 'rofs-after-core': 'C'}}]},
                     {'files': [{'role': 'core', 'p': {'rofs': 'R', 'remove': ['rofs-after-core']}}]})
        self.assertEqual(m['files'][0]['p'], {'core': 'C', 'rofs': 'R'})

    def test_aliases_and_default(self):
        self.assertEqual(D.resolve('n95-8gb'), 'nokia-n95-8gb-rm320')
        self.assertEqual(D.resolve('default'), 'nokia-n95-8gb-rm320')

    def test_qemu_args(self):
        a = D.qemu_args(D.load('nokia-n95-8gb-rm320'))
        for want in ('syborg-fb.width=240', 'syborg-fb.height=320', '168M', 'arm1136-r2'):
            self.assertIn(want, a)

    @unittest.skipUnless(os.path.exists(MAP), 'no generated key map yet (scripts/build-syborg.sh)')
    def test_built_keymap_is_current(self):
        """the EKEYB table in the build tree (patch qemu-baseport 0005 includes it) = the profile's"""
        gen = '\n'.join(D.keymap_header(D.load('nokia-n95-8gb-rm320')))
        self.assertEqual(open(MAP).read().strip(), gen.strip())

    def test_keymap_header_is_device_independent(self):
        """with QEMU-side translation every device gets the same guest table (shared baseport builds)"""
        heads = {'\n'.join(D.keymap_header(D.load(d))) for d in D.device_ids()
                 if D.get(D.load(d), 'board.input.translation') == 'qemu'}
        self.assertEqual(len(heads), 1)

    @unittest.skipUnless(os.path.exists(os.path.join(D.paths(D.load('nokia-n95-8gb-rm320'))['dir'], 'rofs1.img')), 'no extracted firmware')
    def test_identify(self):
        facts, matches = D.identify(D.paths(D.load('nokia-n95-8gb-rm320'))['dir'])
        self.assertEqual(facts['sw']['type_code'], 'RM-320')
        self.assertEqual((matches[0][0], matches[0][1]), (0, 'nokia-n95-8gb-rm320'))
        self.assertTrue(all(n for n, d, _ in matches[1:]))        # the RM-321 profile does not match


    def test_user_data_layer(self):
        import tempfile
        self.assertEqual(D.size_bytes('64M'), 64 << 20)
        self.assertEqual(D.size_bytes('1G'), 1 << 30)
        p = D.load('nokia-n8-rm596-v111')
        self.assertTrue([f for f in D.uda_entries(p) if f.get('drive') == 'C'])   # inherited from the N8 base
        self.assertIsNone(D.uda_image(p, 'E'))
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, 'c.img')
            self.assertIn('blank', D.drive_image(p, 'nvmem', out, uda=False))
            self.assertEqual(os.path.getsize(out), D.size_bytes('64M'))
            src = D.uda_image(p, 'C')
            if os.path.exists(src):                     # extracted on this machine (firmware not in git)
                os.remove(out)
                self.assertIn('user data layer', D.drive_image(p, 'nvmem', out, uda=True))
                self.assertEqual(os.path.getsize(out), os.path.getsize(src))
                self.assertEqual(open(out, 'rb').read(512), open(src, 'rb').read(512))

class InstanceTest(unittest.TestCase):
    """tools/instance.py: instances in a throw-away QEMBIAN_HOME, [overrides] through tools/device.py"""

    def setUp(self):
        import tempfile
        self.tmp = tempfile.mkdtemp()
        self.env = dict(os.environ)
        os.environ['QEMBIAN_HOME'] = self.tmp
        os.environ.pop('SYBORG_INSTANCE', None)
        os.environ.pop('SYBORG_DEVICE', None)
        import instance as I
        self.I = I

    def tearDown(self):
        import shutil
        os.environ.clear(); os.environ.update(self.env)
        shutil.rmtree(self.tmp)

    def run_i(self, *args):
        return self.I.main(list(args))

    def test_create_primary_and_overrides(self):
        I = self.I
        self.assertEqual(self.run_i('create', 'a', 'nokia-n8-rm596-delight'), 0)
        self.assertEqual(self.run_i('create', 'b', 'nokia-n8-rm596@111.040.1511~delight-6.7'), 0)
        self.assertEqual(I.device_id(I.read('b')), 'nokia-n8-rm596-delight')       # model@fw = the firmware's own id
        self.assertTrue(I.read('a')['primary']); self.assertFalse(I.read('b')['primary'])
        self.assertEqual(I.primary_for('nokia-n8-rm596-delight')['name'], 'a')
        self.assertEqual(self.run_i('set', 'b', 'primary', 'true'), 0)
        self.assertFalse(I.read('a')['primary'])                                   # one primary per device
        d = I.idir('b')
        with open(os.path.join(d, 'instance.toml'), 'a') as f:
            f.write('\n[overrides.board]\nram_mb = 200\n')
        os.environ['SYBORG_INSTANCE'] = d
        self.assertEqual(D.load()['_id'], 'nokia-n8-rm596-delight')                 # the instance names the device
        self.assertEqual(D.get(D.load(), 'board.ram_mb'), 200)
        self.assertNotEqual(D.get(D.load('nokia-n95-8gb-rm320'), 'board.ram_mb'), 200)   # another device: no overrides
        with open(os.path.join(d, 'instance.toml'), 'a') as f:
            f.write('\n[overrides.rom]\nx = 1\n')
        with self.assertRaises(D.DeviceError):
            D.load()                                                                # composed tables are refused

    def test_firmware_change_needs_a_drive_decision(self):
        self.assertEqual(self.run_i('create', 'n8', 'nokia-n8-rm596'), 0)
        self.assertEqual(self.run_i('set', 'n8', 'firmware', '013.016'), 2)
        self.assertEqual(self.run_i('set', 'n8', 'firmware', '013.016', '--wipe-drives'), 0)
        self.assertEqual(self.I.device_id(self.I.read('n8')), 'nokia-n8-rm596-v13')
        self.assertEqual(open(os.path.join(self.I.idir('n8'), 'device')).read().strip(), 'nokia-n8-rm596-v13')

    def test_clone_rename_delete(self):
        self.assertEqual(self.run_i('create', 'x', 'nokia-n95-8gb-rm320', '--variant', 'audio-exp'), 0)
        os.makedirs(os.path.join(self.I.idir('x'), 'drives'))
        open(os.path.join(self.I.idir('x'), 'drives', 'nvmemory.img'), 'wb').write(b'C' * 4096)
        self.assertEqual(self.run_i('clone', 'x', 'y'), 0)
        self.assertEqual(open(os.path.join(self.I.idir('y'), 'drives', 'nvmemory.img'), 'rb').read(), b'C' * 4096)
        self.assertTrue(self.I.rom(self.I.read('y')).endswith('hybrid-exp.img'))
        self.assertEqual(self.run_i('rename', 'y', 'z'), 0)
        self.assertEqual(self.I.read('z')['name'], 'z')
        self.assertEqual(self.run_i('delete', 'z'), 2)                               # needs --yes
        self.assertEqual(self.run_i('delete', 'z', '--yes'), 0)
        self.assertEqual(self.run_i('create', 'x', 'nokia-n95-8gb-rm320', '--replace'), 2)   # not ephemeral


if __name__ == '__main__':
    unittest.main()
