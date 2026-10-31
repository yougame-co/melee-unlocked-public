"""A hidden game in the player box never keeps or takes the keyboard (Windows, window.cpp)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def case(body, label):
    start = body.index(label)
    end = body.find("\n    case ", start + len(label))
    return body[start:end if end >= 0 else len(body)]


class EmbedFocusWiringTest(unittest.TestCase):
    def setUp(self):
        self.window = (ROOT / "port/runtime/host/window.cpp").read_text(encoding="utf-8-sig")

    def test_hide_gives_the_keyboard_back_before_hiding(self):
        show = case(self.window, "case kBoxShow: {")
        release = show.index("if (!visible && GetFocus() == h) {")
        self.assertIn("SetFocus(GetParent(h));", show[release:])
        self.assertLess(release, show.index("ShowWindow(h, visible ? SW_SHOWNA : SW_HIDE);"))

    def test_focus_only_while_visible(self):
        focus = case(self.window, "case kBoxFocus:")
        focus = focus[focus.index("// Only a visible game"):]
        self.assertIn("if (w && IsWindowVisible(h)) {", focus)
        self.assertEqual(focus.count("SetFocus(h);"), 1)
        self.assertEqual(focus.count("embed: focus handback rc=%p err=%lu focus-after=%s"), 1)
        self.assertIn("const DWORD error = GetLastError();", focus)
        self.assertIn('!after ? "none" : after == h ? "self" : after == GetParent(h) ? "parent" : "other"', focus)
        self.assertLess(focus.index("IsWindowVisible(h)"), focus.index("SetFocus(h);"))

    def test_nothing_activates_the_embedded_window(self):
        for call in ("SetForegroundWindow(", "SetActiveWindow(", "BringWindowToTop(", "AttachThreadInput("):
            self.assertIsNone(re.search(r"\b" + re.escape(call), self.window), call)


if __name__ == "__main__":
    unittest.main()
