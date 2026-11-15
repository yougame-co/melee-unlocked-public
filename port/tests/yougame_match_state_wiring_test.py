"""Host-only match presence and verbatim native header pins; no app or user profile access."""
import hashlib
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MatchStateWiring(unittest.TestCase):
    def test_native_headers_are_the_supplied_0b5798f46_copies(self):
        for name, digest in {
            'yougame_start.h': '2e0ba3ca0b2372bd103b23008bba98f3b682936b66af23c795cafc454c4956a1',
            'yougame.h': '67f9e2053e91ba00b95e2821437a675d46c5d68caf82c9ec378454e65acab34e',
        }.items():
            self.assertEqual(hashlib.sha256((ROOT / 'port/third_party/yougame_start' / name).read_bytes()).hexdigest(), digest)

    def test_online_start_uses_connected_frame_one_even_before_a_previous_report(self):
        online = (ROOT / 'port/runtime/hle/slippi_online.cpp').read_text()
        start = online.index('yougame::online_match_state(true);')
        self.assertIn('if (!is_disconnected()) ', online[start-30:start])
        self.assertIn('if (frame == 1)', online[start-500:start])
        self.assertLess(start, online.index('if (!g_summary.active && !is_disconnected())'))
        self.assertEqual(online.count('yougame::online_match_state(true);'), 1)
        self.assertIn('yougame::online_match_state(false);', online[online.index('static void finish_summary(const char* result) {'):])

    def test_only_worker_sends_the_hint_and_refusals_bypass_gameplay_policy(self):
        link = (ROOT / 'port/runtime/hle/yougame_link.cpp').read_text()
        self.assertIn('match_hint_worker().start([](int open) { yg_match_state(open, nullptr, nullptr); });', link)
        sender = link.split('void yg_match_state(int open, yg_callback, void*) {', 1)[1].split('\n}', 1)[0]
        self.assertIn('{"type", "matchState"}', sender)
        self.assertIn('{"open", open != 0}', sender)
        self.assertNotIn('bridge(', sender)
        hook = link.split('void online_match_state(bool open) {', 1)[1].split('\n}', 1)[0]
        self.assertNotIn('bridge', hook)
        self.assertNotIn('std::thread', hook)
        self.assertNotIn('available()', hook)
        self.assertIn('match_hint_worker().set(open);', hook)
        for fn in ['game_ended(const GameEnd& end)', 'end_session(Leave why)']:
            body = link.split('void ' + fn + ' {', 1)[1].split('\n}', 1)[0]
            self.assertIn('online_match_state(false);', body)

    def test_mac_hook_and_frameworks_are_enabled_without_enabling_linux_hook(self):
        link = (ROOT / 'port/runtime/hle/yougame_link.cpp').read_text()
        self.assertIn('#if defined(_WIN32) || defined(__APPLE__)\n#define YOUGAME_START_IMPLEMENTATION', link)
        self.assertIn('#if !defined(_WIN32) && !defined(__APPLE__)\nextern "C" yg_start_result yg_start', link)
        self.assertIn('o.tray_from = MELEE_YOUGAME_MAC_TRAY_FROM;', link)
        build = (ROOT / 'port/mac/build_full.py').read_text()
        for framework in ['CoreServices', 'CoreFoundation']:
            self.assertIn("'-framework','" + framework + "'", build)


if __name__ == '__main__':
    unittest.main()
