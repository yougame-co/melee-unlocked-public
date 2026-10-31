"""Inside YouGame, V-Sync and the FPS counter follow their own saved keys (pc_settings.cpp)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(body, signature):
    start = body.index(signature)
    end = body.index("\n}\n", start)
    return body[start:end]


class DisplayDefaultsWiringTest(unittest.TestCase):
    def setUp(self):
        self.settings = (ROOT / "port/runtime/gx/pc_settings.cpp").read_text(encoding="utf-8-sig")
        self.load = function(self.settings, "void load_pc_settings(RenderOptions& options, int& volume) {")
        self.write = function(self.settings, "static bool write_settings_file(const SettingsState& state, const RenderOptions& options) {")

    def test_load_reads_both_keys_and_applies_the_policy_after_the_file(self):
        self.assertIn('else if (key == "vsync_yougame") saved_vsync_yougame = value == "1";', self.load)
        self.assertIn('else if (key == "showfps_yougame") saved_showfps_yougame = value == "1";', self.load)
        loop_end = self.load.index("split_old_trigger_tables();")
        for call in ("yougame::display_defaults::vsync(g_display_keys.yougame, kWindows, options.vsync, saved_vsync_yougame)",
                     "yougame::display_defaults::show_fps(g_display_keys.yougame, options.show_fps, saved_showfps_yougame)"):
            self.assertGreater(self.load.index(call), loop_end)
        # The old keys' values are captured before the policy replaces them, and a key the file
        # lacks is captured as its fresh-install value, not as an earlier load's YouGame default.
        capture = self.load.index("vsync_seen ? options.vsync : fresh.vsync, showfps_seen ? options.show_fps : fresh.show_fps,")
        self.assertLess(capture, self.load.index("options.vsync = yougame::display_defaults::vsync("))
        self.assertIn('else if (key == "vsync") { options.vsync = value == "1"; vsync_seen = true; }', self.load)
        self.assertIn('else if (key == "showfps") { options.show_fps = value == "1"; showfps_seen = true; }', self.load)

    def test_write_keeps_the_old_keys_and_adds_the_youGame_ones_only_inside_youGame(self):
        self.assertIn('<< "\\nvsync " << (g_display_keys.own_vsync ? g_display_keys.vsync : options.vsync)', self.write)
        self.assertIn('(g_display_keys.own_vsync ? std::string("\\nvsync_yougame ")', self.write)
        self.assertIn('<< "\\nshowfps " << (g_display_keys.yougame ? g_display_keys.show_fps : options.show_fps)', self.write)
        self.assertIn('(g_display_keys.yougame ? std::string("\\nshowfps_yougame ")', self.write)
        self.assertNotIn('<< "\\nvsync " << options.vsync', self.write)
        # A save outside YouGame keeps the YouGame keys the file already had.
        self.assertIn('g_display_keys.saved_vsync_yougame >= 0 ? "\\nvsync_yougame " + std::to_string(g_display_keys.saved_vsync_yougame)', self.write)
        self.assertIn('g_display_keys.saved_showfps_yougame >= 0 ? "\\nshowfps_yougame " + std::to_string(g_display_keys.saved_showfps_yougame)', self.write)

    def test_the_state_starts_as_outside_youGame(self):
        self.assertIn("  bool yougame = false, own_vsync = false, vsync = true, show_fps = false;\n"
                      "  int saved_vsync_yougame = -1, saved_showfps_yougame = -1;\n} g_display_keys;", self.settings)
        self.assertLess(self.settings.index("g_display_keys;"), self.settings.index("void load_pc_settings("))


if __name__ == "__main__":
    unittest.main()
