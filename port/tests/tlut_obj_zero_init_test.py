"""TLUT objects reach gxData without stack bytes (Friends join-in-progress compares gxData)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
GX_TEXTURE = "+++ b/libs/dolphin/src/dolphin/gx/GXTexture.c"


def function(section, signature):
    return section.split(signature, 1)[1].split("\n }", 1)[0]


class TlutObjZeroInitTest(unittest.TestCase):
    def setUp(self):
        patch = (ROOT / "sourceport/patches/melee-native.patch").read_text(encoding="utf-8", errors="surrogateescape")
        self.assertEqual(patch.count(GX_TEXTURE), 1)
        self.section = patch.split(GX_TEXTURE, 1)[1].split("\ndiff --git ", 1)[0]

    def test_init_clears_the_whole_object_first(self):
        init = function(self.section, "void GXInitTlutObj(")
        clear = "+#ifdef MU_NATIVE\n" + init.split("+#ifdef MU_NATIVE\n", 1)[1].split("+#endif\n", 1)[0]
        self.assertIn("+    memset(t, 0, sizeof(*t));\n", clear)
        # Before any field is written, so the padding after numEntries and loadTlut0's unset bits stay zero.
        self.assertLess(init.index("memset(t, 0, sizeof(*t));"), init.index("t->tlut = 0;"))

    def test_load_still_copies_the_whole_object(self):
        # If GXLoadTlut stops copying the caller's object whole, this clear may no longer be needed.
        load = function(self.section, "void GXLoadTlut(")
        self.assertIn("r->tlutObj = *t;", load)


if __name__ == "__main__":
    unittest.main()
