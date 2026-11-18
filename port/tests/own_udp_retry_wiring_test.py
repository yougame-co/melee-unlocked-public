"""Own UDP (a standalone seat: no relay) says on screen when it keeps failing, with what to try (116)."""
# SPDX-License-Identifier: GPL-2.0-or-later
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class OwnUdpRetryWiring(unittest.TestCase):
    def setUp(self):
        self.link = (ROOT / 'port/runtime/hle/yougame_link.cpp').read_text()
        self.search = (ROOT / 'port/runtime/hle/yougame_search.h').read_text()
        self.settings = (ROOT / 'port/runtime/gx/pc_settings.cpp').read_text()

    def test_line_says_what_to_try(self):
        self.assertIn('kOwnUdpRetry = "We couldn\'t connect you two. Looking for another opponent...\\n"', self.search)
        self.assertIn('"Allow Melee Unlocked through your firewall."', self.search)

    def test_failed_own_udp_moves_onto_the_room_relay_when_every_seat_agrees(self):
        start = self.link.index('const auto own_udp_relay = [&]() -> bool {')
        body = self.link[start:self.link.index('\n  };\n', start)]
        self.assertIn('{"kind", "mu-relay-ok"}, {"v", 1}, {"compat", compat}', body)
        self.assertIn('while (agreed.size() < want.size() && now_ms() < until && !cancelled())', body)
        self.assertIn('if (cancelled() || agreed.size() < want.size())', body)   # all seats or nobody relays
        self.assertIn('relay_start(room.get(), room->socket.get(), my_seat + 1, ports)', body)
        self.assertIn('g_relay_own = true;', body)
        self.assertIn('relay_match = true;', body)
        # The relay's ENet takes YouGame sockets even on a copy with no links; the match binds no own port.
        self.assertIn('((g_direct && !g_own_udp_match) || g_relay_own)', self.link)
        self.assertIn('out.udp_port = links_mode || relay_match ? 0 : bound_port;', self.link)
        self.assertIn('links_hello && !links_mode && !relay_match) { g_own_udp_match = true;', self.link)
        self.assertEqual(self.link.count('g_relay_own = false;'), 4)   # its definition and every g_own_udp_match reset

    def test_second_own_udp_failure_feeds_the_counter_and_sets_the_hint(self):
        start = self.link.index('} else if (!punch(udp, my_seat + 1, peers, ingress, cancelled) && !own_udp_relay()) {')
        block = self.link[start:self.link.index('if (cancelled()) return fail("Cancelled");\n  { std::lock_guard', start)]
        self.assertIn('repeated = g_direct_link_failures.failed(unreached, room->me);', block)
        self.assertIn('!(peer.found && peer.acked)) unreached.push_back(p);', block)
        self.assertIn('if (repeated) g_own_udp_blocked = true;', block)
        self.assertIn('repeated && !direct && !lobby_match ? search_policy::kLinkRetry : search_policy::kLinkFailed', block)

    def test_status_shows_the_hint_and_any_connect_clears_it(self):
        self.assertIn('g_own_udp_blocked && s == search_policy::kLinkRetry ? std::string(search_policy::kOwnUdpRetry) : s', self.link)
        self.assertIn('if (!cancelled()) g_own_udp_blocked = false;', self.link)
        self.assertIn('g_own_udp_blocked = false;   // links relay', self.link)

    def test_retry_line_only_on_the_automatic_research_and_never_direct(self):
        self.assertIn('repeated_link_failure = !direct && g_direct_link_failures.repeated() &&', self.link)
        self.assertIn('now_ms() - g_last_link_failure < search_policy::kRetryLineMs;', self.link)
        self.assertEqual(self.link.count('g_last_link_failure = now_ms();'), 2)

    def test_overlay_draws_retry_lines(self):
        self.assertIn('status == yougame::search_policy::kLinkRetry || status == yougame::search_policy::kOwnUdpRetry ||', self.settings)
        self.assertIn('#include "yougame_search.h"', self.settings)

    def test_firewall_note_once_per_run_before_the_first_bind_windows_only(self):
        start = self.link.index('const auto open_own_udp = [&]() -> std::string {')
        body = self.link[start:self.link.index('udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);', start)]
        self.assertIn('#ifdef _WIN32', body)
        self.assertIn('if (!g_firewall_bound) {', body)
        self.assertIn('if (g_status == "searching" || g_status == blend::kSearchYouGameOnly) {', body)
        self.assertIn('g_firewall_note_until = now_ms() + search_policy::kFirewallNoteMs;', body)
        self.assertIn('"Choose Allow so other players can reach you."', self.search)
        paused = self.link[self.link.index('std::string search_paused_line() {'):]
        paused = paused[:paused.index('\n}\n')]
        self.assertIn('if (!g_search_paused.empty()) return g_search_paused;', paused)
        self.assertIn('std::string(search_policy::kFirewallNote)', paused)

    def test_every_connect_attempt_sends_one_link_report_without_addresses(self):
        transport = (ROOT / 'port/runtime/hle/yougame_transport.h').read_text()
        self.assertIn('{{"t", "linkReport"}, {"ok", ok}, {"via", via}, {"ms", ms}, {"transport", own ? "own" : "links"}}', transport)
        self.assertEqual(self.link.count('room->socket->send(transport::link_report('), 3)
        self.assertIn('transport::link_report(false, "none", now_ms() - connect_start, true)', self.link)
        self.assertIn('transport::link_report(false, "links", now_ms() - connect_start, false)', self.link)
        self.assertIn('std::string(answered_via(peers.front(), peers.front().chosen))', self.link)
        self.assertIn('if (!own && !path.empty()) out["path"] = path;', transport)
        self.assertIn('r == "relay" ? 2', transport)
        self.assertIn('transport::links_path(room->links.ids,', self.link)


if __name__ == '__main__':
    unittest.main()
