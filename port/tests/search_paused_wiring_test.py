# Rooms' "searching".paused line (a lone standalone seat, a test build's pool) reaches the CSS status row.
# SPDX-License-Identifier: GPL-2.0-or-later
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
LINK = (ROOT / "port/runtime/hle/yougame_link.cpp").read_text()
HEADER = (ROOT / "port/runtime/hle/yougame_link.h").read_text()
OVERLAY = (ROOT / "port/runtime/gx/pc_settings.cpp").read_text()


class SearchPausedWiring(unittest.TestCase):
    def test_queue_frame_sets_it(self):
        self.assertIn('if (!cancelled() && t == "searching")\n'
                      '        set_search_paused(m.count("paused") && m["paused"].is_string() ? m["paused"].get<std::string>() : std::string());',
                      LINK)

    def test_each_search_starts_clear(self):
        self.assertIn("if (g_search_generation.current(epoch)) { g_blend_reason.clear(); g_search_paused.clear(); }", LINK)

    def test_line_is_cleaned_for_one_row(self):
        self.assertIn("constexpr size_t kPausedMax = 72;", LINK)
        self.assertIn('out += "..."; i += 2;', LINK)
        self.assertIn("std::string search_paused_line();", HEADER)

    def test_overlay_shows_it_only_over_youGame_only_search(self):
        self.assertIn('(status == "searching" || status == yougame::blend::kSearchYouGameOnly)\n'
                      '        ? yougame::search_paused_line() : std::string();', OVERLAY)
        self.assertIn("status == yougame::blend::kSearchYouGameOnly || !paused.empty();", OVERLAY)
        self.assertIn("if (paused.empty()) ImGui::TextUnformatted(yougame::slippi_link_status(status).c_str());", OVERLAY)


if __name__ == "__main__":
    unittest.main()
