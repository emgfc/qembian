#!/usr/bin/env python3
"""Capture reader must reject damaged recordings, and preserve every payload byte."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('gpu_capture', Path(__file__).resolve().parents[1] / 'tools/gpu_capture.py')
cap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cap)

class CaptureTest(unittest.TestCase):
    def read(self, content):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'capture.bin'
            p.write_bytes(content)
            return list(cap.records(p))

    def test_full_payload_and_owners(self):
        payload = struct.pack('<1028I', 20, 1028, 0x1234, 100, *range(1024))
        record = struct.pack('<II', 42, len(payload) // 4) + payload
        got = self.read(b'SYGLCAP1' + record + record)
        self.assertEqual(got[0][2], 42)
        self.assertEqual(got[0][4], payload)
        self.assertEqual(got[1][1], 8 + len(record))

    def test_corruption(self):
        valid = b'SYGLCAP1' + struct.pack('<5I', 42, 3, 1, 3, 99)
        for broken in (b'bad', valid[:-1], valid[:12], b'SYGLCAP1' + struct.pack('<4I',42,2,1,3)):
            with self.subTest(broken=broken), self.assertRaises(ValueError):
                self.read(broken)

if __name__ == '__main__':
    unittest.main()
