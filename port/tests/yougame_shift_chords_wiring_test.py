"""YouGame's Shift chords never reach the game's keyboard, even when only the key state shows them."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ShiftChordsWiringTest(unittest.TestCase):
    def test_shift_chords_are_not_the_games(self):
        # The mini window takes Shift+Tab/Enter/Backspace with a hotkey: no message, but the key state
        # still reads down, and Shift+Enter chose a menu row.
        src = (ROOT / "port/runtime/host/window.cpp").read_text(encoding="utf-8")
        chord = src[src.index("auto youGame_chord = [&](int vk) {"):]
        chord = chord[:chord.index("};")]
        self.assertIn("vk != VK_TAB && vk != VK_RETURN && vk != VK_BACK && vk != VK_ESCAPE", chord)
        self.assertIn("!s_chord_down[vk] && (GetAsyncKeyState(VK_SHIFT) & 0x8000)", chord)
        self.assertIn("if (!down) s_chord_held[vk] = false;", chord)
        key = src[src.index("auto key = [&](int vk) {", src.index("auto youGame_chord")):]
        key = key[:key.index("};")]
        self.assertIn("if (youGame_chord(vk)) return false;", key)
        self.assertLess(key.index("youGame_chord(vk)"), key.index("GetAsyncKeyState(vk)"))


if __name__ == "__main__":
    unittest.main()
