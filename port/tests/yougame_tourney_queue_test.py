"""Compile the pure tournament SDK/queue policy against mocked JSON, without submodules."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from online_menu_test import compiler_command

ROOT = Path(__file__).resolve().parents[2]


class TournamentQueueTest(unittest.TestCase):
    def test_production_queue_and_match_policy(self):
        with tempfile.TemporaryDirectory(prefix="tourney-queue-", dir=ROOT) as tmp:
            exe = Path(tmp) / "queue-test"
            compiled = subprocess.run([
                *compiler_command(), "-std=c++17", "-pthread",
                "-I", str(ROOT / "port/runtime/hle"), "-I", str(ROOT / "port/third_party"),
                str(ROOT / "port/tests/yougame_tourney_test.cpp"), "-o", str(exe),
            ], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            print(ran.stdout.strip())


if __name__ == "__main__":
    unittest.main()
