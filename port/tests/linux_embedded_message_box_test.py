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

    def test_xlib_threads_are_on_before_sdl_video_starts(self):
        win = (ROOT / "port/linux/window_sdl.inl").read_text(encoding="utf-8-sig")
        body = win[win.index("void* window_create("):]
        self.assertLess(body.index("XInitThreads();"), body.index("SDL_InitSubSystem("))


class AgentTestSaysWhyItReadNothingTest(unittest.TestCase):
    def test_hook_builds_log_the_silent_cases(self):
        src = (ROOT / "port/runtime/hle/yougame_agent_test.h").read_text(encoding="utf-8-sig")
        self.assertIn('"agent-test.json not looked for: no YouGame bridge or launcher"', src)
        self.assertIn('"agent-test.json not looked for: YOUGAME_DATA is empty"', src)
        self.assertIn('"agent-test.json not found in the game\'s data folder (YOUGAME_DATA)"', src)


if __name__ == "__main__":
    unittest.main()
