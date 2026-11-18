"""The pool merge (11-17): a seat Rooms marks links:false (a standalone copy) puts the whole match on own UDP +
STUN, every seat alike; with no such seat nothing changes. A Unix broker copy has no sockets and says so."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
LINK = (ROOT / "port/runtime/hle/yougame_link.cpp").read_text(encoding="utf-8-sig")
TRANSPORT = (ROOT / "port/runtime/hle/yougame_transport.h").read_text(encoding="utf-8-sig")


def body(text, marker):
    start = text.index("{", text.index(marker))
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start + 1:end - 1]


class LinksFalseWiring(unittest.TestCase):
    def test_flag_reads_only_false(self):
        seat = body(TRANSPORT, "inline bool seat_without_links(")
        self.assertIn('seat.find("links")', seat)
        self.assertIn("l->is_boolean() && !l->get<bool>()", seat)

    def test_decision_follows_the_roster(self):
        self.assertIn("const bool links_hello = g_direct;\n  bool links_mode = links_hello;", LINK)
        decide = body(LINK, "const auto decide_transport = [&]")
        self.assertIn("if (!links_hello) return;", decide)
        self.assertIn("!transport::any_seat_without_links(participants)", decide)
        self.assertIn("cands.clear();", decide)
        self.assertIn("sent_at.clear();", decide)
        self.assertIn('participants = m["participants"]; decide_transport(); }', LINK)
        # Decided before the lobby's first attach (an adopted lobby's roster) and before any address goes out.
        self.assertLess(LINK.index("  decide_transport();\n"),
                        LINK.index('if (links_mode) probe_relay(lobby_route ? "before the lobby" : "before the match");'))

    def test_own_socket_opens_once_a_match_needs_it(self):
        gate = LINK.index("if (full && !links_mode && udp == INVALID_SOCKET) {")
        self.assertLess(gate, LINK.index("// My addresses, point to point, each peer with a token of its own."))
        block = body(LINK, "if (full && !links_mode && udp == INVALID_SOCKET) {")
        self.assertIn("if (!own_udp_allowed) {", block)
        self.assertIn("open_own_udp();", block)
        self.assertIn('std::getenv("YOUGAME_UDP_PORT")', body(LINK, "const auto open_own_udp = [&]() -> std::string"))

    def test_unix_broker_has_no_own_sockets(self):
        self.assertIn("#ifdef _WIN32\n  const bool own_udp_allowed = true;\n#else\n  const bool own_udp_allowed = !g_broker;\n#endif", LINK)

    def test_match_enet_stays_native_until_the_next_search(self):
        select = body(LINK, "void select_match_transport(bool slippi)")
        self.assertIn("yg_enet_select(!slippi && g_direct && !g_own_udp_match);", select)
        self.assertIn("yg_enet_select(!slippi && g_broker && !g_own_udp_match);", select)
        self.assertIn("if (!cancelled() && links_hello && !links_mode) { g_own_udp_match = true; select_match_transport(g_slippi_match); }", LINK)
        self.assertIn("g_own_udp_match = false;\n    select_match_transport(false);", LINK)          # a search starts
        self.assertIn("g_own_udp_match = false; select_match_transport(false); }", LINK)          # the session ends
        self.assertIn("g_own_udp_match = false;\n  select_match_transport(true);", LINK)            # a Slippi match
        self.assertIn('"own UDP (a seat has no YouGame links)"', LINK)

    def test_lobby_links_never_linger(self):
        self.assertIn("early.keep = links_mode;", LINK)
        self.assertIn("if (links_mode && !relay_self) early.links.open<update::Required>", LINK)


if __name__ == "__main__":
    unittest.main()
