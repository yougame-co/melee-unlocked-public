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

    def test_x_errors_are_logged_not_fatal(self):
        win = (ROOT / "port/linux/window_sdl.inl").read_text(encoding="utf-8-sig")
        body = win[win.index("void* window_create("):]
        video = body.index("SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER")
        self.assertLess(video, body.index("XSetErrorHandler(x_error_logged);"))
        self.assertIn("XSetIOErrorHandler(x_io_error_logged);", body)
        self.assertIn("return 0;", win[win.index("int x_error_logged("):win.index("int x_io_error_logged(")])


class AgentTestSaysWhyItReadNothingTest(unittest.TestCase):
    def test_hook_builds_log_the_silent_cases(self):
        src = (ROOT / "port/runtime/hle/yougame_agent_test.h").read_text(encoding="utf-8-sig")
        self.assertIn('"agent-test.json not looked for: no YouGame bridge or launcher"', src)
        self.assertIn('"agent-test.json not looked for: YOUGAME_DATA is empty"', src)
        self.assertNotIn('if (std::getenv("YOUGAME_AGENT_COPY"))\n      out.log = !active', src)
        self.assertIn('"agent-test.json not found in the game\'s data folder (YOUGAME_DATA)"', src)


if __name__ == "__main__":
    unittest.main()
