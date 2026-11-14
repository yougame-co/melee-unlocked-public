"""YouGame builds report Stereo to the game so peers' memory stays equal (upstream 0.9.95 made it a saved choice)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SoundModeTest(unittest.TestCase):
    def test_game_sees_stereo_and_the_choice_is_still_saved(self):
        host = (ROOT / "port/app/source_host.cpp").read_text(encoding="utf-8")
        self.assertRegex(host, r"\nint32_t h_sound_mode\(\) \{ return 1; \}\n")
        self.assertIn("if (!console_sound_mode().set(mode))", host)
        # The game copies the value into its own memory at boot.
        shim = (ROOT / "sourceport/game/shim/mu_os.c").read_text(encoding="utf-8")
        self.assertIn("u32 OSGetSoundMode(void) { return (u32) mu_host->sound_mode(); }", shim)
        source = (ROOT / "port/app/main_source.cpp").read_text(encoding="utf-8")
        self.assertRegex(source, r"int32_t h_sound_mode\(\) \{ return 1; \}")


if __name__ == "__main__":
    unittest.main()
