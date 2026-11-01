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
        self.assertIn('else if (key == "showfps_yougame2") saved_showfps_yougame = value == "1";', self.load)
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
        self.assertIn('yougame::display_defaults::fps_source(saved_showfps_yougame)', self.load)
        self.assertIn('fps counter %s (%s)', self.load)

    def test_write_keeps_the_old_keys_and_adds_the_youGame_ones_only_inside_youGame(self):
        self.assertIn('<< "\\nvsync " << (g_display_keys.own_vsync ? g_display_keys.vsync : options.vsync)', self.write)
        self.assertIn('(g_display_keys.own_vsync ? std::string("\\nvsync_yougame ")', self.write)
        self.assertIn('<< "\\nshowfps " << (g_display_keys.yougame ? g_display_keys.show_fps : options.show_fps)', self.write)
        self.assertIn('(g_display_keys.yougame ? std::string("\\nshowfps_yougame2 ")', self.write)
        self.assertNotIn('<< "\\nvsync " << options.vsync', self.write)
        # A save outside YouGame keeps the YouGame keys the file already had.
        self.assertIn('g_display_keys.saved_vsync_yougame >= 0 ? "\\nvsync_yougame " + std::to_string(g_display_keys.saved_vsync_yougame)', self.write)
        self.assertIn('g_display_keys.saved_showfps_yougame >= 0 ? "\\nshowfps_yougame2 " + std::to_string(g_display_keys.saved_showfps_yougame)', self.write)

    def test_the_state_starts_as_outside_youGame(self):
        self.assertIn("  bool yougame = false, own_vsync = false, vsync = true, show_fps = false;\n"
                      "  int saved_vsync_yougame = -1, saved_showfps_yougame = -1;\n} g_display_keys;", self.settings)
        self.assertLess(self.settings.index("g_display_keys;"), self.settings.index("void load_pc_settings("))


    def test_agent_runs_are_detected_at_runtime_not_by_build_flag(self):
        main = (ROOT / "port/app/main.cpp").read_text(encoding="utf-8-sig")
        load = main.index("yougame::agent_test::load(yougame::active()")
        flag = main.index("yougame::display_defaults::agent_test_run = agent_test.agent_file;")
        practice = main.index("yougame::ranked_cpu::practice_allowed = agent_test.agent_file;")
        self.assertLess(flag, practice)
        self.assertNotIn("\n", main[flag:practice].rstrip())
        guards = []
        for line in main[:practice].splitlines():
            if line.startswith("#if"):
                guards.append(line)
            elif line.startswith("#endif"):
                guards.pop()
        self.assertIn("#ifdef YOUGAME_AGENT_TEST_HOOK", guards)
        self.assertLess(practice, main.index("#endif", flag))
        self.assertLess(load, flag)
        self.assertLess(flag, main.index("gx::load_pc_settings(gfx, o.volume);"))
        defaults = (ROOT / "port/runtime/gx/yougame_display_defaults.h").read_text(encoding="utf-8-sig")
        self.assertNotIn("YOUGAME_AGENT_TEST_HOOK", defaults)
        self.assertIn("inline bool agent_test_run = false;", defaults)
        agent = (ROOT / "port/runtime/hle/yougame_agent_test.h").read_text(encoding="utf-8-sig")
        self.assertIn("  out.agent_file = true;", agent)

    def test_the_fps_choice_is_saved_under_the_new_key(self):
        self.assertIn('std::string("\\nshowfps_yougame2 ")', self.write)
        self.assertNotIn('"\\nshowfps_yougame "', self.write)

if __name__ == "__main__":
    unittest.main()
