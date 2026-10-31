"""Source regressions for the v0.9.71 queue and Dolphin import conflicts.

Behavioral queue/profile checks and Windows validation run in later release
stages. Existing listing, loader, bridge and focus tests remain unchanged.
"""
from pathlib import Path
import re
import unittest

from relay_pads_wiring_test import block, code, function

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


class YouGame0971MergeTest(unittest.TestCase):
    def test_conflicted_files_have_no_markers(self):
        for path in ("port/runtime/gx/frame_queue.h",
                     "port/runtime/host/dolphin_profile.cpp"):
            with self.subTest(path=path):
                self.assertIsNone(re.search(r"^(?:<{7}|={7}|>{7})(?: |$)", source(path), re.M))

    def test_both_producers_use_memory_limit_inside_wait_accounting(self):
        queue = code(source("port/runtime/gx/frame_queue.h"))
        for name in ("push", "push_and_recycle"):
            with self.subTest(name=name):
                body = function(queue, name)
                self.assertRegex(body, r"\{\s*host::perf::Scope wait_cost\(host::perf::Stage::Wait\);"
                                       r"\s*changed.wait\(lock, \[&\] \{ return finished \|\| has_room\(\); \}\);\s*\}")
                self.assertLess(body.index("has_room()"), body.index("if (finished) return false;"))
                self.assertLess(body.index("if (finished) return false;"), body.index("frames.push_back("))
        room_start = re.search(r"bool has_room\(\) const\s*\{", queue)
        self.assertIsNotNone(room_start)
        room = block(queue, room_start.end())
        for condition in ("if (frames.size() >= 32) return false;",
                          "if (frames.size() < 2) return true;",
                          "for (const Frame& f : frames) bytes += frame_bytes(f);",
                          "return bytes < kQueuedBytes;"):
            self.assertIn(condition, room)

    def test_dolphin_uses_upstream_null_checked_full_action_table(self):
        profile = source("port/runtime/host/dolphin_profile.cpp")
        self.assertIn("constexpr const char* kDolphinKeys[kProfileActions] = {", profile)
        read = function(code(profile), "dolphin_profile_read")
        self.assertRegex(read, r"for \(int i = 0; i < kProfileActions; \+\+i\)\s*"
                               r"if \(kDolphinKeys\[i\] && key == kDolphinKeys\[i\]\) values\[i\] = value;")
        # The old assertion counted only mapped keys; it rejects upstream's null-filled tail.
        self.assertNotIn("static_assert(std::size(kDolphinKeys)", profile)

    def test_exit_reason_bridge_remains_connected(self):
        link = source("port/runtime/hle/yougame_link.cpp")
        notify = function(code(link), "notify_mod_refused")
        self.assertIn("mode_policy::mod_exit_request(active(), stdio_bridge(), explicit_layer, refusal, mod, version)", notify)
        self.assertIn("bridge_stdio(request, 500)", notify)
        reason = source("port/runtime/hle/yougame_exit_reason.h")
        self.assertIn('"exitReason"', reason)
        self.assertIn("yougame::notify_mod_refused(!g_explicit_mod_layers.empty(), yougame::mode_policy::ModRefusal::ModdedDol)",
                      source("port/app/source_host.cpp"))

    def test_new_regression_is_registered_in_both_build_paths(self):
        for path, prefix in (("CMakeLists.txt", "port/"), ("port/CMakeLists.txt", "")):
            with self.subTest(path=path):
                command = ('add_test(NAME port_yougame_0971_merge COMMAND "${Python3_EXECUTABLE}" '
                           '"${CMAKE_CURRENT_SOURCE_DIR}/' + prefix + 'tests/yougame_0971_merge_test.py")')
                self.assertEqual(source(path).count(command), 1)


if __name__ == "__main__":
    unittest.main()
