"""Stage map JObjs get the same ID-table key in every process (Friends join-in-progress compares the table)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def section(patch, path):
    return patch.split(f"+++ b/{path}", 1)[1].split("\ndiff --git ", 1)[0]


class StageJointKeyTest(unittest.TestCase):
    def setUp(self):
        self.patch = (ROOT / "sourceport/patches/melee-native.patch").read_text(encoding="utf-8", errors="surrogateescape")

    def test_stage_joint_is_static(self):
        ground = section(self.patch, "src/melee/gr/ground.c")
        head = " static HSD_JObj* get_jobj_inline(float phi_f0)\n"
        self.assertIn(head, ground, "get_jobj_inline is no longer a context line of a ground.c hunk")
        body = ground.split(head, 1)[1].split("\n@@ ", 1)[0]
        self.assertIn("+#ifdef MU_NATIVE\n", body)
        native = body.split("+#ifdef MU_NATIVE\n", 1)[1].split("+#else\n", 1)[0]
        self.assertIn("+    static HSD_Joint sp14;\n", native)
        self.assertIn("+    sp14 = Ground_803B7E0C;\n", native)
        self.assertIn("jobj = HSD_JObjLoadJoint(&sp14);", body)

    def test_jobj_still_keys_by_joint_address(self):
        # If JObjLoad stops keeping the joint's address, the static joint may no longer be needed.
        self.assertIn("jobj->id = (HSD_IDKey) joint;", section(self.patch, "src/sysdolphin/baselib/jobj.c"))


if __name__ == "__main__":
    unittest.main()
