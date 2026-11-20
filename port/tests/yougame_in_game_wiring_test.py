""""YouGame in game": a game the player started uses the local channel to the YouGame app only while this player
setting is on (the app or the launcher starting the game is unchanged). The build default is a CMake option, OFF in the
source; the player's choice in settings.ini wins over it; a YouGame row of the Online menu turns it on and starts the
channel without a restart. docs/yougame-hook.md says the same in plain words.
SPDX-License-Identifier: GPL-2.0-or-later
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def read(p):
    return (ROOT / p).read_text(encoding="utf-8")


def body(src, signature):
    """The body of the function whose definition starts with `signature` (to its closing brace at column 0)."""
    m = re.search(re.escape(signature) + r"[^\n]*\n(.*?)\n\}\n", src, re.S)
    return m.group(1) if m else ""


class YouGameInGameWiringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = read("CMakeLists.txt")
        cls.mac = read("port/mac/build_full.py")
        cls.link = read("port/runtime/hle/yougame_link.cpp")
        cls.link_h = read("port/runtime/hle/yougame_link.h")
        cls.main = read("port/app/main.cpp")
        cls.settings = read("port/runtime/gx/pc_settings.cpp")
        cls.online = read("port/runtime/hle/slippi_online.cpp")
        cls.doc = read("docs/yougame-hook.md")

    # --- the build default ---
    def test_cmake_option_defaults_off_and_always_defines(self):
        self.assertRegex(self.cmake, r'option\(MELEE_YOUGAME_IN_GAME_DEFAULT "[^"\n]*(\\"[^"\n]*)*" OFF\)')
        self.assertIn("add_compile_definitions(MELEE_YOUGAME_IN_GAME_DEFAULT=1)", self.cmake)
        self.assertIn("add_compile_definitions(MELEE_YOUGAME_IN_GAME_DEFAULT=0)", self.cmake)
        self.assertIn("-DMELEE_YOUGAME_IN_GAME_DEFAULT={1 if args.yougame_in_game_default else 0}", self.mac)

    def test_define_reaches_the_code(self):
        self.assertIn("#if defined(MELEE_YOUGAME_IN_GAME_DEFAULT) && MELEE_YOUGAME_IN_GAME_DEFAULT\n"
                      "constexpr bool kInGameDefault = true;\nconst char kInGameBuildTag[] = \"yougame-in-game-default=on\";\n#else\nconstexpr bool kInGameDefault = false;\nconst char kInGameBuildTag[] = \"yougame-in-game-default=off\";\n#endif",
                      self.link)
        self.assertIn("return saved < 0 ? kInGameDefault : saved == 1;", self.link)

    # --- the setting in settings.ini ---
    def test_settings_ini_reads_and_writes_the_tri_state(self):
        self.assertIn('if (key == "yougame_in_game") { yougame_in_game = value == "1" ? 1 : value == "0" ? 0 : -1; continue; }',
                      self.settings)
        self.assertIn("yougame::in_game::loaded(yougame_in_game, options.settings_path);", self.settings)
        # No line until the player chose: the build default keeps applying.
        self.assertIn('if (yougame::in_game::saved() >= 0) file << "\\nyougame_in_game " << yougame::in_game::saved();',
                      self.settings)
        for decl in ("void loaded(int saved, const std::string& settings_path);", "int saved();", "bool on();",
                     "bool applies();", "void set(bool on, const char* why);"):
            self.assertIn(decl, self.link_h)

    def test_settings_screen_toggle(self):
        self.assertIn('if (settings_toggle("YouGame in game", &in_game)) yougame::in_game::set(in_game, "Settings");',
                      self.settings)
        self.assertIn("if (yougame::in_game::applies()) {", self.settings)

    # --- the gate ---
    def test_every_probe_is_gated_on_the_setting(self):
        self.assertIn("if (!g_channel_allowed || !in_game_on() || g_channel_up) return false;", self.link)
        # Only start_channel_probe opens a connection (probe_channel), and only probing hellos skip g_channel_up.
        self.assertEqual(len(re.findall(r"[^_]probe_channel\(", self.link)), 2)   # its definition and the probe thread
        self.assertEqual(self.link.count("channel::Connection::open("), 1)

    def test_start_channel_gated_and_tests_still_refuse(self):
        start = body(self.link, "void start_channel(bool automated) {")
        self.assertIn('if (g_channel_allowed && host::test_env::getenv("MELEE_NO_YOUGAME_CHANNEL")) g_channel_allowed = false;', start)
        self.assertIn('if (g_channel_allowed && automated && !host::test_env::getenv("MELEE_TEST_YOUGAME_CHANNEL")) g_channel_allowed = false;',
                      start)
        self.assertIn("if (g_channel_allowed && in_game_on()) start_channel_watch();", start)
        self.assertIn("g_channel_started = true;", start)
        # The probe and the 4 s look start once, whenever the setting first is on.
        watch = body(self.link, "void start_channel_watch() {")
        self.assertIn("std::call_once(once", watch)
        self.assertIn("start_channel_probe(true);", watch)
        self.assertIn("std::this_thread::sleep_for(std::chrono::seconds(4));", watch)

    def test_main_starts_the_channel_after_the_settings_load(self):
        start = self.main.find("if (!settings_window_only) yougame::start_channel(automated);")
        self.assertGreater(start, 0)
        self.assertLess(self.main.find("    gx::load_pc_settings(gfx, o.volume);\n    gfx.settings_open = false;\n  }"), start)
        self.assertLess(start, self.main.find("if (!settings_window_only) yougame::start_hook_at_boot(automated);"))

    def test_nothing_else_reaches_out_while_off(self):
        self.assertIn("if (active() || g_launcher || !coming_soon_build() || !in_game_on()) return;",
                      body(self.link, "void start_hook_at_boot(bool automated) {"))
        self.assertIn("if (!coming_soon() || !in_game_on()) return;", body(self.link, "void online_menu_opened() {"))
        self.assertIn("if (g_stdio || !g_channel_allowed || !in_game_on()) return false;",
                      body(self.link, "bool channel_menu_key(const char* kind) {"))

    # --- the Online menu: never a dead end ---
    def test_online_menu_row_turns_it_on_and_starts_the_channel(self):
        picked = body(self.link, "void first_row_picked() {")
        enable = picked.find('if (!in_game_on()) in_game::set(true, "Online menu");')
        self.assertGreater(enable, -1)
        self.assertLess(enable, picked.find("if (g_channel_up.load()) return;"))
        self.assertLess(enable, picked.find("reach_ask_on_use();"))
        # The menu's YouGame row (0x20) still reaches first_row_picked while YouGame is not connected.
        self.assertIn("if (yougame::coming_soon()) yougame::first_row_picked();", self.online)
        setter = body(self.link, "void set(bool turn_on, const char* why) {")
        self.assertIn('line = "yougame_in_game " + std::to_string(value);', setter)
        self.assertIn('text += "yougame_in_game " + std::to_string(value) + "\\n";', setter)
        self.assertIn("std::filesystem::rename(temporary, file, ec);", setter)
        # Started without a restart, and never from the settings window's process.
        self.assertIn("if (turn_on && g_channel_started.load()) start_channel(g_automated_run.load());", setter)

    # --- the log ---
    def test_startup_log_line(self):
        start = body(self.link, "void start_channel(bool automated) {")
        self.assertIn('host::log("yougame in game: %s (%s)%s [%s]", in_game_on() ? "on" : "off",', start)
        self.assertIn('kInGameBuildTag);', start)
        self.assertIn('saved >= 0 ? "setting" : kInGameDefault ? "build default on" : "build default off",', start)
        self.assertLess(start.find("std::call_once(once"), start.find('host::log("yougame in game:'))

    def test_doc(self):
        for words in ("MELEE_YOUGAME_IN_GAME_DEFAULT", "OFF", "YouGame in game", "yougame_in_game", "leave the option OFF"):
            self.assertIn(words, self.doc)


if __name__ == "__main__":
    unittest.main()
