"""Ranked extension integration that cannot run without the production host."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RankedFixWiringTest(unittest.TestCase):
    def test_mapping_refusal_is_logged_before_return(self):
        source = (ROOT / "port/runtime/hle/slippi_online.cpp").read_text()
        refusal = source.split("yougame::ranked_rules::selection_error(g_ranked_rules)", 1)[1]
        self.assertIn('host::log("yougame: Ranked refused: %s", reason);', refusal.split("return reason;", 1)[0])

    def test_nosp_source_list_keeps_readers_but_excludes_command_provider(self):
        source = (ROOT / "sourceport/game/CMakeLists.txt").read_text()
        removal = source.split("if(MU_NO_SLIPPI)", 1)[1].split("endforeach()", 1)[0]
        self.assertIn("mu_slippi_menu.c", removal)
        self.assertNotIn("mu_ranked_set.c", removal)
        shim = (ROOT / "sourceport/game/shim/mu_ranked_set.c").read_text()
        self.assertEqual(shim.count("#ifdef MU_NO_SLIPPI\n    return 0;\n#else"), 2)

    def test_new_cpp_targets_are_available_on_linux(self):
        source = (ROOT / "CMakeLists.txt").read_text()
        targets = source.split("option(MELEE_BUILD_NATIVE_LINUX", 1)[0]
        self.assertIn("foreach(variant optional nosp)", targets)
        linux = (ROOT / "tools/build_linux.py").read_text()
        for variant in ("optional", "nosp"):
            self.assertIn(f"port_yougame_ranked_{variant}_test", linux)

    def test_one_optional_command_dispatches_both_selectors(self):
        source = (ROOT / "port/runtime/hle/slippi_online.cpp").read_text()
        handler = source.split("case CMD_YOUGAME_DATA:", 1)[1].split("case CMD_YOUGAME_RANKED:", 1)[0]
        self.assertIn("switch (payload[0])", handler)
        self.assertIn("case MU_YG_SUB_RANK:", handler)
        self.assertIn("case MU_YG_SUB_POLICY:", handler)
        self.assertIn("default: return false;", handler)
        wire = (ROOT / "port/runtime/abi/mu_ranked_wire.h").read_text()
        self.assertNotIn("0xFF", wire)


if __name__ == "__main__":
    unittest.main()
