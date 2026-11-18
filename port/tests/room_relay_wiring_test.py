"""Offline source wiring for the room relay: an app whose links helper is missing (directAttach/directOpen
answered no_direct, or never answered) carries the match's ENet datagrams over the room's msg relay."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8-sig")


def between(text, start, end):
    head = text.index(start)
    return text[head:text.index(end, head)]


class RoomRelayWiringTest(unittest.TestCase):
    def test_relay_header_contract(self):
        relay = source("port/runtime/hle/yougame_room_relay.h")
        self.assertIn('constexpr const char* kKind = "mu.relay";', relay)
        self.assertIn('constexpr const char* kLog = "yougame: no direct links helper, using the room relay";', relay)
        self.assertIn("constexpr int kFlushMs = 16;", relay)
        self.assertIn("constexpr double kMsgsPerSec = 60", relay)
        self.assertIn("constexpr size_t kMaxFrame = 12 * 1024;", relay)
        # One msg with no `to`: Rooms relays it to the match's other seats.
        self.assertIn('out.push_back({{"t", "msg"}, {"d", {{"k", kKind}, {"s", me}, {"p", std::move(p)}}}});', relay)
        self.assertNotIn('{"to"', relay)
        # Only the entries addressed to this seat.
        self.assertIn("if (e[0].get<int>() != me) continue;", relay)
        header = source("port/third_party/enet/include/enet/yg_enet_socket.h")
        self.assertIn("YG_RELAY_LINK0=0xFE00", header)
        self.assertIn("constexpr unsigned kLink0 = 0xFE00;", relay)

    def test_backend_branches_leave_the_ring_alone(self):
        c = source("port/third_party/enet/yg_enet_socket.c")
        for fn in ("int yg_enet_relay_begin(", "void yg_enet_relay_end(", "int yg_enet_relay_in("):
            self.assertIn(fn, c)
        self.assertIn("if(relay_on) return relay_out(a,buffers,count,total);", c)
        self.assertIn("if(relay_on) return relay_take(s,a,buffers,count);", c)
        self.assertIn("n=relay_link(link)?2:state(link);", c)
        # Bounded queue, oldest out first.
        self.assertIn("relay_drops", c)
        # wait() blocks on a condition variable on both platforms.
        self.assertIn("SleepConditionVariableSRW", c)
        self.assertIn("pthread_cond_timedwait", c)

    def test_matchmaking_attach_goes_to_the_relay(self):
        link = source("port/runtime/hle/yougame_link.cpp")
        probe = between(link, "const auto probe_relay = [&](const char* when) {", "const auto relay_flag")
        self.assertIn("catch (const BridgeNoAnswer& e) { why = \"the app did not answer the attach\";", probe)
        self.assertIn("if (!no_direct && !attach_refused())", probe)
        self.assertIn("relay_self = true;", probe)
        self.assertIn("room_relay::kLog", probe)
        # Only an explicit no_direct is remembered, for a cooldown; a working attach clears it.
        self.assertIn("if (!no_helper_cooling()) {", probe)
        self.assertIn("if (no_direct) { no_helper_cooldown();", probe)
        self.assertIn("try { attach_links(); helper_attached(); return; }", probe)
        self.assertIn("constexpr uint64_t kNoHelperCooldownMs = 60000;", link)
        self.assertNotIn("g_no_helper_epoch", link)
        # Every links search probes before its first candidate (friends, 1v1, Doubles, the lobby).
        self.assertIn('if (links_mode) probe_relay(lobby_route ? "before the lobby" : "before the match");', link)
        self.assertLess(link.index('probe_relay(lobby_route ? "before the lobby"'), link.index("if (invited && !adopted) hello_by"))
        # The candidate says so, and a relaying seat makes the whole match relay.
        self.assertIn('if (links_mode && relay_self) cand["relay"] = true;', link)
        self.assertIn("for (const auto& kv : cands) if (relay_flag(kv.second)) relay_match = true;", link)
        self.assertIn("relay_start(room.get(), room->socket.get(), my_seat + 1, ports)", link)
        self.assertIn('relay_match ? std::string("relay")', link)
        # kHelperMissing stays only for a relay that cannot start (no room socket).
        block = between(link, "if (relay_match) {", "} else if (links_mode) {")
        self.assertIn("return fail(console::kHelperMissing, false);", block)
        self.assertNotIn("if (helper_missing) return fail(console::kHelperMissing, false);", link)

    def test_no_relay_survives_into_a_match_on_links(self):
        link = source("port/runtime/hle/yougame_link.cpp")
        stale = between(link, "void relay_clear_stale() {", "\n}\n")
        self.assertIn("if (!yg_enet_relay_active()) return;", stale)
        self.assertIn("relay_stop(nullptr);", stale)
        # A parked room it adopts (keep_links: end_links never ran) ends its relay.
        self.assertIn("if (adopted && relay_running(room.get())) { yg_enet_enable(0); relay_stop(room.get()); }", link)
        self.assertLess(link.index("if (adopted && relay_running(room.get()))"), link.index("room->games = transport::Games{};"))
        # Backstop: the links branch and the console attach end any relay still on before links start.
        self.assertIn("} else if (links_mode) {\n    relay_clear_stale();\n    try {", link)
        attach = between(link, "  bool attach(int ms) {", "\n  }\n")
        self.assertLess(attach.index("relay_clear_stale();"), attach.index("datagrams = yg_enet_create();"))

    def test_relay_sends_read_no_open_state(self):
        link = source("port/runtime/hle/yougame_link.cpp")
        self.assertIn("return broker_ ? broker_->send_now(j, [](const json& q, int ms) { return bridge(q, ms); }) : send(j);", link)
        stream = source("port/runtime/hle/yougame_transport.h")
        send_now = between(stream, "bool send_now(const json& m, const Request& via) {", "\n  }\n")
        self.assertNotIn("request_", send_now)

    def test_relay_ends_with_the_links(self):
        room = source("port/runtime/hle/yougame_room.h")
        self.assertIn("std::function<void()> end_relay = [] {};", room)
        end_links = room[room.index("io.end_relay();") - 400:room.index("io.end_relay();")]
        self.assertIn("end_links", end_links)
        link = source("port/runtime/hle/yougame_link.cpp")
        self.assertIn("io.end_relay = [&r] { if (relay_running(&r)) { yg_enet_enable(0); relay_stop(&r); } };", link)
        self.assertIn("~Socket() { relay_forget(this); close(); }", link)
        self.assertIn("if (!relay_intercept(this, out)) return true;", link)

    def test_console_path_relays(self):
        link = source("port/runtime/hle/yougame_link.cpp")
        attach = between(link, "  bool attach(int ms) {", "\n  }\n")
        self.assertIn('use_relay("no answer")', attach)
        self.assertIn("bridge_raw(", attach)
        self.assertIn("io.relay_open = [b](int me,const std::vector<int>& ports) { return b->relay_open(me,ports); };", link)
        console = source("port/runtime/hle/yougame_console_link.cpp")
        self.assertIn('if (i.relay()) d["relay"] = true;', console)
        self.assertIn("if (i.relay() || !relay_peers.empty()) {", console)
        self.assertIn('if (!relayed && !relay_peers.empty() && !open_relay("a friend has no links helper"))', console)
        header = source("port/runtime/hle/yougame_console_link.h")
        self.assertIn("std::function<bool()> relay = [] { return false; };", header)

    def test_relay_tests_run_with_the_direct_tests(self):
        script = source("port/tests/yougame_direct_test.sh")
        self.assertIn("yg_enet_relay_test.c", script)
        self.assertIn("yougame_room_relay_test.cpp", script)


if __name__ == "__main__":
    unittest.main()
