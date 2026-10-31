"""The online menu's Ranked query reaches Slippi's handler, not the Hack Pack's (shared 0xFA)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


class RankedMenuWiringTest(unittest.TestCase):
    def test_empty_0xfa_goes_to_slippi_before_any_other_0xfa_branch(self):
        host = source("port/app/source_host.cpp")
        route = host.index("if (c == slippi::online::CMD_YOUGAME_RANKED && n == 0) {")
        self.assertIn("return slippi::online::handle(c, p, n, reply);", host[route:route + 600])
        for branch in ("if (c == MU_HP_COMMAND && n >= 1 && p[0] == MU_HP_OP_CPU_WORD) {", "if (c == MU_HP_COMMAND) {"):
            self.assertLess(route, host.index(branch), branch)
        # The other 0xFA users always carry a payload, so they never take an empty 0xFA.
        self.assertIn("if (c == CMD_REPLAY_GATE && n == 5)", host)
        self.assertIn("if (c == CMD_VOICE_MATCH && n == 8)", host)

    def test_ids_still_collide_so_the_route_is_needed(self):
        bridge = source("port/runtime/hle/native_slippi_bridge.h")
        self.assertRegex(bridge, r"CMD_YOUGAME_RANKED = 0xFA")
        self.assertRegex(source("sourceport/game/shim/mu_hp.c"), r"#define MU_HP_COMMAND\s+0xFAu")

    def test_game_asks_with_an_empty_payload_and_hides_ranked_without_an_answer(self):
        menu = source("sourceport/game/shim/mu_slippi_menu.c")
        self.assertIn("mu_slippi_cmd(CMD_YOUGAME_RANKED, NULL, 0, r, sizeof r, &got)", menu)
        self.assertIn("(sel == MU_SLP_MODE_RANKED && ranked_offered)", menu)


if __name__ == "__main__":
    unittest.main()
