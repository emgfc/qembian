import struct
import tempfile
import unittest
from pathlib import Path
import dct4_extract as d


def record(kind, payload=b'hello', address=0x1000000):
    if kind == 0x14:
        h = bytes([kind]) + address.to_bytes(4, 'big') + bytes([d.checksum(payload)]) + len(payload).to_bytes(3, 'big')
        return h + bytes([d.checksum(h[1:])]) + payload
    if kind == 0x21:
        return bytes([kind]) + bytes(6) + address.to_bytes(4, 'big') + bytes(1) + len(payload).to_bytes(3, 'big') + bytes(1) + payload
    if kind == 0x2e:
        return bytes([kind]) + b'\x00\x02\x00\x10\x00' + len(payload).to_bytes(3, 'big') + b'\x00' + payload
    if kind == 0x20:
        return bytes([kind]) + bytes(2) + len(payload).to_bytes(2, 'big') + bytes(1) + payload
    raise AssertionError(kind)


def container(records, tokens=b'', count=0):
    header = count.to_bytes(4, 'big') + tokens
    return b'\xa2' + len(header).to_bytes(4, 'big') + header + records


def rom():
    data = bytearray(0x400)
    base = 0x50000000
    struct.pack_into('<III', data, 0x8c, base, len(data), base + 0x200)
    struct.pack_into('<III', data, 0x200, 1, 0, base + 0x220)
    name = 'hello.txt'.encode('utf-16le')
    entry = struct.pack('<IIBB', 5, base + 0x300, 0, 9) + name
    struct.pack_into('<I', data, 0x220, 4 + len(entry))
    data[0x224:0x224+len(entry)] = entry
    data[0x300:0x305] = b'world'
    return bytes(data)


class Tests(unittest.TestCase):
    def test_all_record_types(self):
        data = container(b''.join(record(k) for k in (0x14, 0x21, 0x2e, 0x20)) + b'\x25\x03abc' + b'\x26' + bytes(9))
        parsed = d.parse(data)
        self.assertEqual(len(parsed['records']), 6)
        self.assertTrue(parsed['records'][0]['checksums']['header_ok'])
        self.assertTrue(parsed['records'][0]['checksums']['data_ok'])
        self.assertEqual(parsed['records'][2]['address'], 0x2000)

    def test_tokens(self):
        tokens = b'\xc2\x04DCT4\xf3\x00\x03abc\xe1\x00\x02xy'
        self.assertEqual([x['text'] for x in d.parse(container(b'', tokens, 3))['tokens']], ['DCT4', 'abc', 'xy'])

    def test_bad_boundaries(self):
        data = container(record(0x14))
        for length in range(len(data)):
            if length == 9:  # Header-only container is valid.
                continue
            with self.assertRaises(d.FormatError, msg=f'length {length}'):
                d.parse(data[:length])
        with self.assertRaises(d.FormatError):
            d.parse(container(b'\x99'))

    def test_end_to_end(self):
        for swapped in (False, True):
            with self.subTest(swapped=swapped), tempfile.TemporaryDirectory() as td:
                td = Path(td)
                image = d.swap16(rom()) if swapped else rom()
                source = td / 'RH2941.0C1'
                source.write_bytes(container(record(0x14, image[:512]) + record(0x14, image[512:], 0x1000200)))
                result = d.extract(source, td / 'output', strict=True)
                self.assertEqual(len(result['segments']), 1)
                self.assertEqual(result['symbian_roms'][0]['swapped16'], swapped)
                self.assertEqual(next((td/'output').rglob('hello.txt')).read_bytes(), b'world')
                with self.assertRaises(FileExistsError):
                    d.extract(source, td / 'output')

    def test_checksums_and_malicious_tree(self):
        data = bytearray(container(record(0x14)))
        data[-1] ^= 1
        self.assertFalse(d.parse(data)['records'][0]['checksums']['data_ok'])
        with tempfile.TemporaryDirectory() as td:
            source = Path(td)/'bad'
            source.write_bytes(data)
            with self.assertRaises(d.FormatError):
                d.extract(source, Path(td)/'output', strict=True)
            self.assertFalse((Path(td)/'output').exists())
        for name in ('..', '../escape', 'C:escape', 'a\\b'):
            with self.assertRaises(d.FormatError):
                d.safe_name(name)
        bad = bytearray(rom())
        struct.pack_into('<I', bad, 0xe8, 0x101f7afc)
        with self.assertRaises(d.FormatError):
            d.read_eka1(bad)


if __name__ == '__main__':
    unittest.main()
