#!/usr/bin/env python3
"""Synthetic MUHD fixtures; no game or disc needed."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('handoff_diff', ROOT / 'tools/handoff_diff.py')
diff = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(diff)


class DiffTest(unittest.TestCase):
    def test_ranges(self):
        a, b = bytearray(400), bytearray(400)
        for i in (0, 64, 129, 130, 200, 399):
            b[i] = 1
        self.assertEqual(diff.differences([(0x80000000, 200), (0x90000000, 200)], a, b), [
            (0x80000000, 65, 2), (0x80000081, 2, 2),
            (0x90000000, 1, 1), (0x900000c7, 1, 1)])
        self.assertEqual(diff.differences([(1, 400)], a, a), [])
        self.assertEqual(diff.differences([(1, 1), (65, 1)], b'aa', b'bb'), [(1, 65, 2)])
        self.assertEqual(diff.differences([(1, 1), (66, 1)], b'aa', b'bb'), [(1, 1, 1), (66, 1, 1)])

    def test_files_and_cli(self):
        # ctest supplies its writable build dir. Direct execution uses a worktree-local directory.
        directory = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'port/tests'
        a, b = directory / 'handoff_diff_test_a.bin', directory / 'handoff_diff_test_b.bin'
        header = struct.pack('<4sIIQQI', b'MUHD', 1, 1, 3, 0x80000000, 3)
        try:
            a.write_bytes(header + b'abc')
            b.write_bytes(header + b'axc')
            self.assertEqual(diff.read_dump(a), ([(0x80000000, 3)], b'abc'))
            result = subprocess.run([sys.executable, str(ROOT / 'tools/handoff_diff.py'), str(a), str(b)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn('0x0000000080000001-0x0000000080000002 1 bytes', result.stdout)
            b.write_bytes(a.read_bytes())
            result = subprocess.run([sys.executable, str(ROOT / 'tools/handoff_diff.py'), str(a), str(b)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0)
            valid = a.read_bytes()
            for content in (b'', valid[:19], b'NOPE' + valid[4:], valid[:4] + b'\x02' + valid[5:],
                            valid[:-1], valid + b'\0', valid[:20] + b'\0'*12 + b'abc'):
                a.write_bytes(content)
                with self.assertRaises(ValueError):
                    diff.read_dump(a)
            a.write_bytes(valid)
            b.write_bytes(struct.pack('<4sIIQQI', b'MUHD', 1, 1, 3, 0x90000000, 3) + b'abc')
            result = subprocess.run([sys.executable, str(ROOT / 'tools/handoff_diff.py'), str(a), str(b)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('different span tables', result.stderr)
        finally:
            a.unlink(missing_ok=True)
            b.unlink(missing_ok=True)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
