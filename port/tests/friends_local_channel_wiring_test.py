"""A game started on its own with the local channel to YouGame up: the split layout's Friends row opens YouGame's
friend flow (a Friends room, its picker, the lobby), and an accepted invite joins that room. The app keeps its own
path (active()), and the old launcher bridge keeps Slippi-style Direct."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HLE = ROOT / "port/runtime/hle"


def read(p):
    return (ROOT / p).read_text()


class FriendsLocalChannelWiringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.menu = read("sourceport/game/shim/mu_slippi_menu.c")
        cls.link = (HLE / "yougame_link.cpp").read_text()
        cls.link_h = (HLE / "yougame_link.h").read_text()
        cls.online = (HLE / "slippi_online.cpp").read_text()
        cls.net = (HLE / "slippi_net.cpp").read_text()
        cls.practice = (HLE / "native_practice.cpp").read_text()

    # --- the gate itself ---
    def test_friends_flow_is_the_app_or_the_local_channel(self):
        self.assertIn("bool friends_flow();", self.link_h)
        self.assertIn("bool friends_flow() { return active() || (launcher() && g_channel_up.load()); }", self.link)

    # --- the game's menu ---
    def test_split_friends_row_needs_the_channel_flag(self):
        self.assertRegex(self.menu, r"YG_COMING_SOON = 64, YG_FRIENDS_FLOW = 128 \};")
        self.assertIn("enum { SPLIT_FRIENDS = 2 };", self.menu)
        self.assertIn("return split_layout() && split_page == 0 && (yg_flags & YG_FRIENDS_FLOW) != 0;", self.menu)
        self.assertIn("return split_layout() ? split_friends() && sel == SPLIT_FRIENDS", self.menu)
        self.assertIn(": mu_slippi_menu_option(sel) == MU_SLP_OPT_FRIENDS;", self.menu)
        self.assertIn("int friends_visible = (ranked_supported && !slippi_submenu) || split_friends();", self.menu)

    def test_doubles_queries_stay_app_only(self):
        # The split layout has no Doubles row: its home/teams queries keep ranked_supported (the app).
        self.assertEqual(self.menu.count("if (ranked_supported && friends_visible && mn_804A04F0.cur_menu == MENU_KIND_8)"), 2)

    def test_split_friends_confirm_asks_the_host_after_the_publish(self):
        start = self.menu.index("on_friends_row(mn_804A04F0.hovered_selection))")
        branch = self.menu[start:self.menu.index("} else if", start)]
        self.assertIn("if (split_layout()) {", branch)
        for line in ("yg_pick = 1;", "last_slot = SPLIT_FRIENDS;", "yougame_query(1, NULL, 0);", "split_friends_press = 1;"):
            self.assertIn(line, branch)
        # The confirm comes before the generic split confirm that would open Direct.
        self.assertLess(start, self.menu.index("} else if (buttons & MenuInput_Confirm) {", start))
        publish = self.menu.index("mu_slippi_cmd(CMD_YOUGAME_FRIENDS_MENU, &friends_menu, 1, NULL, 0, NULL);")
        press = self.menu.index("u8 press = 17;")
        self.assertLess(publish, press)
        self.assertIn("const int friends_hovered = on_friends_row(mn_804A04F0.hovered_selection);", self.menu)

    def test_returns_land_on_the_split_friends_row(self):
        self.assertEqual(len(re.findall(
            r"split_page = 0;\s*(?:last_slot = SPLIT_FRIENDS;\s*)?\*hovered = cursor_stops\(SPLIT_FRIENDS\)", self.menu)), 2)
        self.assertIn("} else if (split_layout() && from_friends_online) {", self.menu)

    # --- the host ---
    def test_host_takes_the_press_only_off_the_app(self):
        start = self.online.index("if (payload[0] == 17) {")
        branch = self.online[start:self.online.index("return true;", start)]
        self.assertIn("if (!yougame::active() && yougame::friends_flow()) {", branch)
        self.assertIn("yougame::friends::events.online_menu(true, true);", branch)
        self.assertIn("yougame::friends::events.request_host();", branch)
        self.assertIn(
            "yougame::friends::events.online_menu(yougame::friends_flow() && (payload[0] & 3) != 0, (payload[0] & 3) == 2);",
            self.online)

    def test_menu_flag_is_the_channel_only(self):
        # The old launcher bridge leaves bit 7 clear and keeps Direct's connect code.
        self.assertIn("if ((q[0] & 4) && yougame::channel_connected() && yougame::friends_flow()) q[0] |= 128;", self.online)

    def test_friend_room_and_lobby_use_the_session(self):
        self.assertIn("if (!yougame::session()) { fail(\"The Friends lobby needs YouGame\"); return; }", self.net)
        self.assertIn("native_ready(yougame::session(), mode, connect_code, friend_room)", self.online)

    def test_menu_tick_runs_under_the_channel_with_the_tournament_app_only(self):
        self.assertIn("const bool via_channel = !yougame::active();", self.practice)
        self.assertIn("if (yougame::friends_flow() && !slippi::playback::enabled()) {", self.practice)
        self.assertIn("if (!via_channel && g_bridge && major == 1 && safe && yougame::tournament_pending()) {", self.practice)
        self.assertEqual(self.practice.count("if (via_channel) yougame::set_launcher_session(true);"), 2)


if __name__ == "__main__":
    unittest.main()
