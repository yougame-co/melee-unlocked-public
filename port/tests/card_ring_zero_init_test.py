"""Card commands reach the ring without stack bytes (Friends join-in-progress compares the ring)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
CARD = '"${D}/src/sysdolphin/baselib/card.c"'


class CardRingZeroInitTest(unittest.TestCase):
    def test_card_locals_are_zeroed(self):
        cmake = (ROOT / "sourceport/game/CMakeLists.txt").read_text()
        self.assertEqual(cmake.count(CARD), 1)
        rule = cmake.split(f"set_source_files_properties({CARD} PROPERTIES", 1)[1].split(")", 1)[0]
        self.assertEqual(rule.split(), ["COMPILE_OPTIONS", '"-ftrivial-auto-var-init=zero"'])
        # The GCC 14 floor is what makes the flag available on every OS.
        self.assertIn('CMAKE_C_COMPILER_VERSION VERSION_LESS 14', cmake.split("add_library", 1)[0])

    def test_ring_still_takes_whole_stack_commands(self):
        # If the enqueue stops copying whole stack structs, this rule may no longer be needed.
        patch = (ROOT / "sourceport/patches/melee-native.patch").read_text()
        card = patch.split("+++ b/src/sysdolphin/baselib/card.c", 1)[1].split("\ndiff --git ", 1)[0]
        enqueue = card.split("+int fn_803AC168(const CardCmd* cmd)", 1)[1].split("\n+}", 1)[0]
        self.assertIn("memcpy(&commands[idx], cmd, sizeof(CardCmd));", enqueue)
        self.assertIn("+    CardCmd cmd;\n", card)


if __name__ == "__main__":
    unittest.main()
