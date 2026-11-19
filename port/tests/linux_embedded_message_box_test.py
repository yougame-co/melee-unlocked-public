"""Inside the YouGame app's player box, the Linux GL setup never opens an SDL message box (it would hang)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LinuxEmbeddedMessageBoxTest(unittest.TestCase):
    def test_every_box_in_the_gl_setup_is_skipped_when_embedded(self):
        src = (ROOT / "port/linux/full/gx_backend_linux.cpp").read_text(encoding="utf-8-sig")
        lines = src.splitlines()
        boxes = [i for i, l in enumerate(lines) if "SDL_ShowSimpleMessageBox(" in l]
        self.assertEqual(len(boxes), 2)
        for i in boxes:
            self.assertIn("!host::window_embedded()", lines[i - 1])


if __name__ == "__main__":
    unittest.main()
