"""The Mac build offers Ranked exactly when the CMake builds (Windows, Linux) do."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


class MacRankedBranchTest(unittest.TestCase):
    def test_mac_default_follows_cmake_option(self):
        cmake = source("port/CMakeLists.txt")
        default = re.search(r'option\(YOUGAME_RANKED_BRANCH "[^"]*" (ON|OFF)\)', cmake)
        self.assertIsNotNone(default)
        build = source("port/mac/build_full.py")
        self.assertIn(f"RANKED_BRANCH = {default.group(1) == 'ON'}", build)

    def test_mac_build_defines_it_for_the_host_units(self):
        build = source("port/mac/build_full.py")
        self.assertRegex(build, r"if RANKED_BRANCH and str\(source\)\.startswith\(\('port/runtime/', 'port/app/', "
                                r"'port/linux/', 'port/mac/'\)\):\s*\n(?:\s*#[^\n]*\n)?\s*opts \+= \['-DYOUGAME_RANKED_BRANCH=1'\]")
        # Before the object fingerprint, so a changed switch recompiles every affected object.
        self.assertLess(build.index("-DYOUGAME_RANKED_BRANCH=1"), build.index("fingerprint = hashlib.sha256(repr(opts)"))

    def test_policy_reads_the_define(self):
        policy = source("port/runtime/hle/yougame_mode_policy.h")
        self.assertIn("#if defined(YOUGAME_RANKED_BRANCH) && YOUGAME_RANKED_BRANCH", policy)
        self.assertIn("constexpr bool yougame_ranked_offered = true;", policy)


if __name__ == "__main__":
    unittest.main()
