"""A double-click finds the disc the YouGame app (or any earlier run) already used: disc_memory.h,
compiled with the host's own compiler, so it runs on any machine."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from online_menu_test import compiler_command

ROOT = Path(__file__).resolve().parents[2]


class DiscMemoryTest(unittest.TestCase):
    def test_remembered_discs(self):
        with tempfile.TemporaryDirectory(prefix="disc-memory-", dir=ROOT) as tmp:
            exe = Path(tmp) / "disc-memory-test"
            compiled = subprocess.run([*compiler_command(), "-std=c++17", str(ROOT / "port/tests/disc_memory_test.cpp"),
                                       "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(exe), str(Path(tmp) / "root")], capture_output=True, text=True, timeout=20)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_bare_launch_uses_it(self):
        launch = (ROOT / "port/app/standalone_launch_win.h").read_text(encoding="utf-8")
        self.assertIn('#include "disc_memory.h"', launch)
        self.assertIn("fs::path iso = earlier_disc(data.parent_path());", launch)
        self.assertIn("return is_melee_102(disc);", launch)   # a remembered disc is checked like a picked one
        self.assertIn("ofn.hwndOwner = owner.hwnd;", launch)   # the picker is centered, not at 0,0
        self.assertIn("identify_vanilla(disc).kind == source_port::mods::Kind::Vanilla", launch)   # launcher.ini discs get the whole-disc check
        self.assertIn("disc_memory::remember(ini, iso);", launch)   # portable, so the test can check what is written


if __name__ == "__main__":
    unittest.main()
