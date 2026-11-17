"""Standalone direct links (11-17): a copy the player started itself talks to the app
over the local channel. When that hello's netBroker carries `direct`, the app already has the game's process
and puts the page's ticket in the links pool, so the game takes direct links like an app-started copy.
Without `direct` it keeps own UDP. Windows only: the app offers no direct to a Unix standalone copy."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
LINK = (ROOT / "port/runtime/hle/yougame_link.cpp").read_text(encoding="utf-8-sig")


class StandaloneLinksWiring(unittest.TestCase):
    def windows_hello(self):
        start = LINK.index("const auto mode = transport::Mode::hello(hello);")
        return LINK[start:LINK.index("#else", start)]

    def test_channel_hello_with_direct_takes_links(self):
        block = self.windows_hello()
        self.assertIn("g_direct = (g_stdio || via_channel) && mode.direct;", block)
        self.assertIn('if (via_channel) host::log("yougame channel: hello direct=%d"', block)

    def test_broker_and_version_refusal_stay_app_child_only(self):
        block = self.windows_hello()
        self.assertIn("g_broker = g_stdio && mode.broker;", block)
        # An unknown links version on the channel falls back to own UDP rather than refusing to play.
        self.assertIn("if (g_stdio && mode.unsupported) throw", block)

    def test_unix_channel_keeps_own_sockets(self):
        self.assertIn("if (route != transport::DatagramRoute::Standalone && !via_channel) g_broker = true;", LINK)
        self.assertIn("g_direct = g_stdio && route == transport::DatagramRoute::Ring;", LINK)

    def test_links_decision_reads_the_hello(self):
        self.assertIn("const bool links_hello = g_direct;", LINK)

    def test_links_reattach_after_app_reconnect(self):
        # A self-started copy outlives the app: a new channel connection attaches to the new section.
        self.assertIn("same_app = g_stdio || g_links_epoch == epoch;", LINK)
        self.assertIn("if (same_app && yg_enet_channel_ready()) return;", LINK)
        self.assertIn("g_links_epoch = epoch;", LINK)


if __name__ == "__main__":
    unittest.main()
