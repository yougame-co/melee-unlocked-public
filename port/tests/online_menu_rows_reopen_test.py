"""The Online menu opens YouGame's rows while it is up, not only on the next visit."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MENU = ROOT / "sourceport/game/shim/mu_slippi_menu.c"


class OnlineMenuRowsReopenTest(unittest.TestCase):
    def setUp(self):
        self.src = MENU.read_text()
        start = self.src.index("void mu_slippi_online_menu_think(")
        self.think = self.src[start:self.src.index("if (auto_slot >= 0 && accepts_input)", start)]

    def test_closed_rows_are_polled_on_the_online_menu(self):
        branch = self.think[self.think.index("} else if (mn_804A04F0.cur_menu == MENU_KIND_8 && split_layout()"):]
        self.assertIn("!(yg_flags & (YG_ROWS_OPEN | YG_COMING_SOON))", branch)
        self.assertRegex(branch, r"\+\+yg_closed_frames % 60u == 0")
        self.assertIn("yougame_query(-1, NULL, 0)", branch)
        self.assertRegex(branch, r"if \(flags & YG_ROWS_OPEN\) \{\s*yg_flags = flags;")

    def test_a_waiting_pick_keeps_its_own_poll(self):
        self.assertLess(self.think.index("if (yg_wait_slot >= 0)"), self.think.index("yg_closed_frames"))

    def test_counter_is_declared(self):
        self.assertTrue(re.search(r"^static unsigned yg_closed_frames;", self.src, re.M))


if __name__ == "__main__":
    unittest.main()
