"""Pin the CSS diagnostic to the stored decomp patch and the native change-only logger."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CssStateWiringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        patch = (ROOT / "sourceport/patches/melee-native.patch").read_text()
        cls.css = patch.split("diff --git a/src/melee/mn/mncharsel.c b/src/melee/mn/mncharsel.c\n", 1)[1].split("diff --git ", 1)[0]
        cls.shim = (ROOT / "sourceport/game/shim/mu_css_state.c").read_text()

    def test_frame_hook_is_native_and_at_end_of_css_frame(self):
        hunk = next(h for h in re.split(r"(?m)^@@", self.css)
                    if "@@ void mnCharSel_Scene_OnFrame(void)" in h.splitlines()[0]
                    and "+    mu_css_state_frame();" in h)
        self.assertIn("+#ifdef MU_NATIVE\n+    mu_css_state_frame();\n+#endif\n }", hunk)
        self.assertNotIn("mu_slippi_css_online()", hunk)
        self.assertEqual(self.css.count("+    mu_css_state_frame();"), 2)  # entry and frame
        self.assertIn("+    mu_css_state_reset();", self.css)
        self.assertIn("mu_ranked_reveal_enter();\n+    mu_css_state_frame();", self.css)

    def test_door_values_use_dynamic_icon_table_and_coin_state(self):
        for line in (
            "door->sel_icon < CSS_NONE ? icons[door->sel_icon].char_kind : -1",
            "doors[i][0] = door->p_kind;", "doors[i][1] = door->sel_icon;",
            "doors[i][2] = fighter;",
            "mnCharSel_804D6CB0->match_type == TRAINING_MODE ? 2 : mnCharSel_804D6CF5",
            "doors[i][3] = i < coin_count &&",
            "mnCharSel_804A0BD0[i] != NULL && mnCharSel_804A0BD0[i]->x5 == 0",
            "mu_hp_ckind_name(fighter, 1)", "mu_mex_fighter_name(ext)",
            "fighter <= ChKind_Popo", "lbl_803D4FDC[fighter]",
            "gm_GetCurrentGameMode(), gm_GetCurrentSceneIndex()",
        ):
            self.assertIn(line, self.css)

    def test_logger_compares_saved_state_before_emitting(self):
        self.assertIn("#ifdef MU_NATIVE", self.shim)
        self.assertIn('"css-state: {', self.shim)
        compare = self.shim.index("if (previous_valid && memcmp(previous_doors, doors, sizeof previous_doors) == 0) return;")
        emit = self.shim.index("mu_host->log(line);")
        save = self.shim.index("memcpy(previous_doors, doors, sizeof previous_doors);")
        self.assertLess(compare, emit)
        self.assertLess(emit, save)
        self.assertIn("previous_valid = 0;", self.shim)
        self.assertIn("previous_valid = 1;", self.shim[save:])

    def test_squad_count_field_only_on_the_squad_css(self):
        self.assertIn('",\\"squad\\":{\\"count\\":%d}", squad', self.shim)
        self.assertIn("if (squad != previous_squad) previous_valid = 0;", self.shim)
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("needs a C compiler")
        with tempfile.TemporaryDirectory(prefix="css-state-") as directory:
            for squad in ([], ["-DMU_SQUAD=1"]):
                exe = Path(directory) / ("squad" if squad else "plain")
                subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-DMU_NATIVE", *squad,
                                "-I", str(ROOT / "sourceport/game/include"), "-I", str(ROOT / "port/runtime/abi"),
                                str(ROOT / "port/tests/css_state_test.c"),
                                str(ROOT / "sourceport/game/shim/mu_css_state.c"), "-o", str(exe)],
                               check=True, capture_output=True, text=True)
                out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
                self.assertRegex(out, r"css_state: \d+ checks passed")


if __name__ == "__main__":
    unittest.main()
